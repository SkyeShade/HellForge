using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.Core;
using Microsoft.Win32;

namespace HellForge.Session;

// A play session of an installation (HellForge.Session.exe play): genuine ownership, the copy's programs blocked by the
// persistent network block, a loopback backend, memory-only initialization and routing edits on the copied game, until
// the game exits or stop-observation.signal appears in the run directory. No game files are changed, except that for the
// session the copy's steam_api64.dll (and, for multiplayer, PartyWin.dll and PlayFabMultiplayerWin.dll) are the
// HellForge stand-ins, with the verified originals saved and put back afterwards (or at the next session's start).
// The steps, the safety checks and the session files (result.json and the rest in logs/sessions/<run>) are those of the
// session script this replaces (scripts/Start-LocalBoot.ps1 -Play -SteamOnline -InstallRoot).
sealed partial class PlaySession(PlayOptions options)
{
    static readonly string[] ShimNames = ["PartyWin.dll", "PlayFabMultiplayerWin.dll"];
    const string CloudName = "steam_api64.dll", CloudReal = "steam_api64_hellforge.dll", CensusLog = "network-census.jsonl";

    readonly JsonObject result = new();
    readonly List<FileStream> locks = [];
    // Several installations (GameProcesses.MultiInstance): the installed game's files another installation's session held
    // when this one started, taken over once that session ends (every 5 s, on the timer's thread); released at clean-up.
    Timer? normalHoldRetry;
    readonly List<FileStream> retaken = [];
    bool retakeStopped;
    readonly List<string> swapped = [];
    ModsHold? modsHold;
    readonly Dictionary<string, string> shimPins = new(StringComparer.Ordinal);
    // HELLFORGE_DEBUG=1 writes the full step reports (addresses, before/after bytes) and the per-second diagnostics into
    // the run folder, for development. Normal sessions keep only what happened, not how.
    static readonly bool Debug = Environment.GetEnvironmentVariable("HELLFORGE_DEBUG") == "1";
    InstallLayout install = null!;
    // gameOut: where the game writes its logs and the editor its sandboxed saves (run, or run/game labelled low when sandboxed)
    string SandboxUserData => Path.Combine(gameOut, "userdata");
    string profileName = "";
    string gameOut = "", run = "", copy = "", exe = "", gameDll = "", bundle = "", shimOriginals = "", app = "";
    string? normal, hint, erlBackup, profileFolder;
    string[] erlBefore = [];
    FileStream? lease;
    HostBoundedProcess? job;
    Process? server, broker;
    nint lowToken;
    Thread? applier;
    DateTime? gameStarted;
    TextWriter? log;
    // Whether this session asked Steam itself (Ownership): its second check then does too.
    bool fullOwnershipCheckRan;

    string Shims => Path.Combine(app, "sdk-shims");
    string Scripts => Path.Combine(app, "scripts");
    string Cli => Path.Combine(app, "cli", "HellForge.Cli.exe");

    void Say(string line)
    {
        var text = $"{DateTime.Now:HH:mm:ss} {Privacy.Scrub(line)}";
        log?.WriteLine(text);
        log?.Flush();
    }

    // Scrubbed (Privacy): readers take only its status and messages from it. Written beside it and moved into place, so a
    // reader never sees half of it; a reader still holding it (the launcher polls each second) is waited out.
    void Save()
    {
        var path = Path.Combine(run, "result.json");
        File.WriteAllText(path + ".partial", Privacy.ScrubJson(result), new UTF8Encoding(false));
        for (var attempt = 1; ; attempt++)
            try { File.Move(path + ".partial", path, overwrite: true); return; }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException && attempt < 40) { Thread.Sleep(50); }
    }

    public int Run()
    {
        install = new InstallLayout(Path.GetFullPath(options.Install));
        if (!install.Exists) throw new SessionFailure("Not a HellForge installation: " + install.Root);
        // Without Local PlayFab the session clears the platform matchmaking selector, which points the game's own lobby
        // search at Steam's lobbies, where vanilla games list theirs under the same build string (docs/multiplayer.md step
        // 7). Multiplayer goes through the stand-ins and the backend's directory only.
        if (options.Multiplayer && !options.LocalPlayFab)
            throw new SessionFailure("Multiplayer needs Local PlayFab: turn on Experiments > Local PlayFab in the launcher and start again.");
        app = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, ".."));
        copy = install.Game;
        exe = Path.GetFullPath(Path.Combine(copy, "bin", "helldivers2.exe"));
        bundle = Path.Combine(copy, "data");
        gameDll = Path.GetFullPath(Path.Combine(bundle, "game", "game.dll"));
        shimOriginals = Path.Combine(install.Temp, "sdk-shim-originals");
        var temp = Directory.CreateDirectory(install.Temp).FullName;
        Environment.SetEnvironmentVariable("TEMP", temp);
        Environment.SetEnvironmentVariable("TMP", temp);
        var profileDirectory = Path.GetFullPath(options.Profile ?? Path.Combine(install.Root, "profiles", "default"));
        var runId = Guid.NewGuid().ToString("N");
        run = Directory.CreateDirectory(Path.Combine(install.Sessions, runId)).FullName;
        // Sandboxed, the game may write only into folders labelled low: its logs go to run/game, and the rest of the run
        // folder (this runner's files, the backend's evidence and the DLL it loads) stays out of its reach.
        gameOut = options.Sandbox ? LowIntegrityHelper.CreateLowFolder(Path.Combine(run, "game")) : run;
        log = new StreamWriter(Path.Combine(run, "session.log"), append: true, new UTF8Encoding(false));
        result["runId"] = runId;
        result["status"] = "preparing";
        result["gameLaunched"] = false;
        result["launchPrepared"] = false;
        result["ownershipConfirmed"] = false;
        result["startedUtc"] = DateTime.UtcNow.ToString("o");
        result["scope"] = "The copied game runs on this PC against the local backend; its traffic is blocked from the internet for the listed programs.";
        result["play"] = true;
        result["installRoot"] = install.Root;
        result["engine"] = typeof(PlaySession).Assembly.GetName().Version?.ToString();
        RecordFacts(options);
        File.WriteAllText(Path.Combine(install.Logs, "play-session.json"),
            new JsonObject { ["runId"] = runId, ["run"] = run, ["startedUtc"] = (string?)result["startedUtc"] }.ToJsonString());
        try
        {
            Prepare(profileDirectory);
            Play();
            result["status"] = "observed";
        }
        catch (Exception error) when (error is SessionFailure or IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception or InvalidOperationException or JsonException)
        {
            result["status"] = "failed";
            result["failure"] = error.Message;
            Say("Session stopped: " + error.Message);
        }
        finally { CleanUp(); }
        Say("Session result: " + Path.Combine(run, "result.json"));
        log.Dispose();
        return (string?)result["status"] == "observed" ? 0 : 1;
    }

    // The versions, the system and the settings, first in the log and kept for a crash report (CrashReport). Whatever goes
    // wrong collecting them, the session starts: the report then says they were not recorded.
    void RecordFacts(PlayOptions options)
    {
        try
        {
            var facts = SessionHeader.Facts(options, (string?)result["engine"] ?? "unknown");
            result["report"] = facts.ToJson();
            foreach (var line in facts.Lines()) Say(line);
        }
        catch (Exception error) { Say("Session facts not recorded: " + error.Message); }
    }

    // Holds a file of the installed game closed (no other handle, so it cannot start) into `into`; false when another
    // process has it open.
    static bool HoldClosed(string path, List<FileStream> into)
    {
        try { into.Add(new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.None)); return true; }
        catch (IOException) { return false; }
    }

    // Runs HellForge.Cli: its JSON output, or null (its messages go to the run's cli-output.txt).
    string? HellForgeCli(params string[] arguments)
    {
        var (exit, output, error) = Tools.Run(Cli, arguments, timeoutMs: 30 * 60 * 1000);
        File.AppendAllText(Path.Combine(run, "cli-output.txt"), Privacy.Scrub(error));
        return exit == 0 || (exit == 2 && arguments[0] == "prepare-session") ? output : null;
    }

    void Prepare(string profileDirectory)
    {
        CheckPreconditions();
        FindNormalGame();
        result["normalGame"] = normal;
        profileName = Path.GetFileName(profileDirectory);
        if (!System.Text.RegularExpressions.Regex.IsMatch(profileName, "^[a-z0-9-]{1,32}$")) throw new SessionFailure("The profile folder name is not a valid profile name.");
        RecoverStandIns();
        StageMods(profileDirectory);
        // The player's mods are held read-only so nothing the game runs can change or plant a mod.
        try { modsHold = ModsHold.Take(install.Mods); }
        catch (IOException error) { throw new SessionFailure(error.Message); }
        result["modsHeld"] = modsHold.Held;
        HoldGameFiles();
        PrepareSessionData(profileDirectory);
    }

    void CheckPreconditions()
    {
        if (!Environment.Is64BitProcess) throw new SessionFailure("The session runner must run as a 64-bit process.");
        // installations set up before setup checked for them, or a runtime uninstalled since
        if (VisualCppRuntimes.Problem("press Play again.") is { } runtimes) throw new SessionFailure(runtimes);
        if (GameProcesses.Blocking(install, GameProcesses.Running(), GameProcesses.MultiInstance) is { } busy) throw new SessionFailure(busy);
        result["multiInstance"] = GameProcesses.MultiInstance;
        if (Process.GetProcessesByName("HD2RuntimeModBuilder").Length > 0) throw new SessionFailure("External mod builder is running.");
        // One session at a time per installation: a second one (Play pressed again, or a session runner from an earlier session
        // that has not ended) finds the lock held.
        try { lease = new FileStream(Path.Combine(install.Temp, "session.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None); }
        catch (IOException) when (File.Exists(Path.Combine(install.Temp, "session.lock")))
        {
            throw new SessionFailure("Another HellForge session is still running for this installation. Wait for it to end, or close " +
                "HellForge.Session.exe in Task Manager (or restart the PC), then press Play again.");
        }
    }

    // The normal game, if Steam has one, is held closed during the session; find it in Steam's libraries.
    void FindNormalGame()
    {
        var steamPath = Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", null) as string;
        if (steamPath != null && HellForgeCli("discover", "--steam-root", steamPath) is { } manifests && Tools.ParseJson(manifests) is JsonArray found)
            foreach (var manifest in found)
            {
                var candidate = Path.Combine(Path.GetDirectoryName((string)manifest!)!, "common", "Helldivers 2");
                if (File.Exists(Path.Combine(candidate, "bin", "helldivers2.exe"))) { normal = candidate; break; }
            }
    }

    // A session that was cut short leaves the stand-ins in the copy: put the saved originals back first. Their pins
    // are the originals the stand-ins were built from; the verification below ties those to the build.
    void RecoverStandIns()
    {
        var build = Tools.ReadJson(Path.Combine(Shims, "build.json")) ?? throw new SessionFailure("The SDK stand-ins are missing; reinstall HellForge.");
        foreach (var name in ShimNames.Append(CloudName))
        {
            shimPins[name] = (string?)build["originals"]?[name] ?? throw new SessionFailure("The stand-ins' build record is incomplete; reinstall HellForge.");
            var target = Path.Combine(copy, "bin", name);
            var saved = Path.Combine(shimOriginals, name);
            if (Tools.Sha256(target) != shimPins[name] && File.Exists(saved) && Tools.Sha256(saved) == shimPins[name])
            {
                File.Copy(saved, target, overwrite: true);
                result["sdkShimOriginalsRecovered"] = true;
            }
        }
        var leftover = Path.Combine(copy, "bin", CloudReal);
        if (File.Exists(leftover)) File.Delete(leftover);
    }

    // Mods: stage data mods (and remove the last session's) before the copy's files are locked; --progress lets the
    // launcher show source mods being compiled.
    void StageMods(string profileDirectory)
    {
        var loaderPlan = Path.Combine(run, "modloader-plan.txt");
        // --modset: the gameplay mods with their content hashes, for the backend (mod sync); --sync: after a mod sync, the
        // host's mods instead of the player's own (ModSyncSession).
        // --premium-snapshot, --premium-locked: mods built on premium content follow the profile's account snapshot
        // (HellForge.Core PremiumMods); the locked ones' items stay out of the inventory (the backend's --premium-locked).
        string[] stage = ["mods-stage", "--install", install.Root, "--plan", loaderPlan, "--logs", gameOut, "--progress", Path.Combine(run, "mod-builds.json"),
            "--modset", ModSetFile, .. PremiumStageArguments(profileDirectory)];
        if (options.ModSync != null) stage = [.. stage, "--sync", Path.GetFullPath(options.ModSync)];
        var planText = HellForgeCli(stage);
        if (Tools.ParseJson(planText ?? "") is not JsonObject plan) throw new SessionFailure("Mods could not be loaded; see cli-output.txt in this session's log folder.");
        if ((bool?)plan["loaderRequired"] == true)
        {
            var loader = Path.Combine(Shims, "hellforge_modloader.dll");
            if (!File.Exists(loader)) throw new SessionFailure("The HellForge mod loader is missing; reinstall HellForge.");
            // The game inherits this environment (HostBoundedProcess).
            Environment.SetEnvironmentVariable("HELLFORGE_MODLOADER", Path.GetFullPath(loader));
            Environment.SetEnvironmentVariable("HELLFORGE_MODLOADER_PLAN", loaderPlan);
        }
        modDigest = (string?)plan["digest"] ?? "none";
        modList = (string?)plan["modlist"];
        modNative = (bool?)plan["native"] == true;
        nativeFolders = (plan["nativeFolders"] as JsonArray)?.Select(f => (string?)f).OfType<string>().ToList();
        var loadedMods = (plan["mods"] as JsonArray ?? []).Select(m => $"{(string?)m!["id"]} {(string?)m["version"]}").ToList();
        result["mods"] = new JsonObject
        {
            ["loaded"] = new JsonArray(loadedMods.Select(m => (JsonNode?)JsonValue.Create(m)).ToArray()),
            ["problems"] = plan["problems"]?.DeepClone() ?? new JsonArray(),
            ["stagedFiles"] = (plan["staged"] as JsonArray)?.Count ?? 0,
            ["codeMods"] = plan["codeMods"]?.DeepClone(),
            ["digest"] = modDigest,
            ["syncedWith"] = plan["syncedWith"]?.DeepClone()
        };
        if ((string?)plan["syncedWith"] is { } syncedWith)
        {
            Say($"Playing with {syncedWith}'s mods (mod sync).");
            if (options.ModSync != null) rejoinFile = Path.GetFullPath(options.ModSync);
        }
        SayMods(loadedMods);
        foreach (var problem in plan["problems"] as JsonArray ?? []) Say("Mod not loaded: " + (string?)problem);
    }

    // Every game file (the build's, and everything in data/: staged mods and the player's own patches) is held
    // read-only for the session, then verified against the supported build while data/settings.ini is pointed at
    // this profile's user data inside the installation. Other files in the copy are the player's (ReShade's settings,
    // presets and shaders) and stay writable; so does a steam_appid.txt left by a session that was cut short, which
    // Play removes (issue 6: held here, it could never be removed again).
    void HoldGameFiles()
    {
        var settingsPath = Path.GetFullPath(Path.Combine(copy, "data", "settings.ini"));
        var data = Path.GetFullPath(Path.Combine(copy, "data")) + Path.DirectorySeparatorChar;
        var gameFiles = SupportedBuild.Current.Files.Select(f => Path.GetFullPath(Path.Combine(copy, f.Path))).ToHashSet(StringComparer.OrdinalIgnoreCase);
        foreach (var file in Directory.EnumerateFiles(copy, "*", SearchOption.AllDirectories))
        {
            var full = Path.GetFullPath(file);
            if (full.Equals(settingsPath, StringComparison.OrdinalIgnoreCase) || full.EndsWith(".partial", StringComparison.OrdinalIgnoreCase)) continue;
            if (!gameFiles.Contains(full) && !full.StartsWith(data, StringComparison.OrdinalIgnoreCase)) continue;
            Tools.NoLinks(full);
            try { locks.Add(new FileStream(full, FileMode.Open, FileAccess.Read, FileShare.Read)); }
            catch (IOException error) when (error is not FileNotFoundException and not DirectoryNotFoundException)
            {
                throw new SessionFailure($"A game file is in use by another program: {Path.GetRelativePath(copy, full)}. Close that program " +
                    "(or restart the PC) and press Play again.");
            }
        }
    }

    // Seeds the profile's user data (a sandbox copy when sandboxed), verifies the supported build, locks settings.ini on
    // this profile, and records the copy's and the installed game's hashes.
    void PrepareSessionData(string profileDirectory)
    {
        // Sandboxed, the game's user data is a copy in gameOut, which carries the low label (SandboxFolders): the profile's
        // settings and saves and the shader cache go in now and come back once the game is gone.
        string[] prepare = ["prepare-session", "--install", install.Root, "--profile", profileName];
        if (options.Sandbox)
        {
            var copiedIn = SandboxFolders.Copy(install.UserData, SandboxUserData, SandboxFolders.UserDataParts(profileName));
            result["sandboxCopiedIn"] = new JsonObject { ["files"] = copiedIn.Files, ["bytes"] = copiedIn.Bytes, ["skipped"] = copiedIn.Skipped };
            prepare = [.. prepare, "--user-data", SandboxUserData];
        }
        var prepared = Tools.ParseJson(HellForgeCli(prepare) ?? "")
            ?? throw new SessionFailure("Session preparation failed; see cli-output.txt in this session's log folder.");
        if (prepared["problems"] is JsonArray { Count: > 0 } problems)
            throw new SessionFailure($"The game copy is not the supported build ({(string?)problems[0]}); repair it from the launcher.");
        var settingsPath = Path.GetFullPath(Path.Combine(copy, "data", "settings.ini"));
        locks.Add(new FileStream(settingsPath, FileMode.Open, FileAccess.Read, FileShare.Read));
        result["verifiedFiles"] = (prepared["gameFiles"] as JsonArray)?.Count ?? 0;
        result["appFilesHeld"] = HoldAppFiles();
        result["userData"] = prepared["profileData"]?.DeepClone();
        result["userDataSeeded"] = prepared["profileSeeded"]?.DeepClone();
        result["gameExeSha256"] = Tools.Sha256(exe);
        result["gameDllSha256"] = Tools.Sha256(gameDll);
        if (normal != null)
        {
            // Several installations: another installation's session may hold the installed game's executable closed
            // already, which keeps it from being read too.
            try { result["installedExeSha256"] = Tools.Sha256(Path.Combine(normal, "bin", "helldivers2.exe")); }
            catch (IOException) when (GameProcesses.MultiInstance) { result["installedExeHeldElsewhere"] = true; }
            result["installedDllSha256"] = Tools.Sha256(Path.Combine(normal, "data", "game", "game.dll"));
        }
        profileDirectoryUsed = profileDirectory;
    }

    // The session's mods, each with its version and content hash, for a bug report to say exactly which mods ran.
    void SayMods(List<string> loaded)
    {
        Say($"Mods: {loaded.Count} loaded, modlist digest {modDigest}");
        foreach (var line in SessionFacts.ModLines(loaded, SessionFiles.ReadJson(ModSetFile))) Say(line);
    }

    string modDigest = "none", profileDirectoryUsed = "";
    string? modList;
    // After a mod sync whose host's mods were all found: the sync file, which the backend uses to bring the game back into
    // the host's lobby (--rejoin, AutoRejoin).
    string? rejoinFile;
    // The session's mod set (mods-stage --modset), for the backend's mod sync.
    string ModSetFile => Path.Combine(run, "modset.json");

    // The mods whose items stay locked for this player (mods-stage --premium-locked), for the backend.
    string PremiumLockedFile => Path.Combine(run, "premium-locked.json");

    // mods-stage's premium check: the locked mods' file, and the profile's account snapshot when it has a place for one.
    string[] PremiumStageArguments(string profileDirectory) => InventorySync.SnapshotDirectoryFromProfileDir(profileDirectory) is { } snapshot
        ? ["--premium-locked", PremiumLockedFile, "--premium-snapshot", snapshot]
        : ["--premium-locked", PremiumLockedFile];

    // What asked the session to stop: "stop" (the launcher) or "mod-sync" (the backend, to restart with a host's mods).
    string ReadSignal()
    {
        try { return File.ReadAllText(Path.Combine(run, "stop-observation.signal")).Trim(); }
        catch (IOException) { return ""; }
    }
    bool modNative;
    List<string>? nativeFolders; // mods-stage's mods with network type edits (null: a mods-stage from before it)

    // HellForge's own programs (the installation's app folder: the launcher, this runner, the backend, the stand-ins the
    // copy loads, the scripts) are held read-only for the session like the game's files, so nothing the game runs can
    // replace them for the next start. A file that cannot be held stops the session. Only an installed runner does it
    // (a development build runs from elsewhere).
    int HoldAppFiles()
    {
        if (!Path.GetFullPath(app).TrimEnd('\\').Equals(Path.GetFullPath(install.App).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase)) return 0;
        var held = 0;
        foreach (var file in Directory.EnumerateFiles(install.App, "*", SearchOption.AllDirectories))
        {
            var full = Path.GetFullPath(file);
            Tools.NoLinks(full);
            try { locks.Add(new FileStream(full, FileMode.Open, FileAccess.Read, FileShare.Read)); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException)
            {
                throw new SessionFailure($"HellForge's file {Path.GetRelativePath(install.App, full)} could not be held read-only for the session ({error.Message}). Close what has it open and start again.");
            }
            held++;
        }
        return held;
    }

    // Network block (the readable scripts in app/scripts): $HellForgeInstallRoot names the installation's rule group.
    string BlockScript(string body) =>
        "$HellForgeInstallRoot=" + Tools.Quote(install.Root) + "; . " + Tools.Quote(Path.Combine(Scripts, "NetworkBlock.Common.ps1")) + "; " + body;

    string[] BlockProblems()
    {
        var (exit, output, error) = Tools.PowerShellCommand(BlockScript(
            "@(Test-HellForgeNetworkBlock (Get-HellForgeBlockedPrograms " + Tools.Quote(copy) + "))|ForEach-Object {$_}"));
        if (exit != 0) throw new SessionFailure("The network block could not be checked: " + error.Trim());
        return output.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
    }

    int BlockRuleCount()
    {
        var (exit, output, error) = Tools.PowerShellCommand(BlockScript(
            "@(Get-NetFirewallRule -PolicyStore ActiveStore -Group $HellForgeBlockGroup|Where-Object {$_.Enabled -eq 'True' -and $_.Action -eq 'Block'}).Count;" +
            "@(Get-NetFirewallProfile -PolicyStore ActiveStore|Where-Object Enabled -ne True).Count"));
        var lines = output.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        if (exit != 0 || lines.Length != 2 || !int.TryParse(lines[0], out var rules) || !int.TryParse(lines[1], out var off))
            throw new SessionFailure("The network block could not be read: " + error.Trim());
        return off > 0 ? -1 : rules;
    }

    // Places the WFP block for the backend's port alone (Install-WfpBlock.ps1, as setup does), with one Windows prompt; its
    // report goes into the result under reportKey. The port is the one the backend already runs on (install.BackendPort:
    // the computed one while the marker names none, else the marker's, which a moved folder keeps). Null when placed,
    // else why not.
    string? PlaceWfpBlock(string reason, string reportName, string reportKey, string message)
    {
        result["networkBlockInstallReason"] = reason;
        result["status"] = "awaiting_network_block_install";
        Save();
        Say(message);
        var report = Path.Combine(run, reportName);
        var port = new Uri((string)result["backendOrigin"]!).Port;
        var elevated = new ProcessStartInfo(Tools.PowerShell, $"-NoProfile -ExecutionPolicy Bypass -File \"{Path.Combine(Scripts, "Install-WfpBlock.ps1")}\" " +
            $"-InstallRoot \"{install.Root}\" -ReportPath \"{report}\" -BackendPort {port}")
            { UseShellExecute = true, Verb = "runas", WindowStyle = ProcessWindowStyle.Hidden };
        string? why = null;
        try { using var installer = Process.Start(elevated); installer?.WaitForExit(); }
        catch (System.ComponentModel.Win32Exception) { why = "Windows administrator consent was not given"; }
        if (Tools.ReadJson(report) is { } placed)
        {
            result[reportKey] = placed.DeepClone();
            if (why == null && (string?)placed["status"] != "installed") why = (string?)placed["failure"] ?? "the install did not finish";
        }
        else why ??= "the install wrote no report";
        result["status"] = "preparing";
        // placed is what the install reports and the marker says (the script writes it only once the block is in place):
        // the backend's port. A marker that named it before (a block placed again) proves nothing by itself.
        return why == null && install.BlockedBackendPort() == port ? null : why ?? "the placed block names another port";
    }

    // Wine (Proton): its ntdll exports wine_get_version, Windows' does not.
    static bool UnderWine()
    {
        try { return System.Runtime.InteropServices.NativeLibrary.TryLoad("ntdll.dll", out var ntdll) && System.Runtime.InteropServices.NativeLibrary.TryGetExport(ntdll, "wine_get_version", out _); }
        catch (Exception error) when (error is DllNotFoundException or BadImageFormatException or EntryPointNotFoundException) { return false; }
    }

    // The network block's probe (app/scripts/OfflineControlNetworkProbe.cs, NetworkProbe.exe), in this process: loopback
    // HTTP reaches the backend; a TCP connect to a documentation address (192.0.2.1, 2001:db8::1) is refused with
    // WSAEACCES (10013), as the block refuses it. Same report, same exit code.
    internal static int NetworkProbeInProcess(int port, string report)
    {
        static int External(System.Net.Sockets.AddressFamily family, string address)
        {
            using var socket = new System.Net.Sockets.Socket(family, System.Net.Sockets.SocketType.Stream, System.Net.Sockets.ProtocolType.Tcp);
            try { return socket.ConnectAsync(System.Net.IPAddress.Parse(address), 443).Wait(2500) ? 0 : -1; }
            catch (AggregateException error) { return error.GetBaseException() is System.Net.Sockets.SocketException s ? s.ErrorCode : -2; }
            catch (System.Net.Sockets.SocketException error) { return error.ErrorCode; }
        }
        var local = false;
        try
        {
            using var client = new System.Net.Sockets.TcpClient(System.Net.Sockets.AddressFamily.InterNetwork);
            if (client.ConnectAsync(System.Net.IPAddress.Loopback, port).Wait(2500))
            {
                client.ReceiveTimeout = 2500; client.SendTimeout = 2500;
                using var stream = client.GetStream();
                var request = Encoding.ASCII.GetBytes("GET /config/locator.json HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
                stream.Write(request, 0, request.Length);
                var buffer = new byte[2048];
                var read = stream.Read(buffer, 0, buffer.Length);
                local = Encoding.ASCII.GetString(buffer, 0, read).StartsWith("HTTP/1.1 200 ", StringComparison.Ordinal);
            }
        }
        catch (Exception error) when (error is System.Net.Sockets.SocketException or IOException or AggregateException) { }
        var v4 = External(System.Net.Sockets.AddressFamily.InterNetwork, "192.0.2.1");
        var v6 = External(System.Net.Sockets.AddressFamily.InterNetworkV6, "2001:db8::1");
        File.WriteAllText(report, "{\"loopbackHttpPassed\":" + (local ? "true" : "false") + ",\"externalIpv4TcpError\":" + v4 + ",\"externalIpv6TcpError\":" + v6 + "}");
        return local && v4 == 10013 ? 0 : 1;
    }

    // The Steam ownership check, against the given steam_api64.dll. A recent passed check stands in for it while
    // SteamOwnershipRecord.Problem allows (the same installation, Steam account and game build, within its limit), unless
    // the session was asked for the full check (--full-ownership-check); once a session has run the full check, it runs it
    // again for the second check too, as before. Either way the session's result says which it was.
    void Ownership(string steamApi, string evidence)
    {
        if (!options.FullOwnershipCheck && !fullOwnershipCheckRan)
        {
            var (record, why) = SteamOwnershipRecord.Reusable(install);
            if (why == null)
            {
                result["ownershipCheck"] = "recent";
                result["ownershipConfirmedUtc"] = record!.ConfirmedUtc.ToString("o");
                Say($"Steam ownership: confirmed {record.ConfirmedUtc:yyyy-MM-dd HH:mm} UTC for this account and build; not asking Steam again.");
                return;
            }
            Say("Steam ownership: asking Steam (" + why + ").");
        }
        fullOwnershipCheckRan = true;
        result["ownershipCheck"] = "full";
        var started = DateTime.UtcNow;
        try { FullOwnershipCheck(steamApi, evidence, started); }
        catch (SessionFailure)
        {
            SteamOwnershipRecord.AfterCheck(install, evidence, passed: false, started);
            throw;
        }
        if (!SteamOwnershipRecord.AfterCheck(install, evidence, passed: true, started))
            Say("Steam ownership: confirmed, but not recorded for later sessions.");
    }

    // The full check: app/scripts/Test-SteamOwnership.ps1 on Windows, the probe itself under Wine (whose powershell.exe
    // runs nothing and returns 0). Either way the evidence it writes decides (SteamOwnershipCheck.Problem): a check that ran
    // nothing passes nothing.
    void FullOwnershipCheck(string steamApi, string evidence, DateTime started)
    {
        if (File.Exists(evidence)) File.Delete(evidence);
        if (LinuxNetworkBlock.UnderWine)
        {
            int exit;
            try
            {
                exit = SteamOwnershipCheck.RunProbe(Path.Combine(app, "steamprobe", "HellForge.SteamProbe.exe"), steamApi,
                    Path.Combine(install.Temp, "ownership"), evidence, TimeSpan.FromSeconds(15));
            }
            catch (InvalidDataException error) { throw new SessionFailure("Steam ownership was not confirmed: " + error.Message); }
            if (exit != 0) throw new SessionFailure("Steam ownership was not confirmed (the probe exited with " + exit + ").");
        }
        else
        {
            var (exit, _, error) = Tools.Run(Tools.PowerShell, ["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File",
                Path.Combine(Scripts, "Test-SteamOwnership.ps1"), "-NoBuild", "-PassThru", "-SteamApi", steamApi,
                "-WorkRoot", Path.Combine(install.Temp, "ownership"), "-EvidencePath", evidence], timeoutMs: 60000);
            if (exit != 0) throw new SessionFailure("Steam ownership was not confirmed: " + error.Trim());
        }
        if (SteamOwnershipCheck.Problem(evidence, started) is { } problem) throw new SessionFailure("Steam ownership was not confirmed: " + problem + ".");
    }

    void Play()
    {
        BackUpGameGuard();
        var build = Tools.ReadJson(Path.Combine(Shims, "build.json"))!;
        result["sdkShims"] = options.Multiplayer;
        if (options.Multiplayer) SwapInShims(build);
        Ownership(Path.Combine(copy, "bin", CloudName), Path.Combine(run, "ownership.json"));
        result["ownershipConfirmed"] = true;
        var realTarget = SwapInCloud(build);
        var authorization = Stopwatch.StartNew();
        HoldInstalledGame();
        WriteAppIdHint();
        StartBackend(realTarget);
        result["steamOnline"] = true;
        var blockRules = SetUpNetworkBlock(out var linuxNamespace, out var wfpOnly);
        ProveNetworkBlock(linuxNamespace, wfpOnly);
        // Confirm genuine Steam sign-in and ownership with the game's rules active.
        Ownership(realTarget, Path.Combine(run, "ownership-with-firewall.json"));
        result["steamOwnershipWithRules"] = true;
        if (authorization.Elapsed.TotalSeconds >= 120) throw new SessionFailure("Ownership authorization expired.");
        var requestLog = PrepareProfileFolder();
        LaunchGame();
        var playFab = ApplyLaunchEdits();
        // The local backend enables from the genuine sign-in; kept aligned each second below. With the local PlayFab
        // stand-in the sign-in completes on loopback, so this is left as it is.
        var clearMatchmaking = !options.LocalPlayFab;
        result["status"] = "observing";
        Save();
        Say($"The game is running against the local backend (PID {job!.Process.Id}).");
        Observe(playFab, clearMatchmaking, requestLog, blockRules);
    }

    // Only copies made before HellForge stopped copying the GameGuard files have this folder; without it there are no
    // logs to keep.
    void BackUpGameGuard()
    {
        var guardFolder = Path.Combine(copy, "bin", "GameGuard");
        if (Directory.Exists(guardFolder))
        {
            erlBackup = Directory.CreateDirectory(Path.Combine(run, "gameguard-originals")).FullName;
            erlBefore = Directory.GetFiles(guardFolder, "*.erl").Select(Path.GetFileName).ToArray()!;
            foreach (var name in erlBefore) File.Copy(Path.Combine(guardFolder, name), Path.Combine(erlBackup, name));
        }
    }

    void SwapInShims(JsonNode build)
    {
        Directory.CreateDirectory(shimOriginals);
        var hashes = new JsonObject();
        foreach (var name in ShimNames)
        {
            if ((string?)build["originals"]![name] != shimPins[name]) throw new SessionFailure($"The SDK stand-ins were built for a different {name}; reinstall HellForge.");
            var standIn = Path.Combine(Shims, name);
            var standInPin = (string)build["standIns"]![name]!;
            if (Tools.Sha256(standIn) != standInPin) throw new SessionFailure("Stand-in changed since it was built: " + name);
            SwapIn(name, standIn, standInPin);
            hashes[name] = standInPin;
        }
        result["sdkShimHashes"] = hashes;
        // The game inherits this environment (HostBoundedProcess).
        Environment.SetEnvironmentVariable("HELLFORGE_SHIM_LOG_DIR", Directory.CreateDirectory(Path.Combine(gameOut, "sdk-shims")).FullName);
    }

    // Steam Cloud stays in the profile: the steam_api64.dll stand-in, with the verified original next to it. No session
    // runs without it, so the copy never writes the Steam account's cloud files. Sandboxed, the bridge build instead
    // (docs/steam-bridge.md): no export forwards, so Valve's DLL, still placed beside it for the broker to load, never
    // loads in the game. Returns the path where the verified original is kept (real).
    string SwapInCloud(JsonNode build)
    {
        var cloudStandIn = Path.Combine(Shims, options.Sandbox ? "steam_api64_bridge.dll" : CloudName);
        var cloudPin = options.Sandbox ? (string?)build["steamBridge"] : (string?)build["standIns"]?[CloudName];
        if (options.Sandbox && cloudPin == null)
            throw new SessionFailure("This HellForge was built without the Steam bridge (it needs the Steamworks SDK), so a sandboxed session cannot start.");
        if (cloudPin == null || !File.Exists(cloudStandIn)) throw new SessionFailure("The Steam Cloud stand-in is missing; reinstall HellForge.");
        if (Tools.Sha256(cloudStandIn) != cloudPin) throw new SessionFailure("Stand-in changed since it was built: " + Path.GetFileName(cloudStandIn));
        result["steamBridge"] = options.Sandbox;
        var realTarget = Path.GetFullPath(Path.Combine(copy, "bin", CloudReal));
        if (Tools.Sha256(Path.Combine(copy, "bin", CloudName)) != shimPins[CloudName]) throw new SessionFailure("The copy's steam_api64.dll is not the verified original.");
        SwapIn(CloudName, cloudStandIn, cloudPin, realTarget);
        // A new profile's cloud folder starts with the player's control bindings (read from the signed-in account's Steam
        // Cloud folder, never written); the save itself starts empty, so the profile has its own loadout.
        var cloudDirectory = Path.Combine((string?)result["userData"] ?? install.UserData, "cloud");
        var cloudSeeded = false;
        if (!Directory.Exists(cloudDirectory))
        {
            Directory.CreateDirectory(cloudDirectory);
            var steamRoot = Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", null) as string;
            var steamUser = Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam\ActiveProcess", "ActiveUser", null);
            if (steamRoot != null && steamUser != null)
            {
                var controls = Path.Combine(steamRoot, "userdata", steamUser.ToString()!, "553850", "remote", "input_settings.config");
                if (File.Exists(controls)) { File.Copy(controls, Path.Combine(cloudDirectory, "input_settings.config")); cloudSeeded = true; }
            }
        }
        Environment.SetEnvironmentVariable("HELLFORGE_CLOUD_DIR", Path.GetFullPath(cloudDirectory));
        Environment.SetEnvironmentVariable("HELLFORGE_CLOUD_LOG", Path.Combine(gameOut, "steam-cloud.log"));
        // The network gate's census (the stand-in's net_gate.c): every connection, name lookup and program the copy
        // attempts, logged beside the firewall, which still blocks them; summarised into the result at the end.
        Environment.SetEnvironmentVariable("HELLFORGE_NETGATE", "census");
        Environment.SetEnvironmentVariable("HELLFORGE_NETGATE_LOG", Path.Combine(gameOut, CensusLog));
        result["steamCloud"] = new JsonObject { ["directory"] = Path.GetFullPath(cloudDirectory), ["standIn"] = cloudPin, ["controlsSeeded"] = cloudSeeded };
        return realTarget;
    }

    // No fallback launch of the installed game during the session: its executable and GameGuard programs are held
    // closed. No firewall rule ever lists the installed game; a lock ends with the session even when it is cut short.
    void HoldInstalledGame()
    {
        if (normal == null) return;
        var programs = new List<string> { Path.Combine(normal, "bin", "helldivers2.exe") };
        programs.AddRange(Directory.EnumerateFiles(Path.Combine(normal, "bin"), "*", SearchOption.AllDirectories)
            .Where(f => (f.EndsWith(".des", StringComparison.OrdinalIgnoreCase) || f.EndsWith(".now", StringComparison.OrdinalIgnoreCase)) &&
                        f.Contains("GameGuard", StringComparison.OrdinalIgnoreCase)));
        var heldElsewhere = programs.Where(path => !HoldClosed(path, locks)).ToList();
        if (heldElsewhere.Count > 0 && !GameProcesses.MultiInstance)
            throw new SessionFailure("Could not hold the installed game closed (is it running?): " + heldElsewhere[0]);
        // Several installations: the first session holds them, and this one takes the hold over once that one ends
        // (its game was refused above, so a held file here is another installation's session's).
        if (heldElsewhere.Count > 0)
        {
            result["installedGameHeldElsewhere"] = heldElsewhere.Count;
            normalHoldRetry = new Timer(_ => { lock (retaken) if (!retakeStopped) heldElsewhere.RemoveAll(path => HoldClosed(path, retaken)); }, null, 5000, 5000);
        }
    }

    void WriteAppIdHint()
    {
        hint = Path.Combine(copy, "bin", "steam_appid.txt");
        // Not a game file: only a session writes it, so one here was left by a session that was cut short.
        if (File.Exists(hint)) { File.Delete(hint); result["staleAppIdHintRemoved"] = true; }
        File.WriteAllText(hint, "553850\n", Encoding.ASCII);
    }

    // Setup places the block as WFP filters (hellforge_wfp) and drops a marker; with it (or HELLFORGE_WFP_ONLY for
    // tests) the Windows Firewall rules are not required or read. An installation without the marker falls back to the
    // firewall rules, installed by InstallPersistentBlock. The probe (ProveNetworkBlock) proves the block either way.
    // Linux (Wine/Proton, docs/linux.md): the block is a network namespace with loopback only; its session must run
    // inside that namespace (HELLFORGE_LINUX_NETNS), told by Wine itself or the marker. Returns the firewall rule count
    // (0 when there are no rules to re-read).
    int SetUpNetworkBlock(out bool linuxNamespace, out bool wfpOnly)
    {
        var marker = Path.Combine(install.Root, "network-block", "wfp-block.json");
        wfpOnly = Environment.GetEnvironmentVariable("HELLFORGE_WFP_ONLY") == "1" || File.Exists(marker);
        linuxNamespace = Environment.GetEnvironmentVariable("HELLFORGE_LINUX_NETNS") == "1";
        if (!linuxNamespace && (UnderWine() || (File.Exists(marker) && File.ReadAllText(marker).Contains("linux-netns", StringComparison.Ordinal))))
            throw new SessionFailure("On Linux, HellForge's game runs only in its network namespace: start HellForge with linux/hellforge-linux.sh.");
        if (!linuxNamespace && !wfpOnly && options.Sandbox) wfpOnly = SwitchToWfpBlock();
        if (linuxNamespace) { result["networkBlock"] = "linux-netns"; return 0; }
        if (wfpOnly)
        {
            result["networkBlock"] = "wfp";
            // A block placed before it was narrowed to the backend's port still permits every loopback port: it is placed
            // again for the backend's port alone.
            result["networkBlockNarrowed"] = !File.Exists(marker) || install.BlockedBackendPort() != null || NarrowWfpBlock();
            return 0;
        }
        return InstallPersistentBlock();
    }

    // Windows Firewall never filters loopback, so with its rules the game reaches every program on this PC. A sandboxed
    // session on an installation that still has them (set up before the WFP block, as 0.2.0 did) places the WFP block
    // first, with one Windows prompt; the old rules stay and only block more. True once it is placed.
    bool SwitchToWfpBlock()
    {
        var why = PlaceWfpBlock("the installation has the older Windows Firewall rules, which a sandboxed session cannot use", "network-block-wfp.json",
            "networkBlockSwitch", "The sandbox needs HellForge's newer network block, which lets the game reach HellForge's backend alone: placing it now. " +
            "Windows will ask for administrator consent once.");
        if (why != null)
            throw new SessionFailure("A sandboxed session needs the WFP network block, and it could not be placed (" + why +
                "). Run Settings > Installation, or turn off Settings > Sandbox.");
        result["networkBlockSwitched"] = true;
        return true;
    }

    // The WFP block placed again for the backend's port alone: whether it now is. A sandboxed game must not reach other
    // programs on this PC; otherwise the session goes on as before.
    bool NarrowWfpBlock()
    {
        var why = PlaceWfpBlock("the block predates its narrowing to the backend's port", "network-block-narrow.json", "networkBlockNarrow",
            "The network block still lets the game reach every program on this PC: placing it again for HellForge's backend alone. " +
            "Windows will ask for administrator consent once.");
        if (why == null) return true;
        if (options.Sandbox)
            throw new SessionFailure("A sandboxed session needs the network block narrowed to HellForge's backend, and it could not be placed again (" + why +
                "). Run Settings > Installation, or turn off Settings > Sandbox.");
        Say("The network block could not be narrowed (" + why + "): the game can still reach every program on this PC. Run Settings > Installation to narrow it.");
        return false;
    }

    int InstallPersistentBlock()
    {
        result["networkBlock"] = "persistent";
        var blockProblems = BlockProblems();
        if (blockProblems.Length > 0)
        {
            // First session, or the copy changed since the install: one Windows prompt, then never again.
            result["networkBlockInstallReason"] = blockProblems[0];
            result["status"] = "awaiting_network_block_install";
            Save();
            Say($"Network block needs a one-time install ({blockProblems[0]}). Windows will ask for administrator consent once.");
            var report = Path.Combine(run, "network-block-install.json");
            var elevated = new ProcessStartInfo(Tools.PowerShell, $"-NoProfile -ExecutionPolicy Bypass -File \"{Path.Combine(Scripts, "Install-NetworkBlock.ps1")}\" " +
                $"-ReportPath \"{report}\" -InstallRoot \"{install.Root}\"") { UseShellExecute = true, Verb = "runas", WindowStyle = ProcessWindowStyle.Hidden };
            try { using var installer = Process.Start(elevated); installer?.WaitForExit(); }
            catch (System.ComponentModel.Win32Exception) { throw new SessionFailure("Windows administrator consent was not given for the network block."); }
            if (Tools.ReadJson(report) is { } installed) result["networkBlockInstall"] = installed;
            blockProblems = BlockProblems();
            if (blockProblems.Length > 0) throw new SessionFailure("Network block is not in place: " + blockProblems[0]);
        }
        var blockRules = BlockRuleCount();
        if (blockRules < 0) throw new SessionFailure("Windows Firewall is off for a network type.");
        result["networkBlockRules"] = blockRules;
        return blockRules;
    }

    // The block's probe: loopback reaches the backend, the outside does not (WSAEACCES). A narrowed block is proved
    // narrow too (another loopback port is refused). Linux: the same probe in this process (NetworkProbe.exe is a .NET
    // Framework program, which a Wine prefix runs only with Wine Mono installed).
    // A WFP block that no longer holds is placed again, once, with one Windows prompt. Its filters name each program by
    // its volume's device path (\device\harddiskvolumeN), which for a drive other than Windows' own can change between
    // boots (a USB drive or another disk plugged in); a moved folder leaves them behind. Either way the marker still
    // says the block is there and the probe's outside connect just waits (0.2.0 tickets: installs on D: and E:, the
    // block proved at install and gone days later, a fresh install the only fix). Placing it again proves it holds, or
    // says what lets traffic past it (a VPN, a proxy, a security suite).
    void ProveNetworkBlock(bool linuxNamespace, bool wfpOnly)
    {
        var probe = Path.Combine(install.Root, "network-block", "NetworkProbe.exe");
        // Made by the network block's install; antivirus programs sometimes remove it.
        if (!linuxNamespace && !File.Exists(probe))
            throw new SessionFailure("The network block's check program is missing (" + probe + "). Antivirus programs sometimes remove it: " +
                "restore it from quarantine and exclude the HellForge folder, or run Settings > Installation again to put the block back.");
        var problem = RunNetworkProbe(probe, linuxNamespace, wfpOnly, "network-probe.json");
        if (problem == null) return;
        if (linuxNamespace) throw new SessionFailure($"The network namespace is not holding: {problem}.");
        if (!wfpOnly) throw new SessionFailure($"The network block is not holding: {problem}. Run Settings > Installation to place it again.");
        result["networkControlBefore"] = result["networkControl"]?.DeepClone();
        var why = PlaceWfpBlock("the block's check failed: " + problem, "network-block-repair.json", "networkBlockRepair",
            $"The network block is not holding ({problem}): placing it again. Windows will ask for administrator consent once.");
        if (why == null && (problem = RunNetworkProbe(probe, false, true, "network-probe-again.json")) == null)
        {
            result["networkBlockRepaired"] = true;
            Say("The network block was placed again and holds.");
            return;
        }
        throw new SessionFailure(why != null
            ? $"The network block is not holding ({problem}), and placing it again did not work: {why}. Press Play to try again, or run Settings > Installation."
            : $"The network block is not holding ({problem}), even placed again. A VPN, a proxy or a security program may be letting the game's traffic past it: turn it off or exclude the HellForge folder, then press Play again.");
    }

    // One run of the probe into reportName; the result's networkControl is what it saw. Null when the block holds, else
    // what is wrong (NetworkBlockProbe.Problem).
    string? RunNetworkProbe(string probe, bool linuxNamespace, bool wfpOnly, string reportName)
    {
        var networkReport = Path.Combine(run, reportName);
        var origin = new Uri((string)result["backendOrigin"]!);
        var otherPort = wfpOnly && install.BlockedBackendPort() != null ? (origin.Port + 1).ToString() : null;
        var (probeExit, _, _) = linuxNamespace ? (NetworkProbeInProcess(origin.Port, networkReport), "", "")
            : Tools.Run(probe, otherPort == null ? [origin.Port.ToString(), networkReport] : [origin.Port.ToString(), networkReport, otherPort], timeoutMs: 10000);
        var network = Tools.ReadJson(networkReport);
        result["networkControl"] = network?.DeepClone();
        return NetworkBlockProbe.Problem(network, otherPort != null) ?? (probeExit != 0 ? $"the network block's check ended with exit code {probeExit}" : null);
    }

    // The profile's environment folders (a sandbox copy when sandboxed), and the pre-launch backend request count.
    // Returns the backend's request log.
    string PrepareProfileFolder()
    {
        profileFolder = Path.Combine(install.UserData, "environment");
        if (options.Sandbox)
        {
            // the game's AppData and temp: a copy in gameOut too, back at the end
            SandboxFolders.Copy(install.UserData, gameOut, ["environment"]);
            profileFolder = Path.Combine(gameOut, "environment");
        }
        foreach (var folder in new[] { "Roaming", "Local", "Temp" }) Directory.CreateDirectory(Path.Combine(profileFolder, folder));
        var requestLog = Path.Combine(run, "backend", "requests.jsonl");
        long requestOffset = 0;
        result["preLaunchBackendRequests"] = Tools.BackendRequests(requestLog, ref requestOffset).Count;
        return requestLog;
    }

    // Starts the copied game in its job (low-integrity and brokered when sandboxed), then resumes it after the status is
    // saved.
    void LaunchGame()
    {
        // The engine's own --log-to-file switch (its console log); whatever .log files it writes are copied at the end.
        gameStarted = DateTime.UtcNow;
        result["engineLog"] = options.EngineLog;
        // then the player's launch options (Steam's Launch Options, such as --use-d3d11), after the session's own
        var gameOptions = HellForge.Core.GameLaunchOptions.CommandLine(options.GameOptions);
        result["gameOptions"] = gameOptions;
        // The game may not start other programs: its job holds it alone. That costs the game's crash reporter (already
        // blocked from the network) and the DLSS updater Steam's overlay starts. The Crash dump diagnostic needs a child
        // (the loader starts HellForge.Cli to write the dump), and HELLFORGE_ALLOW_CHILD_PROCESSES=1 lifts it to diagnose.
        var childReason = !string.IsNullOrEmpty(Environment.GetEnvironmentVariable("HELLFORGE_CRASH_DUMP")) ? "crash dump"
            : Environment.GetEnvironmentVariable("HELLFORGE_ALLOW_CHILD_PROCESSES") == "1" ? "HELLFORGE_ALLOW_CHILD_PROCESSES" : null;
        result["childProcesses"] = childReason == null ? "blocked" : "allowed (" + childReason + ")";
        // Low-integrity sandbox (docs/steam-bridge.md phase 4): the game runs at low mandatory integrity and cannot
        // write to the player's files, registry or other processes. A broker at normal integrity forwards Steam API
        // calls over a named pipe and pushes callbacks through a shared-memory ring.
        string? bridgeSuffix = null;
        if (options.Sandbox)
        {
            bridgeSuffix = (string)result["runId"]!;
            Environment.SetEnvironmentVariable("HELLFORGE_BRIDGE_PIPE", bridgeSuffix);
            // Everything the game writes is in gameOut, made with a low label that its folders inherit: its logs, its
            // environment and user data (copies), and the editor's saves (gameOut/sandbox-stats, applied after the game is
            // gone: SandboxStats).
            var sandboxStats = Directory.CreateDirectory(Path.Combine(gameOut, "sandbox-stats")).FullName;
            Environment.SetEnvironmentVariable("HELLFORGE_SANDBOX_STATS", sandboxStats);
            lowToken = LowIntegrityHelper.CreateLowToken();
            result["sandbox"] = true;
        }
        job = new HostBoundedProcess(exe, $"--bundle-dir \"{bundle}\" --release" + (options.EngineLog ? " --log-to-file" : "") + (gameOptions.Length > 0 ? " " + gameOptions : ""),
            Path.Combine(copy, "bin"), profileFolder, 0, singleProcess: childReason == null, token: lowToken);
        result["gamePid"] = job.Process.Id;
        result["gameLaunched"] = true;
        if (options.Sandbox) StartBroker(bridgeSuffix!);
        result["status"] = "patching";
        Save();
        job.Resume();
    }

    void StartBroker(string bridgeSuffix)
    {
        var brokerExe = Path.Combine(Shims, "hellforge_steam_broker.exe");
        // Pinned like the stand-ins: the broker runs at the player's integrity and loads Valve's DLL for the game.
        var brokerPin = (string?)Tools.ReadJson(Path.Combine(Shims, "build.json"))?["steamBroker"];
        if (!File.Exists(brokerExe) || brokerPin == null) throw new SessionFailure("The Steam broker is missing; reinstall HellForge.");
        if (!Tools.Sha256(brokerExe).Equals(brokerPin, StringComparison.OrdinalIgnoreCase)) throw new SessionFailure("The Steam broker changed since it was built; reinstall HellForge.");
        var brokerStart = new ProcessStartInfo(brokerExe) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardError = true };
        brokerStart.ArgumentList.Add(bridgeSuffix);
        brokerStart.ArgumentList.Add(job!.Process.Id.ToString());
        brokerStart.ArgumentList.Add(Path.GetFullPath(Path.Combine(copy, "bin", CloudReal)));
        // Play with strangers, which the broker reads as the backend does: off, the game's Public Steam lobby is Friends only
        brokerStart.ArgumentList.Add(Path.GetFullPath(install.Mods));
        // the backend's co-op peers (MultiplayerHost.PeersFile), the only Steam users the game's P2P reaches: in the
        // backend's folder, which the low-integrity game cannot write
        brokerStart.ArgumentList.Add(Path.GetFullPath(Path.Combine(run, "backend", "p2p-peers.txt")));
        broker = Process.Start(brokerStart) ?? throw new SessionFailure("The Steam broker did not start.");
        var brokerLog = new StreamWriter(Path.Combine(run, "steam-broker.log")) { AutoFlush = true };
        broker.ErrorDataReceived += (_, e) => { if (e.Data != null) lock (brokerLog) brokerLog.WriteLine(e.Data); };
        broker.BeginErrorReadLine();
        result["brokerPid"] = broker.Id;
    }

    // The memory-only launch edits on the running game: the PlayFab redirect, the native mod applier, the early
    // constructor edit and the backend routing edit. Returns the PlayFab redirect state, which Observe records.
    HellForgeLocalRoutingPatch.PlayFabRedirect? ApplyLaunchEdits()
    {
        // Opt-in: the engine's PlayFab web API base URL is redirected (heap data only) to the backend's local PlayFab
        // stand-in; started before the slow edits so the URL is replaced right after the engine formats it, however long
        // the game takes to get there.
        var playFab = options.LocalPlayFab ? HellForgeLocalRoutingPatch.StartPlayFabRedirect(job!.Process.Id, exe, (string)result["backendOrigin"]! + "/playfab", -1) : null;
        if (modNative)
        {
            var pid = job!.Process.Id;
            applier = new Thread(() =>
            {
                try { NativeModApplier.Run(install, nativeFolders, pid, exe, Path.Combine(run, "native-mods.log"), Path.Combine(run, "native-mods.json")); }
                catch (Exception error) when (error is IOException or System.ComponentModel.Win32Exception or InvalidOperationException) { }
            }) { IsBackground = true, Name = "native mods" };
            applier.Start();
        }
        result["localPlayFab"] = options.LocalPlayFab;
        ApplyEarlyConstructor();
        ApplyRouting();
        result["launchPrepared"] = true;
        return playFab;
    }

    // The memory-only initialization edit: the game runs without its constructor and main check (HellForgeHostEarlyConstructorExperiment).
    void ApplyEarlyConstructor()
    {
        HellForgeHostEarlyConstructorExperiment.TargetExe = exe;
        HellForgeHostEarlyResult experiment;
        try { experiment = HellForgeHostEarlyConstructorExperiment.ApplySteamGame(job!.Process.Id, job.Process.StartTime.Ticks, "SkipConstructorAndMainCheck"); }
        catch (Exception error) when (error is InvalidOperationException or System.ComponentModel.Win32Exception)
        { throw new SessionFailure("The game could not be prepared for the local backend." + (Debug ? " " + error.Message : "")); }
        if (Debug) File.WriteAllText(Path.Combine(run, "launch-step-1.json"), JsonSerializer.Serialize(experiment, SessionJson.Default.HellForgeHostEarlyResult));
        var startupReady = experiment.Written && experiment.MainCheckWritten && experiment.LaterStatusWritten && experiment.ReadbackMatched;
        if (experiment.Outcome != "applied_at_candidate_boundary" || !startupReady)
            throw new SessionFailure("The game could not be prepared for the local backend" + ExitNote() + ".");
    }

    // The memory-only routing edit: the game's backend selection points at the loopback backend (HellForgeLocalRoutingPatch).
    void ApplyRouting()
    {
        HellForgeLocalRoutingResult routing;
        try { routing = HellForgeLocalRoutingPatch.Apply(job!.Process.Id, job.Process.StartTime.Ticks, exe, gameDll, (string)result["backendOrigin"]!, 45000, false); }
        catch (Exception error) when (error is InvalidOperationException or ArgumentException or System.ComponentModel.Win32Exception)
        { throw new SessionFailure("The game could not be connected to the local backend." + (Debug ? " " + error.Message : "")); }
        if (Debug) File.WriteAllText(Path.Combine(run, "launch-step-2.json"), JsonSerializer.Serialize(routing, SessionJson.Default.HellForgeLocalRoutingResult));
        var backendReady = routing.Outcome == "applied" && routing.ReadbackMatched;
        if (!backendReady) throw new SessionFailure("The game could not be connected to the local backend." + (Debug ? " (" + routing.Outcome + ")" : ""));
    }

    // The game sometimes quits before the launch is prepared; its exit code names why, for the player (" (...)", or "").
    string ExitNote()
    {
        try
        {
            job!.Process.Refresh();
            if (job.Process.HasExited)
            {
                var code = job.Process.ExitCode;
                result["gameExitCode"] = $"0x{code:X8}";
                var meaning = Tools.ExitMeaning(code);
                return $" (the game exited with 0x{code:X8}{(meaning.Length > 0 ? ": " + meaning : "")})";
            }
        }
        catch (InvalidOperationException) { }
        return "";
    }

    // The verified original is held open read-only: released, saved, the stand-in put in its place and held the same way.
    // The Steam stand-in keeps the original next to it (real), where it forwards Steam's API.
    void SwapIn(string name, string standIn, string standInPin, string? real = null)
    {
        var target = Path.GetFullPath(Path.Combine(copy, "bin", name));
        if (Tools.Sha256(target) != shimPins[name]) throw new SessionFailure($"The SDK stand-ins were built for a different {name}; reinstall HellForge.");
        foreach (var held in locks.Where(l => string.Equals(l.Name, target, StringComparison.OrdinalIgnoreCase)).ToList()) { held.Dispose(); locks.Remove(held); }
        Directory.CreateDirectory(shimOriginals);
        var saved = Path.Combine(shimOriginals, name);
        File.Copy(target, saved, overwrite: true);
        if (Tools.Sha256(saved) != shimPins[name]) throw new SessionFailure("Could not save the original " + name);
        swapped.Add(name);
        if (real != null)
        {
            File.Copy(target, real, overwrite: true);
            locks.Add(new FileStream(real, FileMode.Open, FileAccess.Read, FileShare.Read));
            if (Tools.Sha256(real) != shimPins[name]) throw new SessionFailure("Could not place the original " + name);
        }
        File.Copy(standIn, target, overwrite: true);
        locks.Add(new FileStream(target, FileMode.Open, FileAccess.Read, FileShare.Read));
        if (Tools.Sha256(target) != standInPin) throw new SessionFailure("Stand-in copy failed: " + name);
    }

    void StartBackend(string realSteamApi)
    {
        var evidence = Path.Combine(run, "backend");
        var backend = Path.Combine(app, "backend", "HellForge.LocalBackend.exe");
        if (!File.Exists(backend)) throw new SessionFailure("The local backend is missing; reinstall HellForge.");
        var start = new ProcessStartInfo(backend) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (var argument in new[] { "--port", install.BackendPort.ToString(), "--startup-fixtures", "true", "--lifetime-seconds", "0", "--evidence", evidence,
                     "--profile-dir", profileDirectoryUsed, "--capture-mission-bodies", "true", "--premium-locked", PremiumLockedFile }) start.ArgumentList.Add(argument);
        if (options.Template != null) { start.ArgumentList.Add("--profile-template"); start.ArgumentList.Add(options.Template); }
        if (options.Sandbox) { start.ArgumentList.Add("--sandbox"); start.ArgumentList.Add("true"); }
        WarArguments(start);
        if (options.Multiplayer) MultiplayerArguments(start, realSteamApi);
        // The block permits only this port, so the backend has no other: say so when another program holds it.
        if (System.Net.NetworkInformation.IPGlobalProperties.GetIPGlobalProperties().GetActiveTcpListeners().Any(l => l.Port == install.BackendPort))
            throw new SessionFailure($"Port {install.BackendPort}, HellForge's backend port, is used by another program on this PC. Close it and start again.");
        server = Process.Start(start) ?? throw new SessionFailure("The local backend did not start.");
        var stdout = new StreamWriter(Path.Combine(run, "backend-stdout.txt")) { AutoFlush = true };
        var stderr = new StreamWriter(Path.Combine(run, "backend-stderr.txt")) { AutoFlush = true };
        server.OutputDataReceived += (_, e) => { if (e.Data != null) lock (stdout) stdout.WriteLine(Privacy.Scrub(e.Data)); };
        server.ErrorDataReceived += (_, e) => { if (e.Data != null) lock (stderr) stderr.WriteLine(Privacy.Scrub(e.Data)); };
        server.BeginOutputReadLine();
        server.BeginErrorReadLine();
        WaitForBackendReady(Path.Combine(evidence, "ready.json"));
    }

    // Follows the live galactic war read-only through the community mirror; fetched before the backend is ready.
    // The all-planets war (every planet but Super Earth a liberation) takes the live war's place.
    void WarArguments(ProcessStartInfo start)
    {
        if (options.OpenWar)
        {
            start.ArgumentList.Add("--war"); start.ArgumentList.Add("open"); result["war"] = "open";
            if (options.WarSeed != 0) { start.ArgumentList.Add("--war-seed"); start.ArgumentList.Add(options.WarSeed.ToString(System.Globalization.CultureInfo.InvariantCulture)); result["warSeed"] = options.WarSeed; }
            if (!options.DealSubfactions) { start.ArgumentList.Add("--war-subfactions"); start.ArgumentList.Add("no"); result["warSubfactions"] = false; }
            if (!options.DealSeaf) { start.ArgumentList.Add("--war-seaf"); start.ArgumentList.Add("no"); result["warSeaf"] = false; }
        }
        else if (options.LiveWar != null) { start.ArgumentList.Add("--live-war"); start.ArgumentList.Add(Path.GetFullPath(options.LiveWar)); result["liveWar"] = true; }
    }

    void MultiplayerArguments(ProcessStartInfo start, string realSteamApi)
    {
        var link = Path.Combine(Directory.CreateDirectory(Path.Combine(run, "steam-link")).FullName, "steam_api64.dll");
        File.Copy(realSteamApi, link);
        if (Tools.Sha256(link) != shimPins[CloudName]) throw new SessionFailure("Could not copy steam_api64.dll for the peer link.");
        var pipe = "hellforge-" + (string)result["runId"]!;
        // --mods: co-op plays with the host's Local Stats (StatsShare): the host's backend reads it, a joiner's restores it
        // --modset: joiners lacking this session's mods may download them (ModSync)
        // Several installations on one PC (GameProcesses.MultiInstance): the backends reach each other over loopback
        // (HELLFORGE_PEER_LINK, a udp: link) and share a folder as the lobby directory (HELLFORGE_DIRECTORY_FOLDER);
        // the backend logs the game in as that link's id (HELLFORGE_IDENTITY, which it reads itself).
        var peerLink = GameProcesses.MultiInstance ? Environment.GetEnvironmentVariable("HELLFORGE_PEER_LINK") : null;
        var directoryFolder = GameProcesses.MultiInstance ? Environment.GetEnvironmentVariable("HELLFORGE_DIRECTORY_FOLDER") : null;
        if (peerLink != null && !peerLink.StartsWith("udp:", StringComparison.Ordinal)) throw new SessionFailure("HELLFORGE_PEER_LINK must be a udp: link.");
        foreach (var argument in new[] { "--pipe", pipe, "--link", peerLink ?? "steam:" + link, "--mod-digest", modDigest, "--mods", install.Mods, "--modset", ModSetFile })
            start.ArgumentList.Add(argument);
        if (directoryFolder != null) { start.ArgumentList.Add("--directory"); start.ArgumentList.Add(Path.GetFullPath(directoryFolder)); }
        if (peerLink != null) result["peerLink"] = peerLink;
        // --mod-list: the digest's mods one by one, so a refused join can say which differ (ModlistDiff)
        if (modList != null)
        {
            var listFile = Path.Combine(run, "modlist.txt");
            File.WriteAllText(listFile, modList);
            start.ArgumentList.Add("--mod-list");
            start.ArgumentList.Add(listFile);
        }
        // --rejoin: after a mod sync, the game is invited back into the host's lobby once it is on the ship (AutoRejoin)
        if (rejoinFile != null)
        {
            start.ArgumentList.Add("--rejoin");
            start.ArgumentList.Add(rejoinFile);
            result["rejoin"] = true;
        }
        // The game inherits this environment (HostBoundedProcess).
        Environment.SetEnvironmentVariable("HELLFORGE_PIPE", pipe);
        result["multiplayer"] = new JsonObject { ["pipe"] = pipe, ["link"] = "steam" };
    }

    // Waits up to 30 s for the backend's ready.json and records its loopback origin and profile.
    void WaitForBackendReady(string ready)
    {
        var clock = Stopwatch.StartNew();
        // Up to 16 s of live war fetches (two requests, 8 s timeout each) come before ready.
        while (!File.Exists(ready))
        {
            if (server!.HasExited || clock.Elapsed.TotalSeconds > 30) throw new SessionFailure("Backend did not become ready.");
            Thread.Sleep(100);
        }
        Thread.Sleep(50);
        var state = Tools.ReadJson(ready)!;
        if (state["urls"] is not JsonArray { Count: 1 } urls) throw new SessionFailure("Unexpected backend listeners.");
        var origin = new Uri((string)urls[0]!);
        if (origin.Host != "127.0.0.1" || origin.Scheme != "http") throw new SessionFailure("Unexpected backend origin.");
        // No trailing slash: the client appends "/Configuration/GameClient" etc.
        result["backendOrigin"] = "http://127.0.0.1:" + origin.Port;
        result["profile"] = state["profile"]?.DeepClone();
        result["profileCreated"] = state["profileCreated"]?.DeepClone();
    }

    void Observe(HellForgeLocalRoutingPatch.PlayFabRedirect? playFab, bool clearMatchmaking, string requestLog, int blockRules)
    {
        var clock = Stopwatch.StartNew();
        var observations = Path.Combine(run, "observations.jsonl");
        long nextSample = 0, nextBlockCheck = 10000, requestOffset = 0;
        var requestCount = 0;
        Say("Play session running. Close the game or press Stop in the launcher to end it.");
        while (true)
        {
            job!.Process.Refresh();
            if (job.Process.HasExited) break;
            CheckBlock(clock, blockRules, ref nextBlockCheck);
            RecordRedirect(playFab);
            // clearMatchmaking holds the game to the genuine sign-in; ClearPlatformMatchmaking runs each loop.
            var matchmaking = clearMatchmaking ? ClearMatchmaking() : null;
            requestCount += Tools.BackendRequests(requestLog, ref requestOffset).Count;
            // Play sessions keep only the periodic samples so the log stays small; the engine's own diagnostics are kept
            // only under HELLFORGE_DEBUG.
            if (clock.ElapsedMilliseconds >= nextSample)
            {
                WriteSample(observations, clock.ElapsedMilliseconds, requestCount, matchmaking);
                nextSample = clock.ElapsedMilliseconds + 30000;
            }
            if (StopRequested()) break;
            Thread.Sleep(1000);
        }
        RecordEnd(clock);
    }

    // The persistent block is re-read every 10 seconds; the game is stopped if a rule or profile goes away.
    void CheckBlock(Stopwatch clock, int blockRules, ref long nextBlockCheck)
    {
        if (clock.ElapsedMilliseconds < nextBlockCheck) return;
        // blockRules is 0 only in the WFP-only mode, which has no firewall rules to re-read.
        if (blockRules > 0 && BlockRuleCount() != blockRules) throw new SessionFailure("Network block rules or firewall profiles changed during the session.");
        nextBlockCheck = clock.ElapsedMilliseconds + 10000;
    }

    void RecordRedirect(HellForgeLocalRoutingPatch.PlayFabRedirect? playFab)
    {
        if (!Debug || playFab == null || result["playFabRedirect"] != null || playFab.Outcome == "waiting") return;
        result["playFabRedirect"] = new JsonObject { ["outcome"] = playFab.Outcome, ["foundMilliseconds"] = playFab.FoundMilliseconds, ["samples"] = playFab.Samples };
        Save();
    }

    string? ClearMatchmaking()
    {
        try { return HellForgeLocalRoutingPatch.ClearPlatformMatchmaking(job!.Process.Id, gameDll); }
        catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException) { return "error"; }
    }

    void WriteSample(string observations, long elapsed, int requestCount, string? matchmaking)
    {
        var row = new JsonObject
        {
            ["utc"] = DateTime.UtcNow.ToString("o"),
            ["elapsedMilliseconds"] = elapsed,
            ["windows"] = new JsonArray(HostWindowText.Read(job!.Process.Id).Select(w => (JsonNode?)new JsonObject
                { ["Title"] = w.Title, ["Controls"] = new JsonArray(w.Controls.Select(c => (JsonNode?)JsonValue.Create(c)).ToArray()) }).ToArray()),
            ["backendRequests"] = requestCount,
            ["sampled"] = true
        };
        if (Debug)
        {
            row["routingSite"] = HellForgeLocalRoutingPatch.Inspect(job.Process.Id, gameDll);
            row["backendState"] = HellForgeLocalRoutingPatch.Diagnose(job.Process.Id, gameDll);
            row["catalogue"] = HellForgeLocalRoutingPatch.DiagnoseCatalogue(job.Process.Id, gameDll);
            row["playFab"] = HellForgeLocalRoutingPatch.DiagnosePlayFab(job.Process.Id, exe);
            if (matchmaking != null) row["platformMatchmaking"] = matchmaking;
        }
        File.AppendAllText(observations, row.ToJsonString() + "\n", new UTF8Encoding(false));
    }

    bool StopRequested()
    {
        if (!File.Exists(Path.Combine(run, "stop-observation.signal"))) return false;
        result["observationStoppedBySignal"] = true;
        // The backend's mod sync stops the session to start it again with the host's mods: the launcher does that.
        if (ReadSignal() == "mod-sync") result["restart"] = "mod-sync";
        return true;
    }

    void RecordEnd(Stopwatch clock)
    {
        result["gameExitedNaturally"] = job!.Process.HasExited;
        if (!job.Process.HasExited)
        {
            if (Debug)
            {
                result["routingSiteAtEnd"] = HellForgeLocalRoutingPatch.Inspect(job.Process.Id, gameDll);
                result["backendStateAtEnd"] = HellForgeLocalRoutingPatch.Diagnose(job.Process.Id, gameDll);
            }
        }
        else result["exitCode"] = job.Process.ExitCode;
        result["observedSeconds"] = Math.Round(clock.Elapsed.TotalSeconds, 2);
    }
}
