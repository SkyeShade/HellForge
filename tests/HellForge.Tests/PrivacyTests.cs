using System.Text;
using System.Text.Json.Nodes;
using HellForge.Core;

// Logs and errors do not name the PC: its user folder and computer name (Privacy, native/common/hf_privacy.h).
static class PrivacyTests
{
    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        var privacy = new Privacy(@"C:\Users\Alex", ["DESKTOP-K2Q7R4T", "desktop-k2q7r4t", "PC"], "/home/alex");

        test("Logs name the user folder %USERPROFILE%, in any slash or letter case, and only as a whole folder", () =>
        {
            check(privacy.Apply(@"mod at C:\Users\Alex\Documents\HellForge\mods") == @"mod at %USERPROFILE%\Documents\HellForge\mods", "Backslashes.");
            check(privacy.Apply("C:/users/ALEX/Documents/x.ttf") == "%USERPROFILE%/Documents/x.ttf", "Forward slashes, other case.");
            check(privacy.Apply("Saved to C:\\Users\\Alex.") == "Saved to %USERPROFILE%.", "The folder itself, before a full stop.");
            check(privacy.Apply(@"C:\Users\Alexa\x and C:\Users\Alex-old\y") == @"C:\Users\Alexa\x and C:\Users\Alex-old\y", "Another folder was changed.");
            check(privacy.Apply("mod\talexmods.vanilla-plus") == "mod\talexmods.vanilla-plus", "A mod id with the account's name was changed.");
            check(privacy.Apply(@"Z:\home\alex\games and /home/alex/x") == @"Z:~\games and ~/x", "A Linux home folder.");
        });

        test("Logs name the computer PC, also inside the game's console log names, but not inside longer names", () =>
        {
            check(privacy.Apply("console_2026-10-21-21.56.41-b2db8104-DESKTOP-K2Q7R4T.txt") == "console_2026-10-21-21.56.41-b2db8104-PC.txt", "Log file name.");
            check(privacy.Apply("host desktop-k2q7r4t ready") == "host PC ready", "Lower case.");
            check(privacy.Apply("DESKTOP-K2Q7R4TX and XDESKTOP-K2Q7R4T") == "DESKTOP-K2Q7R4TX and XDESKTOP-K2Q7R4T", "Part of a longer name.");
            check(privacy.Apply("a PC name of two letters: PCs, OPC") == "a PC name of two letters: PCs, OPC", "Names under 3 characters are left.");
        });

        test("JSON is scrubbed as written, with escaped backslashes and a user folder that is not ASCII", () =>
        {
            var accented = new Privacy(@"C:\Users\Jöhn", ["WORKSTATION"]);
            var node = new JsonObject { ["installRoot"] = @"C:\Users\Jöhn\Documents\HellForge", ["host"] = "WORKSTATION", ["n"] = 1 };
            var json = accented.ApplyJson(node);
            check(!json.Contains("Jöhn") && !json.Contains("WORKSTATION") && !json.Contains(@"\u00F6", StringComparison.OrdinalIgnoreCase), "A name stayed: " + json);
            check((string?)JsonNode.Parse(json)!["installRoot"] == @"%USERPROFILE%\Documents\HellForge", "Not valid JSON or not scrubbed: " + json);
        });

        test("Text files are scrubbed byte for byte with their BOM; binary and non-UTF-8 files are left alone", () =>
        {
            var bom = new byte[] { 0xEF, 0xBB, 0xBF }.Concat(Encoding.UTF8.GetBytes(@"at C:\Users\Alex\x")).ToArray();
            var scrubbed = privacy.ApplyBytes(bom);
            check(scrubbed.Take(3).SequenceEqual(bom.Take(3)) && Encoding.UTF8.GetString(scrubbed, 3, scrubbed.Length - 3) == @"at %USERPROFILE%\x", "BOM text.");
            var binary = Encoding.UTF8.GetBytes("MDMP\0C:\\Users\\Alex");
            check(ReferenceEquals(privacy.ApplyBytes(binary), binary), "A binary file was changed.");
            var latin1 = new byte[] { 0x43, 0x3A, 0xE9, 0xFF };
            check(ReferenceEquals(privacy.ApplyBytes(latin1), latin1), "Non-UTF-8 bytes were changed.");
            check(Privacy.IsText("x/modloader.log") && Privacy.IsText("result.json") && !Privacy.IsText("crash.dmp"), "Text extensions.");
        });

        test("Session logs are listed newest first and exported without programs, with the PC's names scrubbed", () =>
        {
            var sessions = Path.Combine(root, "logs-export", "sessions");
            string Run(string id, string started, string status)
            {
                var folder = Directory.CreateDirectory(Path.Combine(sessions, id)).FullName;
                File.WriteAllText(Path.Combine(folder, "result.json"), $$"""{"status": "{{status}}", "startedUtc": "{{started}}", "gameExitCode": "0x00000000"}""");
                return folder;
            }
            Run(new string('a', 32), "2026-10-20T10:00:00Z", "observed");
            var newest = Run(new string('b', 32), "2026-10-22T10:00:00Z", "failed");
            Directory.CreateDirectory(Path.Combine(sessions, new string('c', 32))); // a session still starting: no result yet
            Directory.CreateDirectory(Path.Combine(sessions, "not-a-run"));
            var profile = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            File.WriteAllText(Path.Combine(newest, "session.log"), "Session result: " + Path.Combine(profile, "HellForge", "result.json"));
            Directory.CreateDirectory(Path.Combine(newest, "steam-link"));
            File.WriteAllBytes(Path.Combine(newest, "steam-link", "steam_api64.dll"), [0x4D, 0x5A, 0, 0]);
            // a sandboxed session's game folder: its logs go, the player's AppData copy and user data do not
            void Put(string relative) { var file = Path.Combine(newest, relative); Directory.CreateDirectory(Path.GetDirectoryName(file)!); File.WriteAllText(file, "x"); }
            Put(Path.Combine("game", "modloader.log"));
            Put(Path.Combine("game", "userdata", "logs", "console_1.txt"));
            Put(Path.Combine("game", "userdata", "crash_data", "settings.dat"));
            Put(Path.Combine("game", "userdata", "profiles", "actual", "saves", "user_settings.config"));
            Put(Path.Combine("game", "userdata", "shader_cache", "pipeline_library.cache"));
            Put(Path.Combine("game", "environment", "Roaming", "Some Browser", "Default", "Local Storage", "leveldb", "000003.log"));
            var runs = SessionLogs.Runs(sessions);
            check(runs.Count == 3 && runs.All(r => r.Id.Length == 32), "Runs: " + string.Join(", ", runs.Select(r => r.Id)));
            check(runs.Single(r => r.Id == new string('b', 32)).Status == "failed" && runs.Single(r => r.Id == new string('c', 32)).Status == null,
                "A run's status is not read from its result.");
            check(runs.IndexOf(runs.Single(r => r.Id == new string('b', 32))) < runs.IndexOf(runs.Single(r => r.Id == new string('a', 32))),
                "The newer session is not listed first.");
            var zip = Path.Combine(root, "logs-export", "logs.zip");
            var exported = SessionLogs.Export(newest, null, zip);
            using var archive = System.IO.Compression.ZipFile.OpenRead(zip);
            var names = archive.Entries.Select(e => e.FullName).ToList();
            check(exported.Skipped == 0 && names.All(n => !n.EndsWith(".dll", StringComparison.OrdinalIgnoreCase)) && names.Any(n => n.EndsWith("session.log")),
                "Export entries: " + string.Join(", ", names));
            check(names.Any(n => n.EndsWith("game/modloader.log")) && names.Any(n => n.EndsWith("userdata/logs/console_1.txt")) &&
                  names.Any(n => n.EndsWith("crash_data/settings.dat")) && !names.Any(n => n.Contains("/environment/") || n.Contains("/profiles/") || n.Contains("/shader_cache/")),
                "A sandboxed session's export holds its logs and crash data, not the player's AppData copy, saves or shader cache: " + string.Join(", ", names));
            using var reader = new StreamReader(archive.Entries.Single(e => e.FullName.EndsWith("session.log")).Open());
            var text = reader.ReadToEnd();
            check(!text.Contains(profile, StringComparison.OrdinalIgnoreCase) && text.Contains(Privacy.ProfileMark), "The exported log names the user folder: " + text);
        });

        test("With no user folder or computer name known, text passes unchanged", () =>
        {
            var none = new Privacy(null, [null, "", "  "]);
            check(none.Apply(@"C:\Users\Alex") == @"C:\Users\Alex" && none.Apply(null) == "", "Unknown names changed text.");
        });
    }
}
