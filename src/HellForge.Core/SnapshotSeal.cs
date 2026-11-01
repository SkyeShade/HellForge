using System.Globalization;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.Core;

// Seals an account snapshot as the capture wrote it: the SHA-256 of its inventory and progression, the Steam account it
// was captured for and when, under an HMAC whose key is kept beside the snapshots, protected for this PC (DPAPI, machine
// scope). The backend unlocks premium items only from a snapshot whose seal holds and whose account is the one signed in
// to Steam, so an edited file, a snapshot copied from another PC or another account's snapshot unlocks nothing.
// A good-faith check, not a guarantee: HellForge is open source and runs with the player's rights.
// The capture (HellForge.CaptureProxy) compiles this file in as well, so it uses nothing else from HellForge.Core.
public static class SnapshotSeal
{
    public const string FileName = "seal.json";
    public const string KeyFileName = "seal.key";
    public const string InventoryFile = "real-inventory.json";
    public const string ProgressionFile = "progression.json";
    const int Version = 1;
    static readonly byte[] Entropy = "HellForge account snapshot seal v1"u8.ToArray();

    public sealed record Info(ulong SteamAccount, DateTime CapturedUtc);

    // Seals the inventory (required) and progression (when present) in folder, making the key on first use.
    public static void Write(string folder, string keyPath, ulong steamAccount, DateTime capturedUtc)
    {
        var inventory = Hash(Path.Combine(folder, InventoryFile)) ?? throw new FileNotFoundException("No inventory to seal.", InventoryFile);
        var progression = Hash(Path.Combine(folder, ProgressionFile));
        var seal = new JsonObject
        {
            ["version"] = Version,
            ["steamAccount"] = steamAccount.ToString(CultureInfo.InvariantCulture),
            ["capturedUtc"] = capturedUtc.ToUniversalTime().ToString("O", CultureInfo.InvariantCulture),
            ["inventorySha256"] = inventory,
            ["progressionSha256"] = progression,
            ["mac"] = Mac(LoadKey(keyPath, create: true), steamAccount, capturedUtc.ToUniversalTime(), inventory, progression)
        };
        File.WriteAllText(Path.Combine(folder, FileName), seal.ToJsonString() + "\n");
    }

    // The seal's account and time when it matches the files in folder and this PC's key; otherwise null.
    public static Info? Verify(string folder, string keyPath)
    {
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(Path.Combine(folder, FileName)));
            var seal = document.RootElement;
            if (Number(seal, "version") != Version || Text(seal, "steamAccount") is not { } accountText ||
                !ulong.TryParse(accountText, NumberStyles.None, CultureInfo.InvariantCulture, out var account) ||
                Text(seal, "capturedUtc") is not { } timeText ||
                !DateTime.TryParse(timeText, CultureInfo.InvariantCulture, DateTimeStyles.RoundtripKind, out var captured)) return null;
            var inventory = Hash(Path.Combine(folder, InventoryFile));
            var progression = Hash(Path.Combine(folder, ProgressionFile));
            if (inventory == null || inventory != Text(seal, "inventorySha256") || progression != Text(seal, "progressionSha256")) return null;
            var expected = Mac(LoadKey(keyPath, create: false), account, captured, inventory, progression);
            return Text(seal, "mac") is { } mac && CryptographicOperations.FixedTimeEquals(Encoding.ASCII.GetBytes(mac), Encoding.ASCII.GetBytes(expected))
                ? new Info(account, captured) : null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or CryptographicException or PlatformNotSupportedException)
        {
            return null;
        }
    }

    static string Mac(byte[] key, ulong account, DateTime capturedUtc, string inventory, string? progression)
    {
        var payload = string.Join('\n', "hellforge-snapshot-seal", Version.ToString(CultureInfo.InvariantCulture),
            account.ToString(CultureInfo.InvariantCulture), capturedUtc.ToString("O", CultureInfo.InvariantCulture), inventory, progression ?? "-");
        return Convert.ToHexString(HMACSHA256.HashData(key, Encoding.UTF8.GetBytes(payload)));
    }

    static string? Hash(string path) => File.Exists(path) ? Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))) : null;

    static string? Text(JsonElement seal, string name) =>
        seal.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() : null;

    static int Number(JsonElement seal, string name) =>
        seal.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out var number) ? number : -1;

    static byte[] LoadKey(string keyPath, bool create)
    {
        if (File.Exists(keyPath)) return MachineData.Unprotect(File.ReadAllBytes(keyPath), Entropy);
        if (!create) throw new CryptographicException("This PC has no snapshot seal key.");
        var key = RandomNumberGenerator.GetBytes(32);
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(keyPath))!);
        File.WriteAllBytes(keyPath, MachineData.Protect(key, Entropy));
        return key;
    }

    // Windows DPAPI with the machine's key: only this PC can unprotect what it protected.
    static class MachineData
    {
        const int UiForbidden = 0x1, LocalMachine = 0x4;

        [StructLayout(LayoutKind.Sequential)]
        struct Blob
        {
            public int Size;
            public IntPtr Data;
        }

        [DllImport("crypt32.dll", SetLastError = true)]
        static extern bool CryptProtectData(ref Blob input, IntPtr description, ref Blob entropy, IntPtr reserved, IntPtr prompt, int flags, out Blob output);

        [DllImport("crypt32.dll", SetLastError = true)]
        static extern bool CryptUnprotectData(ref Blob input, IntPtr description, ref Blob entropy, IntPtr reserved, IntPtr prompt, int flags, out Blob output);

        [DllImport("kernel32.dll")]
        static extern IntPtr LocalFree(IntPtr memory);

        public static byte[] Protect(byte[] data, byte[] entropy) => Transform(data, entropy, protect: true);

        public static byte[] Unprotect(byte[] data, byte[] entropy) => Transform(data, entropy, protect: false);

        static byte[] Transform(byte[] data, byte[] entropy, bool protect)
        {
            if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException("Snapshot seals use Windows DPAPI.");
            var dataHandle = GCHandle.Alloc(data, GCHandleType.Pinned);
            var entropyHandle = GCHandle.Alloc(entropy, GCHandleType.Pinned);
            try
            {
                var input = new Blob { Size = data.Length, Data = dataHandle.AddrOfPinnedObject() };
                var salt = new Blob { Size = entropy.Length, Data = entropyHandle.AddrOfPinnedObject() };
                Blob output;
                var done = protect
                    ? CryptProtectData(ref input, IntPtr.Zero, ref salt, IntPtr.Zero, IntPtr.Zero, UiForbidden | LocalMachine, out output)
                    : CryptUnprotectData(ref input, IntPtr.Zero, ref salt, IntPtr.Zero, IntPtr.Zero, UiForbidden, out output);
                if (!done) throw new CryptographicException(Marshal.GetLastPInvokeError());
                try
                {
                    var result = new byte[output.Size];
                    Marshal.Copy(output.Data, result, 0, output.Size);
                    return result;
                }
                finally { LocalFree(output.Data); }
            }
            finally
            {
                dataHandle.Free();
                entropyHandle.Free();
            }
        }
    }
}
