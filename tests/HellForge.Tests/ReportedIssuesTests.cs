using System.Text.Json.Nodes;
using System.Security.Cryptography;
using System.Text;
using HellForge.Core;

// Reported problems (SkyeShade/HellForge issues), each against the behaviour that fixed it.
static class ReportedIssuesTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("A sync that read nothing says where the game's login stopped, from proxy.log (#346, bug-report-0495, bug-report-0510, bug-report-0551)", () =>
        {
            static string Log(params string[] lines) => string.Join("\n", lines) + "\n";
            string[] start = ["12:43:01.000 hosts line added", "12:43:01.002 system DNS: api.live.prod.thehelldiversgame.com -> 127.0.0.1",
                "12:43:01.003 listening on port 443"];
            // bug-report-0510: the game ran ten minutes and never connected (the resolver went past the hosts line).
            var silent = Log([.. start, "12:43:30.000 status: 0 connections, 0 requests, last connection 30 s ago, game running, captured no",
                "12:53:01.000 nothing captured", "12:53:01.001 summary: 0 connections, 0 requests answered by the API"]);
            check(CaptureStall.Read(silent, noConnectionYet: false, linux: false) == null, "A game that has not had its time yet is called silent.");
            check(CaptureStall.Read(silent, noConnectionYet: true, linux: false) is { Kind: CaptureStall.Kind.NeverConnected } never &&
                never.Message.Contains("VPN", StringComparison.Ordinal) && !never.Message.Contains("title screen", StringComparison.Ordinal),
                "A game that never connected is not told as one.");

            // #346 and bug-report-0495: a Windows system proxy took the capture's upstream (fixed on its own); the message says
            // the servers could not be reached, with the reason.
            string[] reached = ["12:43:20.100 #1 connection from 127.0.0.1:64215",
                "12:43:20.232 #1 TLS Tls13 TLS_AES_256_GCM_SHA384 http/1.1, sni api.live.prod.thehelldiversgame.com in 132 ms",
                "12:43:20.240 #1 GET /api/Configuration/... api.live.prod.thehelldiversgame.com, body 0 bytes (application/json); headers: Accept; query: none",
                "12:43:20.242 upstream connected to 68.154.15.59:443 in 2 ms"];
            var proxied = Log([.. start, .. reached,
                "12:43:21.124 #1 failed while asking the API after 884 ms: HttpRequestException: The proxy tunnel request to proxy 'http://127.0.0.1:7897/' failed with status code '400'."]);
            check(CaptureStall.Read(proxied, noConnectionYet: false, linux: false) is { Kind: CaptureStall.Kind.ServersUnreachable } unreachable &&
                unreachable.Message.Contains("proxy tunnel request", StringComparison.Ordinal) && unreachable.Message.Contains("firewall", StringComparison.Ordinal),
                "A capture that could not reach the servers is not told with its reason.");

            // An antivirus's HTTPS scanning: the signing capture refuses a certificate without the pinned roots.
            var scanned = Log([.. start, .. reached, "12:43:20.300 upstream refused: the API's certificate chain holds none of HellForge's pinned keys",
                "12:43:20.301 #1 failed while asking the API after 61 ms: HttpRequestException: The SSL connection could not be established, see inner exception."]);
            check(CaptureStall.Read(scanned, noConnectionYet: true, linux: false) is { Kind: CaptureStall.Kind.Intercepted } intercepted &&
                intercepted.Message.Contains("HTTPS", StringComparison.Ordinal) && intercepted.Message.Contains("pinned keys", StringComparison.Ordinal),
                "An intercepted upstream is not told as one.");

            // The game refused the capture's certificate; the Linux launcher's port check (bug-report-0551) is not the game.
            string[] portCheck = ["12:43:01.700 #1 connection from 127.0.0.1:51804", "12:43:01.701 #1 closed before sending anything (a port check, not the game)"];
            var refused = Log([.. start, .. portCheck, "12:43:20.100 #2 connection from 127.0.0.1:51900",
                "12:43:20.140 #2 TLS handshake failed after 40 ms: 0x80090326 AuthenticationException: Authentication failed"]);
            check(CaptureStall.Read(refused, noConnectionYet: true, linux: false) is { Kind: CaptureStall.Kind.CertificateRefused } windows &&
                windows.Message.Contains("HellForge.CaptureProxy.exe", StringComparison.Ordinal) &&
                CaptureStall.Read(refused, noConnectionYet: true, linux: true) is { Kind: CaptureStall.Kind.CertificateRefused } linux &&
                linux.Message.Contains("Proton prefix", StringComparison.Ordinal), "A refused certificate is not told for the platform.");
            check(CaptureStall.Read(Log([.. start, .. portCheck]), noConnectionYet: true, linux: true) is { Kind: CaptureStall.Kind.NeverConnected },
                "The launcher's port check counts as the game.");

            // The servers refused the login: not the player's title screen either.
            string[] tls = ["12:43:20.100 #1 connection from 127.0.0.1:55439",
                "12:43:20.108 #1 TLS Tls12 TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384 http/1.1, sni api.live.prod.thehelldiversgame.com in 8 ms"];
            check(CaptureStall.Read(Log([.. start, .. tls, "12:43:20.400 POST /api/Account/Login 503 120 bytes"]), noConnectionYet: true, linux: false)
                is { Kind: CaptureStall.Kind.LoginRefused } down && down.Message.Contains("HTTP 503", StringComparison.Ordinal), "A refused login is not told as one.");

            // #89's log: the login was answered, so what is left is the title screen (no stall); Windows line ends; no log at all.
            var answered = Log([.. start, .. tls, "12:11:29.388 POST /api/Account/Login 200 52 bytes",
                "12:11:29.401 login answered: the game's own reads that follow are kept"]);
            check(CaptureStall.Read(answered, true, false) == null && CaptureStall.Read(answered.Replace("\n", "\r\n", StringComparison.Ordinal), true, false) == null &&
                CaptureStall.Read(null, true, false) == null && CaptureStall.Read("", true, false) == null, "A login the servers answered is called a stall.");
            check(CaptureStall.Read(Log([.. start, .. tls]), true, false) == null, "A game that reached the capture and waits at the title screen is called a stall.");
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
    }
}
