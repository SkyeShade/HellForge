using System.Buffers.Binary;
using System.Diagnostics;
using System.Globalization;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;

namespace HellForge.Core;

// The native Linux side of Sync Inventory. The proxy runs unprivileged and speaks TLS on the unprivileged loopback port
// 18443; the installed root helper owns the marked /etc/hosts lines and the 127.0.0.1:443 relay. The supervisor writes the
// proxy's public CA into HELLDIVERS 2's Proton prefix through Wine's reg.exe: the Windows certificate store serialises a
// Root certificate as the registry value HKLM\Software\Microsoft\SystemCertificates\Root\Certificates\<SHA-1>\Blob.
// Nothing here runs Wine as root or changes the Windows path.
public static class LinuxCapture
{
    public const string ApiHost = "api.live.prod.thehelldiversgame.com";
    public const int CapturePort = 443;
    public const int ProxyPort = 18443;
    public const string HelperPath = "/usr/local/lib/hellforge/capture-helper";
    public const string HelperHashPath = "/usr/local/lib/hellforge/capture-helper.sha256";
    public const string HelperPidFile = "/run/hellforge/capture-helper.pid";
    public const string HelperScript = "capture-helper.sh";
    public const string HelperInstaller = "allow-capture.sh";
    const string CertificateFile = "capture-ca.cer";
    const string CertificateRecordFile = "capture-ca.json";
    const string HelperLogFile = "linux-capture-helper.log";
    const string RegistryCertificateRoot = @"HKEY_LOCAL_MACHINE\Software\Microsoft\SystemCertificates\Root\Certificates";
    static readonly TimeSpan WineTimeout = TimeSpan.FromSeconds(60);
    static readonly TimeSpan HelperCommandTimeout = TimeSpan.FromSeconds(20);

    public sealed record CertificateRecord(string Prefix, string Proton, string SteamRoot, string CaFile, string Thumbprint);
    public sealed record WineResult(int ExitCode, string Output);
    public sealed record HelperState(bool Installed, bool Current);

    // A running root helper started by sudo. The Process is sudo's, so stopping is a privileged helper command, never a
    // signal to a root child from the launcher.
    public sealed class HelperRun : IDisposable
    {
        readonly Process process;
        readonly Task<string> output;
        readonly Task<string> error;
        readonly string syncDir;

        internal HelperRun(Process process, Task<string> output, Task<string> error, string syncDir)
        {
            this.process = process;
            this.output = output;
            this.error = error;
            this.syncDir = syncDir;
        }

        public void Dispose()
        {
            try
            {
                if (process.HasExited)
                {
                    var text = (output.GetAwaiter().GetResult() + error.GetAwaiter().GetResult()).Trim();
                    if (text.Length > 0)
                    {
                        var path = Path.Combine(syncDir, HelperLogFile);
                        File.AppendAllText(path, text + Environment.NewLine);
                        RestrictPrivateFile(path);
                    }
                }
            }
            catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or InvalidOperationException) { }
            process.Dispose();
        }
    }

    public static string? FindPrefix(string? steamRoot = null) =>
        FindPrefix(steamRoot, Environment.GetEnvironmentVariable("STEAM_COMPAT_DATA_PATH"));

    // The HELLDIVERS 2 compatibility folder (Steam's compatdata/553850, which contains pfx), from the same Steam libraries
    // the wrapper searches. A caller's STEAM_COMPAT_DATA_PATH is used only when it is that app's folder: HellForge's own
    // prefix is not where the normal game runs.
    public static string? FindPrefix(string? steamRoot, string? configuredPrefix)
    {
        if (IsGamePrefix(configuredPrefix)) return LocalInstall.FullPath(configuredPrefix!);
        steamRoot ??= SteamLocator.SteamRoot();
        if (steamRoot == null) return null;
        foreach (var manifest in LocalInstall.FindManifests(steamRoot))
        {
            var library = Path.GetDirectoryName(Path.GetDirectoryName(manifest));
            if (library == null) continue;
            var compatibilityData = Path.Combine(library, "steamapps", "compatdata", "553850");
            if (Directory.Exists(Path.Combine(compatibilityData, "pfx"))) return LocalInstall.FullPath(compatibilityData);
        }
        var fallback = Path.Combine(steamRoot, "steamapps", "compatdata", "553850");
        return Directory.Exists(Path.Combine(fallback, "pfx")) ? LocalInstall.FullPath(fallback) : null;
    }

    static bool IsGamePrefix(string? prefix)
    {
        if (string.IsNullOrWhiteSpace(prefix)) return false;
        var full = Path.TrimEndingDirectorySeparator(Path.GetFullPath(prefix));
        return string.Equals(Path.GetFileName(full), "553850", StringComparison.Ordinal) &&
            string.Equals(Path.GetFileName(Path.GetDirectoryName(full)), "compatdata", StringComparison.Ordinal) &&
            Directory.Exists(Path.Combine(full, "pfx"));
    }

    public static string WinePrefix(string compatibilityData) => Path.Combine(compatibilityData, "pfx");

    // Steam's CompatToolMapping is the only authority for the Proton that opens HELLDIVERS 2's own prefix. HellForge's
    // own selected Proton may differ, and running it on Steam's compatdata/553850 would upgrade or downgrade the prefix
    // behind Steam's back.
    public static string? ProtonForGame(string? steamRoot)
    {
        if (steamRoot == null) return null;
        string? toolName;
        try
        {
            var config = Path.Combine(steamRoot, "config", "config.vdf");
            if (!File.Exists(config)) return null;
            toolName = CompatToolName(File.ReadAllText(config));
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return null; }
        return toolName == null ? null : ResolveCompatTool(steamRoot, toolName);
    }

    // The tool Steam maps to HELLDIVERS 2, else its default tool (the "0" entry). Null when neither entry names a tool;
    // callers refuse rather than guessing a Proton.
    public static string? CompatToolName(string configText)
    {
        try
        {
            var mapping = FindCompatToolMapping(ValveData.Parse(configText));
            if (mapping == null) return null;
            if (mapping.Children.TryGetValue("553850", out var game)) return ToolName(game);
            return mapping.Children.TryGetValue("0", out var fallback) ? ToolName(fallback) : null;
        }
        catch (InvalidDataException) { return null; }
    }

    static string? ToolName(ValveData entry) =>
        entry.Values.TryGetValue("name", out var name) && !string.IsNullOrWhiteSpace(name) ? name.Trim() : null;

    static ValveData? FindCompatToolMapping(ValveData node)
    {
        if (node.Children.TryGetValue("CompatToolMapping", out var mapping)) return mapping;
        foreach (var child in node.Children.Values)
            if (FindCompatToolMapping(child) is { } found) return found;
        return null;
    }

    static string? ResolveCompatTool(string steamRoot, string toolName)
    {
        try
        {
            foreach (var library in LocalInstall.FindLibraries(steamRoot))
            {
                if (ProtonInCommon(library, toolName) is { } common) return LocalInstall.FullPath(common);
                var tools = Path.Combine(library, "compatibilitytools.d");
                if (!Directory.Exists(tools)) continue;
                foreach (var directory in Directory.EnumerateDirectories(tools).Order(StringComparer.Ordinal))
                    if (ProtonInCustomTool(directory, toolName) is { } custom) return LocalInstall.FullPath(custom);
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or ArgumentException) { }
        return null;
    }

    // A Proton in steamapps/common: the mapping's name is Steam's internal name (proton_experimental, proton_10), while the
    // folder carries the display name (Proton - Experimental, Proton 10.0). An exact or normalised match wins over a
    // version-prefix match such as proton_9 -> Proton 9.0.
    static string? ProtonInCommon(string library, string toolName)
    {
        var common = Path.Combine(library, "steamapps", "common");
        if (!Directory.Exists(common)) return null;
        string? best = null;
        var bestScore = 0;
        foreach (var directory in Directory.EnumerateDirectories(common).Order(StringComparer.Ordinal))
        {
            var proton = Path.Combine(directory, "proton");
            if (!File.Exists(proton)) continue;
            var score = ToolNameScore(toolName, Path.GetFileName(directory));
            if (score > bestScore) { bestScore = score; best = proton; }
        }
        return best;
    }

    // A custom tool in compatibilitytools.d/<folder>; compatibilitytool.vdf names it (its compat_tools key, the
    // mapping's name usually) and install_path points at the folder holding proton.
    static string? ProtonInCustomTool(string toolDirectory, string toolName)
    {
        var metadata = Path.Combine(toolDirectory, "compatibilitytool.vdf");
        if (!File.Exists(metadata)) return null;
        ValveData root;
        try { root = ValveData.Parse(File.ReadAllText(metadata)); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return null; }
        if (CompatEntries(root) is not { } entries) return null;
        foreach (var entry in entries.Children)
        {
            if (!CustomEntryMatches(toolName, entry.Key, entry.Value)) continue;
            if (CustomProtonPath(toolDirectory, entry.Value) is { } proton) return proton;
        }
        return null;
    }

    static ValveData? CompatEntries(ValveData root)
    {
        if (!root.Children.TryGetValue("compatibilitytools", out var tools)) return null;
        return tools.Children.TryGetValue("compat_tools", out var entries) ? entries : null;
    }

    static bool CustomEntryMatches(string toolName, string key, ValveData entry) =>
        ToolNameScore(toolName, key) != 0 ||
        (entry.Values.TryGetValue("display_name", out var display) && ToolNameScore(toolName, display) != 0);

    static string? CustomProtonPath(string toolDirectory, ValveData entry)
    {
        try
        {
            var install = entry.Values.TryGetValue("install_path", out var path) && !string.IsNullOrWhiteSpace(path) ? path : ".";
            var toolRoot = Path.GetFullPath(Path.Combine(toolDirectory, install));
            if (!LocalInstall.Within(toolRoot, toolDirectory)) return null;
            var proton = Path.Combine(toolRoot, "proton");
            return File.Exists(proton) ? proton : null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or ArgumentException) { return null; }
    }

    static int ToolNameScore(string mappingName, string folderName)
    {
        if (string.Equals(mappingName, folderName, StringComparison.OrdinalIgnoreCase)) return 3;
        var mapping = NormalizeToolName(mappingName);
        var folder = NormalizeToolName(folderName);
        if (mapping.Length == 0 || folder.Length == 0) return 0;
        if (mapping == folder) return 2;
        return folder.StartsWith(mapping, StringComparison.Ordinal) &&
            (folder.Length == mapping.Length || char.IsAsciiDigit(folder[mapping.Length])) ? 1 : 0;
    }

    static string NormalizeToolName(string name) =>
        new(name.Where(char.IsAsciiLetterOrDigit).Select(char.ToLowerInvariant).ToArray());

    public static string? CurrentAddress()
    {
        try { return FirstPublicIPv4(Dns.GetHostAddresses(ApiHost))?.ToString(); }
        catch (SocketException) { return null; }
    }

    // A leftover or user-made /etc/hosts entry can make the API name resolve to loopback, the unspecified address, a
    // link-local address or a private one. The proxy would then connect through its own relay and never reach the API.
    public static bool IsPublicAddress(IPAddress address)
    {
        if (address.AddressFamily != AddressFamily.InterNetwork || IPAddress.IsLoopback(address)) return false;
        var bytes = address.GetAddressBytes();
        if (bytes.Length != 4) return false;
        if (bytes[0] is 0 or 10 or 127) return false;
        if (bytes[0] == 169 && bytes[1] == 254) return false;
        if (bytes[0] == 172 && bytes[1] is >= 16 and <= 31) return false;
        if (bytes[0] == 192 && bytes[1] == 168) return false;
        // 100.64.0.0/10 is shared address space, not a public destination.
        return !(bytes[0] == 100 && bytes[1] is >= 64 and <= 127);
    }

    public static IPAddress? FirstPublicIPv4(IEnumerable<IPAddress> addresses) =>
        addresses.FirstOrDefault(address => address.AddressFamily == AddressFamily.InterNetwork && IsPublicAddress(address));

    public static string WinePath(string hostPath, string prefix) => HellForge.Core.WinePath.ToWinePath(hostPath, prefix);

    // The DER public certificate the proxy wrote. Reading it back also verifies that it is a certificate, not a private
    // key or an arbitrary file.
    public static string CertificateThumbprint(byte[] der)
    {
        using var certificate = X509CertificateLoader.LoadCertificate(der);
        return certificate.Thumbprint;
    }

    // capture-ca.json is user-writable, so every thumbprint is checked before it reaches a registry key name or a file.
    public static string? NormalizeThumbprint(string? thumbprint) =>
        thumbprint is { Length: 40 } && thumbprint.All(Uri.IsHexDigit) ? thumbprint.ToUpperInvariant() : null;

    public static string RegistryKey(string thumbprint)
    {
        var normalized = NormalizeThumbprint(thumbprint) ??
            throw new ArgumentException("A certificate thumbprint is exactly 40 hexadecimal characters.", nameof(thumbprint));
        return RegistryCertificateRoot + @"\" + normalized;
    }

    // crypt32's serialised certificate: one CERT_CERT_PROP_ID (32) element with reserved 1 and the DER length, all
    // little-endian 32-bit fields, followed by the DER bytes.
    public static byte[] CertificateBlob(byte[] der)
    {
        ArgumentNullException.ThrowIfNull(der);
        var blob = new byte[12 + der.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(blob.AsSpan(0, 4), 32);
        BinaryPrimitives.WriteUInt32LittleEndian(blob.AsSpan(4, 4), 1);
        BinaryPrimitives.WriteUInt32LittleEndian(blob.AsSpan(8, 4), (uint)der.Length);
        der.CopyTo(blob, 12);
        return blob;
    }

    public static string AddRegistryText(string thumbprint, byte[] der)
    {
        var key = RegistryKey(thumbprint);
        var hex = string.Join(",", CertificateBlob(der).Select(value => value.ToString("X2", CultureInfo.InvariantCulture)));
        return "Windows Registry Editor Version 5.00\r\n\r\n[" + key + "]\r\n\"Blob\"=hex:" + hex + "\r\n";
    }

    public static string DeleteRegistryText(string thumbprint) =>
        "Windows Registry Editor Version 5.00\r\n\r\n[-" + RegistryKey(thumbprint) + "]\r\n";

    // reg import needs a UTF-16 LE file with a BOM; .reg lines use CRLF.
    public static void WriteRegistryFile(string path, string text)
    {
        File.WriteAllText(path, text, new UnicodeEncoding(bigEndian: false, byteOrderMark: true));
        RestrictPrivateFile(path);
    }

    public static IReadOnlyList<string> ImportRegistryArguments(string wineRegPath) => ["run", "reg", "import", wineRegPath];

    public static IReadOnlyList<string> QueryBlobArguments(string thumbprint) =>
        ["run", "reg", "query", RegistryKey(thumbprint), "/v", "Blob"];

    public static IReadOnlyList<string> QueryKeyArguments(string thumbprint) =>
        ["run", "reg", "query", RegistryKey(thumbprint)];

    // The value data reg query prints after REG_BINARY, folded back into one hex string. Continuation lines are accepted;
    // the key line itself (which carries the thumbprint) is never read as data.
    public static string? QueryBlobHex(string output)
    {
        var lines = output.Replace("\r\n", "\n", StringComparison.Ordinal).Split('\n');
        var hex = new StringBuilder();
        var started = false;
        foreach (var line in lines)
        {
            if (!started)
            {
                var at = line.IndexOf("REG_BINARY", StringComparison.OrdinalIgnoreCase);
                if (at < 0) continue;
                AppendHex(line[(at + "REG_BINARY".Length)..]);
                started = true;
                continue;
            }
            var compact = new string(line.Where(c => !char.IsWhiteSpace(c)).ToArray());
            if (compact.Length == 0) continue;
            if (!compact.All(Uri.IsHexDigit)) break;
            hex.Append(compact);
        }
        return hex.Length == 0 ? null : hex.ToString();

        void AppendHex(string text)
        {
            var compact = new string(text.Where(c => !char.IsWhiteSpace(c)).ToArray());
            if (compact.Length != 0 && compact.All(Uri.IsHexDigit)) hex.Append(compact);
        }
    }

    public static bool BlobMatches(string queryOutput, byte[] blob) =>
        string.Equals(QueryBlobHex(queryOutput), Convert.ToHexString(blob), StringComparison.OrdinalIgnoreCase);

    // reg query exits non-zero for a missing key; the message says so. A Proton failure or an empty/odd answer is not
    // proof that the key is gone.
    public static bool QuerySaysAbsent(WineResult result) =>
        result.ExitCode > 0 && (result.Output.Contains("unable to find", StringComparison.OrdinalIgnoreCase) ||
            result.Output.Contains("not found", StringComparison.OrdinalIgnoreCase) ||
            result.Output.Contains("cannot find", StringComparison.OrdinalIgnoreCase) ||
            result.Output.Contains("does not exist", StringComparison.OrdinalIgnoreCase));

    public static string HelperInstallCommand(string installRoot) =>
        "sudo bash " + Path.Combine(installRoot, "app", "linux", HelperInstaller);

    public static string FileSha256(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream));
    }

    // The installed root copy must still be the helper this installation ships. A package update changes the shipped
    // file, and allow-capture.sh is then needed again; a stale root copy must never run against the new launcher.
    public static string? HelperProblem(string installRoot)
    {
        if (!File.Exists(HelperPath))
            return "The Linux capture helper is not installed. Run this once, then sync again: " + HelperInstallCommand(installRoot);
        try
        {
            var shipped = Path.Combine(new InstallLayout(installRoot).App, "linux", HelperScript);
            if (!File.Exists(shipped))
                return "This installation's " + HelperScript + " is missing. Reinstall HellForge, then sync again.";
            var installedHash = FileSha256(HelperPath);
            if (!installedHash.Equals(FileSha256(shipped), StringComparison.OrdinalIgnoreCase))
                return "The installed Linux capture helper does not match this HellForge version. Run this once, then sync again: " +
                    HelperInstallCommand(installRoot);
            if (File.Exists(HelperHashPath))
            {
                var recorded = File.ReadAllText(HelperHashPath).Trim();
                if (!recorded.Equals(installedHash, StringComparison.OrdinalIgnoreCase))
                    return "The installed Linux capture helper does not match its recorded SHA-256. Run this once, then sync again: " +
                        HelperInstallCommand(installRoot);
            }
            return null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            return "The installed Linux capture helper could not be checked: " + error.Message;
        }
    }

    // Why a capture cannot start. The caller supplies the real game and session checks; the helper checks are skipped in
    // tests where the installed copy cannot be created.
    public static string? RunRefusal(bool gameRunning, bool sessionRunning)
    {
        if (sessionRunning) return "Stop the HellForge session first.";
        if (gameRunning) return "Close HELLDIVERS 2 first: the sync reads its next login.";
        return null;
    }

    public static string? StartRefusal(string installRoot, string? prefix, string? proton, string? address, bool gameRunning,
        bool sessionRunning, HelperState? helper = null)
    {
        if (RunRefusal(gameRunning, sessionRunning) is { } running) return running;
        var state = helper ?? new HelperState(File.Exists(HelperPath), HelperProblem(installRoot) == null);
        if (!state.Installed)
            return "The Linux capture helper is not installed. Run this once, then sync again: " + HelperInstallCommand(installRoot);
        if (!state.Current)
            return HelperProblem(installRoot) ?? ("The installed Linux capture helper is out of date. Run this once, then sync again: " +
                HelperInstallCommand(installRoot));
        if (prefix == null || !Directory.Exists(Path.Combine(prefix, "pfx")))
            return "HELLDIVERS 2's Proton prefix was not found: start the game once from Steam, then sync again.";
        if (proton == null || !File.Exists(proton))
            return "The Proton that Steam maps to HELLDIVERS 2 could not be resolved, or its files are missing: choose HELLDIVERS 2's " +
                "compatibility tool in Steam > Properties > Compatibility, then sync again.";
        if (address == null)
            return "The HELLDIVERS 2 API address could not be resolved to a public IPv4 address: check this PC's network " +
                "(and any /etc/hosts entry for the API), then sync again.";
        if (!IPAddress.TryParse(address, out var parsed) || !IsPublicAddress(parsed))
            return "The HELLDIVERS 2 API address resolved to " + address + ", which is not a public IPv4 address: check /etc/hosts " +
                "and this PC's DNS, then sync again.";
        return null;
    }

    public static void RestrictPrivateDirectory(string path)
    {
        if (OperatingSystem.IsLinux())
            File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
    }

    public static void RestrictPrivateFile(string path)
    {
        if (OperatingSystem.IsLinux() && File.Exists(path))
            File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite);
    }

    public static string CertificatePath(string syncDir) => Path.Combine(syncDir, CertificateFile);
    public static string CertificateRecordPath(string syncDir) => Path.Combine(syncDir, CertificateRecordFile);

    // Written before the prefix store is changed, so a launcher killed during the sync can still remove the certificate.
    public static void WriteCertificateRecord(string syncDir, CertificateRecord record)
    {
        if (NormalizeThumbprint(record.Thumbprint) == null)
            throw new InvalidOperationException("The capture CA record has no valid SHA-1 thumbprint.");
        var path = CertificateRecordPath(syncDir);
        File.WriteAllText(path, JsonSerializer.Serialize(record) + "\n");
        RestrictPrivateFile(path);
    }

    public static CertificateRecord? ReadCertificateRecord(string syncDir)
    {
        try
        {
            var record = JsonSerializer.Deserialize<CertificateRecord>(File.ReadAllText(CertificateRecordPath(syncDir)));
            return record != null && !string.IsNullOrEmpty(record.Prefix) && !string.IsNullOrEmpty(record.Proton) &&
                !string.IsNullOrEmpty(record.SteamRoot) && !string.IsNullOrEmpty(record.CaFile) &&
                NormalizeThumbprint(record.Thumbprint) != null
                ? record : null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or NotSupportedException)
        {
            return null;
        }
    }

    public static void DeleteCertificateRecord(string syncDir)
    {
        try { File.Delete(CertificateRecordPath(syncDir)); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }

    public static IReadOnlyList<string> PendingCertificateFolders(string installRoot) =>
        Directory.Exists(new InstallLayout(installRoot).Logs)
            ? Directory.EnumerateDirectories(new InstallLayout(installRoot).Logs, "sync-*")
                .Where(folder => File.Exists(CertificateRecordPath(folder))).Order(StringComparer.Ordinal).ToArray()
            : [];

    // Import the serialised Root certificate and ask reg.exe for the value. The import is not accepted merely because
    // reg exited 0: the query must return exactly the blob that was written.
    public static void InstallCertificate(CertificateRecord record)
    {
        if (InventorySync.IsGameRunning())
            throw new InvalidOperationException("Close HELLDIVERS 2 first: its prefix is not changed while the game runs.");
        var thumbprint = NormalizeThumbprint(record.Thumbprint) ??
            throw new InvalidOperationException("The capture CA record has no valid SHA-1 thumbprint.");
        var der = File.ReadAllBytes(record.CaFile);
        if (!string.Equals(CertificateThumbprint(der), thumbprint, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("The capture CA file does not match the recorded thumbprint.");
        var blob = CertificateBlob(der);
        WithRegistryFile("capture-add.reg", AddRegistryText(thumbprint, der), reg =>
        {
            var imported = RunWine(record, ImportRegistryArguments(WinePath(reg, record.Prefix)));
            if (imported.ExitCode != 0)
                throw new InvalidOperationException("Wine reg import could not add the capture CA to HELLDIVERS 2's prefix: " + Tail(imported.Output));
            var seen = RunWine(record, QueryBlobArguments(thumbprint));
            if (seen.ExitCode != 0 || !BlobMatches(seen.Output, blob))
                throw new InvalidOperationException("Wine's registry does not hold the capture CA blob in HELLDIVERS 2's prefix: " + Tail(seen.Output));
        });
    }

    public static string? RemoveCertificate(CertificateRecord record)
    {
        if (InventorySync.IsGameRunning())
            return "Close HELLDIVERS 2 first: its prefix is not changed while the game runs.";
        var thumbprint = NormalizeThumbprint(record.Thumbprint);
        if (thumbprint == null) return "The capture CA record has no valid SHA-1 thumbprint; its prefix key was not changed.";
        string? problem = null;
        try
        {
            WithRegistryFile("capture-delete.reg", DeleteRegistryText(thumbprint), reg =>
            {
                var imported = RunWine(record, ImportRegistryArguments(WinePath(reg, record.Prefix)));
                if (imported.ExitCode != 0)
                {
                    problem = "Wine reg import could not remove the capture CA: " + Tail(imported.Output);
                    return;
                }
                var seen = RunWine(record, QueryKeyArguments(thumbprint));
                if (seen.ExitCode == 0)
                    problem = "The capture CA key is still in HELLDIVERS 2's prefix after reg import removed it.";
                else if (!QuerySaysAbsent(seen))
                    problem = "Wine reg query could not prove the capture CA key is gone: " + Tail(seen.Output);
            });
        }
        catch (InvalidOperationException error) { return error.Message; }
        return problem;
    }

    static void WithRegistryFile(string name, string text, Action<string> action)
    {
        DirectoryInfo? folder = null;
        try
        {
            folder = Directory.CreateTempSubdirectory("hellforge-ca-");
            RestrictPrivateDirectory(folder.FullName);
            var path = Path.Combine(folder.FullName, name);
            WriteRegistryFile(path, text);
            action(path);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            throw new InvalidOperationException("The capture CA registry file could not be written: " + error.Message, error);
        }
        finally
        {
            try { folder?.Delete(recursive: true); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        }
    }

    public static WineResult RunWine(CertificateRecord record, IReadOnlyList<string> arguments)
    {
        if (!File.Exists(record.Proton)) return new(-1, "Proton was not found: " + record.Proton);
        var start = new ProcessStartInfo(record.Proton)
        {
            WorkingDirectory = Path.GetDirectoryName(record.Proton)!,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        start.Environment["WINEPREFIX"] = WinePrefix(record.Prefix);
        start.Environment["STEAM_COMPAT_DATA_PATH"] = record.Prefix;
        start.Environment["STEAM_COMPAT_CLIENT_INSTALL_PATH"] = record.SteamRoot;
        start.Environment["STEAM_COMPAT_APP_ID"] = LocalInstall.AppId;
        start.Environment["SteamGameId"] = LocalInstall.AppId;
        try
        {
            using var process = Process.Start(start);
            if (process == null) return new(-1, "Proton did not start.");
            var output = process.StandardOutput.ReadToEndAsync();
            var error = process.StandardError.ReadToEndAsync();
            if (!process.WaitForExit((int)WineTimeout.TotalMilliseconds))
            {
                try { process.Kill(entireProcessTree: true); } catch (InvalidOperationException) { }
                return new(-1, "Proton timed out.");
            }
            return new(process.ExitCode, (output.GetAwaiter().GetResult() + error.GetAwaiter().GetResult()).Trim());
        }
        catch (Exception exception) when (exception is System.ComponentModel.Win32Exception or InvalidOperationException or IOException)
        {
            return new(-1, exception.Message);
        }
    }

    public static HelperRun StartHelper(string installRoot, int proxyPid, string syncDir)
    {
        if (!File.Exists(HelperPath))
            throw new InvalidOperationException("The Linux capture helper is not installed. Run this once: " + HelperInstallCommand(installRoot));
        var check = RunSudo(["check"], TimeSpan.FromSeconds(10));
        if (check.ExitCode != 0)
            throw new InvalidOperationException("The Linux capture helper is not ready: " + Tail(check.Output) +
                ". Run this once, then sync again: " + HelperInstallCommand(installRoot));

        var start = SudoStartInfo(["start"]);
        start.RedirectStandardInput = true;
        start.RedirectStandardOutput = true;
        start.RedirectStandardError = true;
        var process = Process.Start(start) ?? throw new InvalidOperationException("The Linux capture helper did not start.");
        var output = process.StandardOutput.ReadToEndAsync();
        var error = process.StandardError.ReadToEndAsync();
        try
        {
            process.StandardInput.WriteLine(proxyPid.ToString(CultureInfo.InvariantCulture));
            process.StandardInput.Close();
        }
        catch (Exception exception) when (exception is IOException or InvalidOperationException) { }
        if (process.WaitForExit(1000))
        {
            var text = (output.GetAwaiter().GetResult() + error.GetAwaiter().GetResult()).Trim();
            process.Dispose();
            throw new InvalidOperationException("The Linux capture helper stopped while starting" +
                (text.Length > 0 ? ": " + Tail(text) : "."));
        }
        if (!WaitForHelperRelay(TimeSpan.FromSeconds(5)))
        {
            RunSudo(["stop"], HelperCommandTimeout);
            process.WaitForExit(5000);
            var text = process.HasExited ? (output.GetAwaiter().GetResult() + error.GetAwaiter().GetResult()).Trim() : "";
            process.Dispose();
            throw new InvalidOperationException("The Linux capture helper did not open 127.0.0.1:443" +
                (text.Length > 0 ? ": " + Tail(text) : "."));
        }
        return new HelperRun(process, output, error, syncDir);
    }

    static bool WaitForHelperRelay(TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow.Add(timeout);
        while (DateTime.UtcNow < deadline)
        {
            if (File.Exists(HelperPidFile))
                try
                {
                    using var client = new TcpClient();
                    client.Connect(IPAddress.Loopback, CapturePort);
                    return true;
                }
                catch (SocketException) { }
            Thread.Sleep(100);
        }
        return false;
    }

    public static string? CleanupHosts()
    {
        if (!File.Exists(HelperPath))
            return "The Linux capture helper is not installed. Run this once: sudo bash app/linux/" + HelperInstaller + ".";
        var result = RunSudo(["cleanup"], HelperCommandTimeout);
        return result.ExitCode == 0 ? null : "The Linux capture helper could not clean up: " + Tail(result.Output);
    }

    public static string? StopHelper()
    {
        if (!File.Exists(HelperPath)) return null;
        var result = RunSudo(["stop"], HelperCommandTimeout);
        return result.ExitCode == 0 ? null : "The Linux capture helper did not stop: " + Tail(result.Output);
    }

    // Normal proxy exit: stop the relay, remove the marked hosts lines, then remove the CA by thumbprint and forget its
    // record. Any failure leaves the record for the next launcher start to retry.
    public static string? Finish(string syncDir)
    {
        var problems = new List<string>();
        if (StopHelper() is { } stop) problems.Add(stop);
        if (CleanupHosts() is { } cleanup) problems.Add(cleanup);
        if (RemoveRecordedCertificate(syncDir) is { } remove) problems.Add(remove);
        return problems.Count == 0 ? null : string.Join(" ", problems);
    }

    public static string? RemoveRecordedCertificate(string syncDir)
    {
        if (ReadCertificateRecord(syncDir) is not { } record) return null;
        if (RemoveCertificate(record) is { } remove) return remove;
        DeleteCertificateRecord(syncDir);
        return null;
    }

    // The launcher's Clean up action for a run killed before its normal exit. The helper command is idempotent and works
    // when no relay remains; each recorded prefix certificate is removed by its thumbprint.
    public static string? Cleanup(string installRoot)
    {
        if (InventorySync.IsGameRunning())
            return "Close HELLDIVERS 2 first: its prefix is not changed while the game runs.";
        var problems = new List<string>();
        if (StopHelper() is { } stop) problems.Add(stop);
        if (CleanupHosts() is { } hosts) problems.Add(hosts);
        foreach (var folder in PendingCertificateFolders(installRoot))
        {
            if (ReadCertificateRecord(folder) is not { } record) continue;
            if (RemoveCertificate(record) is { } remove) problems.Add(remove);
            else DeleteCertificateRecord(folder);
        }
        return problems.Count == 0 ? null : string.Join(" ", problems);
    }

    static ProcessStartInfo SudoStartInfo(IReadOnlyList<string> arguments)
    {
        var start = new ProcessStartInfo("sudo") { UseShellExecute = false, CreateNoWindow = true };
        start.ArgumentList.Add("-n");
        start.ArgumentList.Add(HelperPath);
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        return start;
    }

    static WineResult RunSudo(IReadOnlyList<string> arguments, TimeSpan timeout)
    {
        var start = SudoStartInfo(arguments);
        start.RedirectStandardOutput = true;
        start.RedirectStandardError = true;
        try
        {
            using var process = Process.Start(start);
            if (process == null) return new(-1, "sudo did not start.");
            var output = process.StandardOutput.ReadToEndAsync();
            var error = process.StandardError.ReadToEndAsync();
            if (!process.WaitForExit((int)timeout.TotalMilliseconds))
            {
                try { process.Kill(entireProcessTree: true); } catch (InvalidOperationException) { }
                return new(-1, "The capture helper command timed out.");
            }
            return new(process.ExitCode, (output.GetAwaiter().GetResult() + error.GetAwaiter().GetResult()).Trim());
        }
        catch (Exception exception) when (exception is System.ComponentModel.Win32Exception or InvalidOperationException or IOException)
        {
            return new(-1, exception.Message);
        }
    }

    static string Tail(string text)
    {
        text = text.Trim();
        if (text.Length == 0) return "no output";
        var lines = text.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        return string.Join(" | ", lines.TakeLast(3));
    }
}
