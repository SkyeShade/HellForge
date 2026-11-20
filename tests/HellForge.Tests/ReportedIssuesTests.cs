using System.Net;
using System.Text.Json.Nodes;
using System.Security.Cryptography;
using System.Text;
using HellForge.Core;

// Reported problems (SkyeShade/HellForge issues), each against the behaviour that fixed it.
static class ReportedIssuesTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("A capture whose hosts line Windows does not follow stops before the game opens, and says what to turn off (bug-report-0510)", () =>
        {
            // bug-report-0510: "hosts line added", then "system DNS: <api> -> 68.154.15.59", the game ran for ten minutes and
            // the capture saw 0 connections; the player was told to pass the title screen.
            IPAddress[] Parse(params string[] addresses) => [.. addresses.Select(IPAddress.Parse)];
            check(CaptureHosts.Redirects(Parse("127.0.0.1")) && CaptureHosts.Redirects(Parse("127.0.0.1", "::1")) &&
                CaptureHosts.Redirects(Parse("::ffff:1.2.3.4", "127.0.0.1")), "The capture's line, as the resolver answers it, was not taken.");
            check(!CaptureHosts.Redirects(Parse("68.154.15.59")) && !CaptureHosts.Redirects(Parse("127.0.0.1", "68.154.15.59")) &&
                !CaptureHosts.Redirects([]) && !CaptureHosts.Redirects(Parse("::1")), "A resolver that still answers the real address (or nothing) was taken.");

            var system32 = @"C:\Windows\system32";
            check(CaptureHosts.MovedHostsFolder(null, system32) == null && CaptureHosts.MovedHostsFolder(" ", system32) == null &&
                CaptureHosts.MovedHostsFolder(@"C:\WINDOWS\System32\drivers\etc\", system32) == null, "The usual hosts folder reads as moved.");
            check(CaptureHosts.MovedHostsFolder(@"D:\tools\hosts-switcher", system32) == @"D:\tools\hosts-switcher", "A moved hosts folder is not named.");

            var code = CaptureStart.ExitCode(CaptureProblem.HostsIgnored);
            check(CaptureStart.ExitCode(CaptureProblem.Signer) == 28 && code == 29 && CaptureStart.FromExitCode(code) == CaptureProblem.HostsIgnored,
                "The earlier problems' exit codes moved, or the new one does not read back.");
            var message = CaptureStart.Message(new CaptureFailure(CaptureProblem.HostsIgnored, CaptureUpstreamPins.ApiHost + " still leads to 68.154.15.59"),
                ["Kaspersky Premium"], @"C:\HellForge\app\capture\HellForge.CaptureProxy.exe", @"C:\HellForge\logs\sync-1\proxy.log");
            check(message.Contains("68.154.15.59", StringComparison.Ordinal) && message.Contains("VPN", StringComparison.Ordinal) &&
                message.Contains("turn it off while you sync", StringComparison.Ordinal) && message.Contains(@"sync-1\proxy.log", StringComparison.Ordinal) &&
                !message.Contains("Kaspersky", StringComparison.Ordinal) && !CaptureStart.SecurityProgramLikely(CaptureProblem.HostsIgnored),
                "The message does not say what is in the way and what to do: " + message);
        });
        test("The Peacemaker's ammunition rows name the Knight and Redeemer only for when they equip it (bug-report-0035)", () =>
        {
            var objects = EditorCatalog.Build()["objects"]!.AsArray().Select(o => o!).ToList();
            IEnumerable<JsonNode> RowsOf(JsonNode node) => node switch
            {
                JsonObject o when o["key"] != null && o["cat"] != null => [o],
                JsonObject o => o.SelectMany(p => p.Value is null ? [] : RowsOf(p.Value)),
                JsonArray a => a.SelectMany(n => n is null ? [] : RowsOf(n)),
                _ => [],
            };
            JsonNode Row(string weapon, string key) => RowsOf(objects.Single(o => o["name"]!.GetValue<string>() == weapon)["page"]!)
                .First(r => r["key"]!.GetValue<string>() == key);
            var damage = Row("P-2 Peacemaker", "damage.standard_damage");
            var also = damage["also"]?.AsArray().Select(a => a!.GetValue<string>()).ToList() ?? [];
            var alsoIf = damage["alsoIf"]?.AsArray().Select(a => a!.GetValue<string>()).ToList() ?? [];
            check(!also.Any(a => a.Contains("Knight") || a.Contains("Redeemer")) && alsoIf.Any(a => a.Contains("Knight")) && alsoIf.Any(a => a.Contains("Redeemer")) &&
                damage["alsoAmmo"]!.GetValue<string>() == "PISTOL-SMG 9x20mm. HOLLOW POINT",
                $"Peacemaker standard damage: also [{string.Join(", ", also)}], when equipped [{string.Join(", ", alsoIf)}]");
            check(damage["shared"]!.GetValue<bool>(), "The row is still marked shared.");
            check(Row("AR-23 Liberator", "damage.standard_damage")["alsoIf"] == null, "An ammunition no other weapon lists stays unconditional.");
        });
        test("A drive without room for the game asks for space before the Steam download writes anything, not for another download", () =>
        {
            var folder = Path.Combine(root, "space");
            var huge = new HellForge.SteamDownload.PlannedFile(new SupportedFile("data/bundles.00.nxa", long.MaxValue / 4, new string('0', 40), 553853), []);
            try
            {
                new HellForge.SteamDownload.DepotAssembler(null!).AssembleAsync(new([huge]), folder, null, default).GetAwaiter().GetResult();
                check(false, "A download larger than the drive started.");
            }
            catch (NotEnoughSpaceException error)
            {
                check(HellForge.SteamDownload.DownloadFailures.Describe(error, "1")?.Fallback == HellForge.SteamDownload.DownloadFallback.FreeSpace, error.Message);
            }
            check(!File.Exists(Path.Combine(folder, "data", "bundles.00.nxa.partial")), "The download wrote before asking for space.");
            // the check before writing: a drive's free space, with folders on one drive added up
            DiskSpace.Require((root, 1), (root, 1));
            try { DiskSpace.Require((root, long.MaxValue / 4), (root, 1)); check(false, "An impossible amount of space was granted."); }
            catch (NotEnoughSpaceException error)
            {
                check(error.Message.Contains("more is needed", StringComparison.Ordinal) &&
                    error.Message.Contains(DiskSpace.ForFolder(root).Name, StringComparison.OrdinalIgnoreCase), error.Message);
            }
        });

        test("A spreadsheet open in Excel beside a mod leaves the modlist code and the source hash readable (issue 18)", () =>
        {
            var mods = Path.Combine(root, "issue18", "mods");
            var folder = Directory.CreateDirectory(Path.Combine(mods, "a.sheet")).FullName;
            File.WriteAllText(Path.Combine(folder, ModCatalog.ManifestName), """{"format":1,"id":"a.sheet","name":"Sheet","version":"1","gameplay":true}""");
            Directory.CreateDirectory(Path.Combine(folder, "data"));
            File.WriteAllText(Path.Combine(folder, "data", "9ba626afa44a3aa3.patch_0"), "patch");
            Directory.CreateDirectory(Path.Combine(folder, "src"));
            File.WriteAllText(Path.Combine(folder, "src", "mod.c"), "int x;");
            File.WriteAllText(Path.Combine(folder, "dps.xlsx"), "sheet");
            File.WriteAllText(Path.Combine(folder, "src", "notes.xlsx"), "sheet");
            string Digest() => ModCatalog.Digest(ModCatalog.Load(mods).Mods);
            var before = Digest();
            var (loaded, problems) = ModCatalog.Load(mods);
            check(before != "none" && loaded.Count == 1 && problems.Count == 0, "The mod did not load: " + string.Join(" | ", problems));
            var sources = ModBuilds.SourceHash(folder);
            // as Excel holds them: the document read/write, sharing reads; its owner file (~$name, hidden) with no sharing
            using (new FileStream(Path.Combine(folder, "dps.xlsx"), FileMode.Open, FileAccess.ReadWrite, FileShare.Read))
            using (new FileStream(Path.Combine(folder, "src", "notes.xlsx"), FileMode.Open, FileAccess.ReadWrite, FileShare.Read))
            using (new FileStream(Path.Combine(folder, "~$dps.xlsx"), FileMode.Create, FileAccess.ReadWrite, FileShare.None))
            using (new FileStream(Path.Combine(folder, "src", "~$notes.xlsx"), FileMode.Create, FileAccess.ReadWrite, FileShare.None))
            using (new FileStream(Path.Combine(folder, "A1B2C3.tmp"), FileMode.Create, FileAccess.ReadWrite, FileShare.None))
            {
                check(Digest() == before, "The modlist code changed (or failed) while the files were open.");
                check(ModBuilds.SourceHash(folder) == sources, "The source hash changed (or failed) while the files were open.");
            }
            File.Delete(Path.Combine(folder, "~$dps.xlsx"));
            File.Delete(Path.Combine(folder, "src", "~$notes.xlsx"));
            File.Delete(Path.Combine(folder, "A1B2C3.tmp"));
            File.WriteAllText(Path.Combine(folder, "dps.xlsx"), "changed sheet");
            check(Digest() != before, "The spreadsheet itself is still part of the mod.");
        });

        test("In the Linux network namespace, missing mod tools are not fetched: the problem says how to get them (issue 20)", () =>
        {
            var repository = ModSdk.Find(Directory.GetCurrentDirectory())!;
            var sdk = repository with { Tools = Directory.CreateDirectory(Path.Combine(root, "issue20", "tools")).FullName };
            var install = new InstallLayout(Directory.CreateDirectory(Path.Combine(root, "issue20", "install")).FullName);
            var folder = ModKit.Create(sdk, Directory.CreateDirectory(Path.Combine(install.Root, "mods")).FullName, "tester.linux-source", "Linux Source");
            Environment.SetEnvironmentVariable(LinuxNetworkBlock.EnvVar, "1");
            try
            {
                var (ready, problems) = ModBuilds.Prepare(install, sdk, [ModCatalog.Parse(folder, EntityProfile.Current)], "25480438");
                check(ready.Count == 0 && problems is [var problem] && problem.Contains("Mods page") && problem.Contains("Install mod tools") &&
                    !problem.Contains("ziglang.org"), "Not the namespace's refusal, but how to get the tools: " + string.Join(" | ", problems));
                check(Directory.GetFileSystemEntries(sdk.Tools).Length == 0, "A download was started.");
            }
            finally { Environment.SetEnvironmentVariable(LinuxNetworkBlock.EnvVar, null); }
        });

        test("A swapped upscaler (DLSS, XeSS, FSR) still verifies; any other changed file does not (issue 13)", () =>
        {
            var game = Path.Combine(root, "issue13", "game");
            Directory.CreateDirectory(Path.Combine(game, "bin"));
            static string Sha1(byte[] bytes) => Convert.ToHexString(SHA1.HashData(bytes));
            var dlss = Encoding.UTF8.GetBytes("dlss 3.7");
            var other = Encoding.UTF8.GetBytes("game library");
            var build = new SupportedBuild(1, "553850", "25480438", "", "", [],
                [new SupportedFile("bin/nvngx_dlss.dll", dlss.Length, Sha1(dlss), 1), new SupportedFile("bin/other.dll", other.Length, Sha1(other), 1)]);
            File.WriteAllBytes(Path.Combine(game, "bin", "nvngx_dlss.dll"), dlss);
            File.WriteAllBytes(Path.Combine(game, "bin", "other.dll"), other);
            check(build.Verify(game).Count == 0 && build.IsComplete(game), "The build's own files did not verify.");
            File.WriteAllText(Path.Combine(game, "bin", "nvngx_dlss.dll"), "dlss 4.0, a newer and longer library");
            check(build.Verify(game).Count == 0 && build.IsComplete(game), "A newer DLSS was refused.");
            check(build.Files.All(f => SupportedBuild.Accepts(Path.Combine(game, f.Path), f)), "A repair would put the old DLSS back.");
            File.WriteAllText(Path.Combine(game, "bin", "other.dll"), "game librarY");
            check(build.Verify(game) is [var changed] && changed == "changed: bin/other.dll", "A changed game file verified.");
            check(!SupportedBuild.Accepts(Path.Combine(game, "bin", "other.dll"), build.Files[1]), "A repair would keep a changed game file.");
            File.Delete(Path.Combine(game, "bin", "nvngx_dlss.dll"));
            check(build.Verify(game).Contains("missing: bin/nvngx_dlss.dll") && !build.IsComplete(game), "A missing upscaler was not noticed.");
            check(new[] { "bin/nvngx_dlss.dll", "bin/libxess.dll", "bin/amd_fidelityfx_upscaler_dx12.dll" }.All(p =>
                SupportedBuild.IsReplaceable(p) && SupportedBuild.Current.Files.Any(f => f.Path == p)), "The upscalers are not the build's.");
        });

        test("A launcher started from a Steam shortcut does not pass the shortcut's app id on (#434)", () =>
        {
            // what Steam sets for a non-Steam shortcut: its own app id and game id
            var shortcut = new Dictionary<string, string?> { ["SteamAppId"] = "3221225472", ["SteamGameId"] = "13835058055282163712", ["SteamOverlayGameId"] = "13835058055282163712" };
            check(SteamShortcutEnvironment.Foreign(shortcut.GetValueOrDefault).SequenceEqual(SteamShortcutEnvironment.Names), "A shortcut's ids were kept.");
            var game = new Dictionary<string, string?> { ["SteamAppId"] = "553850", ["SteamGameId"] = "553850" };
            check(SteamShortcutEnvironment.Foreign(game.GetValueOrDefault).Count == 0, "HELLDIVERS 2's own ids were removed.");
            check(SteamShortcutEnvironment.Foreign(new Dictionary<string, string?> { ["SteamAppId"] = "0" }.GetValueOrDefault).SequenceEqual(["SteamAppId"]),
                "An app id of 0 was kept.");
            if (!OperatingSystem.IsWindows())
            {
                check(SteamShortcutEnvironment.Forget().Count == 0, "Linux lost Steam's variables.");
                return;
            }
            var saved = SteamShortcutEnvironment.Names.ToDictionary(name => name, Environment.GetEnvironmentVariable);
            try
            {
                foreach (var (name, value) in shortcut) Environment.SetEnvironmentVariable(name, value);
                check(SteamShortcutEnvironment.Forget().Count == 3, "Not every shortcut id was removed.");
                var child = new System.Diagnostics.ProcessStartInfo("cmd.exe").Environment;
                check(SteamShortcutEnvironment.Names.All(name => !child.ContainsKey(name)), "A program started afterwards still gets the shortcut's ids.");
                Environment.SetEnvironmentVariable("SteamAppId", "553850");
                check(SteamShortcutEnvironment.Forget().Count == 0 && Environment.GetEnvironmentVariable("SteamAppId") == "553850", "The game's own id was removed.");
            }
            finally { foreach (var (name, value) in saved) Environment.SetEnvironmentVariable(name, value); }
        });

        test("Listing zipped mods while another program holds the hash cache leaves no partial files (#331)", () =>
        {
            var mods = Path.Combine(root, "mods-held-cache");
            Directory.CreateDirectory(mods);
            using (var zip = System.IO.Compression.ZipFile.Open(Path.Combine(mods, "held.zip"), System.IO.Compression.ZipArchiveMode.Create))
            {
                using (var writer = new StreamWriter(zip.CreateEntry(ModCatalog.ManifestName).Open()))
                    writer.Write("""{"format":1,"id":"test.held","name":"Held","version":"1"}""");
                using (var writer = new StreamWriter(zip.CreateEntry("data/0123456789abcdef.patch_0").Open())) writer.Write("patch bytes");
            }
            var cache = Path.Combine(mods, ModArchives.UnpackedFolder);
            // what an earlier version left: one old partial (removed) and one being written now (kept)
            Directory.CreateDirectory(cache);
            var old = Path.Combine(cache, ".zip-hashes.json.0badf00d.partial");
            var young = Path.Combine(cache, ".zip-files.json.0123abcd.partial");
            File.WriteAllText(old, "{}");
            File.SetLastWriteTimeUtc(old, DateTime.UtcNow.AddHours(-1));
            File.WriteAllText(young, "{}");
            var (loaded, problems) = ModCatalog.Load(mods);
            check(loaded.Any(m => m.Id == "test.held"), "The zipped mod did not load: " + string.Join(" | ", problems));
            check(!File.Exists(old) && File.Exists(young), "Left partials: the old one was kept or the young one removed.");
            File.Delete(young);
            var hashes = Path.Combine(cache, ".zip-hashes.json");
            var before = File.ReadAllText(hashes);
            // another program reading the cache without delete sharing: Windows refuses to replace it
            using (new FileStream(hashes, FileMode.Open, FileAccess.Read, FileShare.Read))
                for (var i = 0; i < 3; i++)
                    check(ModCatalog.Load(mods).Mods.Any(m => m.Id == "test.held"), "Listing failed while the cache was held.");
            var left = Directory.GetFiles(cache, "*.partial");
            check(left.Length == 0, "Partial files left: " + string.Join(", ", left.Select(Path.GetFileName)));
            check(File.ReadAllText(hashes) == before, "The held cache changed.");
        });
    }
}
