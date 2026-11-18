using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Security;
using System.Security.Cryptography;
using Microsoft.Win32;

namespace HellForge.Core;

// Where this PC's snapshot signer publishes its public key: a place only administrator or root rights can write. HKLM on
// Windows. On Linux a file under /var/lib, where only root can create anything; the capture helper copies it there from
// the signing capture's run (capture-helper.sh). A Windows program under Wine reads that file through Z:.
public static class SignerPublicKey
{
    public const string LinuxPath = "/var/lib/hellforge/snapshot-signer.pub";
    public const string NoSignerMessage = "This PC has no snapshot signer yet. Sync again.";
    internal static string RegistryKeyPath { get; set; } = @"SOFTWARE\HellForge\SnapshotSigner";
    internal const string RegistryValue = "PublicKey";
    // Tests set these instead of the PC's own key and file.
    internal static byte[]? ForTests { get; set; }
    internal static string LinuxFile { get; set; } = LinuxPath;

    // The signer's SubjectPublicKeyInfo, or null with the reason.
    public static byte[]? Read(out string? problem)
    {
        problem = null;
        if (ForTests is { } test) return test;
        try
        {
            if (OperatingSystem.IsWindows() && !UnderWine())
            {
                if (ReadRegistry() is { } fromRegistry) return CheckedKey(fromRegistry);
                problem = NoSignerMessage;
                return null;
            }
            var path = OperatingSystem.IsWindows() ? "Z:" + LinuxFile.Replace('/', '\\') : LinuxFile;
            if (!File.Exists(path)) { problem = NoSignerMessage; return null; }
            if (!OperatingSystem.IsWindows() && (Writable(path) || Writable(Path.GetDirectoryName(path)!)))
            {
                problem = "The snapshot signer's public key (" + LinuxPath + ") can be changed by other accounts. Sync again to replace it.";
                return null;
            }
            return FromPem(File.ReadAllText(path));
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or SecurityException or CryptographicException or ArgumentException)
        {
            problem = "The snapshot signer's public key cannot be read (" + error.Message + "). Sync again.";
            return null;
        }
    }

    [SupportedOSPlatform("windows")]
    static byte[]? ReadRegistry()
    {
        using var machine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
        using var key = machine.OpenSubKey(RegistryKeyPath);
        return key?.GetValue(RegistryValue) as byte[];
    }

    [SupportedOSPlatform("windows")]
    internal static void WriteRegistry(byte[] subjectPublicKeyInfo, string provider)
    {
        using var machine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
        using var key = machine.CreateSubKey(RegistryKeyPath, writable: true);
        key.SetValue(RegistryValue, subjectPublicKeyInfo, RegistryValueKind.Binary);
        key.SetValue("Provider", provider, RegistryValueKind.String);
    }

    [UnsupportedOSPlatform("windows")]
    static bool Writable(string path) => (File.GetUnixFileMode(path) & (UnixFileMode.GroupWrite | UnixFileMode.OtherWrite)) != 0;

    // A P-256 key only: anything else was not made by the signer.
    static byte[] CheckedKey(byte[] subjectPublicKeyInfo)
    {
        using var key = ECDsa.Create();
        key.ImportSubjectPublicKeyInfo(subjectPublicKeyInfo, out _);
        if (key.KeySize != 256) throw new CryptographicException("the key is not a P-256 key");
        return key.ExportSubjectPublicKeyInfo();
    }

    public static byte[] FromPem(string pem)
    {
        using var key = ECDsa.Create();
        key.ImportFromPem(pem);
        if (key.KeySize != 256) throw new CryptographicException("the key is not a P-256 key");
        return key.ExportSubjectPublicKeyInfo();
    }

    public static string ToPem(byte[] subjectPublicKeyInfo) => PemEncoding.WriteString("PUBLIC KEY", subjectPublicKeyInfo) + "\n";

    // The capture's Windows build runs under Wine when the Windows launcher does; there it reads the host's Linux key.
    static bool UnderWine()
    {
        try { return NativeLibrary.TryLoad("ntdll.dll", out var ntdll) && NativeLibrary.TryGetExport(ntdll, "wine_get_version", out _); }
        catch (Exception error) when (error is DllNotFoundException or BadImageFormatException or EntryPointNotFoundException) { return false; }
    }
}

// The signing side, used only by the capture running elevated (Windows) or as the helper's signing service (Linux). The
// private key never leaves those rights: a non-exportable CNG machine key on Windows, a file in the systemd StateDirectory
// that only the service's own user can read on Linux.
public static class SnapshotSigner
{
    public const string KeyFile = "signer.key";
    public const string PublicKeyFile = "snapshot-signer.pub";
    internal static string KeyName { get; set; } = "HellForge Snapshot Signer";
    // The TPM first; the software provider where the PC has none or it refuses.
    static readonly string[] Providers = ["Microsoft Platform Crypto Provider", "Microsoft Software Key Storage Provider"];

    // Opens or makes the signer and publishes its public key: to HKLM on Windows, beside the capture's evidence on Linux,
    // for the helper to copy (SignerPublicKey). where says which provider or file holds the key.
    public static ECDsa Open(string? stateDirectory, string evidenceDirectory, out string where)
    {
        if (OperatingSystem.IsWindows())
        {
            var key = OpenMachineKey(out var provider);
            SignerPublicKey.WriteRegistry(key.ExportSubjectPublicKeyInfo(), provider);
            where = provider;
            return key;
        }
        if (string.IsNullOrEmpty(stateDirectory))
            throw new CryptographicException("The capture signs only as the capture helper's signing service (no StateDirectory).");
        var file = Path.Combine(stateDirectory, KeyFile);
        var signer = OpenFileKey(file);
        var published = Path.Combine(evidenceDirectory, PublicKeyFile);
        File.WriteAllText(published, SignerPublicKey.ToPem(signer.ExportSubjectPublicKeyInfo()));
        File.SetUnixFileMode(published, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.GroupRead | UnixFileMode.OtherRead);
        where = file;
        return signer;
    }

    internal static ECDsa OpenFileKey(string file)
    {
        var key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        if (File.Exists(file))
        {
            key.ImportFromPem(File.ReadAllText(file));
            if (key.KeySize != 256) throw new CryptographicException("The signer's key file does not hold a P-256 key: " + file);
            return key;
        }
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(file))!);
        var temporary = file + ".tmp";
        var options = new FileStreamOptions { Mode = FileMode.Create, Access = FileAccess.Write };
        if (!OperatingSystem.IsWindows()) options.UnixCreateMode = UnixFileMode.UserRead | UnixFileMode.UserWrite;
        using (var stream = new FileStream(temporary, options))
        using (var writer = new StreamWriter(stream)) writer.Write(PemEncoding.WriteString("PRIVATE KEY", key.ExportPkcs8PrivateKey()) + "\n");
        File.Move(temporary, file, overwrite: true);
        return key;
    }

    [SupportedOSPlatform("windows")]
    static ECDsa OpenMachineKey(out string provider)
    {
        // An existing key wins, so a TPM that appears later does not change the signer and void every seal.
        foreach (var name in Providers)
            if (Exists(name))
            {
                provider = name;
                return new ECDsaCng(CngKey.Open(KeyName, new CngProvider(name), CngKeyOpenOptions.MachineKey));
            }
        CryptographicException? last = null;
        foreach (var name in Providers)
            try
            {
                var key = CngKey.Create(CngAlgorithm.ECDsaP256, KeyName, new CngKeyCreationParameters
                {
                    Provider = new CngProvider(name),
                    KeyCreationOptions = CngKeyCreationOptions.MachineKey,
                    ExportPolicy = CngExportPolicies.None,
                    KeyUsage = CngKeyUsages.Signing
                });
                provider = name;
                return new ECDsaCng(key);
            }
            catch (CryptographicException error) { last = error; }
        throw new CryptographicException("No key storage provider made the snapshot signer's key: " + last?.Message, last);
    }

    [SupportedOSPlatform("windows")]
    static bool Exists(string provider)
    {
        try { return CngKey.Exists(KeyName, new CngProvider(provider), CngKeyOpenOptions.MachineKey); }
        catch (CryptographicException) { return false; }
    }

    // Tests on Windows remove the machine key they made under their own name.
    [SupportedOSPlatform("windows")]
    internal static void DeleteMachineKey()
    {
        foreach (var name in Providers)
            if (Exists(name)) CngKey.Open(KeyName, new CngProvider(name), CngKeyOpenOptions.MachineKey).Delete();
    }
}
