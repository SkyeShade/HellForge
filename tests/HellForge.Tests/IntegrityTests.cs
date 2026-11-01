using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using HellForge.Core;

// Recovery and state files that keep an interrupted session from turning a finished installation back into setup.
static class IntegrityTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, string root)
    {
        test("SDK stand-in recovery restores only saved originals that match their pins", () =>
        {
            var install = new InstallLayout(Path.Combine(root, "shim-recovery"));
            install.Create("25480438");
            var shims = Directory.CreateDirectory(Path.Combine(install.App, "sdk-shims")).FullName;
            var originals = Path.Combine(install.Temp, "sdk-shim-originals");
            Directory.CreateDirectory(originals);
            var pinFolder = Directory.CreateDirectory(Path.Combine(install.Game, "bin")).FullName;
            var pins = new JsonObject();
            var expected = new Dictionary<string, string>();
            foreach (var name in new[] { "PartyWin.dll", "PlayFabMultiplayerWin.dll", "steam_api64.dll" })
            {
                var original = Encoding.UTF8.GetBytes("original " + name);
                var pin = Convert.ToHexString(SHA256.HashData(original));
                expected[name] = pin;
                pins[name] = pin;
                File.WriteAllBytes(Path.Combine(originals, name), original);
                File.WriteAllText(Path.Combine(pinFolder, name), "stand-in " + name);
            }
            File.WriteAllText(Path.Combine(shims, "build.json"), new JsonObject { ["originals"] = pins }.ToJsonString());
            File.WriteAllText(Path.Combine(pinFolder, SdkShimRecovery.LeftoverCloudOriginal), "left behind");
            var result = SdkShimRecovery.Recover(install);
            check(result.Recovered.Count == 3 && expected.All(pair => result.Originals[pair.Key] == pair.Value),
                "The recovery did not report every pinned original.");
            check(expected.Keys.All(name => File.ReadAllText(Path.Combine(pinFolder, name)) == "original " + name),
                "A saved original was not put back.");
            check(!File.Exists(Path.Combine(pinFolder, SdkShimRecovery.LeftoverCloudOriginal)),
                "The session's steam_api64_hellforge.dll was left in the copy.");
        });

        test("A setup receipt is written whole and read back", () =>
        {
            var install = new InstallLayout(Path.Combine(root, "setup-receipt"));
            install.Create("25480438");
            SetupReceipt.Write(install);
            var receipt = SetupReceipt.Read(install);
            check(receipt is { SchemaVersion: 1 } && receipt.BuildId == SupportedBuild.Current.BuildId &&
                receipt.Files == SupportedBuild.Current.Files.Count && File.Exists(SetupReceipt.PathFor(install)) &&
                !File.Exists(SetupReceipt.PathFor(install) + ".partial"), "The setup receipt was not written whole.");
        });

        test("The setup reason names the first missing build file", () =>
        {
            var game = Directory.CreateDirectory(Path.Combine(root, "setup-reason")).FullName;
            var build = new SupportedBuild(1, "553850", "test", "", "", [],
                [new SupportedFile("bin/first.dll", 3, new string('A', 40), 1),
                 new SupportedFile("bin/second.dll", 3, new string('B', 40), 1)]);
            check(build.FirstCompletenessProblem(game) == "missing: bin/first.dll", "The first missing file was not named.");
            Directory.CreateDirectory(Path.Combine(game, "bin"));
            File.WriteAllText(Path.Combine(game, "bin", "first.dll"), "one");
            check(build.FirstCompletenessProblem(game) == "missing: bin/second.dll", "The next missing file was not named.");
        });

        test("settings.ini's original is copied through a partial and renamed", () =>
        {
            var folder = Directory.CreateDirectory(Path.Combine(root, "settings-original")).FullName;
            var settings = Path.Combine(folder, "settings.ini");
            var original = settings + GameSettings.OriginalSuffix;
            File.WriteAllText(settings, "the build's settings");
            File.WriteAllText(original + ".partial", "a previous partial copy");
            GameSettings.CopyOriginalAtomically(settings, original);
            check(File.ReadAllText(original) == "the build's settings" && !File.Exists(original + ".partial"),
                "The original was not replaced atomically, or its partial was left behind.");
        });

        test("The data audit allows build and staged files and quarantines everything else", () =>
        {
            var install = new InstallLayout(Path.Combine(root, "data-audit"));
            install.Create("25480438");
            var data = Directory.CreateDirectory(Path.Combine(install.Game, "data")).FullName;
            File.WriteAllText(Path.Combine(data, "keep"), "build");
            File.WriteAllText(Path.Combine(data, "settings.ini.hellforge-original"), "original");
            File.WriteAllText(Path.Combine(data, "0123456789abcdef.patch_0"), "staged");
            File.WriteAllText(Path.Combine(data, "extra.patch_0"), "partial");
            File.WriteAllText(Path.Combine(data, "notes.partial"), "partial");
            File.WriteAllText(Path.Combine(install.Temp, "staged-mods.json"), """["0123456789abcdef.patch_0"]""");
            var build = new SupportedBuild(1, "553850", "test", "", "", [],
                [new SupportedFile("data/keep", 5, new string('A', 40), 1)]);
            var result = GameDataAudit.Quarantine(install, build);
            check(result.Extras.Count == 2 && result.Extras.Contains("data/extra.patch_0") && result.Extras.Contains("data/notes.partial"),
                "The audit did not name both extras: " + string.Join(", ", result.Extras));
            check(File.Exists(Path.Combine(data, "keep")) && File.Exists(Path.Combine(data, "settings.ini.hellforge-original")) &&
                File.Exists(Path.Combine(data, "0123456789abcdef.patch_0")), "The audit moved an allowed file.");
            check(!File.Exists(Path.Combine(data, "extra.patch_0")) && !File.Exists(Path.Combine(data, "notes.partial")) &&
                Directory.GetFiles(Path.Combine(install.Temp, "data-extras"), "*", SearchOption.AllDirectories).Length == 2,
                "An extra was not quarantined.");
        });

        test("An unclean previous session and a clean one are told apart", () =>
        {
            var install = new InstallLayout(Path.Combine(root, "session-integrity"));
            install.Create("25480438");
            var run = Directory.CreateDirectory(Path.Combine(install.Sessions, "0123456789abcdef0123456789abcdef")).FullName;
            var result = Path.Combine(run, "result.json");
            File.WriteAllText(result, """{"startedUtc":"2026-10-28T10:00:00Z","status":"patching"}""");
            check(SessionIntegrity.LastEndedUnclean(install), "A run without completedUtc was clean.");
            File.WriteAllText(result, """{"startedUtc":"2026-10-28T10:00:00Z","status":"observed","completedUtc":"2026-10-28T10:01:00Z","jobEmpty":true}""");
            check(!SessionIntegrity.LastEndedUnclean(install), "A completed, empty job was unclean.");
            File.WriteAllText(result, """{"startedUtc":"2026-10-28T10:00:00Z","status":"observed","completedUtc":"2026-10-28T10:01:00Z","jobEmpty":false}""");
            check(SessionIntegrity.LastEndedUnclean(install), "A job that was not empty was clean.");
        });

        test("A full verification hashes everything and a cached one skips it", () =>
        {
            var game = Directory.CreateDirectory(Path.Combine(root, "verify-all")).FullName;
            var bytes = Encoding.UTF8.GetBytes("one file");
            var file = Path.Combine(game, "one");
            File.WriteAllBytes(file, bytes);
            var build = new SupportedBuild(1, "553850", "test", "", "", [],
                [new SupportedFile("one", bytes.Length, Convert.ToHexString(SHA1.HashData(bytes)), 1)]);
            var cache = Path.Combine(root, "verify-all-cache.json");
            var first = build.VerifyDetailed(game, cache, useCache: false);
            var second = build.VerifyDetailed(game, cache);
            var third = build.VerifyDetailed(game, cache, useCache: false);
            check(first.Problems.Count == 0 && first.HashedFiles == 1 && second.HashedFiles == 0 &&
                second.CacheSkippedFiles == 1 && third.HashedFiles == 1, "Verification counts did not follow the cache setting.");
        });

        test("Mod staging journals its target before copying and Unstage removes a leftover .staging file", () =>
        {
            var install = new InstallLayout(Path.Combine(root, "staging-journal"));
            install.Create("25480438");
            var mods = Directory.CreateDirectory(Path.Combine(install.Root, "mods")).FullName;
            var folder = Directory.CreateDirectory(Path.Combine(mods, "journal")).FullName;
            File.WriteAllText(Path.Combine(folder, ModCatalog.ManifestName),
                """{"format":1,"id":"test.journal","name":"Journal","version":"1","gameplay":true}""");
            var source = Path.Combine(folder, "data", "9ba626afa44a3aa3.patch_0");
            Directory.CreateDirectory(Path.GetDirectoryName(source)!);
            File.WriteAllText(source, "patch");
            var mod = ModCatalog.Load(mods).Mods.Single();
            File.Delete(source);
            try { ModStaging.Stage(install, [mod]); check(false, "Staging a missing source succeeded."); }
            catch (IOException) { }
            var data = Path.Combine(install.Game, "data");
            var target = Path.Combine(data, "9ba626afa44a3aa3.patch_0");
            var staging = target + ".staging";
            File.WriteAllText(staging, "partial");
            check(File.Exists(Path.Combine(install.Temp, "staged-mods.json")), "The journal was not written before the copy.");
            ModStaging.Unstage(install);
            check(!File.Exists(target) && !File.Exists(staging) && !File.Exists(Path.Combine(install.Temp, "staged-mods.json")),
                "Unstage did not remove the journaled staging file.");
        });
    }
}
