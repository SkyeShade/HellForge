using System.Buffers.Binary;
using System.Diagnostics;
using System.Globalization;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;

namespace HellForge.Core;

// The native Linux side of Sync Inventory. The proxy runs unprivileged and speaks TLS on the unprivileged loopback port
// 18443; the installed root helper owns the marked /etc/hosts lines and runs the installed root-owned relay for
// 127.0.0.1:443. The supervisor writes the proxy's public CA into HELLDIVERS 2's Proton prefix through Wine's reg.exe: the
// Windows certificate store serialises a Root certificate as the registry value
// HKLM\Software\Microsoft\SystemCertificates\Root\Certificates\<SHA-1>\Blob. Nothing here runs Wine as root or changes the
// Windows path.
public static partial class LinuxCapture
{
    public const string ApiHost = "api.live.prod.thehelldiversgame.com";
    public const int CapturePort = 443;
    public const int ProxyPort = 18443;
    public const string HelperPath = "/usr/local/lib/hellforge/capture-helper";
    public const string HelperHashPath = "/usr/local/lib/hellforge/capture-helper.sha256";
    public const string RelayPath = "/usr/local/lib/hellforge/capture-relay";
    public const string RelayHashPath = "/usr/local/lib/hellforge/capture-relay.sha256";
    public const string HelperPidFile = "/run/hellforge/capture-helper.pid";
    public const string HelperScript = "capture-helper.sh";
    public const string RelayBinary = "capture-relay";
    public const string HelperInstaller = "allow-capture.sh";
    const string CertificateFile = "capture-ca.cer";
    const string CertificateRecordFile = "capture-ca.json";
    const string HelperLogFile = "linux-capture-helper.log";
    const string RegistryCertificateRoot = @"HKEY_LOCAL_MACHINE\Software\Microsoft\SystemCertificates\Root\Certificates";
    const string SystemRegFile = "system.reg";
    const string SystemRegExpected = "the key is in system.reg with the expected Blob";
    const string SystemRegKeyAbsent = "the key is not in system.reg";
    static readonly TimeSpan WineTimeout = TimeSpan.FromSeconds(60);
    static readonly TimeSpan WineserverTimeout = TimeSpan.FromSeconds(10);
    static readonly TimeSpan HelperCommandTimeout = TimeSpan.FromSeconds(20);
    static readonly string[] ProtonNoise = ["OpenXR-Loader", "xrizer", "fsync", "Proton: Upgrading prefix"];

    public sealed record CertificateRecord(string Prefix, string Proton, string SteamRoot, string CaFile, string Thumbprint);
    // stdout and stderr stay apart for the log. Registry verdicts come from reg export files and system.reg, never stdout.
    public sealed record WineResult(int ExitCode, string Stdout, string Stderr)
    {
        // For a command whose streams have no separate verdict (the capture helper).
        public string AllText => string.Join(Environment.NewLine, new[] { Stdout, Stderr }.Where(text => text.Length != 0));
    }
    // What Wine's own system.reg text says about one certificate key.
    public sealed record SystemRegCertificate(bool KeyFound, string? BlobHex);
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

    [DllImport("libc", SetLastError = true, EntryPoint = "stat")]
    static extern int StatPath([MarshalAs(UnmanagedType.LPUTF8Str)] string path, IntPtr buffer);

    [DllImport("libc", EntryPoint = "getuid")]
    static extern uint GetUid();

    // Wine names its wineserver socket after the prefix folder's device and inode, exactly as hellforge-offline.sh does
    // with `stat -L -c %D` (hex) and `stat -L -c %i` (hex). x86_64 Linux struct stat puts st_dev at 0 and st_ino at 8.
    public static string? WineserverServer(string prefix)
    {
        if (!OperatingSystem.IsLinux() || RuntimeInformation.ProcessArchitecture != Architecture.X64 || !Directory.Exists(prefix))
            return null;
        nint buffer = Marshal.AllocHGlobal(256);
        try
        {
            Marshal.Copy(new byte[256], 0, buffer, 256);
            if (StatPath(prefix, buffer) != 0) return null;
            var device = (ulong)Marshal.ReadInt64(buffer, 0);
            var inode = (ulong)Marshal.ReadInt64(buffer, 8);
            return "server-" + device.ToString("x", CultureInfo.InvariantCulture) + "-" +
                inode.ToString("x", CultureInfo.InvariantCulture);
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    // Wine 10 keeps the server's socket in $XDG_RUNTIME_DIR/wine; older Wine and Proton in /tmp/.wine-<uid>.
    public static IReadOnlyList<string> WineserverSocketPaths(string server, string runtimeDirectory, uint uid) =>
    [
        Path.Combine(runtimeDirectory, "wine", server, "socket"),
        Path.Combine("/tmp", ".wine-" + uid.ToString(CultureInfo.InvariantCulture), server, "socket")
    ];

    // A wineserver of this prefix still running outside the capture is a Windows program (Steam's game or a tool) that
    // would share the imported registry and sockets. The check fails closed: "cannot tell" is not "not running".
    public static string? WineserverProblem(string compatibilityData, Func<string, LinuxSteamBridge.SocketState>? check = null)
    {
        if (!OperatingSystem.IsLinux()) return null;
        var prefix = WinePrefix(compatibilityData);
        if (!Directory.Exists(prefix)) return null;
        var server = WineserverServer(prefix);
        if (server == null)
            return "Cannot tell whether a Windows program is still running in HELLDIVERS 2's Proton prefix (" + prefix +
                "): its device and inode could not be read. Close Steam's game/tools in that prefix first, then sync again.";
        check ??= path => LinuxSteamBridge.Check(path, out _);
        foreach (var socket in WineserverSocketPaths(server, LinuxSteamBridge.SocketDirectory(), GetUid()))
        {
            switch (check(socket))
            {
                case LinuxSteamBridge.SocketState.Listening:
                    return "A Windows program is still running in HELLDIVERS 2's Proton prefix (" + socket +
                        "): close Steam's game/tools in that prefix first, then sync again.";
                case LinuxSteamBridge.SocketState.CannotTell:
                    return "Cannot tell whether a Windows program is still running in HELLDIVERS 2's Proton prefix (" + socket +
                        "): close Steam's game/tools in that prefix first, then sync again.";
            }
        }
        return null;
    }

    // The Proton version file records which tool last opened the prefix; a change during the import is evidence that
    // another Proton may be upgrading it.
    public static string? PrefixVersion(string compatibilityData)
    {
        try
        {
            var path = Path.Combine(compatibilityData, PrefixVersionFile);
            return File.Exists(path) ? File.ReadAllText(path).Trim() : null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return null; }
    }

    // The prefix the game library actually opens (steamapps/compatdata/553850) can differ from a record made in a second
    // library or before the game moved. Importing the CA into the wrong prefix would never be seen by the game.
    public static string? PrefixMismatchProblem(CertificateRecord record)
    {
        try
        {
            if (!IsGamePrefix(record.Prefix))
                return "The capture CA record's prefix is not HELLDIVERS 2's steamapps/compatdata/553850: " + record.Prefix + ".";
            var gamePrefix = FindPrefix(record.SteamRoot, null);
            if (gamePrefix == null || PathsEqual(gamePrefix, record.Prefix)) return null;
            return "The capture CA record names the Steam prefix " + record.Prefix + ", but the HELLDIVERS 2 library's prefix is " +
                gamePrefix + " (a second Steam library?): start the game once from Steam, then sync again.";
        }
        catch (Exception error) when (error is ArgumentException or NotSupportedException or IOException or UnauthorizedAccessException)
        {
            return "The capture CA record's prefix could not be compared with HELLDIVERS 2's library prefix: " + error.Message;
        }
    }

    static bool PathsEqual(string left, string right) => string.Equals(
        Path.TrimEndingDirectorySeparator(Path.GetFullPath(left)), Path.TrimEndingDirectorySeparator(Path.GetFullPath(right)),
        StringComparison.Ordinal);

    // The prefix's own wineserver, from the Proton that will run reg.exe (the same search as hf-inner.sh).
    public static string? WineserverPath(string proton)
    {
        var root = Path.GetDirectoryName(proton);
        if (root == null) return null;
        foreach (var relative in new[] { Path.Combine("files", "bin", "wineserver"), Path.Combine("dist", "bin", "wineserver"),
            Path.Combine("bin", "wineserver") })
        {
            var path = Path.Combine(root, relative);
            if (File.Exists(path)) return path;
        }
        return null;
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

    // `proton run` can drop reg.exe's console output entirely, so the proof is the UTF-16 LE file reg export writes.
    public static IReadOnlyList<string> ExportKeyArguments(string thumbprint, string winePath) =>
        ["run", "reg", "export", RegistryKey(thumbprint), winePath, "/y"];

    // A reg export file is UTF-16 LE with a BOM. Its Blob is the same comma-separated, backslash-continued hex form as
    // system.reg. Null means no readable export or no Blob for this key; stdout is never read.
    public static string? ReadRegistryExportBlob(string path, string thumbprint)
    {
        try
        {
            var bytes = File.ReadAllBytes(path);
            if (bytes.Length < 2 || bytes[0] != 0xFF || bytes[1] != 0xFE) return null;
            return ReadExportBlobHex(Encoding.Unicode.GetString(bytes, 2, bytes.Length - 2), thumbprint);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or ArgumentException) { return null; }
    }

    public static string? ReadExportBlobHex(string text, string thumbprint)
    {
        var normalized = NormalizeThumbprint(thumbprint) ??
            throw new ArgumentException("A certificate thumbprint is exactly 40 hexadecimal characters.", nameof(thumbprint));
        return ReadRegistryCertificate(text, RegistryKey(normalized)).BlobHex;
    }

    // Wine's system.reg text: the section line, then "Blob"=hex: with CRLF-comma-separated bytes and optional backslash
    // continuation lines. KeyFound is separate from BlobHex so a key without a value is not called a missing import.
    public static SystemRegCertificate ReadSystemRegCertificate(string text, string thumbprint)
    {
        var normalized = NormalizeThumbprint(thumbprint) ??
            throw new ArgumentException("A certificate thumbprint is exactly 40 hexadecimal characters.", nameof(thumbprint));
        return ReadRegistryCertificate(text, @"Software\\Microsoft\\SystemCertificates\\Root\\Certificates\\" + normalized);
    }

    static SystemRegCertificate ReadRegistryCertificate(string text, string key)
    {
        var lines = text.Replace("\r\n", "\n", StringComparison.Ordinal).Split('\n');
        var start = SystemRegSection(lines, key);
        if (start < 0) return new(false, null);
        for (var index = start + 1; index < lines.Length; index++)
        {
            var line = lines[index];
            if (line.StartsWith('[')) return new(true, null);
            var value = line.TrimStart();
            if (!value.StartsWith("\"Blob\"", StringComparison.OrdinalIgnoreCase)) continue;
            var at = value.IndexOf("=hex:", StringComparison.OrdinalIgnoreCase);
            if (at < 0) continue;
            return new(true, ReadContinuedHex(lines, index, line.Length - value.Length + at + "=hex:".Length));
        }
        return new(true, null);
    }

    static int SystemRegSection(string[] lines, string key)
    {
        for (var index = 0; index < lines.Length; index++)
        {
            if (!lines[index].StartsWith('[')) continue;
            var end = lines[index].IndexOf(']');
            if (end < 0) continue;
            if (string.Equals(lines[index][1..end], key, StringComparison.OrdinalIgnoreCase)) return index;
        }
        return -1;
    }

    static string? ReadContinuedHex(string[] lines, int start, int valueStart)
    {
        var hex = new StringBuilder();
        for (var index = start; index < lines.Length; index++)
        {
            var text = (index == start ? lines[index][valueStart..] : lines[index]).Trim();
            var continued = text.EndsWith('\\');
            if (continued) text = text[..^1].TrimEnd();
            foreach (var character in text)
            {
                if (Uri.IsHexDigit(character)) hex.Append(character);
                else if (!char.IsWhiteSpace(character) && character != ',') return null;
            }
            if (!continued) break;
        }
        return hex.Length == 0 ? null : hex.ToString().ToUpperInvariant();
    }

    public static string HelperInstallCommand(string installRoot) =>
        "sudo bash " + Path.Combine(installRoot, "app", "linux", HelperInstaller);

    public static string FileSha256(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream));
    }

    // The installed root copies must still be the helper and relay this installation ships. A package update changes the
    // shipped files, so the launcher installs the new copies through pkexec before the next sync. Null means ready.
    public static string? HelperProblem(string installRoot)
    {
        if (!File.Exists(HelperPath)) return "The Linux capture helper is not installed.";
        if (!File.Exists(RelayPath)) return "The Linux capture relay is not installed.";
        try
        {
            var shippedFolder = Path.Combine(new InstallLayout(installRoot).App, "linux");
            if (InstalledCopyProblem(HelperPath, HelperHashPath, Path.Combine(shippedFolder, HelperScript), "capture helper",
                    installRoot) is { } helperProblem) return helperProblem;
            return InstalledCopyProblem(RelayPath, RelayHashPath, Path.Combine(shippedFolder, RelayBinary), "capture relay",
                installRoot);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            return "The installed Linux capture helper or relay could not be checked: " + error.Message;
        }
    }

    // One installed root copy against the shipped file and its recorded SHA-256. Internal so tests can use temporary files;
    // HelperProblem applies it to both the helper and the relay. The launcher offers to reinstall when this is not null.
    internal static string? InstalledCopyProblem(string installed, string hashPath, string shipped, string name, string installRoot)
    {
        if (!File.Exists(shipped))
            return "This installation's " + Path.GetFileName(shipped) + " is missing. Reinstall HellForge, then sync again.";
        var installedHash = FileSha256(installed);
        if (!installedHash.Equals(FileSha256(shipped), StringComparison.OrdinalIgnoreCase))
            return "The installed Linux " + name + " does not match this HellForge version.";
        if (File.Exists(hashPath))
        {
            var recorded = File.ReadAllText(hashPath).Trim();
            if (!recorded.Equals(installedHash, StringComparison.OrdinalIgnoreCase))
                return "The installed Linux " + name + " does not match its recorded SHA-256.";
        }
        return null;
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
        if (!state.Installed || !state.Current)
        {
            var helperProblem = HelperProblem(installRoot) ?? (state.Installed
                ? "The installed Linux capture helper is out of date."
                : "The Linux capture helper is not installed.");
            return helperProblem + " The launcher could not install it; run this once, then sync again: " +
                HelperInstallCommand(installRoot);
        }
        if (prefix == null || !Directory.Exists(Path.Combine(prefix, "pfx")))
            return "HELLDIVERS 2's Proton prefix was not found: start the game once from Steam, then sync again.";
        if (WineserverProblem(prefix) is { } wineserver)
            return wineserver;
        if (proton == null || !File.Exists(proton))
            return "The Proton that opens HELLDIVERS 2's prefix could not be resolved. Looked for Steam's CompatToolMapping " +
                "for 553850 or its \"0\" default first, then the prefix's compatdata/553850/config_info and version: neither " +
                "named an installed Proton whose CURRENT_PREFIX_VERSION matches the prefix. Start HELLDIVERS 2 once through " +
                "Steam (that writes config_info), then sync again.";
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

    // Import the serialised Root certificate and prove it with a reg export file. `proton run` can drop reg.exe's
    // stdout entirely, so no console answer is parsed: the export file and the prefix's system.reg must both hold the
    // Blob. stdout and stderr stay apart in launcher.log only.
    public static void InstallCertificate(CertificateRecord record)
    {
        if (InventorySync.IsGameRunning())
            throw new InvalidOperationException("Close HELLDIVERS 2 first: its prefix is not changed while the game runs.");
        if (PrefixMismatchProblem(record) is { } mismatch)
            throw new InvalidOperationException(mismatch);
        if (WineserverProblem(record.Prefix) is { } running)
            throw new InvalidOperationException(running);
        var thumbprint = NormalizeThumbprint(record.Thumbprint) ??
            throw new InvalidOperationException("The capture CA record has no valid SHA-1 thumbprint.");
        var der = File.ReadAllBytes(record.CaFile);
        if (!string.Equals(CertificateThumbprint(der), thumbprint, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("The capture CA file does not match the recorded thumbprint.");
        var blob = CertificateBlob(der);
        WithRegistryFile("capture-add.reg", AddRegistryText(thumbprint, der), (reg, folder) =>
            InstallCertificateThroughWine(record, thumbprint, blob, reg, Path.Combine(folder, "capture-export.reg")));
    }

    // One import, one bounded wait for the prefix's wineserver, then reg export to a private file and system.reg. Both
    // checks must hold the Blob; the failure message says which one disagreed.
    static void InstallCertificateThroughWine(CertificateRecord record, string thumbprint, byte[] blob, string reg,
        string exportPath)
    {
        var versionBefore = PrefixVersion(record.Prefix);
        var imported = RunWine(record, ImportRegistryArguments(WinePath(reg, record.Prefix)));
        LogWine(record, "reg import", imported);
        if (imported.ExitCode != 0)
            throw new InvalidOperationException("Wine reg import could not add the capture CA to HELLDIVERS 2's prefix (exit " +
                imported.ExitCode.ToString(CultureInfo.InvariantCulture) + "; see launcher.log).");
        FlushWineserver(record);
        var exported = RunWine(record, ExportKeyArguments(thumbprint, WinePath(exportPath, record.Prefix)));
        LogWine(record, "reg export Blob", exported);
        RestrictPrivateFile(exportPath);
        var exportProblem = ExportBlobProblem(exported, exportPath, thumbprint, blob);
        Note(record, "reg export check: " + (exportProblem ?? "the export file holds the expected Blob"));
        var evidence = SystemRegEvidence(record, thumbprint, blob);
        Note(record, "system.reg check: " + evidence);
        var versionAfter = PrefixVersion(record.Prefix);
        Note(record, PrefixVersionEvidence(versionBefore, versionAfter));
        if (exportProblem != null || evidence != SystemRegExpected)
            throw new InvalidOperationException(ExportFailure(exported, exportProblem, evidence, versionBefore, versionAfter));
    }

    static string? ExportBlobProblem(WineResult exported, string exportPath, string thumbprint, byte[] blob)
    {
        if (exported.ExitCode != 0)
            return "reg export exited " + exported.ExitCode.ToString(CultureInfo.InvariantCulture);
        if (!File.Exists(exportPath)) return "reg export wrote no file";
        var hex = ReadRegistryExportBlob(exportPath, thumbprint);
        if (hex == null) return "the export file has no Blob for the key";
        return string.Equals(hex, Convert.ToHexString(blob), StringComparison.OrdinalIgnoreCase)
            ? null : "the export file's Blob does not match the certificate";
    }

    static void FlushWineserver(CertificateRecord record)
    {
        var wineserver = WineserverPath(record.Proton);
        if (wineserver == null)
        {
            Note(record, "wineserver -w: not found beside " + record.Proton + "; system.reg is read without waiting for a flush.");
            return;
        }
        var flushed = RunProcess(record, wineserver, ["-w"], WineserverTimeout);
        LogWine(record, "wineserver -w", flushed);
    }

    static string SystemRegEvidence(CertificateRecord record, string thumbprint, byte[] blob)
    {
        var path = Path.Combine(WinePrefix(record.Prefix), SystemRegFile);
        string text;
        try { text = File.ReadAllText(path); }
        catch (Exception error) when (error is FileNotFoundException or DirectoryNotFoundException)
        {
            return "the key is not in system.reg: the import did not land";
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            return "system.reg could not be read (" + path + "): " + error.Message;
        }
        var found = ReadSystemRegCertificate(text, thumbprint);
        if (!found.KeyFound) return "the key is not in system.reg: the import did not land";
        if (found.BlobHex == null) return "the key is in system.reg but its Blob value is missing";
        if (!found.BlobHex.Equals(Convert.ToHexString(blob), StringComparison.OrdinalIgnoreCase))
            return "the key is in system.reg but its Blob value does not match the certificate";
        return SystemRegExpected;
    }

    static string ExportFailure(WineResult exported, string? exportProblem, string evidence, string? versionBefore,
        string? versionAfter)
    {
        var exportText = exportProblem == null
            ? "reg export holds the expected Blob"
            : "Wine reg export did not confirm the capture CA (exit " +
                exported.ExitCode.ToString(CultureInfo.InvariantCulture) + "): " + exportProblem;
        var systemText = evidence == SystemRegExpected ? "system.reg holds the expected Blob" : evidence;
        var text = "Wine could not confirm the capture CA in HELLDIVERS 2's prefix: " + exportText + "; " + systemText + ".";
        if (!string.Equals(versionBefore, versionAfter, StringComparison.Ordinal))
            text += " The prefix version changed from " + (versionBefore ?? "none") + " to " + (versionAfter ?? "none") +
                " during the import; if another Proton is upgrading this prefix, close Steam's game/tools in that prefix first, then sync again.";
        return text;
    }

    static string PrefixVersionEvidence(string? before, string? after) =>
        string.Equals(before, after, StringComparison.Ordinal)
            ? "prefix version unchanged: " + (before ?? "none")
            : "prefix version changed during the import: " + (before ?? "none") + " -> " + (after ?? "none");

    // Both streams, in full, in launcher.log beside the proxy's log; known Proton noise stays but is marked.
    static void LogWine(CertificateRecord record, string step, WineResult result)
    {
        var text = new StringBuilder("Wine ").Append(step).Append(": exit ")
            .Append(result.ExitCode.ToString(CultureInfo.InvariantCulture)).Append('.');
        AppendStream(text, "stdout", result.Stdout);
        AppendStream(text, "stderr", result.Stderr);
        Note(record, text.ToString());
    }

    static void AppendStream(StringBuilder text, string name, string value)
    {
        text.Append(' ').Append(name).Append(':');
        if (value.Length == 0)
        {
            text.Append(" (empty)");
            return;
        }
        foreach (var line in value.Replace("\r\n", "\n", StringComparison.Ordinal).Split('\n'))
            text.Append("\n  ").Append(MarkProtonNoise(line));
    }

    public static string MarkProtonNoise(string line) =>
        ProtonNoise.Any(marker => line.Contains(marker, StringComparison.OrdinalIgnoreCase))
            ? "[known Proton noise] " + line
            : line;

    static void Note(CertificateRecord record, string message)
    {
        try
        {
            var syncDir = Path.GetDirectoryName(Path.GetFullPath(record.CaFile));
            if (syncDir != null) InventorySync.Note(syncDir, message);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or ArgumentException) { }
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
            WithRegistryFile("capture-delete.reg", DeleteRegistryText(thumbprint), (reg, folder) =>
            {
                var imported = RunWine(record, ImportRegistryArguments(WinePath(reg, record.Prefix)));
                LogWine(record, "reg import (remove)", imported);
                if (imported.ExitCode != 0)
                {
                    problem = "Wine reg import could not remove the capture CA (exit " +
                        imported.ExitCode.ToString(CultureInfo.InvariantCulture) + "; see launcher.log).";
                    return;
                }
                FlushWineserver(record);
                var exportPath = Path.Combine(folder, "capture-remove-export.reg");
                var exported = RunWine(record, ExportKeyArguments(thumbprint, WinePath(exportPath, record.Prefix)));
                LogWine(record, "reg export (remove)", exported);
                RestrictPrivateFile(exportPath);
                var evidence = SystemRegKeyEvidence(record, thumbprint);
                Note(record, "reg export (remove): exit " + exported.ExitCode.ToString(CultureInfo.InvariantCulture) +
                    (File.Exists(exportPath) ? ", wrote a file" : ", wrote no file") + "; system.reg: " + evidence);
                if (exported.ExitCode == 0 || File.Exists(exportPath))
                    problem = "The capture CA removal is not proven: reg export " +
                        (exported.ExitCode == 0 ? "exited 0" : "wrote a file") + "; " + evidence + ".";
                else if (evidence != SystemRegKeyAbsent)
                    problem = "Wine reg export did not find the capture CA key, but " + evidence +
                        " after the flush; the key is not proven gone.";
            });
        }
        catch (InvalidOperationException error) { return error.Message; }
        return problem;
    }

    static string SystemRegKeyEvidence(CertificateRecord record, string thumbprint)
    {
        var path = Path.Combine(WinePrefix(record.Prefix), SystemRegFile);
        string text;
        try { text = File.ReadAllText(path); }
        catch (Exception error) when (error is FileNotFoundException or DirectoryNotFoundException)
        {
            return SystemRegKeyAbsent;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            return "system.reg could not be read (" + path + "): " + error.Message;
        }
        return ReadSystemRegCertificate(text, thumbprint).KeyFound ? "the key is in system.reg" : SystemRegKeyAbsent;
    }

    static void WithRegistryFile(string name, string text, Action<string, string> action)
    {
        DirectoryInfo? folder = null;
        try
        {
            folder = Directory.CreateTempSubdirectory("hellforge-ca-");
            RestrictPrivateDirectory(folder.FullName);
            var path = Path.Combine(folder.FullName, name);
            WriteRegistryFile(path, text);
            action(path, folder.FullName);
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
        if (!File.Exists(record.Proton)) return new(-1, "", "Proton was not found: " + record.Proton);
        return RunProcess(record, record.Proton, arguments, WineTimeout);
    }

    static WineResult RunProcess(CertificateRecord record, string executable, IReadOnlyList<string> arguments, TimeSpan timeout)
    {
        var start = new ProcessStartInfo(executable)
        {
            WorkingDirectory = Path.GetDirectoryName(executable)!,
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
            if (process == null) return new(-1, "", Path.GetFileName(executable) + " did not start.");
            var output = process.StandardOutput.ReadToEndAsync();
            var error = process.StandardError.ReadToEndAsync();
            if (!process.WaitForExit((int)timeout.TotalMilliseconds))
            {
                try { process.Kill(entireProcessTree: true); } catch (InvalidOperationException) { }
                return new(-1, "", Path.GetFileName(executable) + " timed out.");
            }
            return new(process.ExitCode, output.GetAwaiter().GetResult().Trim(), error.GetAwaiter().GetResult().Trim());
        }
        catch (Exception exception) when (exception is System.ComponentModel.Win32Exception or InvalidOperationException or IOException)
        {
            return new(-1, "", exception.Message);
        }
    }

    public static HelperRun StartHelper(string installRoot, int proxyPid, string syncDir)
    {
        if (!File.Exists(HelperPath))
            throw new InvalidOperationException((HelperProblem(installRoot) ?? "The Linux capture helper is not installed.") +
                " The launcher could not install it; run this once, then sync again: " + HelperInstallCommand(installRoot));
        var check = RunSudo(["check"], TimeSpan.FromSeconds(10));
        if (check.ExitCode != 0)
            throw new InvalidOperationException("The Linux capture helper is not ready: " + Tail(check.AllText) +
                ". The launcher could not install it; run this once, then sync again: " + HelperInstallCommand(installRoot));

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
        return result.ExitCode == 0 ? null : "The Linux capture helper could not clean up: " + Tail(result.AllText);
    }

    public static string? StopHelper()
    {
        if (!File.Exists(HelperPath)) return null;
        var result = RunSudo(["stop"], HelperCommandTimeout);
        return result.ExitCode == 0 ? null : "The Linux capture helper did not stop: " + Tail(result.AllText);
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
            if (process == null) return new(-1, "", "sudo did not start.");
            var output = process.StandardOutput.ReadToEndAsync();
            var error = process.StandardError.ReadToEndAsync();
            if (!process.WaitForExit((int)timeout.TotalMilliseconds))
            {
                try { process.Kill(entireProcessTree: true); } catch (InvalidOperationException) { }
                return new(-1, "", "The capture helper command timed out.");
            }
            return new(process.ExitCode, output.GetAwaiter().GetResult().Trim(), error.GetAwaiter().GetResult().Trim());
        }
        catch (Exception exception) when (exception is System.ComponentModel.Win32Exception or InvalidOperationException or IOException)
        {
            return new(-1, "", exception.Message);
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
