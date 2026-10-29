using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.Core;

// The launcher's Sync Inventory: captures the player's own HELLDIVERS 2 inventory and progression into a profile's account
// snapshot. The local backend takes premium ownership from it on every start; the rest reaches the profile only through
// an import the player reviews (AccountImportSettings, HellForge.LocalBackend AccountImport).
//
// The capture proxy terminates TLS for the normal game's login and, with that login's session, reads /api/Progression
// and /api/Progression/inventory. It seals both for the signed-in Steam account (SnapshotSeal) as soon as the inventory
// is in, keeps forwarding the game's traffic until the game exits, and then removes its redirect and CA. The files land
// in a private sync folder under logs/, and FinalizeCapture moves them into account-snapshot/<profile>/ once the
// capture's state (CaptureState) says they are sealed, while the game plays on.
//
// Windows keeps the elevated app/capture/HellForge.CaptureProxy.exe, its hosts file and its LocalMachine Root store.
// Native Linux starts the unprivileged app/capture-linux proxy below the installed root helper, which owns the marked
// /etc/hosts lines and the 127.0.0.1:443 relay, and the supervisor imports the proxy's public CA into HELLDIVERS 2's
// prefix Root store through Wine's reg.exe.
public static class InventorySync
{
    public const string SnapshotFolder = "account-snapshot";
    public const string InventoryFile = SnapshotSeal.InventoryFile;
    public const string ProgressionFile = SnapshotSeal.ProgressionFile;
    public const string MetadataFile = "snapshot.json";
    // Written by the proxy once it listens; the launcher opens the game only then.
    public const string ListeningFile = CaptureState.FileName;
    // The proxy's own line in the hosts file, above its mapping.
    public const string HostsMarker = CaptureHosts.Marker;
    public const string ProxyExe = "HellForge.CaptureProxy.exe";
    public const string LinuxProxyExe = "HellForge.CaptureProxy";
    public const int TimeoutMinutes = 10;
    const string GameProcess = "helldivers2";

    public static string? FindCaptureProxy(string? installRoot)
    {
        if (installRoot == null) return null;
        var app = new InstallLayout(installRoot).App;
        var path = OperatingSystem.IsLinux() && !LinuxNetworkBlock.UnderWine
            ? Path.Combine(app, "capture-linux", LinuxProxyExe)
            : Path.Combine(app, "capture", ProxyExe);
        return File.Exists(path) ? path : null;
    }

    public static string SnapshotDirectory(string installRoot, string profileName) =>
        Path.Combine(installRoot, SnapshotFolder, profileName);

    // profiles/<name> -> account-snapshot/<name>, both under the installation root.
    public static string? SnapshotDirectoryFromProfileDir(string profileDir)
    {
        var full = Path.GetFullPath(profileDir.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
        var profileName = Path.GetFileName(full);
        var installRoot = Path.GetDirectoryName(Path.GetDirectoryName(full));
        if (installRoot == null || string.IsNullOrEmpty(profileName)) return null;
        return Path.Combine(installRoot, SnapshotFolder, profileName);
    }

    // One key per installation, beside every profile's snapshot.
    public static string SealKeyFor(string snapshotDir) =>
        Path.Combine(Path.GetDirectoryName(Path.GetFullPath(snapshotDir.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)))!,
            SnapshotSeal.KeyFileName);

    // The inventory makes a snapshot; progression is applied when it was captured too.
    public static bool HasSnapshot(string snapshotDir) => File.Exists(Path.Combine(snapshotDir, InventoryFile));

    public static SnapshotInfo? ReadMetadata(string snapshotDir)
    {
        var path = Path.Combine(snapshotDir, MetadataFile);
        if (!File.Exists(path)) return null;
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(path));
            var root = doc.RootElement;
            DateTime? captured = root.TryGetProperty("capturedUtc", out var utc) && utc.ValueKind == JsonValueKind.String &&
                DateTime.TryParse(utc.GetString(), CultureInfo.InvariantCulture, DateTimeStyles.RoundtripKind, out var when) ? when : null;
            var items = root.TryGetProperty("items", out var count) && count.ValueKind == JsonValueKind.Number && count.TryGetInt32(out var n) ? n : 0;
            long? experience = root.TryGetProperty("experience", out var xp) && xp.ValueKind == JsonValueKind.Number && xp.TryGetInt64(out var value) ? value : null;
            // Snapshots from before the flag had progression exactly when they had XP.
            var progression = root.TryGetProperty("progression", out var flag) && flag.ValueKind is JsonValueKind.True or JsonValueKind.False
                ? flag.GetBoolean() : experience != null;
            return new SnapshotInfo(captured, items, experience, progression);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException)
        {
            return null;
        }
    }

    public static bool IsGameRunning()
    {
        var running = Process.GetProcessesByName(GameProcess);
        if (OperatingSystem.IsLinux() && !LinuxNetworkBlock.UnderWine)
            running = [.. running, .. Process.GetProcessesByName(GameProcess + ".exe")];
        foreach (var process in running) process.Dispose();
        return running.Length > 0;
    }

    // The hosts file the running platform reads. Wine programs resolve through the host resolver (review-C3.md Part 2),
    // so native Linux checks /etc/hosts; Windows keeps its system hosts path byte-for-byte.
    public static string HostsPath => OperatingSystem.IsWindows() && !LinuxNetworkBlock.UnderWine
        ? Path.Combine(Environment.SystemDirectory, "drivers", "etc", "hosts")
        : "/etc/hosts";

    // A capture that was killed before its cleanup leaves the live API host pointed at loopback, so the normal game
    // cannot reach Arrowhead's servers until the helper cleans up.
    public static bool HasLeftoverRedirect() => HasLeftoverRedirect(HostsPath);

    public static bool HasLeftoverRedirect(string hostsPath)
    {
        try
        {
            return File.Exists(hostsPath) && File.ReadLines(hostsPath).Any(line => line.Contains(HostsMarker, StringComparison.OrdinalIgnoreCase));
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            return false;
        }
    }

    // Starts the platform's proxy, sealing what it captures for steamAccount. Windows blocks until its one UAC prompt is
    // answered. Native Linux starts the unprivileged proxy, proves its CA is in the game's prefix, then starts the root
    // relay. Call it off the UI thread.
    public static CaptureRun? StartCaptureProxy(string installRoot, string proxyPath, string syncDir, string snapshotDir,
        ulong steamAccount, bool sessionRunning)
    {
        Directory.CreateDirectory(syncDir);
        if (OperatingSystem.IsLinux() && !LinuxNetworkBlock.UnderWine)
            return StartLinuxCapture(installRoot, proxyPath, syncDir, snapshotDir, steamAccount, sessionRunning);
        var started = StartElevated(proxyPath, [
            "--timeout-min", TimeoutMinutes.ToString(CultureInfo.InvariantCulture),
            "--output", Path.Combine(syncDir, InventoryFile),
            "--progression", Path.Combine(syncDir, ProgressionFile),
            "--evidence", syncDir,
            "--seal-key", SealKeyFor(snapshotDir),
            "--steam-account", steamAccount.ToString(CultureInfo.InvariantCulture)]);
        return started == null ? null : new CaptureRun(started, null, null);
    }

    // The early check runs before Steam or DNS is touched, so a running game or session refuses without side effects; the
    // second check closes the race between that look and the prefix change.
    internal static CaptureRun StartLinuxCapture(string installRoot, string proxyPath, string syncDir, string snapshotDir,
        ulong steamAccount, bool sessionRunning, Func<bool>? gameRunning = null)
    {
        gameRunning ??= IsGameRunning;
        if (LinuxCapture.RunRefusal(gameRunning(), sessionRunning) is { } running)
            throw new InvalidOperationException(running);
        var steamRoot = SteamLocator.SteamRoot(new InstallLayout(installRoot));
        var prefix = LinuxCapture.FindPrefix(steamRoot);
        var proton = LinuxCapture.ProtonForGame(steamRoot);
        var address = LinuxCapture.CurrentAddress();
        if (steamRoot == null)
            throw new InvalidOperationException("Steam was not found: start Steam and sign in, then sync again.");
        if (LinuxCapture.StartRefusal(installRoot, prefix, proton, address, gameRunning(), sessionRunning) is { } refusal)
            throw new InvalidOperationException(refusal);

        LinuxCapture.RestrictPrivateDirectory(syncDir);
        var certificatePath = LinuxCapture.CertificatePath(syncDir);
        Process? proxy = null;
        LinuxCapture.HelperRun? helper = null;
        LinuxCapture.CertificateRecord? record = null;
        try
        {
            proxy = Process.Start(LinuxProxyStartInfo(proxyPath, syncDir, snapshotDir, steamAccount, address!, certificatePath))
                ?? throw new InvalidOperationException("The Linux capture proxy did not start.");
            WaitForLinuxProxy(proxy, certificatePath, syncDir);
            record = new(prefix!, proton!, steamRoot, certificatePath,
                LinuxCapture.CertificateThumbprint(File.ReadAllBytes(certificatePath)));
            LinuxCapture.WriteCertificateRecord(syncDir, record);
            LinuxCapture.InstallCertificate(record);
            helper = LinuxCapture.StartHelper(installRoot, proxy.Id, syncDir);
            return new CaptureRun(proxy!, helper, syncDir);
        }
        catch
        {
            AbortLinuxCapture(proxy, helper, record, syncDir);
            throw;
        }
    }

    static ProcessStartInfo LinuxProxyStartInfo(string proxyPath, string syncDir, string snapshotDir, ulong steamAccount,
        string address, string certificatePath)
    {
        var start = new ProcessStartInfo(proxyPath) { WorkingDirectory = syncDir, UseShellExecute = false, CreateNoWindow = true };
        foreach (var argument in new[]
        {
            "--host", LinuxCapture.ApiHost,
            "--timeout-min", TimeoutMinutes.ToString(CultureInfo.InvariantCulture),
            "--output", Path.Combine(syncDir, InventoryFile),
            "--progression", Path.Combine(syncDir, ProgressionFile),
            "--evidence", syncDir,
            "--seal-key", SealKeyFor(snapshotDir),
            "--steam-account", steamAccount.ToString(CultureInfo.InvariantCulture),
            "--no-hosts", "--listen-port", LinuxCapture.ProxyPort.ToString(CultureInfo.InvariantCulture),
            "--real-ip", address, "--ca-cert", certificatePath
        })
            start.ArgumentList.Add(argument);
        return start;
    }

    static void WaitForLinuxProxy(Process proxy, string certificatePath, string syncDir)
    {
        var deadline = DateTime.UtcNow.AddSeconds(20);
        while (!File.Exists(certificatePath) || !IsListening(syncDir))
        {
            if (proxy.HasExited) throw new InvalidOperationException("The Linux capture proxy stopped before it listened. " + ProxyLogTail(syncDir));
            if (DateTime.UtcNow >= deadline) throw new InvalidOperationException("The Linux capture proxy did not start within 20 seconds. " + ProxyLogTail(syncDir));
            Thread.Sleep(100);
        }
    }

    static void AbortLinuxCapture(Process? proxy, LinuxCapture.HelperRun? helper, LinuxCapture.CertificateRecord? record, string syncDir)
    {
        try { if (proxy is { HasExited: false }) proxy.Kill(entireProcessTree: true); } catch (InvalidOperationException) { }
        proxy?.Dispose();
        helper?.Dispose();
        if (record != null && LinuxCapture.RemoveCertificate(record) == null)
            LinuxCapture.DeleteCertificateRecord(syncDir);
    }

    static string ProxyLogTail(string syncDir)
    {
        var path = Path.Combine(syncDir, "proxy.log");
        try
        {
            return File.Exists(path) ? "Last log: " + string.Join(" | ", File.ReadLines(path).TakeLast(3)) : "It wrote no log.";
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return "Its log could not be read."; }
    }

    // Removes the Linux host helper's leftovers and every recorded prefix certificate. Null on success.
    public static string? CleanupLinux(string installRoot) => LinuxCapture.Cleanup(installRoot);

    public static bool HasPendingLinuxCleanup(string installRoot) =>
        HasLeftoverRedirect() || LinuxCapture.PendingCertificateFolders(installRoot).Count > 0;

    // A Windows capture proxy, or the native Linux proxy plus the root relay that owns /etc/hosts and 443. The launcher
    // treats either as one run: HasExited and ExitCode read the proxy; Dispose ends the Linux helper and removes its CA.
    public sealed class CaptureRun : IDisposable
    {
        readonly Process proxy;
        readonly string? syncDir;
        LinuxCapture.HelperRun? helper;
        int disposed;

        internal CaptureRun(Process proxy, LinuxCapture.HelperRun? helper, string? syncDir)
        {
            this.proxy = proxy;
            this.helper = helper;
            this.syncDir = syncDir;
        }

        public bool HasExited => proxy.HasExited;
        public int ExitCode => proxy.ExitCode;
        public int Id => proxy.Id;

        public void Dispose()
        {
            if (Interlocked.Exchange(ref disposed, 1) == 1) return;
            proxy.Dispose();
            if (syncDir == null) { helper?.Dispose(); helper = null; return; }
            string? stop = null;
            try { stop = LinuxCapture.StopHelper(); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidOperationException)
            {
                stop = error.Message;
            }
            helper?.Dispose();
            helper = null;
            var folder = syncDir;
            _ = Task.Run(() =>
            {
                var problem = stop == null ? LinuxCapture.RemoveRecordedCertificate(folder) : LinuxCapture.Finish(folder);
                if (problem != null) Note(folder, "Linux capture cleanup: " + problem);
            });
        }
    }

    // Removes a leftover hosts line and CA (one UAC prompt). Null when elevation was declined.
    public static Process? StartCleanup(string proxyPath) => StartElevated(proxyPath, ["--cleanup"]);

    static Process? StartElevated(string path, string[] arguments)
    {
        var start = new ProcessStartInfo(path) { UseShellExecute = true, Verb = "runas", WindowStyle = ProcessWindowStyle.Hidden };
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        try
        {
            return Process.Start(start);
        }
        catch (Win32Exception)
        {
            return null;
        }
    }

    public static bool IsListening(string syncDir) => File.Exists(Path.Combine(syncDir, ListeningFile));

    // The launcher's side of a sync, beside the capture's proxy.log: one timestamped line per step. A folder the launcher
    // cannot write (a standard account, where the capture runs as the administrator) only loses these lines.
    public const string LauncherLogFile = "launcher.log";

    public static void Note(string syncDir, string message)
    {
        try { File.AppendAllText(Path.Combine(syncDir, LauncherLogFile), DateTime.Now.ToString("HH:mm:ss.fff", CultureInfo.InvariantCulture) + " " + message + Environment.NewLine); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Trace.WriteLine("sync note lost: " + error.Message); }
    }

    // The normal game's %APPDATA%. Windows uses this process's profile. Native Linux must use the game's Proton prefix:
    // the launcher's own ApplicationData is ~/.config, not drive_c/users/steamuser/AppData/Roaming.
    static string GameAppData()
    {
        if (OperatingSystem.IsLinux() && !LinuxNetworkBlock.UnderWine)
            try
            {
                if (LinuxCapture.FindPrefix() is { } prefix)
                    return Path.Combine(LinuxCapture.WinePrefix(prefix), "drive_c", "users", "steamuser", "AppData", "Roaming");
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        return Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
    }

    // Copies Easy Anti-Cheat's launcher logs written since the sync started (under %APPDATA%\EasyAntiCheat, one per game
    // deployment) into the sync folder as eac-<deployment>.log, and says whether the game wrote a crash report. Returns
    // what it found, for launcher.log.
    public static IReadOnlyList<string> CollectGameLogs(string syncDir, DateTime sinceUtc, string? appData = null)
    {
        appData ??= GameAppData();
        var found = new List<string>();
        var eac = Path.Combine(appData, "EasyAntiCheat");
        var logs = Directory.Exists(eac) ? Directory.EnumerateFiles(eac, "anticheatlauncher.log", SearchOption.AllDirectories) : [];
        foreach (var log in logs.Where(file => File.GetLastWriteTimeUtc(file) >= sinceUtc))
        {
            var copy = Path.Combine(syncDir, "eac-" + Path.GetFileName(Path.GetDirectoryName(log)) + ".log");
            try
            {
                File.Copy(log, copy, overwrite: true);
                found.Add("Easy Anti-Cheat launcher log copied to " + Path.GetFileName(copy));
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { found.Add("Easy Anti-Cheat launcher log not copied: " + error.Message); }
        }
        if (found.Count == 0) found.Add("no Easy Anti-Cheat launcher log written since the sync started");
        var crashes = Path.Combine(appData, "Arrowhead", "Helldivers2", "crash_data");
        var reports = Directory.Exists(crashes) ? Directory.EnumerateFileSystemEntries(crashes, "*", SearchOption.AllDirectories)
            .Where(entry => File.GetLastWriteTimeUtc(entry) >= sinceUtc).ToList() : [];
        found.Add(reports.Count == 0 ? "no game crash report since the sync started"
            : $"the game wrote {reports.Count} crash report file(s) since the sync started, under " + crashes);
        return found;
    }

    // The capture has sealed its files and keeps forwarding until the game exits: FinalizeCapture can take them now.
    public static bool IsSealed(string syncDir) => CaptureState.Read(syncDir) is { Sealed: true };

    public static void OpenVanillaGame() =>
        Process.Start(new ProcessStartInfo("steam://rungameid/553850") { UseShellExecute = true })?.Dispose();

    // Once the proxy has sealed its capture (or exited with success): moves its sealed files into the snapshot folder and
    // writes the metadata.
    // Returns null on success, or what went wrong. A previous snapshot is replaced only by a complete, sealed new one; an
    // inventory-only capture (a failed sync) replaces one without progression, never one with it.
    public static string? FinalizeCapture(string syncDir, string snapshotDir)
    {
        if (ReadInventory(Path.Combine(syncDir, InventoryFile), out var items) is { } inventoryProblem) return inventoryProblem;
        if (ReadProgression(Path.Combine(syncDir, ProgressionFile), out var experience) is { } progressionProblem) return progressionProblem;
        if (SnapshotSeal.Verify(syncDir, SealKeyFor(snapshotDir), out var sealProblem) == null)
            return sealProblem ?? "The capture is not sealed for this PC. Sync again.";
        // A capture without progression is a failed sync: the last complete snapshot keeps the level and unlocks it imported.
        if (!File.Exists(Path.Combine(syncDir, ProgressionFile)) && ReadMetadata(snapshotDir) is { Progression: true } kept)
            return "The capture got your inventory but not your progression, and the snapshot from " +
                (kept.CapturedUtc?.ToLocalTime().ToString("g", CultureInfo.CurrentCulture) ?? "an earlier sync") +
                " has one: the last snapshot is kept. Sync again.";
        try
        {
            Directory.CreateDirectory(snapshotDir);
            // A file from an earlier capture must not stay beside newer ones: progression may be missing from this one.
            foreach (var file in new[] { InventoryFile, ProgressionFile, SnapshotSeal.FileName })
            {
                var from = Path.Combine(syncDir, file);
                var to = Path.Combine(snapshotDir, file);
                if (File.Exists(from)) File.Move(from, to, overwrite: true);
                else File.Delete(to);
            }
            var metadata = new JsonObject
            {
                ["capturedUtc"] = DateTime.UtcNow.ToString("O", CultureInfo.InvariantCulture),
                ["items"] = items,
                ["experience"] = experience,
                ["progression"] = File.Exists(Path.Combine(snapshotDir, ProgressionFile))
            };
            File.WriteAllText(Path.Combine(snapshotDir, MetadataFile), metadata.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n");
            return null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            return "Could not save the snapshot: " + error.Message;
        }
    }

    static string? ReadInventory(string path, out int items)
    {
        items = 0;
        if (!File.Exists(path)) return "The capture finished without an inventory.";
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(path));
            if (!doc.RootElement.TryGetProperty("items", out var list) || list.ValueKind != JsonValueKind.Array)
                return "The captured inventory has no items list.";
            items = list.GetArrayLength();
            return null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException)
        {
            return "Could not read the captured inventory: " + error.Message;
        }
    }

    // Progression is optional: a capture may end with the inventory alone.
    static string? ReadProgression(string path, out long? experience)
    {
        experience = null;
        if (!File.Exists(path)) return null;
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(path));
            if (doc.RootElement.ValueKind != JsonValueKind.Object) return "The captured progression is not an object.";
            if (doc.RootElement.TryGetProperty("experience", out var xp) && xp.ValueKind == JsonValueKind.Number && xp.TryGetInt64(out var value)) experience = value;
            return null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException)
        {
            return "Could not read the captured progression: " + error.Message;
        }
    }
}

// Progression: the capture got the account's progression too; without it only the inventory can be imported.
public sealed record SnapshotInfo(DateTime? CapturedUtc, int Items, long? Experience, bool Progression);
