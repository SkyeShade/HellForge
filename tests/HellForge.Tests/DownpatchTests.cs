using System.IO.Compression;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using HellForge.Core;

// Build patches listed online (downpatch/index.json) and what setup accepts from one (Downpatcher).
static class DownpatchTests
{
    sealed class Respond(Func<HttpRequestMessage, HttpResponseMessage> answer) : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken) =>
            Task.FromResult(answer(request));
    }

    static string Hex(int n) => new string((char)('A' + n % 6), 64);

    static string Entry(string source, string url, string sha256, long size = 1000) =>
        $$"""{"sourceBuild": "{{source}}", "targetBuild": "{{SupportedBuild.Current.BuildId}}", "url": "{{url}}", "sha256": "{{sha256}}", "size": {{size}}, "sourceExeSha256": "{{Hex(1)}}", "sourceGameDllSha256": "{{Hex(2)}}"}""";

    static string Sha1(byte[] bytes) => Convert.ToHexString(SHA1.HashData(bytes));

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        const string Good = "https://files.hf-mod.com/downpatch/downpatch-26088801-to-25480438.zip";

        test("The patch list keeps only well-formed packs on files.hf-mod.com/downpatch/", () =>
        {
            var packs = Downpatcher.ParseIndex("""{"schemaVersion": 1, "packs": [""" + string.Join(",",
                Entry("26088801", Good, Hex(0).ToLowerInvariant()),
                Entry("1", "http://files.hf-mod.com/downpatch/a.zip", Hex(0)),
                Entry("2", "https://example.com/downpatch/a.zip", Hex(0)),
                Entry("3", "https://files.hf-mod.com/downpatch/../a.zip", Hex(0)),
                Entry("4", "https://files.hf-mod.com/downpatch/sub/a.zip", Hex(0)),
                Entry("5", Good, "1234"),
                Entry("6x", Good, Hex(0)),
                Entry("7", Good, Hex(0), size: 0)) + "]}");
            check(packs.Count == 1 && packs[0].SourceBuild == "26088801" && packs[0].Sha256 == Hex(0) && packs[0].Url == Good,
                "Packs kept: " + string.Join(", ", packs.Select(p => p.SourceBuild)));
            reject(() => Downpatcher.ParseIndex("""{"schemaVersion": 2, "packs": []}"""));
        });

        test("Setup reads the published patch list, else the copy saved last time, else the built-in packs", () =>
        {
            var cache = Path.Combine(root, "downpatch-list", "downpatch-index.json");
            var published = """{"schemaVersion": 1, "packs": [""" + Entry("26088801", Good, Hex(0)) + "]}";
            using (var online = new HttpClient(new Respond(_ => new HttpResponseMessage(HttpStatusCode.OK) { Content = new StringContent(published) })))
                check(Downpatcher.RefreshAsync(online, cache).GetAwaiter().GetResult() == Downpatcher.ListSource.Online, "Not read online.");
            check(Downpatcher.FindPack("26088801") != null && Downpatcher.FindPack(Downpatcher.BuiltIn[0].SourceBuild) == Downpatcher.BuiltIn[0],
                "A published or built-in pack is missing.");
            using (var offline = new HttpClient(new Respond(_ => throw new HttpRequestException("offline"))))
            {
                check(Downpatcher.RefreshAsync(offline, cache).GetAwaiter().GetResult() == Downpatcher.ListSource.Cached && Downpatcher.FindPack("26088801") != null,
                    "The saved list was not used offline.");
                File.Delete(cache);
                check(Downpatcher.RefreshAsync(offline, cache).GetAwaiter().GetResult() == Downpatcher.ListSource.BuiltIn && Downpatcher.FindPack("26088801") == null &&
                    Downpatcher.Packs.Count == Downpatcher.BuiltIn.Length, "Not the built-in packs alone.");
            }
            using (var broken = new HttpClient(new Respond(_ => new HttpResponseMessage(HttpStatusCode.OK) { Content = new StringContent("<html>") })))
                check(Downpatcher.RefreshAsync(broken, cache).GetAwaiter().GetResult() == Downpatcher.ListSource.BuiltIn && !File.Exists(cache),
                    "A broken list was used or saved.");
        });

        test("A build patch writes only the build's files, each with the build's hash, or nothing past the bad one", () =>
        {
            var folder = Path.Combine(root, "downpatch-apply");
            var steam = Path.Combine(folder, "steam");
            var game = Path.Combine(folder, "game");
            Directory.CreateDirectory(Path.Combine(steam, "bin"));
            var same = Encoding.UTF8.GetBytes("unchanged between builds");
            var added = Encoding.UTF8.GetBytes("only in the supported build");
            File.WriteAllBytes(Path.Combine(steam, "bin", "same.dll"), same);
            var build = new SupportedBuild(1, "553850", "25480438", "", "", [],
                [new SupportedFile("bin/same.dll", same.Length, Sha1(same), 1), new SupportedFile("data/added.bin", added.Length, Sha1(added), 1)]);
            var pack = new DownpatchPack("26088801", "25480438", Good, Hex(0), 1);

            string Pack(string name, string manifestFiles, byte[] addBytes)
            {
                var path = Path.Combine(folder, name + ".zip");
                Directory.CreateDirectory(folder);
                if (File.Exists(path)) File.Delete(path);
                using var zip = ZipFile.Open(path, ZipArchiveMode.Create);
                using (var writer = new StreamWriter(zip.CreateEntry("downpatch.json").Open()))
                    writer.Write("""{"sourceBuild": "26088801", "targetBuild": "25480438", "files": [""" + manifestFiles + "]}");
                using (var stream = zip.CreateEntry("add/data/added.bin").Open()) stream.Write(addBytes);
                return path;
            }
            const string Copy = """{"path": "bin/same.dll", "op": "copy"}""";
            const string Add = """{"path": "data/added.bin", "op": "add"}""";
            void Apply(string zip) => Downpatcher.ApplyPack(new(steam, game, zip, pack, build, "", null), null, default);

            // outside the list, ../ or a file left out: refused before anything is written
            foreach (var (name, files) in new[] { ("escape", Copy + "," + Add + """,{"path": "../evil.dll", "op": "add"}"""),
                         ("extra", Copy + "," + Add + """,{"path": "bin/other.dll", "op": "add"}"""), ("short", Add), ("twice", Copy + "," + Copy + "," + Add) })
            {
                reject(() => Apply(Pack(name, files, added)));
                check(!Directory.Exists(game) || Directory.GetFiles(game, "*", SearchOption.AllDirectories).Length == 0, $"The {name} pack wrote files.");
            }
            check(!File.Exists(Path.Combine(folder, "evil.dll")), "A file was written outside the game folder.");

            // a file that does not come out with the build's hash is not kept
            reject(() => Apply(Pack("wrong", Copy + "," + Add, Encoding.UTF8.GetBytes("something else entirely!!!"))));
            check(!File.Exists(Path.Combine(game, "data", "added.bin")) && !File.Exists(Path.Combine(game, "data", "added.bin.partial")), "A wrong file was kept.");

            // a pack for another build is refused
            reject(() => Downpatcher.ApplyPack(new(steam, game, Pack("other", Copy + "," + Add, added), pack with { SourceBuild = "1" }, build, "", null), null, default));

            Apply(Pack("good", Copy + "," + Add, added));
            check(File.ReadAllBytes(Path.Combine(game, "bin", "same.dll")).SequenceEqual(same) &&
                File.ReadAllBytes(Path.Combine(game, "data", "added.bin")).SequenceEqual(added), "The good pack did not write the build's files.");

            // Steam's copy changed since the pack was made: a copy no longer matches, and setup says to validate
            File.Delete(Path.Combine(game, "bin", "same.dll"));
            File.WriteAllBytes(Path.Combine(steam, "bin", "same.dll"), Encoding.UTF8.GetBytes("a newer build's bytes...."));
            try { Apply(Pack("good", Copy + "," + Add, added)); check(false, "A changed Steam file was accepted."); }
            catch (InvalidDataException error) { check(error.Message.Contains("Validate the game in Steam"), error.Message); }
        });
    }
}
