using System.Globalization;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.Core;

// Seals an account snapshot as the capture received it: the SHA-256 of its inventory and progression, the Steam account
// and the API's time of the capture. The backend unlocks premium items only from a seal that holds for the account signed
// in to Steam. Windows (version 2): signed by this PC's snapshot signer, whose key only administrator code holds
// (SnapshotSigner). Linux (version 1, 0.4's): an HMAC whose key beside the snapshots is wrapped under the machine id
// (MachineData); the backend runs under Wine there, and Wine's CNG cannot import the signer's key (#500). A Linux host
// (native, or the backend under Wine) takes only the Linux seal, Windows only the signed one. A deterrent, not a
// guarantee (docs/linux-account-sync.md). The capture (HellForge.CaptureProxy) compiles this file in as well, so it uses
// nothing else from HellForge.Core.
public static class SnapshotSeal
{
    public const string FileName = "seal.json";
    // The Linux seal's wrapped HMAC key, beside every profile's snapshot (KeyPathFor).
    public const string KeyFileName = "seal.key";
    public const string InventoryFile = "real-inventory.json";
    public const string ProgressionFile = "progression.json";
    const int SignedVersion = 2;
    const string SignedTag = "hellforge-snapshot-seal-v2";
    const int MachineVersion = 1;
    const string MachineTag = "hellforge-snapshot-seal";
    static readonly byte[] Entropy = "HellForge account snapshot seal v1"u8.ToArray();

    public const string OlderSealMessage = "This snapshot was sealed by an older HellForge. Sync again.";
    public const string OtherSignerMessage = "This snapshot was sealed on another PC, or before this PC's snapshot signer changed. Sync again.";
    // A signed Linux snapshot from a 0.5 test build: Linux reads only its own seal now.
    public const string LinuxSignedMessage = "Sync once more: Linux snapshots use the simpler seal again.";
    public const string NoKeyMessage = "This PC has no snapshot seal key. Sync again.";
    public const string OtherMachineMessage = "This snapshot was sealed on another PC, or before this PC's machine id changed. Sync again.";
    public const string IssueLink = "https://github.com/SkyeShade/HellForge/issues/500";

    public sealed record Info(ulong SteamAccount, DateTime CapturedUtc);

    // A Linux host: native Linux, or a Windows program under Wine (the backend in a session, through Proton). Tests set it.
    internal static bool? LinuxHostForTests { get; set; }
    public static bool LinuxHost => LinuxHostForTests ?? MachineData.LinuxHost;

    // Where a snapshot's Linux seal key is: one per installation, beside every profile's snapshot folder.
    public static string KeyPathFor(string snapshotDir) =>
        Path.Combine(Path.GetDirectoryName(Path.GetFullPath(snapshotDir.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)))!,
            KeyFileName);

    // Windows: signs the files in folder by the hashes of the bytes the capture received, not by reading the files back:
    // what the capture wrote is what is signed, whatever touches the folder in between.
    public static void Write(string folder, ECDsa signer, ulong steamAccount, DateTime capturedUtc, string inventorySha256, string? progressionSha256)
    {
        var captured = capturedUtc.ToUniversalTime();
        var signature = signer.SignData(SignedPayload(steamAccount, captured, inventorySha256, progressionSha256), HashAlgorithmName.SHA256,
            DSASignatureFormat.IeeeP1363FixedFieldConcatenation);
        var seal = new JsonObject
        {
            ["version"] = SignedVersion,
            ["steamAccount"] = steamAccount.ToString(CultureInfo.InvariantCulture),
            ["capturedUtc"] = captured.ToString("O", CultureInfo.InvariantCulture),
            ["inventorySha256"] = inventorySha256,
            ["progressionSha256"] = progressionSha256,
            ["keyId"] = KeyId(signer.ExportSubjectPublicKeyInfo()),
            ["signature"] = Convert.ToBase64String(signature)
        };
        File.WriteAllText(Path.Combine(folder, FileName), seal.ToJsonString() + "\n");
    }

    // Linux: seals the same hashes with the HMAC key at keyPath, making the key on first use, and replacing one this
    // machine cannot open (another machine id, or a key from another system).
    public static void WriteMachineSealed(string folder, string keyPath, ulong steamAccount, DateTime capturedUtc, string inventorySha256,
        string? progressionSha256)
    {
        var captured = capturedUtc.ToUniversalTime();
        var seal = new JsonObject
        {
            ["version"] = MachineVersion,
            ["steamAccount"] = steamAccount.ToString(CultureInfo.InvariantCulture),
            ["capturedUtc"] = captured.ToString("O", CultureInfo.InvariantCulture),
            ["inventorySha256"] = inventorySha256,
            ["progressionSha256"] = progressionSha256,
            ["mac"] = Mac(LoadKey(keyPath, create: true), steamAccount, captured, inventorySha256, progressionSha256)
        };
        File.WriteAllText(Path.Combine(folder, FileName), seal.ToJsonString() + "\n");
    }

    public static string Sha256(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes));

    public static string KeyId(byte[] subjectPublicKeyInfo) => Sha256(subjectPublicKeyInfo);

    // The seal's account and time when it holds for the snapshot folder's files on this PC, however old.
    public static Info? Verify(string snapshotDir) => Verify(snapshotDir, KeyPathFor(snapshotDir), out _);

    // Same, with problem saying why when there is more to say than "the files changed".
    public static Info? Verify(string snapshotDir, out string? problem) => Verify(snapshotDir, KeyPathFor(snapshotDir), out problem);

    // folder's seal, with the Linux key at keyPath: a sync folder's capture is checked with its snapshot's key.
    public static Info? Verify(string folder, string keyPath, out string? problem)
    {
        if (LinuxHost) return VerifyMachineSealed(folder, keyPath, out problem);
        var publicKey = SignerPublicKey.Read(out var keyProblem);
        var info = VerifySigned(folder, publicKey, out problem);
        if (info == null && publicKey == null && problem == null) problem = keyProblem;
        return info;
    }

    // A Windows seal under the signer's public key (null: none, so nothing verifies). A Linux seal is older here.
    internal static Info? VerifySigned(string folder, byte[]? publicKey, out string? problem)
    {
        problem = null;
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(Path.Combine(folder, FileName)));
            var seal = document.RootElement;
            var version = Number(seal, "version");
            if (version is > 0 and < SignedVersion) { problem = OlderSealMessage; return null; }
            if (version != SignedVersion || !Fields(seal, out var account, out var captured)) return null;
            if (FilesHash(folder, seal) is not { } hashes || publicKey == null) return null;
            if (Text(seal, "keyId") != KeyId(publicKey)) { problem = OtherSignerMessage; return null; }
            try
            {
                if (!Signed(publicKey, SignedPayload(account, captured, hashes.Inventory, hashes.Progression), Text(seal, "signature"))) return null;
            }
            catch (Exception error) when (error is CryptographicException or PlatformNotSupportedException)
            {
                problem = TechnicalMessage("the seal's signature", error);
                return null;
            }
            return new Info(account, captured);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or FormatException)
        {
            return null;
        }
    }

    // A Linux seal with the HMAC key at keyPath. A signed one (a 0.5 test build's) asks for one more sync.
    internal static Info? VerifyMachineSealed(string folder, string keyPath, out string? problem)
    {
        problem = null;
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(Path.Combine(folder, FileName)));
            var seal = document.RootElement;
            var version = Number(seal, "version");
            if (version == SignedVersion) { problem = LinuxSignedMessage; return null; }
            if (version != MachineVersion || !Fields(seal, out var account, out var captured)) return null;
            if (FilesHash(folder, seal) is not { } hashes) return null;
            byte[] key;
            try { key = LoadKey(keyPath, create: false); }
            catch (MachineData.SealProblemException error) { problem = error.Message; return null; }
            catch (AuthenticationTagMismatchException) { problem = OtherMachineMessage; return null; }
            catch (Exception error) when (error is CryptographicException or PlatformNotSupportedException)
            {
                problem = TechnicalMessage("the snapshot's seal key", error);
                return null;
            }
            string expected;
            try { expected = Mac(key, account, captured, hashes.Inventory, hashes.Progression); }
            finally { CryptographicOperations.ZeroMemory(key); }
            return Text(seal, "mac") is { } mac && CryptographicOperations.FixedTimeEquals(Encoding.ASCII.GetBytes(mac), Encoding.ASCII.GetBytes(expected))
                ? new Info(account, captured) : null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or FormatException)
        {
            return null;
        }
        catch (Exception error) when (error is CryptographicException or PlatformNotSupportedException)
        {
            problem = TechnicalMessage("the snapshot's seal", error);
            return null;
        }
    }

    // A check that failed for a reason on this PC's side, not the snapshot's: another sync would not help.
    internal static string TechnicalMessage(string what, Exception error) =>
        "HellForge could not check " + what + (MachineData.UnderWine ? " under Wine" : "") + " (" + error.Message.TrimEnd('.') +
        "), so premium content stays locked; syncing again does not help. See " + IssueLink + ".";

    static bool Fields(JsonElement seal, out ulong account, out DateTime captured)
    {
        account = 0;
        captured = default;
        return Text(seal, "steamAccount") is { } accountText &&
            ulong.TryParse(accountText, NumberStyles.None, CultureInfo.InvariantCulture, out account) &&
            Text(seal, "capturedUtc") is { } timeText &&
            DateTime.TryParse(timeText, CultureInfo.InvariantCulture, DateTimeStyles.RoundtripKind, out captured) &&
            captured.Kind == DateTimeKind.Utc;
    }

    // The files' hashes when they are the ones the seal names; otherwise null.
    static (string Inventory, string? Progression)? FilesHash(string folder, JsonElement seal)
    {
        var inventory = Hash(Path.Combine(folder, InventoryFile));
        var progression = Hash(Path.Combine(folder, ProgressionFile));
        if (inventory == null || inventory != Text(seal, "inventorySha256") || progression != Text(seal, "progressionSha256")) return null;
        return (inventory, progression);
    }

    static bool Signed(byte[] publicKey, byte[] payload, string? signature)
    {
        if (signature == null) return false;
        using var key = ECDsa.Create();
        key.ImportSubjectPublicKeyInfo(publicKey, out _);
        return key.VerifyData(payload, Convert.FromBase64String(signature), HashAlgorithmName.SHA256, DSASignatureFormat.IeeeP1363FixedFieldConcatenation);
    }

    static byte[] SignedPayload(ulong account, DateTime capturedUtc, string inventory, string? progression) =>
        Encoding.UTF8.GetBytes(string.Join('\n', SignedTag, account.ToString(CultureInfo.InvariantCulture),
            capturedUtc.ToString("O", CultureInfo.InvariantCulture), inventory, progression ?? "-"));

    // 0.4's HMAC, byte for byte, so a Linux snapshot 0.4 sealed still holds.
    static string Mac(byte[] key, ulong account, DateTime capturedUtc, string inventory, string? progression)
    {
        var payload = string.Join('\n', MachineTag, MachineVersion.ToString(CultureInfo.InvariantCulture),
            account.ToString(CultureInfo.InvariantCulture), capturedUtc.ToString("O", CultureInfo.InvariantCulture), inventory, progression ?? "-");
        return Convert.ToHexString(HMACSHA256.HashData(key, Encoding.UTF8.GetBytes(payload)));
    }

    static string? Hash(string path) => File.Exists(path) ? Sha256(File.ReadAllBytes(path)) : null;

    static string? Text(JsonElement seal, string name) =>
        seal.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() : null;

    static int Number(JsonElement seal, string name) =>
        seal.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out var number) ? number : -1;

    // Writing replaces a key this machine cannot open (another machine id, a key from another system); verifying never does.
    static byte[] LoadKey(string keyPath, bool create)
    {
        if (File.Exists(keyPath))
        {
            try { return MachineData.Unprotect(File.ReadAllBytes(keyPath), Entropy); }
            catch (MachineData.MachineIdException) { throw; }
            catch (CryptographicException) when (create) { }
        }
        else if (!create) throw new MachineData.ForeignSealException(NoKeyMessage);
        var key = RandomNumberGenerator.GetBytes(32);
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(keyPath))!);
        File.WriteAllBytes(keyPath, MachineData.Protect(key, Entropy));
        return key;
    }

    // The Linux seal key is wrapped for this PC: AES-256-GCM under a key derived from the machine id, the same for native
    // Linux and for a Windows program under Wine (which reads the id through Z:). A short marker starts the blob. HKDF is
    // written out with HMAC-SHA256 (RFC 5869) rather than HKDF.DeriveKey, so under Wine this needs only the HMAC-SHA256,
    // SHA-256 and AES-GCM that 0.4 already ran there.
    internal static class MachineData
    {
        const int MarkerSize = 4, NonceSize = 12, TagSize = 16;
        const int HeaderSize = MarkerSize + NonceSize + TagSize;
        const string OtherSystemMessage = "This snapshot was sealed on another system. Sync again.";
        const string NoMachineIdMessage = "This PC has no usable machine id, so the snapshot cannot be sealed or verified. Sync again.";

        static ReadOnlySpan<byte> Marker => "HFL1"u8;
        internal static readonly bool UnderWine = DetectWine();
        internal static readonly bool LinuxHost = OperatingSystem.IsLinux() || UnderWine;

        // Tests point this at a synthetic machine id; production always uses the paths in MachineIdPaths.
        internal static string? MachineIdPathForTests { get; set; }

        internal abstract class SealProblemException : CryptographicException
        {
            protected SealProblemException(string message) : base(message) { }
        }

        internal sealed class ForeignSealException : SealProblemException
        {
            public ForeignSealException(string message) : base(message) { }
        }

        internal sealed class MachineIdException : SealProblemException
        {
            public MachineIdException(string message) : base(message) { }
        }

        static bool DetectWine()
        {
            if (!OperatingSystem.IsWindows()) return false;
            try { return NativeLibrary.TryLoad("ntdll.dll", out var ntdll) && NativeLibrary.TryGetExport(ntdll, "wine_get_version", out _); }
            catch (Exception error) when (error is DllNotFoundException or BadImageFormatException or EntryPointNotFoundException) { return false; }
        }

        public static byte[] Protect(byte[] data, byte[] entropy)
        {
            var key = MachineKey(entropy);
            try
            {
                var nonce = RandomNumberGenerator.GetBytes(NonceSize);
                var ciphertext = new byte[data.Length];
                var tag = new byte[TagSize];
                using (var aes = new AesGcm(key, TagSize)) aes.Encrypt(nonce, data, ciphertext, tag);
                var result = new byte[HeaderSize + ciphertext.Length];
                Marker.CopyTo(result);
                nonce.CopyTo(result.AsSpan(MarkerSize));
                tag.CopyTo(result.AsSpan(MarkerSize + NonceSize));
                ciphertext.CopyTo(result.AsSpan(HeaderSize));
                return result;
            }
            finally { CryptographicOperations.ZeroMemory(key); }
        }

        public static byte[] Unprotect(byte[] data, byte[] entropy)
        {
            // 0.4 on Windows wrapped its key with DPAPI, which has no marker.
            if (!data.AsSpan().StartsWith(Marker)) throw new ForeignSealException(OtherSystemMessage);
            if (data.Length <= HeaderSize) throw new ForeignSealException("The snapshot seal key is truncated. Sync again.");
            var key = MachineKey(entropy);
            try
            {
                var plaintext = new byte[data.Length - HeaderSize];
                using (var aes = new AesGcm(key, TagSize))
                    aes.Decrypt(data.AsSpan(MarkerSize, NonceSize), data.AsSpan(HeaderSize), data.AsSpan(MarkerSize + NonceSize, TagSize), plaintext);
                return plaintext;
            }
            finally { CryptographicOperations.ZeroMemory(key); }
        }

        static byte[] MachineKey(byte[] entropy) => DeriveKey(Encoding.ASCII.GetBytes(MachineId()), entropy);

        // HKDF-SHA256 with no salt and one 32-byte block; HKDF.DeriveKey(SHA256, secret, 32, salt: null, info) gives the same.
        internal static byte[] DeriveKey(byte[] secret, byte[] info)
        {
            var pseudoRandomKey = HMACSHA256.HashData(new byte[32], secret);
            var block = new byte[info.Length + 1];
            info.CopyTo(block, 0);
            block[^1] = 1;
            try { return HMACSHA256.HashData(pseudoRandomKey, block); }
            finally { CryptographicOperations.ZeroMemory(pseudoRandomKey); }
        }

        static string MachineId()
        {
            foreach (var path in MachineIdPaths())
            {
                try
                {
                    if (!File.Exists(path)) continue;
                    var id = File.ReadAllText(path).Trim();
                    if (id.Length == 32 && id.All(char.IsAsciiHexDigit)) return id.ToLowerInvariant();
                }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
            }
            throw new MachineIdException(NoMachineIdMessage);
        }

        static IEnumerable<string> MachineIdPaths()
        {
            if (MachineIdPathForTests is { } testPath)
            {
                yield return testPath;
                yield break;
            }
            if (OperatingSystem.IsWindows())
            {
                // A Windows program under Wine on a Linux host: its Z: drive is the host's root (in the Steam Linux Runtime
                // the container's, which pressure-vessel gives the host's machine id).
                yield return @"Z:\etc\machine-id";
                yield return @"Z:\var\lib\dbus\machine-id";
                yield break;
            }
            yield return "/etc/machine-id";
            yield return "/var/lib/dbus/machine-id";
        }
    }
}
