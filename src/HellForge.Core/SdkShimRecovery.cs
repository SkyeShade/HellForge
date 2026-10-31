using System.Text.Json;

namespace HellForge.Core;

// A session swaps the copy's SDK DLLs for HellForge's stand-ins. If the session is cut short, the saved originals stay
// in temp/sdk-shim-originals. Restoring them belongs to Core so setup can do it before deciding the copy is incomplete.
public sealed record SdkShimRecoveryResult(IReadOnlyDictionary<string, string> Originals, IReadOnlyList<string> Recovered);

public static class SdkShimRecovery
{
    public const string LeftoverCloudOriginal = "steam_api64_hellforge.dll";
    static readonly string[] Names = ["PartyWin.dll", "PlayFabMultiplayerWin.dll", "steam_api64.dll"];

    // The stand-ins' build record and saved originals. A saved original is trusted only when its SHA-256 matches the
    // record's pin; an already restored target is left alone. Missing files are left for the caller's completeness or
    // session check to report.
    public static SdkShimRecoveryResult Recover(InstallLayout install)
    {
        var record = Path.Combine(install.App, "sdk-shims", "build.json");
        var build = SessionFiles.ReadJson(record)
            ?? throw new InvalidDataException("The SDK stand-ins are missing; reinstall HellForge.");
        if (build.ValueKind != JsonValueKind.Object || !build.TryGetProperty("originals", out var pins) || pins.ValueKind != JsonValueKind.Object)
            throw new InvalidDataException("The stand-ins' build record is incomplete; reinstall HellForge.");
        var originals = new Dictionary<string, string>(StringComparer.Ordinal);
        var recovered = new List<string>();
        var savedFolder = Path.Combine(install.Temp, "sdk-shim-originals");
        foreach (var name in Names)
        {
            var pin = SessionFiles.Text(pins, name);
            if (pin.Length == 0) throw new InvalidDataException("The stand-ins' build record is incomplete; reinstall HellForge.");
            originals[name] = pin;
            var target = Path.Combine(install.Game, "bin", name);
            var saved = Path.Combine(savedFolder, name);
            if (File.Exists(target) && LocalInstall.Hash(target).Equals(pin, StringComparison.OrdinalIgnoreCase)) continue;
            if (!File.Exists(saved) || !LocalInstall.Hash(saved).Equals(pin, StringComparison.OrdinalIgnoreCase)) continue;
            File.Copy(saved, target, overwrite: true);
            if (!LocalInstall.Hash(target).Equals(pin, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("The restored " + name + " does not match its pin.");
            recovered.Add(name);
        }
        var leftover = Path.Combine(install.Game, "bin", LeftoverCloudOriginal);
        if (File.Exists(leftover)) File.Delete(leftover);
        return new(originals, recovered);
    }
}
