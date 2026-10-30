using System.IO.Compression;
using System.Text.Json;
using HellForge.Core;

// A session's opening facts (SessionFacts), the crash report's further scrub (ReportPrivacy) and the report itself
// (CrashReport): its summary, and the zip's contents with and without the crash dump.
static class CrashReportTests
{
    const string Friend = "Ragnar Swiftboots", Host = "LadyLiberty", Counted = "Quiet Comet";
    // NewId: an account id past 2,039,734,272, whose SteamID64 starts 765612
    const ulong FriendId = 76561198012345678, OwnId = 76561197960287930, NewId = 76561202012345678;

    static readonly Privacy Pc = new(@"C:\Users\Alex", ["DESKTOP-K2Q7R4T"], "/home/alex");

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("A crash report's scrub leaves out Steam ids in both forms, IP addresses but loopback, and named players", () =>
        {
            var scrub = new ReportPrivacy(Pc, [Friend, null, "ab"]);
            var hex = FriendId.ToString("X");
            var text = scrub.Apply($"peer {FriendId} joined from 203.0.113.9:27015 as {Friend}; peer 0x{hex} again, {OwnId} hosts on 127.0.0.1:62000, {NewId} joins " +
                @"and [2001:db8::5]:1; at C:\Users\Alex\x on DESKTOP-K2Q7R4T");
            check(!text.Contains(FriendId.ToString()) && !text.Contains(hex) && !text.Contains(OwnId.ToString()), "A Steam id stayed: " + text);
            check(text.Contains("peer steam-id-1 joined") && text.Contains("peer steam-id-1 again"),
                "The two forms of one id are not the same placeholder: " + text);
            check(text.Contains("steam-id-2 hosts") && text.Contains("steam-id-3 joins") && !text.Contains(NewId.ToString()),
                "Another account did not get its own placeholder: " + text);
            check(!text.Contains("203.0.113.9") && text.Contains("ip-1:27015") && text.Contains("127.0.0.1:62000") && !text.Contains("2001:db8"), "IP addresses: " + text);
            check(!text.Contains(Friend) && text.Contains("as player-1"), "A named player stayed: " + text);
            check(text.Contains(@"%USERPROFILE%\x on PC"), "Privacy's own scrub was not applied: " + text);
        });

        test("A crash report's scrub keeps what only looks like an id or an address", () =>
        {
            var scrub = new ReportPrivacy(Pc, []);
            foreach (var kept in new[] { "12:34:56.789 game.dll+0xF8F670", "Windows 11 24H2 (10.0.26100.4652)", "engine 0.1.40.0, driver 32.0.15.8129",
                "sha 0a1100001deadbeef12", "std::string at 0.0.0.0", "digest 76561198012345678901", "digest 765612020123456789", "lobby 109775241047563412" })
                check(scrub.Apply(kept) == kept, "Changed: " + kept + " -> " + scrub.Apply(kept));
            check(scrub.ApplyNames("Mod: a.b 1.0.0.0") == "Mod: a.b 1.0.0.0", "A four-part version was taken for an address.");
        });

        test("Session facts: their opening lines, scrubbed, and back from result.json", () =>
        {
            var facts = new SessionFacts("0.3.0-beta (abc1234)", "0.1.40.0", 15, "25480438", "Windows 11 24H2 (10.0.26100.4652)", "9.0",
                [new GpuInfo("NVIDIA GeForce RTX 4070", "32.0.15.8129 (NVIDIA 581.29)")],
                new SessionSettings(true, "live", true, false, false, true, ["-log", Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "x.txt")]));
            var lines = facts.Lines().ToList();
            check(lines[0] == "HellForge 0.3.0-beta (abc1234), engine 0.1.40.0, mod SDK 15, game build 25480438", "Versions: " + lines[0]);
            check(lines[1] == "System: Windows 11 24H2 (10.0.26100.4652), under Wine 9.0" && lines[2] == "GPU: NVIDIA GeForce RTX 4070, driver 32.0.15.8129 (NVIDIA 581.29)",
                "System: " + string.Join(" | ", lines));
            check(lines[3].StartsWith("Settings: sandboxed yes, war live, Local PlayFab on, multiplayer off, engine log off, crash dump on, launch options -log ") &&
                  lines[3].Contains(Privacy.ProfileMark), "Settings, with the launch options' user folder scrubbed: " + lines[3]);
            using var document = JsonDocument.Parse(facts.ToJson().ToJsonString());
            var back = SessionFacts.From(document.RootElement)!;
            check(back.Lines().SequenceEqual(lines), "Not the same after result.json: " + string.Join(" | ", back.Lines()));
            check(SessionFacts.From(JsonDocument.Parse("[]").RootElement) == null, "An old session's missing facts were read.");
        });

        test("A log's dated line: the local date and time, the offset from UTC and which times follow", () =>
        {
            check(LogHeader.Line(new DateTimeOffset(2026, 10, 26, 5, 34, 56, TimeSpan.FromHours(-7)), utcTimes: true) ==
                "log opened 2026-10-26 05:34:56 local time (UTC-07:00); the times below are UTC", "West of UTC.");
            check(LogHeader.Line(new DateTimeOffset(2026, 10, 26, 18, 4, 56, new TimeSpan(5, 30, 0)), utcTimes: false) ==
                "log opened 2026-10-26 18:04:56 local time (UTC+05:30); the times below are local", "East of UTC, local times.");
            check(LogHeader.Line(new DateTimeOffset(2026, 1, 2, 3, 4, 5, TimeSpan.Zero), utcTimes: true).Contains("(UTC+00:00)"), "UTC itself.");
        });

        test("System facts: Windows 11 by its build, NVIDIA's driver number, and what this PC says", () =>
        {
            check(SystemFacts.Describe(10, 0, 26100, 4652, "24H2") == "Windows 11 24H2 (10.0.26100.4652)" && SystemFacts.Describe(10, 0, 19044, 2364, null) == "Windows 10 (10.0.19044.2364)",
                "Windows names.");
            check(SystemFacts.Driver("NVIDIA GeForce RTX 4070", "32.0.15.8129") == "32.0.15.8129 (NVIDIA 581.29)" &&
                  SystemFacts.Driver("AMD Radeon RX 7800 XT", "32.0.21013.1000") == "32.0.21013.1000" && SystemFacts.Driver("x", null) == "unknown", "Driver versions.");
            var here = SystemFacts.Windows();
            check(here.Length > 0 && Privacy.Scrub(here) == here, "This PC's system names it: " + here);
            check(SystemFacts.Gpus().All(g => g.Name.Length > 0 && Privacy.Scrub(g.Name) == g.Name), "A GPU entry is empty or names the PC.");
        });

        test("Mod lines carry each mod's version and content hash", () =>
        {
            using var modset = JsonDocument.Parse("""{"mods": [{"id": "skye.d11", "version": "1.2.0", "hash": "0123456789abcdef0123456789abcdef"}]}""");
            var lines = SessionFacts.ModLines(["skye.d11 1.2.0", "bingus.colors 2.0"], modset.RootElement);
            check(lines.SequenceEqual(["Mod: skye.d11 1.2.0, content hash 0123456789abcdef", "Mod: bingus.colors 2.0"]), "Mods: " + string.Join(" | ", lines));
        });

        var sessions = Path.Combine(root, "crash-report", "sessions");
        string Session(string id, string result, bool sandboxed = false)
        {
            var run = Directory.CreateDirectory(Path.Combine(sessions, id)).FullName;
            File.WriteAllText(Path.Combine(run, "result.json"), result);
            Directory.CreateDirectory(Path.Combine(run, "backend"));
            if (sandboxed) Directory.CreateDirectory(Path.Combine(run, "game"));
            return run;
        }

        test("A session crashed when the game left a dump, or ended by itself with an error or a crash note; not when it was stopped", () =>
        {
            check(CrashReport.Crashed(Session(new string('1', 32), """{"gameLaunched": true, "gameExitCode": "0xC0000005"}""")), "An access violation exit.");
            check(CrashReport.ExitText(Path.Combine(sessions, new string('1', 32))) == "0xC0000005 (access violation)", "The launcher's text for the exit.");
            check(!CrashReport.Crashed(Session(new string('2', 32), """{"gameLaunched": true, "gameExitCode": "0x00000000"}""")), "A clean exit.");
            check(!CrashReport.Crashed(Session(new string('3', 32), """{"gameLaunched": true, "observationStoppedBySignal": true, "gameExitCode": "0x00000001"}""")),
                "A session the launcher stopped.");
            check(!CrashReport.Crashed(Session(new string('4', 32), """{"gameLaunched": false, "status": "failed"}""")), "A session that never launched the game.");
            var noted = Session(new string('5', 32), """{"gameLaunched": true, "gameExitCode": "0x00000000"}""", sandboxed: true);
            File.WriteAllText(Path.Combine(noted, "game", "modloader.log"), "10:00:00.000 crash: fail-fast about to end the game (thread 7, asked from ucrtbase.dll+0x9a1b); stack:\n");
            check(CrashReport.Crashed(noted), "A crash the mod loader noted in a sandboxed session's log.");
            var dumped = Session(new string('6', 32), "{}");
            File.WriteAllBytes(Path.Combine(dumped, "crash-42.dmp"), [0x4D, 0x44, 0x4D, 0x50]);
            check(CrashReport.Crashed(dumped), "A session that left a dump.");
            check(CrashReport.ExitText(dumped) == null, "No exit code kept: no text.");
            check(CrashReport.ExitText(Session(new string('8', 32), """{"gameLaunched": true, "exitCode": "-1073741515"}""")) ==
                "0xC0000135 (a DLL the game needs could not be found)", "The runner's own exit code, as a start-up DLL failure.");
            var handled = Session(new string('7', 32), """{"gameLaunched": true, "observationStoppedBySignal": true, "gameExitCode": "0x00000001"}""");
            File.WriteAllBytes(Path.Combine(handled, "crash-43.dmp"), [0x4D, 0x44, 0x4D, 0x50]);
            check(!CrashReport.Crashed(handled), "A session the launcher stopped after a fault the game handled left a dump.");
            check(CrashReport.Meaning(0xC0000005) == "access violation" && CrashReport.Meaning(0xC0000094) == "integer divide by zero" &&
                CrashReport.Meaning(0x1) == null, "Exit code meanings.");
        });

        var crashed = Session(new string('c', 32), $$"""
            {"status": "observed", "gameLaunched": true, "gameExitCode": "0xC0000005", "startedUtc": "2026-10-25T10:00:00Z", "completedUtc": "2026-10-25T10:30:00Z",
             "mods": {"loaded": ["skye.d11 1.2.0", "bingus.colors 2.0"], "problems": ["broken.mod: did not compile at C:\\Users\\Alex\\mods"], "syncedWith": "{{Host}}"},
             "report": {"hellforge": "0.3.0-beta (abc1234)", "engine": "0.1.40.0", "modSdk": 15, "gameBuild": "25480438", "system": "Windows 11 24H2 (10.0.26100.4652)",
                        "wine": null, "gpus": [{"name": "NVIDIA GeForce RTX 4070", "driver": "32.0.15.8129"}], "sandboxed": false, "war": "open (seed 7)",
                        "localPlayFab": true, "multiplayer": true, "engineLog": false, "crashDump": true, "gameOptions": []} }
            """);
        File.WriteAllText(Path.Combine(crashed, "modset.json"), """{"mods": [{"id": "skye.d11", "version": "1.2.0", "hash": "fedcba9876543210fedcba9876543210", "folder": "C:\\Users\\Alex\\mods\\d11"}]}""");
        var loaderLines = Enumerable.Range(1, 260).Select(i => $"10:{i / 60:00}:{i % 60:00}.000 loader event {i}").ToList();
        loaderLines.Add($"10:29:00.000 crash: c0000005 on thread 4242 at game.dll+0xf8f670 (0x7ff6ab8f8f670), reading 0x0; rax 0 rbx {FriendId}; stack: g+0x1");
        loaderLines.Add("10:29:00.001 crash: by module: at game.dll+0xf8f670; rcx kernel32.dll+0x1234; stack outside the game: none");
        loaderLines.Add($"10:29:00.002 network: {Friend} at 198.51.100.4");
        File.WriteAllLines(Path.Combine(crashed, "modloader.log"), loaderLines);
        File.WriteAllText(Path.Combine(crashed, "session.log"), $"10:00:00 HellForge 0.3.0-beta\n10:31:00 Session result: C:\\Users\\Alex\\HellForge\\result.json\n");
        File.WriteAllText(Path.Combine(crashed, "backend", "multiplayer.log"),
            $"10:04:00.000 Steam friends running HellForge: 2 ({Friend}, {Counted}).\n10:05:00.000 Profile of {Friend} received for the friends list.\n10:06:00.000 Mod sync: {Host}'s lobby: download, skye.d11 download.\n" +
            $"10:07:00.000 peer {FriendId:X} at 192.0.2.77:27015\n");
        File.WriteAllBytes(Path.Combine(crashed, "crash-4242.dmp"), [0x4D, 0x44, 0x4D, 0x50, 0, 1, 2, 3]);
        Directory.CreateDirectory(Path.Combine(crashed, "steam-link"));
        File.WriteAllBytes(Path.Combine(crashed, "steam-link", "steam_api64.dll"), [0x4D, 0x5A, 0, 0]);

        test("The crash note is found: code, module and offset, thread, and the module line after it", () =>
        {
            var fault = CrashReport.FindFault(File.ReadAllLines(Path.Combine(crashed, "modloader.log")))!;
            check(fault.Code == "c0000005" && fault.Thread == "4242" && fault.Where == "game.dll+0xf8f670" && fault.ByModule == "game.dll+0xf8f670" && fault.Line == 260,
                $"Fault: {fault}");
            check(CrashReport.FindFault(["10:00:00.000 nothing here"]) == null, "A fault in a log without one.");
        });

        test("The friends the backend counted are left out, also when their profile never arrived", () =>
        {
            var names = ReportPrivacy.NamesIn(crashed);
            check(names.Contains(Counted) && names.Contains(Friend) && names.Contains(Host), "Names: " + string.Join(", ", names));
        });

        test("The summary states versions, system, settings, the crash, the mods and the last 200 log lines, all scrubbed", () =>
        {
            var summary = CrashReport.Summary(crashed, new ReportPrivacy(Pc, ReportPrivacy.NamesIn(crashed)), new DateTime(2026, 10, 25, 11, 0, 0, DateTimeKind.Utc), false);
            foreach (var expected in new[] { "HellForge 0.3.0-beta (abc1234), engine 0.1.40.0, mod SDK 15, game build 25480438", "System: Windows 11 24H2 (10.0.26100.4652)",
                "GPU: NVIDIA GeForce RTX 4070, driver 32.0.15.8129", "war open (seed 7)", "Game exit code: 0xC0000005 (access violation)",
                "Crash: c0000005 at game.dll+0xf8f670 on thread 4242 (by module: game.dll+0xf8f670)", "Mods (2):",
                "Mod: skye.d11 1.2.0, content hash fedcba9876543210", "Mod: bingus.colors 2.0", "Crash dump: not included",
                "Loader events before the crash (modloader.log):", "loader event 241", "Last 200 log lines (modloader.log):", "Session " + new string('c', 32) })
                check(summary.Contains(expected), "The summary lacks \"" + expected + "\":\n" + summary);
            var tail = summary[summary.IndexOf("Last 200 log lines", StringComparison.Ordinal)..].Split('\n').Count(l => l.Contains(" loader event ") || l.Contains("crash:") || l.Contains("network:"));
            check(tail == 200, "The tail has " + tail + " lines.");
            check(!summary.Contains("loader event 63" + Environment.NewLine) && summary.Contains("loader event 64" + Environment.NewLine), "The tail does not end the log's last 200 lines.");
            foreach (var leaked in new[] { "Alex", "DESKTOP-K2Q7R4T", Friend, Host, FriendId.ToString(), "198.51.100.4" })
                check(!summary.Contains(leaked, StringComparison.OrdinalIgnoreCase), "The summary names " + leaked + ":\n" + summary);
        });

        test("The crash report zip: summary, the scrubbed logs without programs or dump, and the dump only when asked for", () =>
        {
            var without = CrashReport.Save(crashed, null, Path.Combine(root, "crash-report", "without.zip"), includeDump: false, Pc);
            using (var zip = ZipFile.OpenRead(without.Zip))
            {
                var names = zip.Entries.Select(e => e.FullName).ToList();
                check(!without.Dump && names[0] == "summary.txt" && names.All(n => n == "summary.txt" || n.StartsWith("logs/" + new string('c', 32) + "/")),
                    "Entries: " + string.Join(", ", names));
                check(names.Contains($"logs/{new string('c', 32)}/modloader.log") && names.Contains($"logs/{new string('c', 32)}/backend/multiplayer.log") &&
                      !names.Any(n => n.EndsWith(".dmp") || n.EndsWith(".dll")), "Logs: " + string.Join(", ", names));
                foreach (var entry in zip.Entries)
                {
                    using var reader = new StreamReader(entry.Open());
                    var text = reader.ReadToEnd();
                    foreach (var leaked in new[] { "Alex", "DESKTOP-K2Q7R4T", Friend, Host, Counted, FriendId.ToString(), FriendId.ToString("X"), "192.0.2.77", "198.51.100.4" })
                        check(!text.Contains(leaked, StringComparison.OrdinalIgnoreCase), entry.FullName + " names " + leaked + ":\n" + text);
                }
            }
            var with = CrashReport.Save(crashed, null, Path.Combine(root, "crash-report", "with.zip"), includeDump: true, Pc);
            using (var zip = ZipFile.OpenRead(with.Zip))
            {
                var dump = zip.Entries.SingleOrDefault(e => e.FullName == "dump/crash-4242.dmp");
                check(with.Dump && dump != null && dump.Length == 8, "The dump is missing or changed.");
                using var reader = new StreamReader(zip.GetEntry("summary.txt")!.Open());
                check(reader.ReadToEnd().Contains("cannot be scrubbed"), "The summary does not warn that the dump cannot be scrubbed.");
            }
        });

        test("A session from before the facts were kept still gets a summary, with this PC's system", () =>
        {
            var old = Session(new string('d', 32), """{"status": "observed", "gameLaunched": true, "gameExitCode": "0xC0000409"}""");
            var summary = CrashReport.Summary(old, new ReportPrivacy(Pc, []), DateTime.UtcNow, false);
            check(summary.Contains("Versions and settings: not recorded") && summary.Contains("This PC now: ") && summary.Contains("(fail-fast") &&
                  summary.Contains("Crash: the mod loader noted none") && summary.Contains("Crash dump: none"), summary);
        });
    }
}
