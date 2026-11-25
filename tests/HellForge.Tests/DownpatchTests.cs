using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json;
using HellForge.Core;
using HellForge.SteamDownload;

// Downpatch files made and applied on small synthetic builds: the binary delta on edits, moves and edge sizes, a pack's
// round trip passing the build's own verification, and the refusals (another source or target build, a damaged pack,
// a full drive, paths that leave the game folder, Steam files that changed, a stopped run).
static class DownpatchTests
{
    static byte[] Bytes(int seed, int length)
    {
        var bytes = new byte[length];
        new Random(seed).NextBytes(bytes);
        return bytes;
    }

    static string Sha1(byte[] bytes) => Convert.ToHexString(SHA1.HashData(bytes));

    // A newer build as Steam has it (Source) and the older one HellForge runs, as an installation's game copy (Game).
    sealed class Fixture
    {
        public readonly string Home, Source, Game;
        public readonly SupportedBuild Build;
        public readonly DownpatchSource Steam;
        public readonly Dictionary<string, byte[]> Target = new(StringComparer.Ordinal);

        public Fixture(string root, string name)
        {
            Home = Path.Combine(root, name);
            Source = Path.Combine(Home, "Steam", "steamapps", "common", "Helldivers 2");
            Game = Path.Combine(Home, "HellForge", "game");
            var big = Bytes(1, 3 << 20);
            // the older build: big.bin with a part cut out, bytes changed in place and a run inserted
            var older = big.Take(100_000).Concat(Bytes(2, 5000)).Concat(big.Skip(150_000)).ToArray();
            for (var i = 2_000_000; i < 2_000_040; i++) older[i] ^= 0x5A;
            var moved = Bytes(3, 200_000);
            var exe = Bytes(4, 300_000);
            var olderExe = (byte[])exe.Clone();
            olderExe[1234] ^= 1;
            Put(Source, "bin/helldivers2.exe", exe);
            Put(Source, "data/game/game.dll", Bytes(5, 100_000));
            Put(Source, "data/big.bin", big);
            Put(Source, "data/renamed.bin", moved);
            Put(Source, "data/settings.ini", "[player's own]"u8.ToArray());
            Put(Source, "data/only-in-steam.bin", Bytes(6, 70_000));
            Put(Source, "bin/nvngx_dlss.dll", Bytes(7, 90_000));
            Target["bin/helldivers2.exe"] = olderExe;
            Target["data/game/game.dll"] = File.ReadAllBytes(Path.Combine(Source, "data/game/game.dll"));
            Target["data/big.bin"] = older;
            Target["data/moved.bin"] = moved;
            Target["data/settings.ini"] = "[build's own]"u8.ToArray();
            Target["data/added.bin"] = Bytes(8, 120_000);
            Target["data/empty"] = [];
            Target["bin/nvngx_dlss.dll"] = Bytes(9, 80_000);
            foreach (var (path, content) in Target) Put(Game, path, content);
            Build = new SupportedBuild(1, LocalInstall.AppId, "100", Hash256(olderExe), "", [new SupportedDepot(553851, "1")],
                [.. Target.OrderBy(t => t.Key, StringComparer.Ordinal).Select(t => new SupportedFile(t.Key, t.Value.Length, Sha1(t.Value), 553851))]);
            // a player's newer upscaler in the installation: any version counts
            Put(Game, "bin/nvngx_dlss.dll", Bytes(10, 85_000));
            Steam = new DownpatchSource(Source, new BuildPin("200", Hash256(exe), Hash256(File.ReadAllBytes(Path.Combine(Source, "data/game/game.dll")))),
                [new SupportedDepot(553851, "2")]);
        }

        static string Hash256(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes));

        public static void Put(string root, string path, byte[] content)
        {
            var full = Path.Combine(root, path);
            Directory.CreateDirectory(Path.GetDirectoryName(full)!);
            File.WriteAllBytes(full, content);
        }

        public string Pack => Path.Combine(Home, "out", Downpatch.FileName("200", "100"));

        public DownpatchResult Make(CancellationToken cancellation = default) => DownpatchMaker.Make(Game, Steam, Build, Pack, null, cancellation, new(Workers: 2));

        public DownpatchResult Apply(string into, string? pack = null, DownpatchSource? source = null, SupportedBuild? build = null) =>
            Downpatch.Apply(pack ?? Pack, source ?? Steam, into, build ?? Build);
    }

    // The pack rewritten: its manifest changed, an entry's bytes replaced, or extra entries added.
    static string Rewrite(string pack, string name, Func<DownpatchManifest, DownpatchManifest>? manifest = null,
        Func<string, byte[], byte[]>? entry = null, IReadOnlyDictionary<string, byte[]>? extra = null)
    {
        var output = Path.Combine(Path.GetDirectoryName(pack)!, name);
        using var from = ZipFile.OpenRead(pack);
        using var to = ZipFile.Open(output, ZipArchiveMode.Create);
        foreach (var item in from.Entries)
        {
            using var read = new MemoryStream();
            using (var stream = item.Open()) stream.CopyTo(read);
            var bytes = read.ToArray();
            if (item.FullName == Downpatch.ManifestName && manifest != null)
                bytes = JsonSerializer.SerializeToUtf8Bytes(manifest(JsonSerializer.Deserialize(bytes, CoreJsonCamel.Default.DownpatchManifest)!), CoreJsonCamel.Default.DownpatchManifest);
            else if (entry != null) bytes = entry(item.FullName, bytes);
            using var write = to.CreateEntry(item.FullName, CompressionLevel.NoCompression).Open();
            write.Write(bytes);
        }
        foreach (var (extraName, bytes) in extra ?? new Dictionary<string, byte[]>())
        {
            using var write = to.CreateEntry(extraName).Open();
            write.Write(bytes);
        }
        return output;
    }

    static void Refused(Action action, string expected, Action<bool, string> check)
    {
        try { action(); }
        catch (Exception error) when (error is InvalidDataException or IOException)
        {
            check(error.Message.Contains(expected, StringComparison.Ordinal), $"Refused, but for another reason: {error.Message}");
            return;
        }
        throw new Exception($"Not refused (expected \"{expected}\").");
    }

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("The binary delta rebuilds files through edits, moves, empty and tiny files, and refuses a damaged one", () =>
        {
            var folder = Directory.CreateDirectory(Path.Combine(root, "delta")).FullName;
            var a = Bytes(20, 2 << 20);
            var cases = new (byte[]? Source, byte[] Target)[]
            {
                (a, a),
                (a, a.Skip(1000).Concat(a.Take(1000)).ToArray()),
                (a, a.Take(700_000).Concat(Bytes(21, 3)).Concat(a.Skip(700_000)).ToArray()),
                (a, a.Reverse().ToArray()),
                (a, []),
                ([], a.Take(10_000).ToArray()),
                (null, a.Take(5).ToArray()),
                (a.Take(100).ToArray(), a.Take(200).ToArray()),
            };
            for (var i = 0; i < cases.Length; i++)
            {
                var (source, target) = cases[i];
                var sourcePath = source == null ? null : Path.Combine(folder, $"s{i}");
                if (source != null) File.WriteAllBytes(sourcePath!, source);
                File.WriteAllBytes(Path.Combine(folder, $"t{i}"), target);
                using var delta = new MemoryStream();
                BinaryDelta.Create(sourcePath, Path.Combine(folder, $"t{i}"), delta);
                delta.Position = 0;
                using var handle = sourcePath == null ? null : File.OpenHandle(sourcePath);
                using var output = new MemoryStream();
                BinaryDelta.Apply(handle, delta, output);
                check(output.ToArray().AsSpan().SequenceEqual(target), $"Case {i} did not round-trip.");
                if (i == 2) check(delta.Length < 2000, $"A 3-byte insertion took a {delta.Length}-byte delta.");
                if (i == 3) check(delta.Length > 1 << 20, "Unrelated data was not carried.");
            }
            File.WriteAllBytes(Path.Combine(folder, "edited"), cases[2].Target);
            using var good = new MemoryStream();
            BinaryDelta.Create(Path.Combine(folder, "s0"), Path.Combine(folder, "edited"), good);
            var bytes = good.ToArray();
            bytes[bytes.Length / 2] ^= 0xFF;
            using var source0 = File.OpenHandle(Path.Combine(folder, "s0"));
            reject(() => BinaryDelta.Apply(source0, new MemoryStream(bytes), Stream.Null));
            reject(() => BinaryDelta.Apply(source0, new MemoryStream(good.ToArray()[..(int)(good.Length / 2)]), Stream.Null));
        });

        test("A downpatch file makes the build from Steam's newer one, passes the build's verification, and resumes", () =>
        {
            var f = new Fixture(root, "downpatch-roundtrip");
            var made = f.Make();
            check(made is { Same: 2, Patched: 3, New: 3 }, $"Operations: {made.Same} same, {made.Patched} patched, {made.New} new.");
            var manifest = Downpatch.ReadManifest(f.Pack);
            check(manifest.Source.BuildId == "200" && manifest.Target.BuildId == "100" && manifest.Files.Count == f.Build.Files.Count, "The manifest is wrong.");
            check(manifest.Files.Single(e => e.Path == "data/moved.bin") is { Op: "same", From: "data/renamed.bin" }, "A moved file was not found by its hash.");
            check(manifest.Files.Single(e => e.Path == "data/settings.ini").Op == "new", "settings.ini was not carried whole.");
            check(made.PackBytes < 300_000, $"The pack holds {made.PackBytes} bytes: more than what differs.");
            var into = Path.Combine(f.Home, "applied");
            var applied = f.Apply(into);
            check(f.Build.Verify(into).Count == 0, "The applied copy does not verify: " + string.Join(", ", f.Build.Verify(into)));
            check(File.ReadAllBytes(Path.Combine(into, "data/big.bin")).AsSpan().SequenceEqual(f.Target["data/big.bin"]), "big.bin differs.");
            check(applied.Kept == 0 && !Directory.EnumerateFiles(into, "*.partial", SearchOption.AllDirectories).Any(), "Leftovers after the apply.");
            File.Delete(Path.Combine(into, "data/big.bin"));
            check(f.Apply(into).Kept == f.Build.Files.Count - 1 && f.Build.Verify(into).Count == 0, "A second run did not keep the files in place.");
            check(new FileInfo(Path.Combine(f.Source, "data/settings.ini")).Length == "[player's own]".Length, "Steam's folder was changed.");
        });

        test("A downpatch file refuses another source build (naming the one it needs), another target build, and a changed Steam file", () =>
        {
            var f = new Fixture(root, "downpatch-builds");
            f.Make();
            Refused(() => f.Apply(Path.Combine(f.Home, "a"), source: f.Steam with { Pin = new("300", "AA", "BB") }), "made from HELLDIVERS 2 build 200", check);
            Refused(() => f.Apply(Path.Combine(f.Home, "b"), source: f.Steam with { Pin = f.Steam.Pin with { ExeSha256 = "AA" } }), "verify the game in Steam", check);
            Refused(() => f.Apply(Path.Combine(f.Home, "c"), build: f.Build with { BuildId = "101" }), "this HellForge runs build 101", check);
            var steamFile = Path.Combine(f.Source, "data/big.bin");
            var original = File.ReadAllBytes(steamFile);
            var changed = (byte[])original.Clone();
            changed[3_000_000] ^= 1;
            File.WriteAllBytes(steamFile, changed);
            Refused(() => f.Apply(Path.Combine(f.Home, "d")), "Verify the game in Steam", check);
            check(!File.Exists(Path.Combine(f.Home, "d", "data/big.bin")) && !File.Exists(Path.Combine(f.Home, "d", "data/big.bin.partial")), "A bad file was kept.");
            File.WriteAllBytes(steamFile, original);
            Fixture.Put(f.Game, "data/added.bin", Bytes(99, 120_000));
            Refused(() => DownpatchMaker.Make(f.Game, f.Steam, f.Build, Path.Combine(f.Home, "out", "x.zip")), "run Check and repair first", check);
            Refused(() => DownpatchMaker.Make(f.Game, f.Steam with { Pin = f.Steam.Pin with { BuildId = "100" } }, f.Build, Path.Combine(f.Home, "out", "y.zip")), "needs no downpatch file", check);
        });

        test("A damaged downpatch file is refused and leaves nothing it wrote half-done", () =>
        {
            var f = new Fixture(root, "downpatch-damaged");
            f.Make();
            var flipped = Rewrite(f.Pack, "flipped.zip", entry: (name, bytes) =>
            {
                if (name.EndsWith(".hfd", StringComparison.Ordinal) && bytes.Length > 100) bytes[bytes.Length / 2] ^= 0x10;
                return bytes;
            });
            var into = Path.Combine(f.Home, "flipped");
            Refused(() => f.Apply(into, flipped), "damaged", check);
            check(!Directory.EnumerateFiles(into, "*.partial", SearchOption.AllDirectories).Any(), "A .partial file was left.");
            var cut = Path.Combine(f.Home, "out", "cut.zip");
            File.WriteAllBytes(cut, File.ReadAllBytes(f.Pack)[..(int)(new FileInfo(f.Pack).Length * 2 / 3)]);
            Refused(() => f.Apply(Path.Combine(f.Home, "cut"), cut), "damaged", check);
            var notOne = Path.Combine(f.Home, "out", "other.zip");
            using (var zip = ZipFile.Open(notOne, ZipArchiveMode.Create)) zip.CreateEntry("readme.txt");
            Refused(() => f.Apply(Path.Combine(f.Home, "other"), notOne), "not a HellForge downpatch file", check);
            var lying = Rewrite(f.Pack, "lying.zip", manifest: m => m with
            {
                Files = [.. m.Files.Select(e => e.Path == "data/added.bin" ? e with { Sha1 = new string('0', 40) } : e)]
            });
            Refused(() => f.Apply(Path.Combine(f.Home, "lying"), lying), "not build 100's", check);
        });

        test("A downpatch file's paths cannot leave the game folders, and a full drive is refused before anything is written", () =>
        {
            var f = new Fixture(root, "downpatch-paths");
            f.Make();
            var outside = Path.Combine(f.Home, "evil.txt");
            foreach (var (from, path) in new[] { ("../../../evil.txt", "data/moved.bin"), ("data/../../x", "data/moved.bin"), (outside, "data/moved.bin"),
                         ("C:/Windows/win.ini", "data/moved.bin"), ("data/renamed.bin", "../evil.txt") })
            {
                var bad = Rewrite(f.Pack, "bad.zip", manifest: m => m with
                {
                    Files = [.. m.Files.Select(e => e.Path == "data/moved.bin" ? e with { From = from, Path = path } : e)]
                }, extra: new Dictionary<string, byte[]> { ["../../evil.txt"] = "x"u8.ToArray() });
                Refused(() => f.Apply(Path.Combine(f.Home, "paths"), bad), "downpatch file", check);
                File.Delete(bad);
            }
            check(!File.Exists(outside) && !File.Exists(Path.Combine(root, "evil.txt")), "A file was written outside the game folder.");
            var full = Downpatch.SpaceOf;
            try
            {
                Downpatch.SpaceOf = _ => new("TEST:", 1);
                try { f.Apply(Path.Combine(f.Home, "full")); throw new Exception("Applied onto a full drive."); }
                catch (NotEnoughSpaceException) { }
                check(!Directory.Exists(Path.Combine(f.Home, "full", "data")), "Files were written onto a full drive.");
                try { DownpatchMaker.Make(f.Game, f.Steam, f.Build, Path.Combine(f.Home, "full-out", "x.zip")); throw new Exception("Made onto a full drive."); }
                catch (NotEnoughSpaceException) { }
                check(!File.Exists(Path.Combine(f.Home, "full-out", "x.zip.partial")), "A pack was started on a full drive.");
            }
            finally { Downpatch.SpaceOf = full; }
        });

        test("Setup offers a downpatch file last, only with Steam's game installed, and says HellForge does not host them", () =>
        {
            check(DownloadChoices.Offered(reachesSteam: true, downpatchFile: true).Last() == DownloadChoice.DownpatchFile &&
                DownloadChoices.Offered(reachesSteam: false, downpatchFile: true)
                    .SequenceEqual([DownloadChoice.SteamClient, DownloadChoice.DownpatchFile]) &&
                !DownloadChoices.Offered(reachesSteam: true).Contains(DownloadChoice.DownpatchFile),
                "Where the downpatch way is offered.");
            check(DownloadChoices.Default == DownloadChoice.QrCode, "The Steam download is not the default.");
            check(DownloadChoices.Explanation(DownloadChoice.DownpatchFile, "100")
                .Contains("HellForge doesn't host these; you make one and share it yourself."), "The card does not say HellForge hosts none.");
        });

        test("Making a downpatch file can be stopped and leaves no file behind", () =>
        {
            var f = new Fixture(root, "downpatch-stop");
            using var stop = new CancellationTokenSource();
            stop.Cancel();
            try { f.Make(stop.Token); throw new Exception("Not stopped."); }
            catch (OperationCanceledException) { }
            var folder = Path.GetDirectoryName(f.Pack)!;
            check(!Directory.Exists(folder) || !Directory.EnumerateFileSystemEntries(folder).Any(), "A stopped run left files: " + folder);
        });
    }
}
