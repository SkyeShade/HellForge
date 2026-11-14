using System.Diagnostics;
using System.Reflection.Metadata;
using System.Reflection.PortableExecutable;
using System.Security.Cryptography;
using System.Text;
using HellForge.Core;

// The backend pin (HellForge.Core BackendPin) a session carries: its digest's one canonical form, the check's three
// answers, and a build that carries no pin left over from an earlier one in obj/.
static class BackendPinTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, string root)
    {
        test("Backend pin: the digest is every file's path and SHA-256 in ordinal order, and any change refuses the backend", () =>
        {
            var folder = Directory.CreateDirectory(Path.Combine(root, "backend-pin", "backend")).FullName;
            Directory.CreateDirectory(Path.Combine(folder, "lib"));
            File.WriteAllText(Path.Combine(folder, "lib", "b.dll"), "b");
            File.WriteAllText(Path.Combine(folder, "HellForge.LocalBackend.exe"), "exe");
            static string Hex(string text) => Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(text)));
            // ordinal: upper case before lower case, '/' between folders whatever the system's separator
            var expected = Hex(Hex("exe") + " HellForge.LocalBackend.exe\n" + Hex("b") + " lib/b.dll\n");
            var pin = BackendPin.Digest(folder);
            check(pin == expected, $"The digest is not the canonical one: {pin}, expected {expected}.");
            var held = new List<FileStream>();
            try
            {
                check(BackendPin.Check(folder, "", held) == BackendPin.None && held.Count == 0, "A session without a pin checked its backend.");
                check(BackendPin.Check(folder, pin.ToLowerInvariant(), held) == BackendPin.Match && held.Count == 2, "The pinned backend was refused, or its files not held.");
                if (OperatingSystem.IsWindows())
                    check(Throws(() => File.WriteAllText(Path.Combine(folder, "lib", "b.dll"), "swapped")), "A held backend file could be written.");
            }
            finally { foreach (var file in held) file.Dispose(); }
            foreach (var change in new Action[]
                     {
                         () => File.WriteAllText(Path.Combine(folder, "lib", "b.dll"), "c"),
                         () => File.WriteAllText(Path.Combine(folder, "extra.dll"), "x"),
                         () => File.Delete(Path.Combine(folder, "extra.dll")),
                     })
            {
                change();
                var refused = new List<FileStream>();
                try { check(BackendPin.Check(folder, pin, refused) == BackendPin.Refused, "A changed backend folder was not refused."); }
                finally { foreach (var file in refused) file.Dispose(); }
            }
        });

        test("Backend pin: a session built after a pinned build carries no pin from obj/", () =>
        {
            var repository = Repository();
            var pin = new string('A', 64);
            var output = Path.Combine(root, "backend-pin-build");
            var pinned = Build(repository, Path.Combine(output, "pinned"), "-p:HellForgeBackendPin=" + pin);
            check(Constant(pinned) == pin, "The pin passed to the build is not compiled in.");
            var plain = Build(repository, Path.Combine(output, "plain"));
            check(Constant(plain) == "", "A build without a pin kept the earlier build's from obj/.");
        });
    }

    static bool Throws(Action action)
    {
        try { action(); return false; }
        catch (IOException) { return true; }
    }

    // HellForge.Session built in a configuration of its own (its obj/ folder shared by both builds, as two Release builds
    // share theirs); the assembly's path.
    static string Build(string repository, string output, params string[] properties)
    {
        var start = new ProcessStartInfo("dotnet") { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false };
        foreach (var argument in new[] { "build", Path.Combine(repository, "src", "HellForge.Session", "HellForge.Session.csproj"), "-c", "PinTest", "--nologo", "-v", "q", "-o", output })
            start.ArgumentList.Add(argument);
        foreach (var property in properties) start.ArgumentList.Add(property);
        using var process = Process.Start(start)!;
        var text = process.StandardOutput.ReadToEnd() + process.StandardError.ReadToEnd();
        process.WaitForExit();
        if (process.ExitCode != 0) throw new InvalidOperationException("dotnet build failed: " + text);
        return Path.Combine(output, "HellForge.Session.dll");
    }

    // BuildPins.Backend's value, read from the assembly's metadata.
    static string? Constant(string assembly)
    {
        using var stream = File.OpenRead(assembly);
        using var pe = new PEReader(stream);
        var metadata = pe.GetMetadataReader();
        foreach (var handle in metadata.TypeDefinitions)
        {
            var type = metadata.GetTypeDefinition(handle);
            if (metadata.GetString(type.Name) != "BuildPins") continue;
            foreach (var fieldHandle in type.GetFields())
            {
                var field = metadata.GetFieldDefinition(fieldHandle);
                if (metadata.GetString(field.Name) != "Backend") continue;
                var constant = metadata.GetConstant(field.GetDefaultValue());
                var blob = metadata.GetBlobReader(constant.Value);
                return blob.Length == 0 ? "" : blob.ReadUTF16(blob.Length);
            }
        }
        return null;
    }

    static string Repository()
    {
        for (var folder = new DirectoryInfo(AppContext.BaseDirectory); folder != null; folder = folder.Parent)
            if (File.Exists(Path.Combine(folder.FullName, "HellForge.slnx"))) return folder.FullName;
        throw new DirectoryNotFoundException("The repository is not above the tests.");
    }
}
