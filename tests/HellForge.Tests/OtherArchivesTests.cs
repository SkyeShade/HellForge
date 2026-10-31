using System.Text;
using HellForge.Core;

// Mods shared as .rar or .7z (OtherArchives, SharpCompress): unpacked on any system, plain files only, and taken from the
// mods folder itself when dropped there (ModInstaller.TakeDropped).
static class OtherArchivesTests
{
    // A RAR 4 archive storing the files uncompressed (method 0x30), as the RAR technote lays it out: RAR's own tools are not
    // free, so the tests write their archives themselves. Names use '\', as RAR does.
    static byte[] Rar(params (string Name, byte[] Data)[] files)
    {
        var o = new MemoryStream();
        void Block(byte type, ushort flags, byte[] body)
        {
            var head = new MemoryStream();
            head.WriteByte(type);
            head.Write(BitConverter.GetBytes(flags));
            head.Write(BitConverter.GetBytes((ushort)(body.Length + 7)));
            head.Write(body);
            var rest = head.ToArray();
            o.Write(BitConverter.GetBytes((ushort)Crc32(rest)));
            o.Write(rest);
        }
        o.Write([0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00]);
        Block(0x73, 0, new byte[6]);
        foreach (var (name, data) in files)
        {
            var bytes = Encoding.ASCII.GetBytes(name.Replace('/', '\\'));
            var body = new MemoryStream();
            body.Write(BitConverter.GetBytes((uint)data.Length)); // packed size
            body.Write(BitConverter.GetBytes((uint)data.Length)); // unpacked size
            body.WriteByte(2); // Windows
            body.Write(BitConverter.GetBytes(Crc32(data)));
            body.Write(BitConverter.GetBytes(0x5B000000u)); // a DOS time
            body.WriteByte(20); // RAR 2.0 unpacks it
            body.WriteByte(0x30); // stored
            body.Write(BitConverter.GetBytes((ushort)bytes.Length));
            body.Write(BitConverter.GetBytes(0x20u)); // an archive file's attributes
            body.Write(bytes);
            Block(0x74, 0x8000, body.ToArray());
            o.Write(data);
        }
        Block(0x7B, 0x4000, []);
        return o.ToArray();
    }

    static uint Crc32(byte[] data)
    {
        var crc = 0xFFFFFFFFu;
        foreach (var b in data)
        {
            crc ^= b;
            for (var i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xEDB88320u & (uint)-(int)(crc & 1));
        }
        return ~crc;
    }

    // a data patch, so a test mod changes something (a manifest alone is not a mod)
    const string Patch = "9ba626afa44a3aa3.patch_0";

    static byte[] Text(string text) => Encoding.UTF8.GetBytes(text);
    static string Manifest(string id) => $$"""{"format":1,"id":"{{id}}","name":"{{id}}","version":"1","gameplay":true}""";

    // A 7z written by the system's libarchive tar: Windows' tar.exe, or bsdtar on Linux (as CosmeticPacksTests' does).
    static void SevenZip(string from, string what, string archive)
    {
        var tar = OperatingSystem.IsLinux() ? "bsdtar" : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "tar.exe");
        using var process = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(tar)
            { ArgumentList = { "--format", "7zip", "-cf", archive, "-C", from, what }, UseShellExecute = false, CreateNoWindow = true })!;
        process.WaitForExit();
        if (process.ExitCode != 0) throw new InvalidOperationException("tar could not write " + archive);
    }

    // A RAR 5 archive whose one file is password-protected (SharpCompress's tests, MIT: Rar5.comment.filesOnly.rar).
    const string EncryptedRar5 = "UmFyIRoHAQDB319WAwEEACUj1XYTAwKRAASRAADbmNhBgAABA0NNVDEyMzQ1Njc4OTAxMjM0NcOpIl8OhFoCAzyQAASQAKSDArptO0aAAAELcGF5bG9hZC50eHQwAQADD13c65WTcAZSNvu8bFvPrp44Xmnqdqo/Ccst7oxi06X6PS2Wq0VheH57ncKUCgMTvn6/at61lx/rKCsu2ZwwIftCb4tuniujHXdWUQMFBAA=";

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("A .rar mod is unpacked without Windows' tar and added as a folder; unsafe, protected or broken archives are refused", () =>
        {
            var install = new InstallLayout(Path.Combine(root, "rar-install"));
            install.Create("25480438");
            Directory.CreateDirectory(install.Mods);
            var source = Directory.CreateDirectory(Path.Combine(root, "rar-source")).FullName;
            var rar = Path.Combine(source, "Rar Mod.rar");
            File.WriteAllBytes(rar, Rar(("rar-mod/" + ModCatalog.ManifestName, Text(Manifest("a.rar"))), ("rar-mod/data/9ba626afa44a3aa3.patch_0", Text("rar"))));
            var (candidates, problems) = ModInstaller.Inspect(install, [rar]);
            check(problems.Count == 0 && candidates.Single() is { Id: "a.rar", IsZip: false }, "Read: " + string.Join(" | ", problems));
            ModInstaller.Add(install, candidates.Single());
            var (mods, loadProblems) = ModCatalog.Load(install.Mods);
            check(loadProblems.Count == 0 && mods.Single().Id == "a.rar" &&
                File.ReadAllText(Path.Combine(install.Mods, "rar-mod", "data", "9ba626afa44a3aa3.patch_0")) == "rar",
                "Copied as its folder and loaded: " + string.Join(" | ", loadProblems));

            void Refused(string name, byte[] archive, string why)
            {
                var path = Path.Combine(source, name);
                File.WriteAllBytes(path, archive);
                var (none, refusals) = ModInstaller.Inspect(install, [path]);
                check(none.Count == 0 && refusals.Single().Contains(why), $"{name} is refused ({why}): {string.Join(" | ", refusals)}");
                var target = Path.Combine(root, "rar-refused", name);
                reject(() => OtherArchives.Unpack(path, target));
                check(!Directory.Exists(target) && !Directory.EnumerateDirectories(Path.Combine(root, "rar-refused")).Any(), name + ": nothing is left behind.");
            }
            Directory.CreateDirectory(Path.Combine(root, "rar-refused"));
            Refused("parent.rar", Rar(("../outside.txt", Text("x"))), "unsafe path");
            Refused("drive.rar", Rar(("C:/outside.txt", Text("x"))), "unsafe path");
            Refused("protected.rar", Convert.FromBase64String(EncryptedRar5), "password-protected");
            Refused("not-a.rar", Text("this is not an archive at all, just text named .rar"), "could not unpack");
            check(!File.Exists(Path.Combine(root, "outside.txt")) && !File.Exists(Path.Combine(root, "rar-refused", "outside.txt")), "Nothing was written outside.");
        });

        test("A .rar or .7z dropped into the mods folder is added as a folder; one already installed, or broken, stays and says why", () =>
        {
            if (OperatingSystem.IsLinux() && !File.Exists("/usr/bin/bsdtar"))
            {
                Console.WriteLine("  (skipped: no bsdtar to write the test's .7z)");
                return;
            }
            var install = new InstallLayout(Path.Combine(root, "dropped-install"));
            install.Create("25480438");
            Directory.CreateDirectory(install.Mods);
            var source = Directory.CreateDirectory(Path.Combine(root, "dropped-source")).FullName;
            var seven = Directory.CreateDirectory(Path.Combine(source, "seven-mod")).FullName;
            File.WriteAllText(Path.Combine(seven, ModCatalog.ManifestName), Manifest("a.seven"));
            File.WriteAllText(Path.Combine(Directory.CreateDirectory(Path.Combine(seven, "data")).FullName, Patch), "seven");
            SevenZip(source, "seven-mod", Path.Combine(install.Mods, "Seven Mod.7z"));
            File.WriteAllBytes(Path.Combine(install.Mods, "rar-mod.rar"), Rar((ModCatalog.ManifestName, Text(Manifest("a.dropped"))), ("data/" + Patch, Text("dropped"))));
            File.WriteAllText(Path.Combine(install.Mods, "broken.7z"), "not a 7z");

            var notes = ModInstaller.TakeDropped(install);
            var (mods, _) = ModCatalog.Load(install.Mods);
            check(mods.Select(m => m.Id).Order().SequenceEqual(["a.dropped", "a.seven"]) && Directory.Exists(Path.Combine(install.Mods, "seven-mod")) &&
                Directory.Exists(Path.Combine(install.Mods, "rar-mod")) && !File.Exists(Path.Combine(install.Mods, "Seven Mod.7z")) &&
                !File.Exists(Path.Combine(install.Mods, "rar-mod.rar")), "Both are folders now, their archives gone: " + string.Join(", ", mods.Select(m => m.Id)));
            check(notes.Single().StartsWith("broken.7z: ", StringComparison.Ordinal) && File.Exists(Path.Combine(install.Mods, "broken.7z")),
                "The broken one stays and says why: " + string.Join(" | ", notes));
            check(ModInstaller.TakeDropped(install).SequenceEqual(notes), "Asked again, the same line, the file untouched.");

            // the same mod again: the installed one stays until the player replaces it on Add mods
            File.WriteAllBytes(Path.Combine(install.Mods, "again.rar"), Rar((ModCatalog.ManifestName, Text(Manifest("a.dropped"))), ("data/" + Patch, Text("dropped"))));
            var again = ModInstaller.TakeDropped(install);
            check(again.Any(n => n.StartsWith("again.rar: a.dropped is installed already", StringComparison.Ordinal)) && File.Exists(Path.Combine(install.Mods, "again.rar")) &&
                ModCatalog.Load(install.Mods).Mods.Count(m => m.Id == "a.dropped") == 1, "An installed mod's archive stays: " + string.Join(" | ", again));
            check(!Directory.EnumerateDirectories(install.Temp, "mod-dropped-*").Any() && !Directory.EnumerateDirectories(install.Temp, "mod-unpacked-*").Any(),
                "Nothing is left in temp.");
        });
    }
}
