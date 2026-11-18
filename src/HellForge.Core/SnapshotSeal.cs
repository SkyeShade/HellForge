using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.Core;

// Seals an account snapshot as the capture received it: the SHA-256 of its inventory and progression, the Steam account,
// and the API's own time of the capture, signed by this PC's snapshot signer (SnapshotSigner). Only code running with
// administrator or root rights holds the signer's private key, and HellForge reads the public key from a place only those
// rights can write, so a program running with the player's rights cannot make a seal that verifies here.
// A deterrent, not a guarantee: an administrator, or a changed HellForge the player lets run elevated, can still sign.
// The capture (HellForge.CaptureProxy) compiles this file in as well, so it uses nothing else from HellForge.Core.
public static class SnapshotSeal
{
    public const string FileName = "seal.json";
    // The HMAC key of version 1 seals, beside the snapshots; a sync deletes it.
    public const string LegacyKeyFileName = "seal.key";
    public const string InventoryFile = "real-inventory.json";
    public const string ProgressionFile = "progression.json";
    public static readonly TimeSpan MaxAge = TimeSpan.FromDays(14);
    const int Version = 2;
    const string Tag = "hellforge-snapshot-seal-v2";

    public const string OlderSealMessage = "This snapshot was sealed by an older HellForge. Sync again.";
    public const string OtherSignerMessage = "This snapshot was sealed on another PC, or before this PC's snapshot signer changed. Sync again.";
    public static string ExpiredMessage => $"The snapshot is older than {MaxAge.TotalDays:0} days. Sync again.";

    public sealed record Info(ulong SteamAccount, DateTime CapturedUtc)
    {
        public DateTime ExpiresUtc => CapturedUtc + MaxAge;
    }

    // Seals the files in folder by the hashes of the bytes the capture received, not by reading the files back: what the
    // capture wrote is what is signed, whatever touches the folder in between.
    public static void Write(string folder, ECDsa signer, ulong steamAccount, DateTime capturedUtc, string inventorySha256, string? progressionSha256)
    {
        var captured = capturedUtc.ToUniversalTime();
        var signature = signer.SignData(Payload(steamAccount, captured, inventorySha256, progressionSha256), HashAlgorithmName.SHA256,
            DSASignatureFormat.IeeeP1363FixedFieldConcatenation);
        var seal = new JsonObject
        {
            ["version"] = Version,
            ["steamAccount"] = steamAccount.ToString(CultureInfo.InvariantCulture),
            ["capturedUtc"] = captured.ToString("O", CultureInfo.InvariantCulture),
            ["inventorySha256"] = inventorySha256,
            ["progressionSha256"] = progressionSha256,
            ["keyId"] = KeyId(signer.ExportSubjectPublicKeyInfo()),
            ["signature"] = Convert.ToBase64String(signature)
        };
        File.WriteAllText(Path.Combine(folder, FileName), seal.ToJsonString() + "\n");
    }

    public static string Sha256(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes));

    public static string KeyId(byte[] subjectPublicKeyInfo) => Sha256(subjectPublicKeyInfo);

    // The seal's account and time when it holds for the files in folder, under this PC's signer, and is not expired.
    public static Info? Verify(string folder) => Verify(folder, out _);

    // Same, with problem saying why when there is more to say than "the files changed".
    public static Info? Verify(string folder, out string? problem)
    {
        var publicKey = SignerPublicKey.Read(out var keyProblem);
        var info = Verify(folder, publicKey, DateTime.UtcNow, out problem);
        if (info == null && publicKey == null && problem == null) problem = keyProblem;
        return info;
    }

    internal static Info? Verify(string folder, byte[]? publicKey, DateTime nowUtc, out string? problem)
    {
        problem = null;
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(Path.Combine(folder, FileName)));
            var seal = document.RootElement;
            var version = Number(seal, "version");
            if (version is > 0 and < Version) { problem = OlderSealMessage; return null; }
            if (version != Version || Text(seal, "steamAccount") is not { } accountText ||
                !ulong.TryParse(accountText, NumberStyles.None, CultureInfo.InvariantCulture, out var account) ||
                Text(seal, "capturedUtc") is not { } timeText ||
                !DateTime.TryParse(timeText, CultureInfo.InvariantCulture, DateTimeStyles.RoundtripKind, out var captured) ||
                captured.Kind != DateTimeKind.Utc) return null;
            var inventory = Hash(Path.Combine(folder, InventoryFile));
            var progression = Hash(Path.Combine(folder, ProgressionFile));
            if (inventory == null || inventory != Text(seal, "inventorySha256") || progression != Text(seal, "progressionSha256")) return null;
            if (publicKey == null) return null;
            if (Text(seal, "keyId") != KeyId(publicKey)) { problem = OtherSignerMessage; return null; }
            if (!Signed(publicKey, Payload(account, captured, inventory, progression), Text(seal, "signature"))) return null;
            if (nowUtc - captured > MaxAge) { problem = ExpiredMessage; return null; }
            return new Info(account, captured);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or CryptographicException or FormatException)
        {
            return null;
        }
    }

    static bool Signed(byte[] publicKey, byte[] payload, string? signature)
    {
        if (signature == null) return false;
        using var key = ECDsa.Create();
        key.ImportSubjectPublicKeyInfo(publicKey, out _);
        return key.VerifyData(payload, Convert.FromBase64String(signature), HashAlgorithmName.SHA256, DSASignatureFormat.IeeeP1363FixedFieldConcatenation);
    }

    static byte[] Payload(ulong account, DateTime capturedUtc, string inventory, string? progression) =>
        Encoding.UTF8.GetBytes(string.Join('\n', Tag, account.ToString(CultureInfo.InvariantCulture),
            capturedUtc.ToString("O", CultureInfo.InvariantCulture), inventory, progression ?? "-"));

    static string? Hash(string path) => File.Exists(path) ? Sha256(File.ReadAllBytes(path)) : null;

    static string? Text(JsonElement seal, string name) =>
        seal.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() : null;

    static int Number(JsonElement seal, string name) =>
        seal.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out var number) ? number : -1;
}
