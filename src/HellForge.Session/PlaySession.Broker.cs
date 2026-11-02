using System.Diagnostics;
using HellForge.Core;

namespace HellForge.Session;

// PlaySession's Steam broker (docs/steam-bridge.md): started for a sandboxed game, waited for before the game runs, and
// watched while it plays. The game's stand-in fails closed without the broker, so a broker that is late or cannot reach
// Steam would leave the game at STEAM NOT DETECTED: the session says why instead (SteamBrokerLog).
sealed partial class PlaySession
{
    // The broker's last lines, read on its log's thread; the flags are read by this runner's thread.
    readonly Queue<string> brokerLines = new();
    readonly ManualResetEventSlim brokerReady = new();
    volatile bool brokerConnected;
    bool brokerEndSaid;

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
        broker.ErrorDataReceived += (_, e) => { if (e.Data != null) BrokerLine(brokerLog, e.Data); };
        broker.BeginErrorReadLine();
        result["brokerPid"] = broker.Id;
    }

    // HELLFORGE_SANDBOX_OVERLAY_PROBE=1 (a diagnostic): the game's stand-in loads Steam's overlay into the sandboxed game,
    // so the census records what it is refused there (steam_cloud.c OverlayProbe, SandboxRefusals). Steam's overlay
    // otherwise loads only into the broker, which calls SteamAPI_Init.
    void SetUpOverlayProbe()
    {
        if (Environment.GetEnvironmentVariable("HELLFORGE_SANDBOX_OVERLAY_PROBE") != "1") return;
        var steam = Microsoft.Win32.Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", null) as string;
        var overlay = steam == null ? null : Path.GetFullPath(Path.Combine(steam, "GameOverlayRenderer64.dll"));
        if (overlay == null || !File.Exists(overlay))
        {
            result["overlayProbe"] = "Steam's GameOverlayRenderer64.dll was not found";
            return;
        }
        Environment.SetEnvironmentVariable("HELLFORGE_OVERLAY_PROBE", overlay);
        result["overlayProbe"] = Privacy.Scrub(overlay);
        Say("Overlay probe: Steam's overlay is loaded into the sandboxed game, to record what the sandbox refuses it.");
    }

    void BrokerLine(StreamWriter brokerLog, string line)
    {
        lock (brokerLog) brokerLog.WriteLine(line);
        lock (brokerLines)
        {
            brokerLines.Enqueue(line);
            if (brokerLines.Count > 40) brokerLines.Dequeue();
        }
        if (SteamBrokerLog.IsReady(line)) brokerReady.Set();
        if (SteamBrokerLog.IsConnected(line)) brokerConnected = true;
    }

    // The game is still suspended: it runs only once the broker has Steam and waits for it, so the stand-in's own wait for
    // the pipe never races the broker's start. A broker that ends first, or is not ready in time, is the session's failure.
    void WaitForBroker()
    {
        var waited = Stopwatch.StartNew();
        var said = false;
        while (!brokerReady.Wait(250))
        {
            if (broker!.HasExited)
            {
                broker.WaitForExit(); // the rest of its log
                result["brokerExitCode"] = broker.ExitCode;
                throw new SessionFailure(SteamBrokerLog.Failure(BrokerLines(), broker.ExitCode));
            }
            if (waited.Elapsed >= SteamBrokerLog.StartLimit) throw new SessionFailure(SteamBrokerLog.NotReady());
            if (!said && waited.ElapsedMilliseconds >= 3000)
            {
                Say("Waiting for HellForge's Steam broker to reach Steam...");
                said = true;
            }
        }
        result["brokerReadyMilliseconds"] = waited.ElapsedMilliseconds;
    }

    // Each Observe loop: a broker that ends while the game runs leaves the game without Steam; said once. The game's own
    // SteamAPI_Shutdown ends it too, as the game quits: not said.
    void CheckBroker()
    {
        if (broker == null || brokerEndSaid || !broker.HasExited) return;
        brokerEndSaid = true;
        broker.WaitForExit();
        job!.Process.Refresh();
        var lines = BrokerLines();
        if (job.Process.HasExited || SteamBrokerLog.EndedWithGame(lines)) return;
        var last = lines.LastOrDefault(l => l.StartsWith("broker: ", StringComparison.Ordinal));
        result["brokerEndedDuringPlay"] = true;
        result["brokerExitCode"] = broker.ExitCode;
        Say($"HellForge's Steam broker stopped while the game was running (exit code {broker.ExitCode}" + (last != null ? ": " + last["broker: ".Length..] : "") +
            "): the game has lost Steam until it is restarted.");
        Save();
    }

    List<string> BrokerLines()
    {
        lock (brokerLines) return [.. brokerLines];
    }
}
