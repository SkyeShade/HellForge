using System.Globalization;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.Core;

// Seals an account snapshot as the capture wrote it: the SHA-256 of its inventory and progression, the Steam account it
// was captured for and when, under an HMAC whose key is kept beside the snapshots, protected for this PC (DPAPI machine
// scope on Windows, AES-GCM under key material derived from the machine id on Linux). The backend unlocks premium items
// only from a snapshot whose seal holds and whose account is the one signed in to Steam, so an edited file, a snapshot
// copied from another PC or another account's snapshot unlocks nothing.
// A good-faith check, not a guarantee: the Linux machine id is readable, and HellForge is open source and runs with the
// player's rights.
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
            ["mac"] = Mac(LoadKey(keyPath, create: true, replaceUnreadable: true), steamAccount, capturedUtc.ToUniversalTime(), inventory, progression)
        };
        File.WriteAllText(Path.Combine(folder, FileName), seal.ToJsonString() + "\n");
    }

    // The seal's account and time when it matches the files in folder and this PC's key; otherwise null.
    public static Info? Verify(string folder, string keyPath) => Verify(folder, keyPath, out _);

    // Same, with problem naming a platform refusal (a Linux key on Windows, a DPAPI key on Linux, or no usable machine id).
    public static Info? Verify(string folder, string keyPath, out string? problem)
    {
        problem = null;
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
        catch (MachineData.SealProblemException error)
        {
            problem = error.Message;
            return null;
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

    // Write replaces a key it cannot open for this system: an old Linux key after a machine-id change, a DPAPI key on
    // Linux, or a Linux key on Windows. Verify never replaces; it reports the refusal instead.
    static byte[] LoadKey(string keyPath, bool create, bool replaceUnreadable = false)
    {
        if (File.Exists(keyPath))
        {
            try { return MachineData.Unprotect(File.ReadAllBytes(keyPath), Entropy); }
            catch (MachineData.MachineIdException) { throw; }
            catch (MachineData.ForeignSealException) when (replaceUnreadable) { }
            catch (CryptographicException) when (replaceUnreadable && MachineData.LinuxHost) { }
        }
        if (!create) throw new CryptographicException("This PC has no snapshot seal key.");
        var key = RandomNumberGenerator.GetBytes(32);
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(keyPath))!);
        File.WriteAllBytes(keyPath, MachineData.Protect(key, Entropy));
        return key;
    }

    // The seal key is wrapped for this PC: DPAPI machine scope on Windows, AES-256-GCM under a key derived from the Linux
    // machine id elsewhere (native Linux, or a Windows program under Wine). A short marker tells the blobs apart.
    internal static class MachineData
    {
        const int UiForbidden = 0x1, LocalMachine = 0x4, LinuxMarkerSize = 4, LinuxNonceSize = 12, LinuxTagSize = 16;
        const int LinuxHeaderSize = LinuxMarkerSize + LinuxNonceSize + LinuxTagSize;
        const string OtherSystemMessage = "This snapshot was sealed on another system: sync again";
        const string NoMachineIdMessage = "This PC has no usable machine id, so the snapshot cannot be sealed or verified. Sync again.";

        static ReadOnlySpan<byte> LinuxMarker => "HFL1"u8;
        internal static readonly bool LinuxHost = OperatingSystem.IsLinux() || UnderWine();

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

        // The native Linux launcher and the capture proxy under Wine must use the same transform; Wine's crypt32 DPAPI
        // blob is prefix-local and cannot be read by the native launcher.
        static bool UnderWine()
        {
            try { return NativeLibrary.TryLoad("ntdll.dll", out var ntdll) && NativeLibrary.TryGetExport(ntdll, "wine_get_version", out _); }
            catch (Exception error) when (error is DllNotFoundException or BadImageFormatException or EntryPointNotFoundException) { return false; }
        }

        public static byte[] Protect(byte[] data, byte[] entropy) =>
            LinuxHost ? ProtectLinux(data, entropy) : Transform(data, entropy, protect: true);

        public static byte[] Unprotect(byte[] data, byte[] entropy)
        {
            if (LinuxHost) return UnprotectLinux(data, entropy);
            if (data.AsSpan().StartsWith(LinuxMarker)) throw new ForeignSealException(OtherSystemMessage);
            return Transform(data, entropy, protect: false);
        }

        static byte[] ProtectLinux(byte[] data, byte[] entropy)
        {
            var key = MachineKey(entropy);
            try
            {
                var nonce = RandomNumberGenerator.GetBytes(LinuxNonceSize);
                var ciphertext = new byte[data.Length];
                var tag = new byte[LinuxTagSize];
                using (var aes = new AesGcm(key, LinuxTagSize)) aes.Encrypt(nonce, data, ciphertext, tag);
                var result = new byte[LinuxHeaderSize + ciphertext.Length];
                LinuxMarker.CopyTo(result);
                nonce.CopyTo(result.AsSpan(LinuxMarkerSize));
                tag.CopyTo(result.AsSpan(LinuxMarkerSize + LinuxNonceSize));
                ciphertext.CopyTo(result.AsSpan(LinuxHeaderSize));
                return result;
            }
            finally { CryptographicOperations.ZeroMemory(key); }
        }

        static byte[] UnprotectLinux(byte[] data, byte[] entropy)
        {
            if (!data.AsSpan().StartsWith(LinuxMarker)) throw new ForeignSealException(OtherSystemMessage);
            if (data.Length <= LinuxHeaderSize) throw new CryptographicException("The Linux snapshot seal key is truncated.");
            var key = MachineKey(entropy);
            try
            {
                var plaintext = new byte[data.Length - LinuxHeaderSize];
                using (var aes = new AesGcm(key, LinuxTagSize))
                    aes.Decrypt(data.AsSpan(LinuxMarkerSize, LinuxNonceSize), data.AsSpan(LinuxHeaderSize),
                        data.AsSpan(LinuxMarkerSize + LinuxNonceSize, LinuxTagSize), plaintext);
                return plaintext;
            }
            finally { CryptographicOperations.ZeroMemory(key); }
        }

        static byte[] MachineKey(byte[] entropy)
        {
            var machineId = Encoding.ASCII.GetBytes(MachineId());
            return HKDF.DeriveKey(HashAlgorithmName.SHA256, machineId, 32, salt: null, info: entropy);
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
                // This is a Windows program under Wine on a Linux host, so its Z: drive is the host's root.
                yield return @"Z:\etc\machine-id";
                yield return @"Z:\var\lib\dbus\machine-id";
                yield break;
            }
            yield return "/etc/machine-id";
            yield return "/var/lib/dbus/machine-id";
        }

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
