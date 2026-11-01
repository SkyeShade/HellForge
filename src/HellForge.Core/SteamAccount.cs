using System.Globalization;
using System.Text.Json;
using Microsoft.Win32;

namespace HellForge.Core;

// The Steam account the local Steam client is signed in to, as a SteamID64: the client keeps its 32-bit account id in
// HKCU\Software\Valve\Steam\ActiveProcess "ActiveUser" (0 while signed out). Null when Steam is signed out or not running.
// The Linux client keeps no file with the signed-in account, so native Linux asks the probe Steam itself uses and keeps
// its private answer for a few minutes. A Windows program under Wine gets the native side's answer from the environment.
public static class SteamAccount
{
    const ulong IndividualAccounts = 76561197960265728;
    public const string EnvironmentVariable = "HELLFORGE_STEAM_ACCOUNT";
    public static readonly TimeSpan ProbeFreshness = TimeSpan.FromMinutes(5);
    static readonly TimeSpan ClockSkew = TimeSpan.FromSeconds(5);
    static readonly object ProbeGate = new();
    static ProbeResult? lastProbe;

    // What HellForge.SteamProbe writes beside its report when a Linux caller asks for the account: the low 32 bits of
    // the SteamID64, never the whole SteamID64. It is not a report and must not be logged or copied as evidence.
    public sealed record ProbeResult(uint AccountId, DateTime CheckedUtc);

    public static ulong FromAccountId(uint accountId) => IndividualAccounts + accountId;

    public static uint AccountId(ulong steamId64) => unchecked((uint)steamId64);

    // The signed-in account, from each process's own source. Native Linux uses only a probe result already in this
    // process; Current(InstallLayout) runs the probe when there is none.
    public static ulong? Current()
    {
        if (LinuxNetworkBlock.UnderWine)
            return FromWineEnvironment(Environment.GetEnvironmentVariable(EnvironmentVariable), underWine: true);
        if (OperatingSystem.IsWindows())
            return Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam\ActiveProcess", "ActiveUser", null) is int id && id != 0
                ? FromAccountId(unchecked((uint)id)) : null;
        if (!OperatingSystem.IsLinux()) return null;
        lock (ProbeGate)
            return Fresh(lastProbe, DateTime.UtcNow) ? FromAccountId(lastProbe!.AccountId) : null;
    }

    // Native Linux: a fresh probe result, else run the ownership probe through the installation's wrapper and use the
    // account it reports. Windows and Wine keep their own rules; install is ignored there.
    public static ulong? Current(InstallLayout install)
    {
        if (Current() is { } cached) return cached;
        if (!OperatingSystem.IsLinux() || LinuxNetworkBlock.UnderWine) return null;
        try { ProbeLinux(install); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or System.ComponentModel.Win32Exception)
        {
            throw new InvalidOperationException("The Steam account probe failed: " + error.Message, error);
        }
        return Current();
    }

    // HELLFORGE_STEAM_ACCOUNT from the native side. Only a Windows program under Wine may use it: native Linux and
    // Windows pass underWine false, so a value set in either environment is ignored.
    public static ulong? FromWineEnvironment(string? value, bool underWine) =>
        underWine && uint.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out var accountId) && accountId != 0
            ? FromAccountId(accountId) : null;

    // The private probe result at path, or null when it is missing, broken, or did not confirm a signed-in account.
    public static ProbeResult? ReadProbeResult(string path)
    {
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(path));
            var root = document.RootElement;
            return root.ValueKind == JsonValueKind.Object &&
                   root.TryGetProperty("loggedOn", out var loggedOn) && loggedOn.ValueKind == JsonValueKind.True &&
                   root.TryGetProperty("accountId", out var account) && account.ValueKind == JsonValueKind.Number &&
                   account.TryGetUInt32(out var accountId) && accountId != 0 &&
                   root.TryGetProperty("checkedUtc", out var checkedUtc) && checkedUtc.ValueKind == JsonValueKind.String &&
                   checkedUtc.TryGetDateTime(out var when)
                ? new(accountId, DateTime.SpecifyKind(when.ToUniversalTime(), DateTimeKind.Utc))
                : null;
        }
        catch (Exception error) when (error is JsonException or IOException or UnauthorizedAccessException) { return null; }
    }

    // A probe result is usable for a few minutes. A result a little in the future is accepted for clock skew; an older
    // one is not, because the player may have switched Steam accounts since.
    public static bool Fresh(ProbeResult? result, DateTime nowUtc) =>
        result != null && result.CheckedUtc <= nowUtc + ClockSkew && nowUtc - result.CheckedUtc < ProbeFreshness;

    // Reads the probe's private account handoff and deletes it. Only a fresh, logged-on result replaces the cached one.
    internal static void RememberProbe(string path) => _ = TakeProbeResult(path);

    // The session runner under Wine adopts the result of its own ownership probe: read it, delete it, cache it, and
    // return the account so the runner can put it in its environment for the backend, the game and later calls.
    public static ulong? TakeProbeResult(string path)
    {
        var result = ReadProbeResult(path);
        try { File.Delete(path); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        if (result is not { } value || !Fresh(value, DateTime.UtcNow)) return null;
        lock (ProbeGate) lastProbe = value;
        return FromAccountId(value.AccountId);
    }

    // The probe through the same wrapper and timeout as setup's ownership check. Its report is evidence only; the
    // account comes from the separate private file, which RunProbeThroughLinuxWrapper reads and deletes.
    static void ProbeLinux(InstallLayout install)
    {
        var wrapper = Path.Combine(install.App, "linux", "hellforge-linux.sh");
        var probe = Path.Combine(install.App, "steamprobe", "HellForge.SteamProbe.exe");
        var steamApi = Path.Combine(install.Game, "bin", "steam_api64.dll");
        var proton = SteamLocator.SelectedProtonScript(install) ?? SteamLocator.ProtonScript();
        var steamRoot = SteamLocator.SteamRoot(install);
        if (!File.Exists(wrapper) || !File.Exists(probe))
            throw new InvalidOperationException("The Steam account probe is missing: reinstall HellForge.");
        if (!File.Exists(steamApi))
            throw new InvalidOperationException("The Steam account probe needs the copied game's steam_api64.dll: finish setup first.");
        if (proton == null || steamRoot == null)
            throw new InvalidOperationException("Steam or Proton was not found: start Steam and sign in, then try again.");
        var folder = Directory.CreateDirectory(Path.Combine(install.Temp, "steam-account")).FullName;
        var run = Guid.NewGuid().ToString("N");
        var evidence = Path.Combine(folder, run + ".evidence.json");
        var account = Path.Combine(folder, run + ".account.json");
        var request = new SteamOwnershipCheck.LinuxProbeRequest(wrapper, proton, install.Root,
            WinePath.PrefixFromInstallRoot(install.Root), steamRoot, probe, steamApi, Path.Combine(install.Temp, "ownership"),
            evidence, SteamOwnershipCheck.LinuxProbeTimeout, account);
        try { SteamOwnershipCheck.RunProbeThroughLinuxWrapper(request); }
        catch (InvalidDataException error) { throw new InvalidOperationException(error.Message, error); }
        finally
        {
            try { File.Delete(evidence); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        }
    }
}
