using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using HellForge.Core;

namespace HellForge.Session;

// PlaySession's clean-up: every teardown step runs through Try so one failure is recorded and the rest still run
// (locks released, stand-ins and GameGuard logs put back, hashes and planted mods checked, a sandbox's data copied back).
sealed partial class PlaySession
{
    void CleanUp()
    {
        StopGameJob();
        CopyEngineLogs();
        StopBrokerAndBackend();
        FinishApplier();
        RecordBackendRequests();
        if (gameStarted != null) Try("networkCensusError", () => result["networkCensus"] = NetworkCensus.Summarize(Path.Combine(gameOut, CensusLog)));
        if (hint != null && File.Exists(hint)) Try("hintError", () => File.Delete(hint));
        ReleaseHolds();
        RestoreStandIns();
        RestoreGameGuardLogs();
        CheckHashes();
        CheckPlantedMods();
        SandboxCopyBack();
        lease?.Dispose();
        result["completedUtc"] = DateTime.UtcNow.ToString("o");
        Save();
    }

    // Runs one clean-up step, recording its error in the result instead of throwing (clean-up carries on).
    void Try(string what, Action action)
    {
        try { action(); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidOperationException or System.ComponentModel.Win32Exception or SessionFailure or JsonException)
        { result[what] = error.Message; }
    }

    void StopGameJob()
    {
        if (job != null && result["gameExitCode"] == null)
            Try("gameExitCodeError", () => { job.Process.Refresh(); if (job.Process.HasExited) result["gameExitCode"] = $"0x{job.Process.ExitCode:X8}"; });
        if (job != null)
            try { job.Stop(); result["jobEmpty"] = job.ActiveProcesses == 0; }
            catch (System.ComponentModel.Win32Exception error) { result["jobStopError"] = error.Message; }
            finally { job.Dispose(); }
    }

    void CopyEngineLogs()
    {
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
    }

    void StopBrokerAndBackend()
    {
        if (broker != null)
            try
            {
                if (!broker.HasExited) { broker.Kill(); broker.WaitForExit(5000); }
                result["brokerStopped"] = broker.HasExited;
                if (broker.HasExited) result["brokerExitCode"] = broker.ExitCode;
            }
            catch (InvalidOperationException) { }
            finally { broker.Dispose(); }
        if (lowToken != 0) { LowIntegrityHelper.CloseToken(lowToken); lowToken = 0; }
        if (server != null)
            try { if (!server.HasExited) { server.Kill(); server.WaitForExit(5000); } result["backendStopped"] = server.HasExited; }
            catch (InvalidOperationException) { }
            finally { server.Dispose(); }
    }

    // The applier ends with the game.
    void FinishApplier()
    {
        if (applier != null)
        {
            applier.Join(5000);
            if (Tools.ReadJson(Path.Combine(run, "native-mods.json")) is { } status) result["nativeMods"] = status;
        }
    }

    void RecordBackendRequests()
    {
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
    }

    void ReleaseHolds()
    {
        foreach (var held in locks) held.Dispose();
        normalHoldRetry?.Dispose();
        lock (retaken)
        {
            retakeStopped = true;
            foreach (var held in retaken) held.Dispose();
        }
        modsHold?.Release();
    }

    // A loaded DLL cannot be replaced, so the originals go back only once the game is gone; otherwise the next session
    // restores them before verifying the copy.
    void RestoreStandIns()
    {
        if (swapped.Count == 0) return;
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

    void RestoreGameGuardLogs()
    {
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
    }

    void CheckHashes()
    {
        if (result["gameExeSha256"] != null)
            Try("hashError", () =>
            {
                result["copyExeUnchanged"] = Tools.Sha256(exe) == (string?)result["gameExeSha256"];
                result["copyDllUnchanged"] = Tools.Sha256(gameDll) == (string?)result["gameDllSha256"];
                if (normal != null && result["installedExeSha256"] != null)
                {
                    // several installations: a later installation's session may hold it closed by now
                    try { result["installedExeUnchanged"] = Tools.Sha256(Path.Combine(normal, "bin", "helldivers2.exe")) == (string?)result["installedExeSha256"]; }
                    catch (IOException) when (GameProcesses.MultiInstance) { result["installedExeHeldElsewhere"] = true; }
                    result["installedDllUnchanged"] = Tools.Sha256(Path.Combine(normal, "data", "game", "game.dll")) == (string?)result["installedDllSha256"];
                }
            });
    }

    // What appeared in mods/ during the session, before HellForge applies the sandboxed editor's saves.
    void CheckPlantedMods()
    {
        if (modsHold != null)
            Try("plantedModsError", () =>
            {
                if (modsHold.Check(Say) is not { } planted) return;
                result["plantedMods"] = new JsonObject
                {
                    ["plantedFiles"] = planted.Files.Count,
                    ["files"] = new JsonArray([.. planted.Files.Take(50).Select(f => (JsonNode?)f)]),
                    ["disabledMods"] = new JsonArray([.. planted.DisabledMods.Select(id => (JsonNode?)id)])
                };
                Say($"WARNING: {planted.Files.Count} file(s) appeared in the mods folder during the session" +
                    (planted.DisabledMods.Count > 0 ? $"; {planted.DisabledMods.Count} mod(s) they appeared in switched off until you turn them on in Mods." : "."));
            });
    }

    void SandboxCopyBack()
    {
        if (options.Sandbox && (bool?)result["jobEmpty"] == true && gameStarted != null)
            Try("sandboxCopyBackError", () =>
            {
                // the game's data back where the next session reads it (written by the game: files only, bounded)
                var back = SandboxFolders.Copy(SandboxUserData, install.UserData, SandboxFolders.UserDataParts(profileName));
                var environment = SandboxFolders.Copy(gameOut, install.UserData, ["environment"]);
                result["sandboxCopiedBack"] = new JsonObject
                {
                    ["files"] = back.Files + environment.Files, ["bytes"] = back.Bytes + environment.Bytes, ["skipped"] = back.Skipped + environment.Skipped
                };
                if (back.Skipped + environment.Skipped > 0)
                    Say($"{back.Skipped + environment.Skipped} file(s) the game wrote were not copied back (links, or over the size limit).");
            });
        if (options.Sandbox && (bool?)result["jobEmpty"] == true)
            Try("sandboxStatsError", () =>
            {
                var applied = SandboxStats.Apply(Path.Combine(gameOut, "sandbox-stats"), install.Mods, EntityProfile.Current);
                result["sandboxStats"] = applied == null ? "none" : new JsonObject
                {
                    ["stats"] = applied.Stats, ["modValues"] = applied.ModValues, ["refused"] = applied.Refused,
                    ["settings"] = applied.Settings, ["presets"] = applied.Presets
                };
                if (applied?.Refused > 0) Say($"{applied.Refused} in-game editor change(s) were left out: the stats file would not accept them.");
            });
    }
}
