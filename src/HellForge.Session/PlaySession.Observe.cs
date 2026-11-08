using System.Diagnostics;
using System.Text;
using System.Text.Json.Nodes;
using HellForge.Core;

namespace HellForge.Session;

// PlaySession's per-second observation of a game it launched itself: Windows low-integrity sessions and ordinary play use
// this; the Linux domain uses ObserveSandbox in PlaySession.Sandbox.cs instead.
sealed partial class PlaySession
{
    void Observe(HellForgeLocalRoutingPatch.PlayFabRedirect? playFab, bool clearMatchmaking, string requestLog, int blockRules)
    {
        var clock = Stopwatch.StartNew();
        var observations = Path.Combine(run, "observations.jsonl");
        long nextSample = 0, nextBlockCheck = 10000, requestOffset = 0;
        var requestCount = 0;
        var quit = new QuitWatch();
        var hung = false;
        Say("Play session running. Close the game or press Stop in the launcher to end it.");
        while (true)
        {
            job!.Process.Refresh();
            if (job.Process.HasExited) break;
            if (WatchQuit(quit, clock, ref hung)) break;
            CheckBlock(clock, blockRules, ref nextBlockCheck);
            CheckOfflineBlock(clock);
            CheckBroker();
            RecordRedirect(playFab);
            RecordSteamInput();
            // clearMatchmaking holds the game to the genuine sign-in; ClearPlatformMatchmaking runs each loop.
            var matchmaking = clearMatchmaking ? ClearMatchmaking() : null;
            requestCount += Tools.BackendRequests(requestLog, ref requestOffset).Count;
            RecordFirstBackendRequest(requestCount);
            // Play sessions keep only the periodic samples so the log stays small; the engine's own diagnostics are kept
            // only under HELLFORGE_DEBUG.
            if (clock.ElapsedMilliseconds >= nextSample)
            {
                WriteSample(observations, clock.ElapsedMilliseconds, requestCount, matchmaking);
                nextSample = clock.ElapsedMilliseconds + 30000;
            }
            if (StopRequested())
            {
                // Stop pressed on a game without a window, or with a hung one: what it was doing, for the logs.
                if (hung || quit.WindowlessMilliseconds(clock.ElapsedMilliseconds) > 0)
                    result["gameAtStop"] = GameWindows.Snapshot(job.Process, gameStarted ?? DateTime.UtcNow);
                break;
            }
            Thread.Sleep(1000);
        }
        RecordEnd(clock);
    }

    // The game closed its window and did not exit (QuitWatch): true when the session should end it now. A hung window
    // is noted once per hang. Either way, what the game's threads were doing goes into the result.
    bool WatchQuit(QuitWatch quit, Stopwatch clock, ref bool hung)
    {
        var (visible, isHung) = GameWindows.Visible(job!.Process.Id, quit.ShownWindows);
        hung = isHung;
        switch (quit.Update(clock.ElapsedMilliseconds, visible, isHung))
        {
            case QuitWatch.Verdict.HungNote:
                Say($"The game's window has not responded for {QuitWatch.HungNoteMilliseconds / 1000} s (still waiting; press Stop in the launcher to end it).");
                result["gameHung"] = GameWindows.Snapshot(job.Process, gameStarted ?? DateTime.UtcNow);
                Save();
                return false;
            case QuitWatch.Verdict.StuckQuitting:
                var snapshot = GameWindows.Snapshot(job.Process, gameStarted ?? DateTime.UtcNow);
                result["stuckQuitting"] = snapshot;
                var holders = (snapshot["holders"] as JsonArray)?.Select(h => (string?)h?["name"]).ToList() ?? [];
                Say($"The game closed its window but was still running {QuitWatch.GraceMilliseconds / 1000} s later: HellForge ends it and finishes " +
                    $"the session ({snapshot["threadCount"]} thread(s) left{(holders.Count > 0 ? ", held open by " + string.Join(", ", holders) : "")}).");
                // Ended here, at once: the game has no window left to ask to close, so the graceful close STOP gives a
                // game (the clean-up's, for a stop the player asked for) would only add its wait.
                try
                {
                    result["jobActiveAtStop"] = job.ActiveProcesses;
                    job.Stop();
                }
                catch (System.ComponentModel.Win32Exception error) { result["jobStopError"] = error.Message; } // the clean-up stops it again
                return true;
            default:
                return false;
        }
    }

    // How long the game took to its first backend request: past its black screen (a shader cache build, ShaderCache).
    // Both observation loops (this one and the Linux domain's) record it.
    void RecordFirstBackendRequest(int requestCount)
    {
        if (requestCount <= ((int?)result["preLaunchBackendRequests"] ?? 0) || result["firstBackendRequestSeconds"] != null || gameStarted is not { } started) return;
        result["firstBackendRequestSeconds"] = Math.Round((DateTime.UtcNow - started).TotalSeconds, 1);
        Save();
    }

    // The persistent block is re-read every 10 seconds; the game is stopped if a rule or profile goes away.
    void CheckBlock(Stopwatch clock, int blockRules, ref long nextBlockCheck)
    {
        if (clock.ElapsedMilliseconds < nextBlockCheck) return;
        // blockRules is 0 only in the WFP-only mode, which has no firewall rules to re-read.
        if (blockRules > 0 && BlockRuleCount() != blockRules) throw new SessionFailure("Network block rules or firewall profiles changed during the session.");
        nextBlockCheck = clock.ElapsedMilliseconds + 10000;
    }

    // The network block's probe while the session started offline (NoteOffline), and the watch that says when it runs
    // and what its result means (OfflineBlockWatch).
    string? offlineProbe;
    OfflineBlockWatch? offlineWatch;

    // The block held on an offline PC (NetworkBlockProbe.Offline, ticket 0352): proved without the outside TCP connect
    // being refused, so the probe runs again while the game runs (CheckOfflineBlock).
    void NoteOffline(string probe)
    {
        if (!NetworkBlockProbe.Offline(result["networkControl"])) return;
        offlineProbe = probe;
        offlineWatch = new OfflineBlockWatch(NetworkBlockProbe.BestInterface());
        result["networkBlockOffline"] = true;
        result["networkControlOffline"] = result["networkControl"]?.DeepClone();
        Say("This PC has no internet connection: the network block is checked without it now, and against the internet as soon as the PC connects.");
    }

    // Started offline (NoteOffline): the probe runs again every 10 seconds, and at once when the outside's interface
    // changes. Still offline keeps the session watched; the outside TCP connect refused proves the block online and ends the
    // watch. A failure is checked once more at once, since a connect can outlast the probe's 2.5 seconds while Windows first
    // looks for a next hop that does not answer; failing twice (the connect got out or waited, the datagram went out, or the
    // block no longer holds) fails the session, and its clean-up stops the game.
    void CheckOfflineBlock(Stopwatch clock)
    {
        if (offlineProbe == null || offlineWatch == null || !offlineWatch.Due(clock.ElapsedMilliseconds, NetworkBlockProbe.BestInterface())) return;
        var problem = RunNetworkProbe(offlineProbe, false, true, "network-probe-online.json");
        if (problem != null) problem = RunNetworkProbe(offlineProbe, false, true, "network-probe-online-again.json");
        offlineWatch.Ran(clock.ElapsedMilliseconds);
        switch (OfflineBlockWatch.Judge(problem, result["networkControl"]))
        {
            case OfflineBlockWatch.Verdict.StillOffline:
                return;
            case OfflineBlockWatch.Verdict.Failed:
                throw new SessionFailure($"The network block's check failed while the game was running ({problem}). " +
                    "HellForge stopped the game: press Play to check the block again.");
        }
        offlineProbe = null;
        offlineWatch = null;
        result["networkBlockProvenOnline"] = true;
        Save();
        Say("This PC is online now, and the network block stops the game's connections to the internet.");
    }

    void RecordRedirect(HellForgeLocalRoutingPatch.PlayFabRedirect? playFab)
    {
        if (!Debug || playFab == null || result["playFabRedirect"] != null || playFab.Outcome == "waiting") return;
        result["playFabRedirect"] = new JsonObject { ["outcome"] = playFab.Outcome, ["foundMilliseconds"] = playFab.FoundMilliseconds, ["samples"] = playFab.Samples };
        Save();
    }

    // Sandboxed: the controllers Steam Input handles, which cannot reach the game (docs/steam-bridge.md, "Controllers and
    // Steam Input"), from the broker's log into result.json for the launcher and the CLI, and the note into the log.
    void RecordSteamInput()
    {
        var controllers = Volatile.Read(ref steamInput);
        if (controllers == null || ReferenceEquals(controllers, steamInputRecorded)) return;
        steamInputRecorded = controllers;
        var record = SteamInputNote.Record(controllers);
        result["steamInput"] = record;
        Save();
        if ((string?)record["note"] is { } note) Say(note);
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
        // "stop-now" (SessionRunner.SignalStop now: true): the game is ended at once, without asking it to close; the
        // backend's mod sync stops the session to start it again with the host's mods: the launcher does that.
        if (SessionEnd.MarkStop(result, ReadSignal())) endGameNow = true;
        return true;
    }

    // Preparation and launch are as interruptible as observation: a signal there ends the session normally, through
    // cleanup, instead of leaving the launcher waiting for a game that never starts.
    void ThrowIfStopRequested()
    {
        if (StopRequested()) throw new SessionStopped();
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
