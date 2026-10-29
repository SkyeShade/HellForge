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
        try
        {
            StopGameJob();
            Try("sandboxPipeRelayStopError", StopSandboxPipeRelay);
            CopyEngineLogs();
            StopBrokerAndBackend();
            FinishApplier();
            RecordBackendRequests();
            if (gameStarted != null)
                Try("networkCensusError", () =>
                {
                    var census = Path.Combine(gameOut, CensusLog);
                    if (OperatingSystem.IsLinux()) census = LocalInstall.NoLinks(census, gameOut);
                    result["networkCensus"] = NetworkCensus.Summarize(census);
                });
            if (hint != null && File.Exists(hint)) Try("hintError", () => File.Delete(hint));
            Try("releaseHoldsError", ReleaseHolds);
            RestoreStandIns();
            RestoreGameGuardLogs();
            CheckHashes();
            CheckPlantedMods();
            SandboxCopyBack();
        }
        catch (Exception error)
        {
            // No clean-up step should get this far, but a result that says cleanup was incomplete is still a result.
            result["cleanupError"] = error.Message;
            Say("Cleanup stopped early: " + error.Message);
        }
        finally
        {
            try { lease?.Dispose(); }
            catch (Exception error) { result["leaseDisposeError"] = error.Message; }
            result["completedUtc"] = DateTime.UtcNow.ToString("o");
            try { Save(); }
            catch (Exception error) { Say("The final result could not be written: " + error.Message); }
        }
    }

    // Runs one clean-up step, recording its error in the result instead of throwing (clean-up carries on).
    void Try(string what, Action action)
    {
        try { action(); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException or InvalidOperationException or
                                         System.ComponentModel.Win32Exception or SessionFailure or JsonException)
        { result[what] = error.Message; }
    }

    void StopGameJob()
    {
        if (linuxSandbox) { StopSandboxDomain(); return; }
        if (job != null && result["gameExitCode"] == null)
            Try("gameExitCodeError", () => { job.Process.Refresh(); if (job.Process.HasExited) result["gameExitCode"] = $"0x{job.Process.ExitCode:X8}"; });
        if (job == null) return;
        var current = job;
        // TerminateJobObject normally returns at once. If it does not, cleanup must still finish and write its result.
        var stop = Task.Run(() => current.Stop());
        bool completed;
        try { completed = stop.Wait(TimeSpan.FromSeconds(10)); }
        catch (AggregateException error)
        {
            result["jobStopError"] = error.InnerException?.Message ?? error.Message;
            try { current.Dispose(); }
            catch (Exception disposeError) when (disposeError is System.ComponentModel.Win32Exception or InvalidOperationException) { result["jobDisposeError"] = disposeError.Message; }
            return;
        }
        if (!completed)
        {
            result["jobStopTimeout"] = true;
            Say("Stopping the game job is taking longer than 10 seconds; cleanup goes on without it.");
            return;
        }
        try { result["jobEmpty"] = current.ActiveProcesses == 0; }
        catch (System.ComponentModel.Win32Exception error) { result["jobStopError"] = error.Message; }
        finally
        {
            try { current.Dispose(); }
            catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException) { result["jobDisposeError"] = error.Message; }
        }
    }

    // STOP and normal cleanup both go through the domain's own job. The supervisor's process-group timeout remains the
    // second path when the host does not answer.
    void StopSandboxDomain()
    {
        result["jobEmpty"] = false;
        var control = Path.Combine(run, "control");
        if (gameStarted == null) { RequestSandboxGameStop(); return; }
        var status = ReadDomainStatus();
        if (status is not { State: "exited" or "stopped" }) RequestSandboxGameStop();
        var deadline = DateTime.UtcNow.AddSeconds(25);
        while (DateTime.UtcNow < deadline)
        {
            status = ReadDomainStatus();
            if (status is { State: "exited" or "stopped" }) break;
            if (File.Exists(Path.Combine(control, "exit.json"))) break;
            Thread.Sleep(250);
        }
        status = ReadDomainStatus();
        if (status is { State: "exited" or "stopped" })
        {
            result["jobEmpty"] = status.JobEmpty;
            result["sandboxDomainState"] = status.State;
            result["gameExitedNaturally"] = status.State == "exited";
            result["gamePid"] = status.GamePid;
            result["sandboxDomainPid"] = status.DomainPid;
            if (status.ExitCode is { } code) result["gameExitCode"] = $"0x{code:X8}";
            if (status.Failure is { Length: > 0 } failure) result["sandboxDomainFailure"] = failure;
        }
        else
        {
            result["sandboxStopTimeout"] = true;
            Say("The Linux sandbox domain did not finish within 25 seconds; cleanup goes on without it.");
        }
        if (Tools.ReadJson(Path.Combine(control, "exit.json")) is JsonObject exit) result["sandboxExit"] = exit.DeepClone();
    }

    // A stop file is seen by the domain host even if it starts after cleanup wrote it. The supervisor's own parent/deadline
    // paths remain the fallback when no domain host ever starts.
    void RequestSandboxGameStop()
    {
        try
        {
            var stop = Path.Combine(gameOut, "domain", "stop");
            Directory.CreateDirectory(Path.GetDirectoryName(stop)!);
            File.WriteAllText(stop, "stop\n");
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { result["sandboxStopError"] = error.Message; }
    }

    void CopyEngineLogs()
    {
        if (options.EngineLog && gameStarted is { } started)
            Try("engineLogError", () =>
            {
                var engineLogs = Path.Combine(run, "engine-logs");
                var found = EngineLogFiles(started);
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

    // The logs and text files this session wrote, in the game copy and in every place the game's profile can write.
    List<FileInfo> EngineLogFiles(DateTime started)
    {
        string?[] places = [Path.Combine(copy, "bin"), profileFolder, Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "Arrowhead", "HellForge"), install.UserData];
        var found = new List<FileInfo>();
        foreach (var place in places)
        {
            if (place == null || !Directory.Exists(place)) continue;
            foreach (var file in Directory.EnumerateFiles(place, "*", SearchOption.AllDirectories))
            {
                var full = ReadableLogFile(file, place);
                if (full == null) continue;
                var info = new FileInfo(full);
                if ((full.EndsWith(".log", StringComparison.OrdinalIgnoreCase) || full.EndsWith(".txt", StringComparison.OrdinalIgnoreCase)) &&
                    !full.StartsWith(run, StringComparison.OrdinalIgnoreCase) && info.LastWriteTimeUtc >= started && info.Length < 200L * 1024 * 1024)
                    found.Add(info);
            }
        }
        return found;
    }

    // One place is a sandbox copy the game can write: never read a log through a link it planted below that place. A link
    // skips the file; the rest of the logs are still copied.
    static string? ReadableLogFile(string file, string place)
    {
        if (!OperatingSystem.IsLinux()) return file;
        try { return LocalInstall.NoLinks(file, place); }
        catch (InvalidDataException) { return null; }
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
        if (linuxSandbox)
        {
            LinuxSandboxCopyBack();
            if (!string.IsNullOrEmpty(run))
                Try("sandboxStateCleanupError", () => SandboxFolders.RemoveSessionState(run));
            return;
        }
        if (options.Sandbox && (bool?)result["jobEmpty"] == true && gameStarted != null)
            Try("sandboxCopyBackError", () =>
            {
                // the game's data back where the next session reads it (written by the game: files only, bounded)
                var back = SandboxFolders.Copy(SandboxUserData, install.UserData, SandboxFolders.UserDataParts(profileName), fromSession: true);
                var environment = SandboxFolders.Copy(gameOut, install.UserData, ["environment"], fromSession: true);
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
        // The overlay upper/work are outside gameOut and no longer read: remove <run>/sandbox, including work/work.
        if (options.Sandbox && !string.IsNullOrEmpty(run))
            Try("sandboxStateCleanupError", () => SandboxFolders.RemoveSessionState(run));
    }

    // The Linux copy-back asks the already-running native supervisor to stage only regular, singly linked files. This
    // Wine-side code then merges that trusted staging folder; it never walks gameOut itself.
    void LinuxSandboxCopyBack()
    {
        if ((bool?)result["jobEmpty"] != true || gameStarted == null)
        {
            result["sandboxCopyBackSkipped"] = "the domain did not report an empty job";
            return;
        }
        Try("sandboxCopyBackError", () =>
        {
            var control = Path.Combine(run, "control");
            var request = Path.Combine(control, "copy-back.request");
            var partial = request + ".partial";
            File.WriteAllText(partial, new JsonObject
            {
                ["runId"] = (string?)result["runId"], ["nonce"] = sandboxNonce, ["profile"] = profileName
            }.ToJsonString());
            File.Move(partial, request, overwrite: true);
            var answer = Path.Combine(control, "copy-back.json");
            var deadline = DateTime.UtcNow.AddSeconds(60);
            while (!File.Exists(answer) && DateTime.UtcNow < deadline) Thread.Sleep(200);
            if (Tools.ReadJson(answer) is not JsonObject receipt) throw new SessionFailure("The native sandbox copy-back did not answer.");
            if ((bool?)receipt["ok"] != true) throw new SessionFailure("The native sandbox copy-back failed: " + ((string?)receipt["error"] ?? "no reason"));
            var staging = Path.Combine(run, "copy-back");
            var back = SandboxFolders.Copy(Path.Combine(staging, "userdata"), install.UserData, SandboxFolders.UserDataParts(profileName));
            var environment = SandboxFolders.Copy(Path.Combine(staging, "game"), install.UserData, ["environment"]);
            result["sandboxCopiedBack"] = new JsonObject
            {
                ["files"] = back.Files + environment.Files, ["bytes"] = back.Bytes + environment.Bytes,
                ["skipped"] = back.Skipped + environment.Skipped, ["nativeSkipped"] = (int?)receipt["skipped"] ?? 0
            };
            var applied = SandboxStats.Apply(Path.Combine(staging, "game", "sandbox-stats"), install.Mods, EntityProfile.Current);
            result["sandboxStats"] = applied == null ? "none" : new JsonObject
            {
                ["stats"] = applied.Stats, ["modValues"] = applied.ModValues, ["refused"] = applied.Refused,
                ["settings"] = applied.Settings, ["presets"] = applied.Presets
            };
            CopySandboxEvidence(Path.Combine(staging, "game", "domain"));
            LocalInstall.NoLinks(staging, run);
            Directory.Delete(staging, recursive: true);
            result["sandboxNativeCopyBack"] = receipt.DeepClone();
            if (back.Skipped + environment.Skipped + ((int?)receipt["skipped"] ?? 0) > 0)
                Say($"{back.Skipped + environment.Skipped + ((int?)receipt["skipped"] ?? 0)} file(s) the game wrote were not copied back (links, special files, hard links or over the size limit).");
        });
    }

    void CopySandboxEvidence(string folder)
    {
        foreach (var name in new[] { "native-mods.log", "native-mods.json" })
        {
            var source = Path.Combine(folder, name);
            if (!File.Exists(source)) continue;
            var target = Path.Combine(run, name);
            File.Copy(source, target, overwrite: true);
            if (name.EndsWith(".json", StringComparison.Ordinal) && Tools.ReadJson(target) is { } status) result["nativeMods"] = status.DeepClone();
        }
    }
}
