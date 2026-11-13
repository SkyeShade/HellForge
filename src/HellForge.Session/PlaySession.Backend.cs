using System.Diagnostics;
using HellForge.Core;

namespace HellForge.Session;

// The local backend a session serves the game from: started from the engine's backend folder only when its files are the
// ones this session was built with (HellForge.Core BackendPin), with this session's run, profile, mods and war.
sealed partial class PlaySession
{
    void StartBackend(string realSteamApi)
    {
        var evidence = Path.Combine(run, "backend");
        var backend = Path.Combine(app, "backend", "HellForge.LocalBackend.exe");
        if (!File.Exists(backend)) throw new SessionFailure("The local backend is missing; reinstall HellForge.");
        // the backend this session was built with (BackendPin), its files held until the session ends
        var pin = BackendPin.Check(Path.GetDirectoryName(backend)!, BuildPins.Backend, locks);
        result["backendPin"] = pin;
        if (pin == BackendPin.Refused) throw new SessionFailure(BackendPin.Refusal);
        var start = new ProcessStartInfo(backend) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (var argument in new[] { "--port", install.BackendPort.ToString(), "--startup-fixtures", "true", "--lifetime-seconds", "0", "--evidence", evidence,
                     "--profile-dir", profileDirectoryUsed, "--capture-mission-bodies", "true", "--premium-locked", PremiumLockedFile, "--mod-warbonds", ModWarbondsFile,
                     "--catalogue", GameCatalog.CopyForSession(install.Root, run) ?? throw new SessionFailure(GameCatalog.SyncFirst) }) start.ArgumentList.Add(argument);
        if (options.Template != null) { start.ArgumentList.Add("--profile-template"); start.ArgumentList.Add(options.Template); }
        if (options.Sandbox) { start.ArgumentList.Add("--sandbox"); start.ArgumentList.Add("true"); }
        WarArguments(start);
        // --modset: joiners lacking this session's mods may download them (ModSync); --mods: co-op plays with the host's
        // Local Stats (StatsShare).
        foreach (var argument in new[] { "--mods", install.Mods, "--modset", ModSetFile }) start.ArgumentList.Add(argument);
        // Experimental, the player's choice (Settings > Experimental): the game's live pipe and the session's mods, which the
        // Mods page and mod sync can change without a restart (LiveMods; a multiplayer session's pipe comes with stats
        // sharing). Without them the backend has no LiveMods, and every change waits for a restart.
        var liveChanges = ModSettings.LiveChanges(install.Mods);
        result["liveChanges"] = liveChanges;
        if (liveChanges)
            foreach (var argument in new[] { "--live-run", (string)result["runId"]!, "--session-mods", SessionModsFile, "--cli", Cli })
                start.ArgumentList.Add(argument);
        if (options.Multiplayer) MultiplayerArguments(start, realSteamApi);
        // The block permits only this port, so the backend has no other: say so when another program holds it.
        if (System.Net.NetworkInformation.IPGlobalProperties.GetIPGlobalProperties().GetActiveTcpListeners().Any(l => l.Port == install.BackendPort))
            throw new SessionFailure($"Port {install.BackendPort}, HellForge's backend port, is used by another program on this PC. Close it and start again.");
        server = Tools.Starting(backend, () => Process.Start(start)) ?? throw new SessionFailure("The local backend did not start.");
        helpers.Add(server);
        if (helpers.Error != null) result["helperJobError"] = helpers.Error;
        var stdout = new StreamWriter(Path.Combine(run, "backend-stdout.txt")) { AutoFlush = true };
        var stderr = new StreamWriter(Path.Combine(run, "backend-stderr.txt")) { AutoFlush = true };
        server.OutputDataReceived += (_, e) => { if (e.Data != null) lock (stdout) stdout.WriteLine(Privacy.Scrub(e.Data)); };
        server.ErrorDataReceived += (_, e) => { if (e.Data != null) lock (stderr) stderr.WriteLine(Privacy.Scrub(e.Data)); };
        server.BeginOutputReadLine();
        server.BeginErrorReadLine();
        WaitForBackendReady(Path.Combine(evidence, "ready.json"));
    }
}
