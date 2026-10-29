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
