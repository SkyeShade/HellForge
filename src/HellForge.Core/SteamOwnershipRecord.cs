using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace HellForge.Core;

// A passed ownership check, as a session may reuse it instead of asking Steam again (issue #43): which installation,
// which Steam account (SteamOwnershipCheck.AccountHash), which game build and when.
public sealed record OwnershipConfirmation(string InstallRoot, DateTime InstallCreatedUtc, string AccountHash, string BuildId, DateTime ConfirmedUtc);

// What a session has now to judge a confirmation by: its installation, the Steam account signed in (null: none, or Steam
// is not running), the build HellForge runs and the time.
public sealed record OwnershipContext(string InstallRoot, DateTime InstallCreatedUtc, uint? ActiveAccount, string BuildId, DateTime NowUtc);

// The last passed ownership check of an installation (temp/steam-ownership.json, InstallLayout.OwnershipRecord), which
// a session reuses while all of Problem's conditions hold; anything else runs the full check. A failed check removes it
// and is never recorded. The record is sealed with Windows DPAPI for the Windows user who wrote it: an edited record, or
// one copied from another PC or Windows account, does not open, and counts as none. It is a convenience, not a licence:
// whoever can run code as that user could write one, as they could change HellForge itself.
public static class SteamOwnershipRecord
{
    // How long a confirmation stands in for the check. A record dated later than now by more than the skew is refused
    // too (a clock set back would otherwise stretch it).
    public static readonly TimeSpan Limit = TimeSpan.FromDays(7);
    static readonly TimeSpan Skew = TimeSpan.FromMinutes(5);
    const int SchemaVersion = 1;
    static readonly byte[] Entropy = Encoding.UTF8.GetBytes("HellForge.SteamOwnershipRecord.v1");

    // Null when the confirmation may stand in for the check now; else why the full check runs.
    public static string? Problem(OwnershipConfirmation? record, OwnershipContext now)
    {
        if (record == null) return "no earlier confirmation";
        if (now.InstallCreatedUtc == DateTime.MinValue || record.InstallCreatedUtc != now.InstallCreatedUtc ||
            !string.Equals(record.InstallRoot, now.InstallRoot, StringComparison.OrdinalIgnoreCase))
            return "the confirmation is for another installation";
        if (now.ActiveAccount is not { } account) return "Steam is not running or no account is signed in";
        if (!string.Equals(record.AccountHash, SteamOwnershipCheck.AccountHash(account), StringComparison.Ordinal))
            return "another Steam account is signed in";
        if (record.BuildId != now.BuildId) return "the confirmation is for another game build";
        if (record.ConfirmedUtc - now.NowUtc > Skew) return "the confirmation is dated in the future";
        if (now.NowUtc - record.ConfirmedUtc >= Limit) return "the confirmation is older than " + Limit.TotalDays.ToString(CultureInfo.InvariantCulture) + " days";
        return null;
    }

    // The confirmation in the record at path; null when there is none or it does not open (edited, from another Windows
    // user or PC, another schema).
    public static OwnershipConfirmation? Read(string path)
    {
        try
        {
            if (!File.Exists(path)) return null;
            using var outer = JsonDocument.Parse(File.ReadAllText(path));
            if (outer.RootElement.ValueKind != JsonValueKind.Object || !outer.RootElement.TryGetProperty("sealed", out var sealedText) ||
                sealedText.ValueKind != JsonValueKind.String || Unseal(Convert.FromBase64String(sealedText.GetString()!)) is not { } payload)
                return null;
            using var inner = JsonDocument.Parse(payload);
            return FromJson(inner.RootElement);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or FormatException or InvalidOperationException)
        {
            return null;
        }
    }

    static OwnershipConfirmation? FromJson(JsonElement root)
    {
        if (root.ValueKind != JsonValueKind.Object || !root.TryGetProperty("schemaVersion", out var version) ||
            !version.TryGetInt32(out var number) || number != SchemaVersion)
            return null;
        string? Text(string name) => root.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() : null;
        DateTime? Time(string name) => DateTime.TryParse(Text(name), CultureInfo.InvariantCulture,
            DateTimeStyles.AdjustToUniversal | DateTimeStyles.AssumeUniversal, out var value) ? value : null;
        return Text("installRoot") is { } installRoot && Time("installCreatedUtc") is { } created && Text("accountHash") is { } account &&
               Text("buildId") is { } build && Time("confirmedUtc") is { } confirmed
            ? new(installRoot, created, account, build, confirmed)
            : null;
    }

    // Writes the record (beside it, then moved into place). False when it could not be sealed or written: the next session
    // then runs the full check.
    public static bool Write(string path, OwnershipConfirmation record)
    {
        var payload = Encoding.UTF8.GetBytes(new JsonObject
        {
            ["schemaVersion"] = SchemaVersion, ["installRoot"] = record.InstallRoot, ["installCreatedUtc"] = Utc(record.InstallCreatedUtc),
            ["accountHash"] = record.AccountHash, ["buildId"] = record.BuildId, ["confirmedUtc"] = Utc(record.ConfirmedUtc)
        }.ToJsonString());
        if (Seal(payload) is not { } sealedBytes) return false;
        var text = new JsonObject
        {
            ["schemaVersion"] = SchemaVersion,
            ["about"] = "HellForge's last passed Steam ownership check of this installation, sealed for this Windows user. Delete it to run the full check.",
            ["sealed"] = Convert.ToBase64String(sealedBytes)
        }.ToJsonString();
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path + ".partial", text);
            File.Move(path + ".partial", path, overwrite: true);
            return true;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return false; }
    }

    static string Utc(DateTime value) =>
        (value.Kind == DateTimeKind.Local ? value.ToUniversalTime() : DateTime.SpecifyKind(value, DateTimeKind.Utc)).ToString("o", CultureInfo.InvariantCulture);

    // Removes the record, after a failed check. False when it is still there.
    public static bool Forget(string path)
    {
        try { File.Delete(path); return true; }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return !File.Exists(path); }
    }

    // After a full check of the installation (started at checkedUtc, its evidence at evidence): a pass the evidence bears out
    // is recorded, anything else removes the record, so a failed check is never reused and ends the reuse of an earlier
    // one. True when the record now stands for this pass.
    public static bool AfterCheck(InstallLayout install, string evidence, bool passed, DateTime checkedUtc)
    {
        if (passed && SteamOwnershipCheck.Problem(evidence, checkedUtc) == null &&
            FromEvidence(evidence, install, SupportedBuild.Current.BuildId, checkedUtc) is { } confirmation)
            return Write(install.OwnershipRecord, confirmation);
        Forget(install.OwnershipRecord);
        return false;
    }

    // The installation's record and whether it may stand in for the check now (why not, or null).
    public static (OwnershipConfirmation? Record, string? Problem) Reusable(InstallLayout install)
    {
        var record = Read(install.OwnershipRecord);
        return (record, Problem(record, Now(install)));
    }

    // The record after a passed check: the evidence's account, this installation and build, the time of the check. Null when
    // the evidence names no account (a probe from before the account was reported).
    public static OwnershipConfirmation? FromEvidence(string evidence, InstallLayout install, string buildId, DateTime checkedUtc) =>
        SteamOwnershipCheck.ConfirmedAccount(evidence) is { } account && Created(install) is { } created
            ? new(install.Root, created, account, buildId, checkedUtc)
            : null;

    // The session's side: this installation, the account the Steam client has signed in, the build HellForge runs, now.
    // An installation whose marker does not read matches no record (DateTime.MinValue is never recorded).
    public static OwnershipContext Now(InstallLayout install) =>
        new(install.Root, Created(install) ?? DateTime.MinValue, ActiveAccount(), SupportedBuild.Current.BuildId, DateTime.UtcNow);

    // When the installation was made (its marker's "createdUtc"): a folder made again in the same place is another
    // installation. Read without InstallLayout.ReadMarker, whose reflection-based JSON the NativeAOT session runner cannot use.
    static DateTime? Created(InstallLayout install)
    {
        try
        {
            var marker = File.Exists(install.Marker) ? install.Marker : Path.Combine(install.Root, InstallLayout.LegacyMarkerFile);
            using var document = JsonDocument.Parse(File.ReadAllText(marker));
            return document.RootElement.ValueKind == JsonValueKind.Object && document.RootElement.TryGetProperty("createdUtc", out var created) &&
                   created.ValueKind == JsonValueKind.String && created.TryGetDateTime(out var value)
                ? DateTime.SpecifyKind(value.ToUniversalTime(), DateTimeKind.Utc)
                : null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException) { return null; }
    }

    // The Steam client's signed-in account (HKCU\Software\Valve\Steam\ActiveProcess\ActiveUser, as GameSettings reads it),
    // or null when no Steam process runs or no account is signed in (0).
    public static uint? ActiveAccount()
    {
        if (!OperatingSystem.IsWindows()) return null;
        var running = Process.GetProcessesByName("steam");
        foreach (var process in running) process.Dispose();
        if (running.Length == 0) return null;
        return Microsoft.Win32.Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam\ActiveProcess", "ActiveUser", null) is int account && account != 0
            ? unchecked((uint)account)
            : null;
    }

    static byte[]? Seal(byte[] data) => Crypt(data, protect: true);
    static byte[]? Unseal(byte[] data) => Crypt(data, protect: false);

    // DPAPI for the current Windows user (CryptProtectData / CryptUnprotectData); null where it fails or is not Windows.
    static byte[]? Crypt(byte[] data, bool protect)
    {
        if (!OperatingSystem.IsWindows()) return null;
        var input = Pin(data);
        var entropy = Pin(Entropy);
        var output = new DataBlob();
        try
        {
            var done = protect
                ? CryptProtectData(ref input.Blob, null, ref entropy.Blob, 0, 0, UiForbidden, ref output)
                : CryptUnprotectData(ref input.Blob, 0, ref entropy.Blob, 0, 0, UiForbidden, ref output);
            if (!done || output.Data == 0) return null;
            var result = new byte[output.Size];
            Marshal.Copy(output.Data, result, 0, result.Length);
            return result;
        }
        finally
        {
            input.Handle.Free();
            entropy.Handle.Free();
            if (output.Data != 0) LocalFree(output.Data);
        }
    }

    static (GCHandle Handle, DataBlob Blob) Pin(byte[] data)
    {
        var handle = GCHandle.Alloc(data, GCHandleType.Pinned);
        return (handle, new DataBlob { Size = data.Length, Data = handle.AddrOfPinnedObject() });
    }

    const int UiForbidden = 1; // CRYPTPROTECT_UI_FORBIDDEN

    [StructLayout(LayoutKind.Sequential)]
    struct DataBlob { public int Size; public nint Data; }

    [DllImport("crypt32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool CryptProtectData(ref DataBlob input, string? description, ref DataBlob entropy, nint reserved, nint prompt, int flags, ref DataBlob output);

    [DllImport("crypt32.dll", SetLastError = true)]
    static extern bool CryptUnprotectData(ref DataBlob input, nint description, ref DataBlob entropy, nint reserved, nint prompt, int flags, ref DataBlob output);

    [DllImport("kernel32.dll")]
    static extern nint LocalFree(nint memory);
}
