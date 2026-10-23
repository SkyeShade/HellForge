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
sealed class PlaySession(PlayOptions options)
{
    static readonly string[] ShimNames = ["PartyWin.dll", "PlayFabMultiplayerWin.dll"];
    const string CloudName = "steam_api64.dll", CloudReal = "steam_api64_hellforge.dll", CensusLog = "network-census.jsonl";

    readonly JsonObject result = new();
    readonly List<FileStream> locks = [];
    readonly List<string> swapped = [];
    readonly Dictionary<string, string> shimPins = new(StringComparer.Ordinal);
    // HELLFORGE_DEBUG=1 writes the full step reports (addresses, before/after bytes) and the per-second diagnostics into
    // the run folder, for development. Normal sessions keep only what happened, not how.
    static readonly bool Debug = Environment.GetEnvironmentVariable("HELLFORGE_DEBUG") == "1";
    InstallLayout install = null!;
    string run = "", copy = "", exe = "", gameDll = "", bundle = "", shimOriginals = "", app = "";
    string? normal, hint, erlBackup, profileFolder;
    string[] erlBefore = [];
    FileStream? lease;
    HostBoundedProcess? job;
    Process? server;
    Thread? applier;
    DateTime? gameStarted;
    TextWriter? log;

    string Shims => Path.Combine(app, "sdk-shims");
    string Scripts => Path.Combine(app, "scripts");
    string Cli => Path.Combine(app, "cli", "HellForge.Cli.exe");

    void Say(string line)
    {
        var text = $"{DateTime.Now:HH:mm:ss} {Privacy.Scrub(line)}";
        log?.WriteLine(text);
        log?.Flush();
    }

    // Scrubbed (Privacy): readers take only its status and messages from it.
    void Save() => File.WriteAllText(Path.Combine(run, "result.json"), Privacy.ScrubJson(result), new UTF8Encoding(false));

    public int Run()
    {
        install = new InstallLayout(Path.GetFullPath(options.Install));
        if (!install.Exists) throw new SessionFailure("Not a HellForge installation: " + install.Root);
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

    // Runs HellForge.Cli: its JSON output, or null (its messages go to the run's cli-output.txt).
    string? HellForgeCli(params string[] arguments)
    {
        var (exit, output, error) = Tools.Run(Cli, arguments, timeoutMs: 30 * 60 * 1000);
        File.AppendAllText(Path.Combine(run, "cli-output.txt"), Privacy.Scrub(error));
        return exit == 0 || (exit == 2 && arguments[0] == "prepare-session") ? output : null;
    }

    void Prepare(string profileDirectory)
    {
        if (!Environment.Is64BitProcess) throw new SessionFailure("The session runner must run as a 64-bit process.");
        foreach (var name in new[] { "helldivers2", "GameGuard", "GameMon", "GameMon64" })
            if (Process.GetProcessesByName(name).Length > 0) throw new SessionFailure("Existing game or GameGuard process.");
        if (Process.GetProcessesByName("HD2RuntimeModBuilder").Length > 0) throw new SessionFailure("External mod builder is running.");
        // One session at a time per installation: a second one (Play pressed again, or a session runner from an earlier session
        // that has not ended) finds the lock held.
        try { lease = new FileStream(Path.Combine(install.Temp, "session.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None); }
        catch (IOException) when (File.Exists(Path.Combine(install.Temp, "session.lock")))
        {
            throw new SessionFailure("Another HellForge session is still running for this installation. Wait for it to end, or close " +
                "HellForge.Session.exe in Task Manager (or restart the PC), then press Play again.");
        }

        // The normal game, if Steam has one, is held closed during the session; find it in Steam's libraries.
        var steamPath = Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", null) as string;
        if (steamPath != null && HellForgeCli("discover", "--steam-root", steamPath) is { } manifests && Tools.ParseJson(manifests) is JsonArray found)
            foreach (var manifest in found)
            {
                var candidate = Path.Combine(Path.GetDirectoryName((string)manifest!)!, "common", "Helldivers 2");
                if (File.Exists(Path.Combine(candidate, "bin", "helldivers2.exe"))) { normal = candidate; break; }
            }
        result["normalGame"] = normal;
        var profileName = Path.GetFileName(profileDirectory);
        if (!System.Text.RegularExpressions.Regex.IsMatch(profileName, "^[a-z0-9-]{1,32}$")) throw new SessionFailure("The profile folder name is not a valid profile name.");

        // A session that was cut short leaves the stand-ins in the copy: put the saved originals back first. Their pins
        // are the originals the stand-ins were built from; the verification below ties those to the build.
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

        // Mods: stage data mods (and remove the last session's) before the copy's files are locked; --progress lets the
        // launcher show source mods being compiled.
        var loaderPlan = Path.Combine(run, "modloader-plan.txt");
        var planText = HellForgeCli("mods-stage", "--install", install.Root, "--plan", loaderPlan, "--logs", run, "--progress", Path.Combine(run, "mod-builds.json"));
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
        result["mods"] = new JsonObject
        {
            ["loaded"] = new JsonArray((plan["mods"] as JsonArray ?? []).Select(m => (JsonNode?)JsonValue.Create($"{(string?)m!["id"]} {(string?)m["version"]}")).ToArray()),
            ["problems"] = plan["problems"]?.DeepClone() ?? new JsonArray(),
            ["stagedFiles"] = (plan["staged"] as JsonArray)?.Count ?? 0,
            ["codeMods"] = plan["codeMods"]?.DeepClone(),
            ["digest"] = modDigest
        };
        foreach (var problem in plan["problems"] as JsonArray ?? []) Say("Mod not loaded: " + (string?)problem);

        // Every game file is held read-only for the session, then verified against the supported build while
        // data/settings.ini is pointed at this profile's user data inside the installation.
        var settingsPath = Path.GetFullPath(Path.Combine(copy, "data", "settings.ini"));
        foreach (var file in Directory.EnumerateFiles(copy, "*", SearchOption.AllDirectories))
        {
            var full = Path.GetFullPath(file);
            if (full.Equals(settingsPath, StringComparison.OrdinalIgnoreCase) || full.EndsWith(".partial", StringComparison.OrdinalIgnoreCase)) continue;
            Tools.NoLinks(full);
            var share = full.Contains(@"\bin\GameGuard", StringComparison.OrdinalIgnoreCase) ? FileShare.ReadWrite : FileShare.Read;
            locks.Add(new FileStream(full, FileMode.Open, FileAccess.Read, share));
        }
        var prepared = Tools.ParseJson(HellForgeCli("prepare-session", "--install", install.Root, "--profile", profileName) ?? "")
            ?? throw new SessionFailure("Session preparation failed; see cli-output.txt in this session's log folder.");
        if (prepared["problems"] is JsonArray { Count: > 0 } problems)
            throw new SessionFailure($"The game copy is not the supported build ({(string?)problems[0]}); repair it from the launcher.");
        locks.Add(new FileStream(settingsPath, FileMode.Open, FileAccess.Read, FileShare.Read));
        result["verifiedFiles"] = (prepared["gameFiles"] as JsonArray)?.Count ?? 0;
        result["userData"] = prepared["profileData"]?.DeepClone();
        result["userDataSeeded"] = prepared["profileSeeded"]?.DeepClone();
        result["gameExeSha256"] = Tools.Sha256(exe);
        result["gameDllSha256"] = Tools.Sha256(gameDll);
        if (normal != null)
        {
            result["installedExeSha256"] = Tools.Sha256(Path.Combine(normal, "bin", "helldivers2.exe"));
            result["installedDllSha256"] = Tools.Sha256(Path.Combine(normal, "data", "game", "game.dll"));
        }
        profileDirectoryUsed = profileDirectory;
    }

    string modDigest = "none", profileDirectoryUsed = "";
    string? modList;
    bool modNative;

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

    // The Steam ownership check, against the given steam_api64.dll: app/scripts/Test-SteamOwnership.ps1 on Windows, the
    // probe itself under Wine (whose powershell.exe runs nothing and returns 0). Either way the evidence it writes decides
    // (SteamOwnershipCheck.Problem): a check that ran nothing passes nothing.
    void Ownership(string steamApi, string evidence)
    {
        var started = DateTime.UtcNow;
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
        // Only copies made before HellForge stopped copying the GameGuard files have this folder; without it there are no
        // logs to keep.
        var guardFolder = Path.Combine(copy, "bin", "GameGuard");
        if (Directory.Exists(guardFolder))
        {
            erlBackup = Directory.CreateDirectory(Path.Combine(run, "gameguard-originals")).FullName;
            erlBefore = Directory.GetFiles(guardFolder, "*.erl").Select(Path.GetFileName).ToArray()!;
            foreach (var name in erlBefore) File.Copy(Path.Combine(guardFolder, name), Path.Combine(erlBackup, name));
        }
        var build = Tools.ReadJson(Path.Combine(Shims, "build.json"))!;
        result["sdkShims"] = options.Multiplayer;
        if (options.Multiplayer)
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
            Environment.SetEnvironmentVariable("HELLFORGE_SHIM_LOG_DIR", Directory.CreateDirectory(Path.Combine(run, "sdk-shims")).FullName);
        }
        Ownership(Path.Combine(copy, "bin", CloudName), Path.Combine(run, "ownership.json"));
        result["ownershipConfirmed"] = true;

        // Steam Cloud stays in the profile: the steam_api64.dll stand-in, with the verified original next to it. No session
        // runs without it, so the copy never writes the Steam account's cloud files.
        var cloudStandIn = Path.Combine(Shims, CloudName);
        var cloudPin = (string?)build["standIns"]?[CloudName];
        if (cloudPin == null || !File.Exists(cloudStandIn)) throw new SessionFailure("The Steam Cloud stand-in is missing; reinstall HellForge.");
        if (Tools.Sha256(cloudStandIn) != cloudPin) throw new SessionFailure("Stand-in changed since it was built: steam_api64.dll");
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
        Environment.SetEnvironmentVariable("HELLFORGE_CLOUD_LOG", Path.Combine(run, "steam-cloud.log"));
        // The network gate's census (the stand-in's net_gate.c): every connection, name lookup and program the copy
        // attempts, logged beside the firewall, which still blocks them; summarised into the result at the end.
        Environment.SetEnvironmentVariable("HELLFORGE_NETGATE", "census");
        Environment.SetEnvironmentVariable("HELLFORGE_NETGATE_LOG", Path.Combine(run, CensusLog));
        result["steamCloud"] = new JsonObject { ["directory"] = Path.GetFullPath(cloudDirectory), ["standIn"] = cloudPin, ["controlsSeeded"] = cloudSeeded };
        var authorization = Stopwatch.StartNew();

        // No fallback launch of the installed game during the session: its executable and GameGuard programs are held
        // closed. No firewall rule ever lists the installed game; a lock ends with the session even when it is cut short.
        if (normal != null)
        {
            var programs = new List<string> { Path.Combine(normal, "bin", "helldivers2.exe") };
            programs.AddRange(Directory.EnumerateFiles(Path.Combine(normal, "bin"), "*", SearchOption.AllDirectories)
                .Where(f => (f.EndsWith(".des", StringComparison.OrdinalIgnoreCase) || f.EndsWith(".now", StringComparison.OrdinalIgnoreCase)) &&
                            f.Contains("GameGuard", StringComparison.OrdinalIgnoreCase)));
            foreach (var path in programs)
                try { locks.Add(new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.None)); }
                catch (IOException) { throw new SessionFailure("Could not hold the installed game closed (is it running?): " + path); }
        }
        hint = Path.Combine(copy, "bin", "steam_appid.txt");
        // Not a game file: only a session writes it, so one here was left by a session that was cut short.
        if (File.Exists(hint)) { File.Delete(hint); result["staleAppIdHintRemoved"] = true; }
        File.WriteAllText(hint, "553850\n", Encoding.ASCII);

        StartBackend(realTarget);
        result["steamOnline"] = true;
        // Setup places the block as WFP filters (hellforge_wfp) and drops a marker; with it (or HELLFORGE_WFP_ONLY for
        // tests) the Windows Firewall rules are not required or read. An installation without the marker falls back to
        // the firewall rules, installed here with one Windows prompt. The probe below proves the block either way
        // (loopback reaches the backend, the outside is refused).
        // The block is WFP when Setup placed it there (the marker it drops) or when a test asks for it.
        var marker = Path.Combine(install.Root, "network-block", "wfp-block.json");
        var wfpOnly = Environment.GetEnvironmentVariable("HELLFORGE_WFP_ONLY") == "1" || File.Exists(marker);
        // Linux (Wine/Proton, docs/linux.md): the block is a network namespace with loopback only, made by
        // linux/hellforge-offline.sh around the launcher; the launcher's setup leaves a marker naming it. Its session must
        // run inside that namespace (the script sets HELLFORGE_LINUX_NETNS); the probe below still proves the block.
        // Linux is told by Wine itself (an installation copied from Windows keeps the Windows marker), and by the marker.
        var linuxNamespace = Environment.GetEnvironmentVariable("HELLFORGE_LINUX_NETNS") == "1";
        if (!linuxNamespace && (UnderWine() || (File.Exists(marker) && File.ReadAllText(marker).Contains("linux-netns", StringComparison.Ordinal))))
            throw new SessionFailure("On Linux, HellForge's game runs only in its network namespace: start HellForge with linux/hellforge-linux.sh.");
        var blockRules = 0;
        if (linuxNamespace)
            result["networkBlock"] = "linux-netns";
        else if (wfpOnly)
            result["networkBlock"] = "wfp";
        else
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
            blockRules = BlockRuleCount();
            if (blockRules < 0) throw new SessionFailure("Windows Firewall is off for a network type.");
            result["networkBlockRules"] = blockRules;
        }

        // The block's probe: loopback reaches the backend, the outside does not (WSAEACCES).
        var networkReport = Path.Combine(run, "network-probe.json");
        var probe = Path.Combine(install.Root, "network-block", "NetworkProbe.exe");
        // Made by the network block's install; antivirus programs sometimes remove it.
        if (!linuxNamespace && !File.Exists(probe))
            throw new SessionFailure("The network block's check program is missing (" + probe + "). Antivirus programs sometimes remove it: " +
                "restore it from quarantine and exclude the HellForge folder, or run Settings > Installation again to put the block back.");
        var origin = new Uri((string)result["backendOrigin"]!);
        // Linux: the same probe in this process (NetworkProbe.exe is a .NET Framework program, which a Wine prefix runs
        // only with Wine Mono installed).
        var (probeExit, _, _) = linuxNamespace ? (NetworkProbeInProcess(origin.Port, networkReport), "", "")
            : Tools.Run(probe, [origin.Port.ToString(), networkReport], timeoutMs: 10000);
        var network = Tools.ReadJson(networkReport);
        result["networkControl"] = network?.DeepClone();
        if (probeExit != 0 || network == null || (bool?)network["loopbackHttpPassed"] != true || (int?)network["externalIpv4TcpError"] != 10013)
            throw new SessionFailure("Required loopback/egress controls failed.");
        // Confirm genuine Steam sign-in and ownership with the game's rules active.
        Ownership(realTarget, Path.Combine(run, "ownership-with-firewall.json"));
        result["steamOwnershipWithRules"] = true;
        if (authorization.Elapsed.TotalSeconds >= 120) throw new SessionFailure("Ownership authorization expired.");

        profileFolder = Path.Combine(install.UserData, "environment");
        foreach (var folder in new[] { "Roaming", "Local", "Temp" }) Directory.CreateDirectory(Path.Combine(profileFolder, folder));
        var requestLog = Path.Combine(run, "backend", "requests.jsonl");
        long requestOffset = 0;
        result["preLaunchBackendRequests"] = Tools.BackendRequests(requestLog, ref requestOffset).Count;

        // The engine's own --log-to-file switch (its console log); whatever .log files it writes are copied at the end.
        gameStarted = DateTime.UtcNow;
        result["engineLog"] = options.EngineLog;
        // then the player's launch options (Steam's Launch Options, such as --use-d3d11), after the session's own
        var gameOptions = HellForge.Core.GameLaunchOptions.CommandLine(options.GameOptions);
        result["gameOptions"] = gameOptions;
        job = new HostBoundedProcess(exe, $"--bundle-dir \"{bundle}\" --release" + (options.EngineLog ? " --log-to-file" : "") + (gameOptions.Length > 0 ? " " + gameOptions : ""),
            Path.Combine(copy, "bin"), profileFolder, 0);
        result["gamePid"] = job.Process.Id;
        result["gameLaunched"] = true;
        result["status"] = "patching";
        Save();
        job.Resume();
        // Opt-in: the engine's PlayFab web API base URL is redirected (heap data only) to the backend's local PlayFab
        // stand-in; started before the slow edits so the URL is replaced right after the engine formats it.
        var playFab = options.LocalPlayFab ? HellForgeLocalRoutingPatch.StartPlayFabRedirect(job.Process.Id, exe, (string)result["backendOrigin"]! + "/playfab", 180000) : null;
        if (modNative)
        {
            var pid = job.Process.Id;
            applier = new Thread(() =>
            {
                try { NativeModApplier.Run(install, pid, exe, Path.Combine(run, "native-mods.log"), Path.Combine(run, "native-mods.json")); }
                catch (Exception error) when (error is IOException or System.ComponentModel.Win32Exception or InvalidOperationException) { }
            }) { IsBackground = true, Name = "native mods" };
            applier.Start();
        }
        result["localPlayFab"] = options.LocalPlayFab;
        HellForgeHostEarlyConstructorExperiment.TargetExe = exe;
        HellForgeHostEarlyResult experiment;
        try { experiment = HellForgeHostEarlyConstructorExperiment.ApplySteamGame(job.Process.Id, job.Process.StartTime.Ticks, "SkipConstructorAndMainCheck"); }
        catch (Exception error) when (error is InvalidOperationException or System.ComponentModel.Win32Exception)
        { throw new SessionFailure("The game could not be prepared for the local backend." + (Debug ? " " + error.Message : "")); }
        if (Debug) File.WriteAllText(Path.Combine(run, "launch-step-1.json"), JsonSerializer.Serialize(experiment, SessionJson.Default.HellForgeHostEarlyResult));
        var startupReady = experiment.Written && experiment.MainCheckWritten && experiment.LaterStatusWritten && experiment.ReadbackMatched;
        if (experiment.Outcome != "applied_at_candidate_boundary" || !startupReady)
        {
            // The game sometimes quits before the launch is prepared; its exit code names why, for the player.
            var note = "";
            try
            {
                job.Process.Refresh();
                if (job.Process.HasExited)
                {
                    var code = job.Process.ExitCode;
                    result["gameExitCode"] = $"0x{code:X8}";
                    var meaning = Tools.ExitMeaning(code);
                    note = $" (the game exited with 0x{code:X8}{(meaning.Length > 0 ? ": " + meaning : "")})";
                }
            }
            catch (InvalidOperationException) { }
            throw new SessionFailure("The game could not be prepared for the local backend" + note + ".");
        }
        HellForgeLocalRoutingResult routing;
        try { routing = HellForgeLocalRoutingPatch.Apply(job.Process.Id, job.Process.StartTime.Ticks, exe, gameDll, (string)result["backendOrigin"]!, 45000, false); }
        catch (Exception error) when (error is InvalidOperationException or ArgumentException or System.ComponentModel.Win32Exception)
        { throw new SessionFailure("The game could not be connected to the local backend." + (Debug ? " " + error.Message : "")); }
        if (Debug) File.WriteAllText(Path.Combine(run, "launch-step-2.json"), JsonSerializer.Serialize(routing, SessionJson.Default.HellForgeLocalRoutingResult));
        var backendReady = routing.Outcome == "applied" && routing.ReadbackMatched;
        if (!backendReady) throw new SessionFailure("The game could not be connected to the local backend." + (Debug ? " (" + routing.Outcome + ")" : ""));
        result["launchPrepared"] = true;
        // The local backend enables from the genuine sign-in; kept aligned each second below. With the local PlayFab
        // stand-in the sign-in completes on loopback, so this is left as it is.
        var clearMatchmaking = !options.LocalPlayFab;
        result["status"] = "observing";
        Save();
        Say($"The game is running against the local backend (PID {job.Process.Id}).");
        Observe(playFab, clearMatchmaking, requestLog, blockRules);
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
        foreach (var argument in new[] { "--port", "0", "--startup-fixtures", "true", "--lifetime-seconds", "0", "--evidence", evidence,
                     "--profile-dir", profileDirectoryUsed, "--capture-mission-bodies", "true" }) start.ArgumentList.Add(argument);
        if (options.Template != null) { start.ArgumentList.Add("--profile-template"); start.ArgumentList.Add(options.Template); }
        // Follows the live galactic war read-only through the community mirror; fetched before the backend is ready.
        // The all-planets war (every planet but Super Earth a liberation) takes the live war's place.
        if (options.OpenWar) { start.ArgumentList.Add("--war"); start.ArgumentList.Add("open"); result["war"] = "open"; }
        else if (options.LiveWar != null) { start.ArgumentList.Add("--live-war"); start.ArgumentList.Add(Path.GetFullPath(options.LiveWar)); result["liveWar"] = true; }
        // A war server: the backend forwards the game's war routes to it, and checks it before it is ready.
        else if (options.WarServer != null)
        {
            foreach (var argument in new[] { "--war", "server", "--war-server", options.WarServer }) start.ArgumentList.Add(argument);
            if (options.WarServerCa != null) { start.ArgumentList.Add("--war-server-ca"); start.ArgumentList.Add(Path.GetFullPath(options.WarServerCa)); }
            if (options.WarServerToken != null) { start.ArgumentList.Add("--war-server-token"); start.ArgumentList.Add(Path.GetFullPath(options.WarServerToken)); }
            result["war"] = "server";
            result["warServer"] = options.WarServer;
            Say($"The war comes from the war server {options.WarServer}.");
        }
        if (options.Multiplayer)
        {
            var link = Path.Combine(Directory.CreateDirectory(Path.Combine(run, "steam-link")).FullName, "steam_api64.dll");
            File.Copy(realSteamApi, link);
            if (Tools.Sha256(link) != shimPins[CloudName]) throw new SessionFailure("Could not copy steam_api64.dll for the peer link.");
            var pipe = "hellforge-" + (string)result["runId"]!;
            // --mods: co-op plays with the host's Local Stats (StatsShare): the host's backend reads it, a joiner's restores it
            foreach (var argument in new[] { "--pipe", pipe, "--link", "steam:" + link, "--mod-digest", modDigest, "--mods", install.Mods }) start.ArgumentList.Add(argument);
            // --mod-list: the digest's mods one by one, so a refused join can say which differ (ModlistDiff)
            if (modList != null)
            {
                var listFile = Path.Combine(run, "modlist.txt");
                File.WriteAllText(listFile, modList);
                start.ArgumentList.Add("--mod-list");
                start.ArgumentList.Add(listFile);
            }
            // The game inherits this environment (HostBoundedProcess).
            Environment.SetEnvironmentVariable("HELLFORGE_PIPE", pipe);
            result["multiplayer"] = new JsonObject { ["pipe"] = pipe, ["link"] = "steam" };
        }
        server = Process.Start(start) ?? throw new SessionFailure("The local backend did not start.");
        var stdout = new StreamWriter(Path.Combine(run, "backend-stdout.txt")) { AutoFlush = true };
        var stderr = new StreamWriter(Path.Combine(run, "backend-stderr.txt")) { AutoFlush = true };
        server.OutputDataReceived += (_, e) => { if (e.Data != null) lock (stdout) stdout.WriteLine(Privacy.Scrub(e.Data)); };
        string? lastError = null;
        server.ErrorDataReceived += (_, e) => { if (e.Data != null) lock (stderr) { lastError = Privacy.Scrub(e.Data); stderr.WriteLine(lastError); } };
        server.BeginOutputReadLine();
        server.BeginErrorReadLine();
        var ready = Path.Combine(evidence, "ready.json");
        var clock = Stopwatch.StartNew();
        // Up to 16 s of live war fetches (two requests, 8 s timeout each), or a war server's hello (20 s at most), come before ready.
        while (!File.Exists(ready))
        {
            if (server.HasExited && options.WarServer != null)
            {
                // Why: the backend's last error line ("... could not be reached: ..."), once its output is all read.
                server.WaitForExit();
                string? why;
                lock (stderr) why = lastError;
                throw new SessionFailure($"The war server {options.WarServer} could not be used" + (why != null ? $" ({why})." : "."));
            }
            if (server.HasExited || clock.Elapsed.TotalSeconds > 30) throw new SessionFailure("Backend did not become ready.");
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
            // The persistent block is re-read every 10 seconds; the game is stopped if a rule or profile goes away.
            if (clock.ElapsedMilliseconds >= nextBlockCheck)
            {
                // blockRules is 0 only in the WFP-only mode, which has no firewall rules to re-read.
                if (blockRules > 0 && BlockRuleCount() != blockRules) throw new SessionFailure("Network block rules or firewall profiles changed during the session.");
                nextBlockCheck = clock.ElapsedMilliseconds + 10000;
            }
            if (Debug && playFab != null && result["playFabRedirect"] == null && playFab.Outcome != "waiting")
            {
                result["playFabRedirect"] = new JsonObject { ["outcome"] = playFab.Outcome, ["foundMilliseconds"] = playFab.FoundMilliseconds, ["samples"] = playFab.Samples };
                Save();
            }
            string? matchmaking = null;
            if (clearMatchmaking)
                try { matchmaking = HellForgeLocalRoutingPatch.ClearPlatformMatchmaking(job.Process.Id, gameDll); }
                catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException) { matchmaking = "error"; }
            requestCount += Tools.BackendRequests(requestLog, ref requestOffset).Count;
            // Play sessions keep only the periodic samples so the log stays small; the engine's own diagnostics are kept
            // only under HELLFORGE_DEBUG.
            if (clock.ElapsedMilliseconds >= nextSample)
            {
                var row = new JsonObject
                {
                    ["utc"] = DateTime.UtcNow.ToString("o"),
                    ["elapsedMilliseconds"] = clock.ElapsedMilliseconds,
                    ["windows"] = new JsonArray(HostWindowText.Read(job.Process.Id).Select(w => (JsonNode?)new JsonObject
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
                nextSample = clock.ElapsedMilliseconds + 30000;
            }
            if (File.Exists(Path.Combine(run, "stop-observation.signal"))) { result["observationStoppedBySignal"] = true; break; }
            Thread.Sleep(1000);
        }
        result["gameExitedNaturally"] = job.Process.HasExited;
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

    void CleanUp()
    {
        void Try(string what, Action action)
        {
            try { action(); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidOperationException or System.ComponentModel.Win32Exception or SessionFailure or JsonException)
            { result[what] = error.Message; }
        }
        if (job != null && result["gameExitCode"] == null)
            Try("gameExitCodeError", () => { job.Process.Refresh(); if (job.Process.HasExited) result["gameExitCode"] = $"0x{job.Process.ExitCode:X8}"; });
        if (job != null)
            try { job.Stop(); result["jobEmpty"] = job.ActiveProcesses == 0; }
            catch (System.ComponentModel.Win32Exception error) { result["jobStopError"] = error.Message; }
            finally { job.Dispose(); }
        if (options.EngineLog && gameStarted is { } started)
            Try("engineLogError", () =>
            {
                var engineLogs = Path.Combine(run, "engine-logs");
                var places = new[] { Path.Combine(copy, "bin"), profileFolder, Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "Arrowhead", "HellForge"), install.UserData };
                var found = places.Where(p => p != null && Directory.Exists(p))
                    .SelectMany(p => Directory.EnumerateFiles(p!, "*", SearchOption.AllDirectories))
                    .Where(f => (f.EndsWith(".log", StringComparison.OrdinalIgnoreCase) || f.EndsWith(".txt", StringComparison.OrdinalIgnoreCase)) &&
                                !f.StartsWith(run, StringComparison.OrdinalIgnoreCase))
                    .Select(f => new FileInfo(f)).Where(f => f.LastWriteTimeUtc >= started && f.Length < 200L * 1024 * 1024).ToList();
                if (found.Count > 0) Directory.CreateDirectory(engineLogs);
                var copied = new JsonArray();
                // The game names its console logs after the computer and writes the user folder in them: the copies don't.
                foreach (var file in found)
                {
                    var target = Path.Combine(engineLogs, Privacy.Scrub(file.Directory!.Name + "-" + file.Name));
                    File.WriteAllBytes(target, Privacy.Current.ApplyBytes(File.ReadAllBytes(file.FullName)));
                    copied.Add((JsonNode?)JsonValue.Create(file.FullName));
                }
                result["engineLogFiles"] = copied;
            });
        if (server != null)
            try { if (!server.HasExited) { server.Kill(); server.WaitForExit(5000); } result["backendStopped"] = server.HasExited; }
            catch (InvalidOperationException) { }
            finally { server.Dispose(); }
        // The applier ends with the game.
        if (applier != null)
        {
            applier.Join(5000);
            if (Tools.ReadJson(Path.Combine(run, "native-mods.json")) is { } status) result["nativeMods"] = status;
        }
        if (result["backendOrigin"] != null)
            Try("backendRequestError", () =>
            {
                long offset = 0;
                var requests = Tools.BackendRequests(Path.Combine(run, "backend", "requests.jsonl"), ref offset);
                result["backendRequestCount"] = requests.Count;
                if ((int?)result["preLaunchBackendRequests"] is { } before) result["gameBackendRequestCount"] = requests.Count - before;
                result["backendRequests"] = new JsonArray(requests.Select(r => (JsonNode?)new JsonObject
                {
                    ["method"] = r["method"]?.DeepClone(), ["path"] = r["path"]?.DeepClone(), ["status"] = r["status"]?.DeepClone(),
                    ["outcome"] = r["outcome"]?.DeepClone(), ["authorization"] = r["authorization"]?.DeepClone()
                }).ToArray());
            });
        if (gameStarted != null) Try("networkCensusError", () => result["networkCensus"] = NetworkCensus.Summarize(Path.Combine(run, CensusLog)));
        if (hint != null && File.Exists(hint)) Try("hintError", () => File.Delete(hint));
        foreach (var held in locks) held.Dispose();
        // A loaded DLL cannot be replaced, so the originals go back only once the game is gone; otherwise the next session
        // restores them before verifying the copy.
        if (swapped.Count > 0)
        {
            result["sdkShimOriginalsRestored"] = false;
            if ((bool?)result["gameLaunched"] != true || (bool?)result["jobEmpty"] == true)
                Try("sdkShimRestoreFailure", () =>
                {
                    foreach (var name in swapped)
                    {
                        var target = Path.Combine(copy, "bin", name);
                        // Windows can hold an exited process's DLL mapping for a moment after the job is empty.
                        for (var attempt = 1; ; attempt++)
                            try { File.Copy(Path.Combine(shimOriginals, name), target, overwrite: true); break; }
                            catch (IOException) when (attempt < 20) { Thread.Sleep(500); }
                        if (Tools.Sha256(target) != shimPins[name]) throw new SessionFailure($"Restored {name} does not match its pin.");
                    }
                    var realCopy = Path.Combine(copy, "bin", CloudReal);
                    for (var attempt = 1; File.Exists(realCopy); attempt++)
                        try { File.Delete(realCopy); }
                        catch (IOException) when (attempt < 20) { Thread.Sleep(500); }
                    result["sdkShimOriginalsRestored"] = true;
                });
        }
        if (erlBackup != null && (bool?)result["gameLaunched"] == true && (bool?)result["jobEmpty"] == true)
            Try("gameGuardLogError", () =>
            {
                var output = Directory.CreateDirectory(Path.Combine(run, "gameguard-output")).FullName;
                var guard = Path.GetFullPath(Path.Combine(copy, "bin", "GameGuard")) + Path.DirectorySeparatorChar;
                foreach (var file in Directory.GetFiles(guard, "*.erl"))
                {
                    Tools.NoLinks(file);
                    if (!Path.GetFullPath(file).StartsWith(guard, StringComparison.OrdinalIgnoreCase)) throw new SessionFailure("Log cleanup target outside the copied GameGuard directory.");
                    File.Copy(file, Path.Combine(output, Path.GetFileName(file)));
                    if (!erlBefore.Contains(Path.GetFileName(file))) File.Delete(file);
                }
                foreach (var name in erlBefore)
                {
                    File.Copy(Path.Combine(erlBackup, name), Path.Combine(guard, name), overwrite: true);
                    if (Tools.Sha256(Path.Combine(erlBackup, name)) != Tools.Sha256(Path.Combine(guard, name))) throw new SessionFailure("Copied GameGuard log restoration failed.");
                }
                result["copiedGameGuardLogsRestored"] = true;
            });
        if (result["gameExeSha256"] != null)
            Try("hashError", () =>
            {
                result["copyExeUnchanged"] = Tools.Sha256(exe) == (string?)result["gameExeSha256"];
                result["copyDllUnchanged"] = Tools.Sha256(gameDll) == (string?)result["gameDllSha256"];
                if (normal != null && result["installedExeSha256"] != null)
                {
                    result["installedExeUnchanged"] = Tools.Sha256(Path.Combine(normal, "bin", "helldivers2.exe")) == (string?)result["installedExeSha256"];
                    result["installedDllUnchanged"] = Tools.Sha256(Path.Combine(normal, "data", "game", "game.dll")) == (string?)result["installedDllSha256"];
                }
            });
        lease?.Dispose();
        result["completedUtc"] = DateTime.UtcNow.ToString("o");
        Save();
    }
}
