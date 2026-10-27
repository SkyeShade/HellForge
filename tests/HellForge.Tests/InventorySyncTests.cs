using HellForge.Core;

// Profile > Account sync (InventorySync, SnapshotSeal, ProfileBackups, AccountImportSettings): where snapshots live, how a
// finished capture becomes one only with a seal for this PC, profile backups and the import choices. Synthetic files only.
static class InventorySyncTests
{
    const string Inventory = """{"items":[{"mixId":1,"amount":1,"isDiscovered":true},{"mixId":2,"amount":3,"isDiscovered":false}]}""";
    const string Progression = """{"experience":485592,"onboardingState":128,"onboardingFlags":7}""";
    const ulong Account = 76561198000000001;

    public static void Register(Action<string, Action> test, Action<bool, string> check, Action<Action> reject, string root)
    {
        test("Sync Inventory keeps each profile's snapshot in account-snapshot beside profiles, and an inventory alone makes one", () =>
        {
            var install = Path.Combine(root, "sync-layout");
            var expected = Path.Combine(install, InventorySync.SnapshotFolder, "player");
            check(InventorySync.SnapshotDirectoryFromProfileDir(Path.Combine(install, "profiles", "player")) == expected, "Profile folder.");
            check(InventorySync.SnapshotDirectoryFromProfileDir(Path.Combine(install, "profiles", "player") + Path.DirectorySeparatorChar) == expected, "Trailing separator.");
            check(InventorySync.SnapshotDirectory(install, "player") == expected, "Launcher and backend disagree on the folder.");
            check(InventorySync.SealKeyFor(expected) == Path.Combine(install, InventorySync.SnapshotFolder, SnapshotSeal.KeyFileName), "The seal key is not shared by the installation.");
            check(!InventorySync.HasSnapshot(expected), "A missing folder is a snapshot.");
            Directory.CreateDirectory(expected);
            File.WriteAllText(Path.Combine(expected, InventorySync.InventoryFile), Inventory);
            check(InventorySync.HasSnapshot(expected), "An inventory without progression is not a snapshot.");
        });

        test("Sync Inventory finds its capture only in an installation's app/capture", () =>
        {
            var install = Path.Combine(root, "sync-proxy");
            check(InventorySync.FindCaptureProxy(null) == null, "Found a capture without an installation.");
            check(InventorySync.FindCaptureProxy(install) == null, "Found a capture that is not there.");
            var proxy = Path.Combine(install, "app", "capture", InventorySync.ProxyExe);
            Directory.CreateDirectory(Path.GetDirectoryName(proxy)!);
            File.WriteAllText(proxy, "synthetic");
            check(InventorySync.FindCaptureProxy(install) == proxy, "The installed capture was not found.");
        });

        test("The capture's hosts line never joins the player's last entry, and cleanup removes only the capture's lines", () => Hosts(root, check));

        test("A snapshot seal holds only for its files, its Steam account and this installation's key", () => Seals(root, check));

        test("A capture becomes the snapshot only sealed and with an inventory list; a capture without progression never replaces one with it", () =>
            Finalize(root, check));

        test("The capture's state says when it listens and when its files are sealed, for the launcher to take while the game plays on", () =>
            State(root, check));

        test("The capture logs a refused request's short fields, never a token-like value, and only names of headers and query parameters", () =>
            Log(check));

        test("The capture's own reads repeat the game's client headers, without the login's own, so the API's language check passes", () =>
            Headers(check));

        test("The capture's diagnostics give shapes, causes and a tally without values, and a sync collects the game's anti-cheat and crash logs", () =>
            Diagnostics(root, check));

        test("Steam's 32-bit account id is the SteamID64 of an individual account", () =>
            check(SteamAccount.FromAccountId(39734273) == 76561198000000001, "Wrong SteamID64."));

        test("Profile backups are kept newest first, ten at most, and only profile copies count", () => Backups(root, check));

        test("Import choices default per category, keep what the file sets and refuse rules a category does not offer", () => Settings(root, check));
    }

    static void Hosts(string root, Action<bool, string> check)
    {
        const string Host = "api.example.test";
        var folder = Directory.CreateDirectory(Path.Combine(root, "hosts")).FullName;
        // Notepad leaves no newline after the last entry; an empty file and one ending in a newline too.
        foreach (var (name, original) in new[] { ("no-newline", "# player\r\n0.0.0.0 foo.com"), ("newline", "0.0.0.0 foo.com\r\n"), ("empty", "") })
        {
            var hosts = Path.Combine(folder, name);
            File.WriteAllText(hosts, original);
            CaptureHosts.Add(hosts, Host);
            var lines = File.ReadAllLines(hosts);
            check(lines.Contains(CaptureHosts.Marker) && lines.Contains("127.0.0.1 " + Host), name + ": the capture's lines are not lines of their own.");
            check(!original.Contains("foo", StringComparison.Ordinal) || lines.Contains("0.0.0.0 foo.com"), name + ": the player's last entry was changed.");
            check(CaptureHosts.Remove(hosts, Host), name + ": cleanup found nothing.");
            check(File.ReadAllLines(hosts).SequenceEqual(original.Split("\r\n", StringSplitOptions.RemoveEmptyEntries)), name + ": cleanup changed the player's entries.");
            check(!CaptureHosts.Remove(hosts, Host), name + ": a second cleanup removed more.");
        }
        check(!CaptureHosts.Remove(Path.Combine(folder, "missing"), Host), "Cleanup of a missing file removed something.");
    }

    static void Seals(string root, Action<bool, string> check)
    {
        var folder = Directory.CreateDirectory(Path.Combine(root, "seal", "account-snapshot", "player")).FullName;
        var key = InventorySync.SealKeyFor(folder);
        File.WriteAllText(Path.Combine(folder, SnapshotSeal.InventoryFile), Inventory);
        File.WriteAllText(Path.Combine(folder, SnapshotSeal.ProgressionFile), Progression);
        var captured = new DateTime(2026, 10, 25, 9, 30, 0, DateTimeKind.Utc);
        SnapshotSeal.Write(folder, key, Account, captured);
        check(File.Exists(key) && SnapshotSeal.Verify(folder, key) is { SteamAccount: Account } info && info.CapturedUtc == captured, "A fresh seal does not verify.");

        File.WriteAllText(Path.Combine(folder, SnapshotSeal.ProgressionFile), Progression.Replace("485592", "999999", StringComparison.Ordinal));
        check(SnapshotSeal.Verify(folder, key) == null, "Edited progression still verifies.");
        File.WriteAllText(Path.Combine(folder, SnapshotSeal.ProgressionFile), Progression);
        File.Delete(Path.Combine(folder, SnapshotSeal.ProgressionFile));
        check(SnapshotSeal.Verify(folder, key) == null, "A seal over progression verifies without it.");
        File.WriteAllText(Path.Combine(folder, SnapshotSeal.ProgressionFile), Progression);
        check(SnapshotSeal.Verify(folder, key) != null, "The restored files no longer verify.");

        var otherKey = Path.Combine(root, "seal-other", SnapshotSeal.KeyFileName);
        var other = Directory.CreateDirectory(Path.Combine(root, "seal-other", "player")).FullName;
        File.WriteAllText(Path.Combine(other, SnapshotSeal.InventoryFile), Inventory);
        SnapshotSeal.Write(other, otherKey, Account, captured);
        check(SnapshotSeal.Verify(folder, otherKey) == null && SnapshotSeal.Verify(other, key) == null, "A seal verifies under another installation's key.");
        check(SnapshotSeal.Verify(folder, Path.Combine(root, "seal", "missing.key")) == null, "A seal verifies without a key.");
        File.WriteAllText(Path.Combine(folder, SnapshotSeal.FileName), "{");
        check(SnapshotSeal.Verify(folder, key) == null, "A broken seal verifies.");
    }

    static void Finalize(string root, Action<bool, string> check)
    {
        var sync = Path.Combine(root, "sync-run");
        var snapshot = Path.Combine(root, "sync-install", InventorySync.SnapshotFolder, "player");
        var key = InventorySync.SealKeyFor(snapshot);
        Directory.CreateDirectory(sync);
        check(InventorySync.FinalizeCapture(sync, snapshot) != null, "A capture without an inventory was saved.");
        File.WriteAllText(Path.Combine(sync, InventorySync.InventoryFile), """{"error":"denied"}""");
        check(InventorySync.FinalizeCapture(sync, snapshot) != null, "An inventory without items was saved.");
        File.WriteAllText(Path.Combine(sync, InventorySync.InventoryFile), Inventory);
        File.WriteAllText(Path.Combine(sync, InventorySync.ProgressionFile), Progression);
        check(InventorySync.FinalizeCapture(sync, snapshot) != null && !Directory.Exists(snapshot), "An unsealed capture was saved.");

        SnapshotSeal.Write(sync, key, Account, DateTime.UtcNow);
        check(InventorySync.FinalizeCapture(sync, snapshot) == null, "A sealed capture was refused.");
        check(!File.Exists(Path.Combine(sync, InventorySync.InventoryFile)) && !File.Exists(Path.Combine(sync, SnapshotSeal.FileName)), "The capture's files stayed in the sync folder.");
        check(SnapshotSeal.Verify(snapshot, key) is { SteamAccount: Account }, "The moved snapshot no longer verifies.");
        check(InventorySync.ReadMetadata(snapshot) is { Items: 2, Experience: 485592, Progression: true, CapturedUtc: not null }, "Metadata does not describe the capture.");

        File.WriteAllText(Path.Combine(sync, InventorySync.InventoryFile), Inventory);
        File.WriteAllText(Path.Combine(sync, InventorySync.ProgressionFile), "[1]");
        check(InventorySync.FinalizeCapture(sync, snapshot) != null && File.Exists(Path.Combine(snapshot, InventorySync.ProgressionFile)),
            "A progression that is not an object was saved, or changed the snapshot.");
        File.Delete(Path.Combine(sync, InventorySync.ProgressionFile));
        SnapshotSeal.Write(sync, key, Account, DateTime.UtcNow);
        check(InventorySync.FinalizeCapture(sync, snapshot) != null && File.Exists(Path.Combine(snapshot, InventorySync.ProgressionFile)) &&
            InventorySync.ReadMetadata(snapshot) is { Items: 2, Experience: 485592, Progression: true },
            "An inventory-only capture replaced the complete snapshot, or was not refused.");
        check(File.Exists(Path.Combine(sync, InventorySync.InventoryFile)), "The refused capture's files were moved.");

        // A progression without XP is still progression; a snapshot from before the flag has it exactly when it has XP.
        File.WriteAllText(Path.Combine(sync, InventorySync.InventoryFile), Inventory);
        File.WriteAllText(Path.Combine(sync, InventorySync.ProgressionFile), """{"onboardingState":128}""");
        SnapshotSeal.Write(sync, key, Account, DateTime.UtcNow);
        check(InventorySync.FinalizeCapture(sync, snapshot) == null && InventorySync.ReadMetadata(snapshot) is { Experience: null, Progression: true },
            "A progression without XP does not count as progression.");

        // With no complete snapshot to keep, an inventory-only capture still makes one: the backend's premium ownership
        // comes from it, while the launcher reports that sync as failed.
        var newcomer = Path.Combine(root, "sync-install", InventorySync.SnapshotFolder, "newcomer");
        File.WriteAllText(Path.Combine(sync, InventorySync.InventoryFile), Inventory);
        File.Delete(Path.Combine(sync, InventorySync.ProgressionFile));
        SnapshotSeal.Write(sync, InventorySync.SealKeyFor(newcomer), Account, DateTime.UtcNow);
        check(InventorySync.FinalizeCapture(sync, newcomer) == null && InventorySync.ReadMetadata(newcomer) is { Items: 2, Experience: null, Progression: false },
            "An inventory-only capture made no snapshot where there was none.");

        File.WriteAllText(Path.Combine(snapshot, InventorySync.MetadataFile), """{"items":2,"experience":7}""");
        check(InventorySync.ReadMetadata(snapshot) is { Progression: true }, "An older snapshot with XP lost its progression.");
        File.WriteAllText(Path.Combine(snapshot, InventorySync.MetadataFile), """{"items":2,"experience":null}""");
        check(InventorySync.ReadMetadata(snapshot) is { Progression: false }, "An older snapshot without XP claims progression.");
    }

    static void State(string root, Action<bool, string> check)
    {
        var sync = Directory.CreateDirectory(Path.Combine(root, "sync-state")).FullName;
        check(CaptureState.Read(sync) == null && !InventorySync.IsListening(sync) && !InventorySync.IsSealed(sync), "A missing state reads as a capture.");
        CaptureState.Write(sync, 42, sealedCapture: false, progression: false);
        check(InventorySync.IsListening(sync) && !InventorySync.IsSealed(sync), "A listening capture reads as sealed, or not as listening.");
        CaptureState.Write(sync, 42, sealedCapture: true, progression: false);
        check(InventorySync.IsSealed(sync) && CaptureState.Read(sync) is { Progression: false }, "A sealed partial capture reads wrong.");
        File.WriteAllText(Path.Combine(sync, CaptureState.FileName), "{\"sealed\":");
        check(!InventorySync.IsSealed(sync), "A half-written state reads as sealed.");
    }

    static void Diagnostics(string root, Action<bool, string> check)
    {
        static byte[] Utf8(string text) => System.Text.Encoding.UTF8.GetBytes(text);
        var shape = CaptureDiagnostics.JsonShape(Utf8("""{"items":[{"mixId":1},{"mixId":2}],"limitSettings":{"a":1,"b":2},"name":"Secret Ship","xp":5}"""));
        check(shape == "object{items[2], limitSettings{2}, name:string, xp:number}", "The body's shape is wrong: " + shape);
        check(!shape.Contains("Secret", StringComparison.Ordinal) && !shape.Contains('5', StringComparison.Ordinal), "A value reached the shape.");
        check(CaptureDiagnostics.JsonShape(Utf8("[1,2,3]")) == "array[3]" && CaptureDiagnostics.JsonShape([]) == "empty" &&
            CaptureDiagnostics.JsonShape(Utf8("nope")) == "not JSON, 4 bytes", "Shapes of other bodies are wrong.");
        var chain = CaptureDiagnostics.Chain(new HttpRequestException("send failed", new IOException("read failed",
            new System.Net.Sockets.SocketException((int)System.Net.Sockets.SocketError.ConnectionReset))));
        check(chain.StartsWith("HttpRequestException: send failed <- IOException: read failed <- SocketException (10054): ", StringComparison.Ordinal),
            "The cause chain is wrong: " + chain);

        var tally = new CaptureTally();
        tally.Add("POST", "/api/Proxy/...", 200);
        tally.Add("POST", "/api/Proxy/...", 200);
        tally.Add("GET", "/api/Progression", 400);
        check(tally.Total == 3 && tally.Lines().SequenceEqual(["GET /api/Progression 400 x1", "POST /api/Proxy/... 200 x2"]), "The tally is wrong.");

        var sync = Directory.CreateDirectory(Path.Combine(root, "sync-diagnostics")).FullName;
        InventorySync.Note(sync, "first step");
        InventorySync.Note(Path.Combine(root, "no-such-folder", "sync"), "lost");
        check(File.ReadAllText(Path.Combine(sync, InventorySync.LauncherLogFile)).TrimEnd().EndsWith(" first step", StringComparison.Ordinal),
            "The launcher's step was not noted.");

        var appData = Path.Combine(root, "appdata");
        var since = DateTime.UtcNow.AddMinutes(-1);
        check(InventorySync.CollectGameLogs(sync, since, appData).SequenceEqual(["no Easy Anti-Cheat launcher log written since the sync started",
            "no game crash report since the sync started"]), "Missing game logs are not reported as missing.");
        var eacLog = Path.Combine(appData, "EasyAntiCheat", "product", "deployment", "anticheatlauncher.log");
        Directory.CreateDirectory(Path.GetDirectoryName(eacLog)!);
        File.WriteAllText(eacLog, "[16:16:38] Launcher finished with: 301");
        var old = Path.Combine(appData, "EasyAntiCheat", "product", "older", "anticheatlauncher.log");
        Directory.CreateDirectory(Path.GetDirectoryName(old)!);
        File.WriteAllText(old, "old");
        File.SetLastWriteTimeUtc(old, since.AddHours(-1));
        var crash = Path.Combine(appData, "Arrowhead", "Helldivers2", "crash_data", "report", "params.txt");
        Directory.CreateDirectory(Path.GetDirectoryName(crash)!);
        File.WriteAllText(crash, "x");
        var found = InventorySync.CollectGameLogs(sync, since, appData);
        check(File.ReadAllText(Path.Combine(sync, "eac-deployment.log")).Contains("301", StringComparison.Ordinal) &&
            !File.Exists(Path.Combine(sync, "eac-older.log")), "The run's Easy Anti-Cheat log was not copied, or an older one was.");
        check(found.Count == 2 && found[1].StartsWith("the game wrote ", StringComparison.Ordinal), "A new crash report was not reported: " + string.Join(" | ", found));
    }

    static void Headers(Action<bool, string> check)
    {
        var login = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            ["User-Agent"] = "Stingray WIN64 : stingray:1 e176252a3ac4eb5436b25e7cef665a8c71afa243",
            ["Accept-Language"] = "de-DE",
            ["Authorization"] = "session 0",
            ["X-Signature"] = new string('a', 44),
            ["Key_ID"] = "1234",
            ["Content-Length"] = "345"
        };
        var now = new DateTimeOffset(2026, 10, 26, 16, 44, 47, TimeSpan.Zero);
        var headers = CaptureHeaders.ForRead(login, now);
        check(headers.Count == 5 && headers["Accept-Language"] == "de-DE" && headers["User-Agent"] == login["User-Agent"] &&
            headers["Accept"] == "application/json" && headers["Content-Type"] == "application/json",
            "The login's language or user agent, or the JSON accept, is missing.");
        check(headers["ct"] == "1793033087000000" && headers["ct"] == CaptureHeaders.Microseconds(now),
            "The client time is not microseconds since the epoch: " + headers["ct"]);
        check(!headers.ContainsKey("Authorization") && !headers.ContainsKey("X-Signature") && !headers.ContainsKey("Key_ID") &&
            !headers.ContainsKey("Content-Length"), "A header of the login request itself was repeated.");

        var bare = CaptureHeaders.ForRead(null, now);
        check(bare["Accept-Language"] == CaptureHeaders.DefaultLanguage && !bare.ContainsKey("User-Agent"),
            "A read without the login's headers has no language, or invented a user agent.");
        var blank = CaptureHeaders.ForRead(new Dictionary<string, string> { ["Accept-Language"] = " " }, now);
        check(blank["Accept-Language"] == CaptureHeaders.DefaultLanguage, "A blank language was repeated.");
        var lower = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase) { ["accept-language"] = "ja-JP" };
        check(CaptureHeaders.ForRead(lower, now)["Accept-Language"] == "ja-JP", "A lower-case header name was not found.");
    }

    static void Log(Action<bool, string> check)
    {
        static string Error(string json) => CaptureLog.ErrorText(System.Text.Encoding.UTF8.GetBytes(json));
        check(Error("""{"title":"Bad Request","status":400,"errors":{"version":["The version field is required."]}}""") ==
            "title=Bad Request; status=400; errors.version[0]=The version field is required.", "An error body's fields are not logged.");
        var session = new string('a', 40);
        var redacted = Error($$"""{"error":"denied","session":"{{session}}"}""");
        check(!redacted.Contains(session, StringComparison.Ordinal) && redacted.Contains("<40 characters>", StringComparison.Ordinal), "A token-like value reached the log.");
        check(Error("<html>nope</html>") == "not JSON, 17 bytes" && CaptureLog.ErrorText([]) == "empty body", "A body that is not JSON is logged.");
        check(Error($$"""{"message":"{{new string('x', 20)}} {{new string('y', 400)}}"}""").Length <= 303, "A long reason is not capped.");
        check(Error("{\"message\":\"line\\nbreak\"}") == "message=line break", "A control character reached the log.");
        check(CaptureLog.QueryNames("/api/Progression?season=5&id=123") == "id, season" && CaptureLog.QueryNames("/api/Progression") == "none",
            "Query names are wrong, or values reached the log.");
        check(CaptureLog.Names(["User-Agent", "authorization", "Accept"]) == "Accept, authorization, User-Agent", "Header names are not sorted.");
    }

    static void Backups(string root, Action<bool, string> check)
    {
        var profile = Directory.CreateDirectory(Path.Combine(root, "backups", "player")).FullName;
        var start = new DateTime(2026, 10, 25, 9, 0, 0, DateTimeKind.Utc);
        check(ProfileBackups.Save(profile, "import", start) == null && ProfileBackups.List(profile).Count == 0, "A profile without profile.json was backed up.");
        File.WriteAllText(Path.Combine(profile, "profile.json"), "{\"n\":0}");
        for (var i = 0; i < ProfileBackups.Keep + 3; i++)
        {
            File.WriteAllText(Path.Combine(profile, "profile.json"), "{\"n\":" + i + "}");
            ProfileBackups.Save(profile, i % 2 == 0 ? "import" : "restore", start.AddMinutes(i));
        }
        File.WriteAllText(Path.Combine(profile, ProfileBackups.Folder, "notes.json"), "{}");
        var backups = ProfileBackups.List(profile);
        check(backups.Count == ProfileBackups.Keep, "Old backups were not pruned: " + backups.Count);
        check(backups[0].SavedUtc == start.AddMinutes(ProfileBackups.Keep + 2) && backups[0].Reason == "import" && File.ReadAllText(backups[0].Path) == "{\"n\":12}",
            "The newest backup is not first or not the profile copy.");
        check(backups.Zip(backups.Skip(1)).All(pair => pair.First.SavedUtc > pair.Second.SavedUtc), "Backups are not newest first.");
        check(RefusesArgument(() => ProfileBackups.Save(profile, "../escape", start)), "A backup reason that is not a word was taken.");
    }

    static void Settings(string root, Action<bool, string> check)
    {
        var defaults = new AccountImportSettings();
        check(!defaults.AutoApply && defaults.Includes("level") && defaults.RuleFor("level") == ImportRule.Higher && defaults.RuleFor("armory") == ImportRule.Merge,
            "Wrong defaults for numbers or items.");
        check(!defaults.Includes("ship") && !defaults.Includes("onboarding"), "Ship and tutorial progress are imported by default.");
        check(RefusesArgument(() => defaults.Set("level", true, ImportRule.Merge)), "Level took a rule it does not offer.");
        check(RefusesArgument(() => defaults.Set("super-credits", true, ImportRule.Vanilla)), "An unknown category was taken.");

        var path = Path.Combine(Directory.CreateDirectory(Path.Combine(root, "import-settings")).FullName, AccountImportSettings.FileName);
        var chosen = new AccountImportSettings { AutoApply = true };
        chosen.Set("level", true, ImportRule.Vanilla);
        chosen.Set("armory", false, ImportRule.Merge);
        chosen.Set("ship", true, ImportRule.Vanilla);
        chosen.Save(path);
        var loaded = AccountImportSettings.Load(path);
        check(loaded.AutoApply && loaded.RuleFor("level") == ImportRule.Vanilla && !loaded.Includes("armory") && loaded.Includes("ship"), "Saved choices did not load back.");

        File.WriteAllText(path, """{"autoApply":"yes","categories":{"level":{"include":false,"rule":"Merge"},"armory":{"include":false,"rule":7},"packages":{"include":false,"rule":"Vanilla"}}}""");
        var edited = AccountImportSettings.Load(path);
        check(!edited.AutoApply && edited.Includes("level") && edited.RuleFor("level") == ImportRule.Higher && edited.Includes("armory"),
            "A hand-edited rule a category does not offer was taken.");
        check(!edited.Includes("packages") && edited.RuleFor("packages") == ImportRule.Vanilla, "A valid hand-edited choice was dropped.");
        check(!AccountImportSettings.Load(Path.Combine(root, "import-settings", "missing.json")).AutoApply, "A missing file is not the defaults.");

        // Warbond unlocks have no checkbox: as in vanilla by default, every item, or none; never left out by include alone.
        var warbonds = new AccountImportSettings();
        check(warbonds.Includes("warbondItems") && warbonds.RuleFor("warbondItems") == ImportRule.Merge, "Warbond unlocks are not as in vanilla by default.");
        warbonds.Set("warbondItems", false, ImportRule.All);
        check(warbonds.Includes("warbondItems") && warbonds.RuleFor("warbondItems") == ImportRule.All, "Unchecking a category without a checkbox left it out.");
        warbonds.Set("warbondItems", true, ImportRule.None);
        check(!warbonds.Includes("warbondItems"), "None still imports warbond unlocks.");
        check(RefusesArgument(() => warbonds.Set("warbondItems", true, ImportRule.Vanilla)) && RefusesArgument(() => warbonds.Set("armory", true, ImportRule.All)),
            "A rule a category does not offer was taken.");
        File.WriteAllText(path, """{"categories":{"warbondItems":{"include":false,"rule":"Merge"}}}""");
        check(AccountImportSettings.Load(path).RuleFor("warbondItems") == ImportRule.None && !AccountImportSettings.Load(path).Includes("warbondItems"),
            "An older file that left warbond items out does not load as None.");
        File.WriteAllText(path, """{"categories":{"warbondItems":{"include":true,"rule":"Vanilla"}}}""");
        check(AccountImportSettings.Load(path).RuleFor("warbondItems") == ImportRule.Merge, "An older Use vanilla for warbond items was not reset to the default.");
        warbonds.Save(path);
        check(AccountImportSettings.Load(path).RuleFor("warbondItems") == ImportRule.None, "None did not save and load back.");
    }

    static bool RefusesArgument(Action action)
    {
        try { action(); }
        catch (ArgumentException) { return true; }
        return false;
    }
}
