using System.Runtime.Versioning;
using System.Security;
using System.Security.Cryptography;
using Microsoft.Win32;

namespace HellForge.Core;

// Where this PC's snapshot signer publishes its public key: HKLM, which only administrator rights can write. Windows
// only: Linux seals with the machine-bound HMAC instead (SnapshotSeal), and never reads a signer's key.
public static class SignerPublicKey
{
    public const string NoSignerMessage = "This PC has no snapshot signer yet. Sync again.";
    internal static string RegistryKeyPath { get; set; } = @"SOFTWARE\HellForge\SnapshotSigner";
    internal const string RegistryValue = "PublicKey";
    // Tests set this instead of the PC's own key.
    internal static byte[]? ForTests { get; set; }

    // The signer's SubjectPublicKeyInfo, or null with the reason.
    public static byte[]? Read(out string? problem)
    {
        problem = null;
        if (ForTests is { } test) return test;
        if (!OperatingSystem.IsWindows())
        {
            problem = NoSignerMessage;
            return null;
        }
        try
        {
            if (ReadRegistry() is { } fromRegistry) return CheckedKey(fromRegistry);
            problem = NoSignerMessage;
            return null;
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

    // A P-256 key only: anything else was not made by the signer.
    static byte[] CheckedKey(byte[] subjectPublicKeyInfo)
    {
        using var key = ECDsa.Create();
        key.ImportSubjectPublicKeyInfo(subjectPublicKeyInfo, out _);
        if (key.KeySize != 256) throw new CryptographicException("the key is not a P-256 key");
        return key.ExportSubjectPublicKeyInfo();
    }
}

// The signing side, used only by the capture running elevated on Windows. The private key never leaves those rights: a
// non-exportable CNG machine key. Linux has no signer (SnapshotSeal).
public static class SnapshotSigner
{
    internal static string KeyName { get; set; } = "HellForge Snapshot Signer";
    // The TPM first; the software provider where the PC has none or it refuses.
    static readonly string[] Providers = ["Microsoft Platform Crypto Provider", "Microsoft Software Key Storage Provider"];

    // Opens or makes the signer and publishes its public key to HKLM (SignerPublicKey). where says which provider holds it.
    public static ECDsa Open(out string where)
    {
        if (!OperatingSystem.IsWindows())
            throw new PlatformNotSupportedException("Only the Windows capture signs; Linux seals with the machine-bound key (--seal-key).");
        var key = OpenMachineKey(out var provider);
        SignerPublicKey.WriteRegistry(key.ExportSubjectPublicKeyInfo(), provider);
        where = provider;
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
