using HellForge.Core;
using HellForge.Core.AssetMigration;
using HellForge.Core.GameData;
using HellForge.SteamDownload;

namespace HellForge.Cli;

// assets-check, assets-strip and assets-rebuild (docs/modding/assets.md) on the console: AssetSharing and AssetFetch with
// a terminal QR sign-in.
static class AssetCommands
{
    // --install, else the installation whose mods folder holds the mod, else the one this program is in; null when none
    // is (the Steam install alone is used then).
    public static InstallLayout? Installation(string? given, string modFolder)
    {
        if (given != null)
        {
            var layout = new InstallLayout(Path.GetFullPath(given));
            return layout.Exists ? layout : throw new InvalidDataException($"{layout.Root} is not a HellForge installation.");
        }
        var parent = Path.GetDirectoryName(Path.TrimEndingDirectorySeparator(modFolder));
        if (parent != null && Path.GetFileName(parent).Equals("mods", StringComparison.OrdinalIgnoreCase) && Path.GetDirectoryName(parent) is { } root &&
            new InstallLayout(root) is { Exists: true } beside) return beside;
        for (var dir = AppContext.BaseDirectory; dir != null; dir = Path.GetDirectoryName(Path.TrimEndingDirectorySeparator(dir)))
            if (new InstallLayout(dir) is { Exists: true } around) return around;
        return null;
    }

    // Checks (outFolder null) or strips the mod (AssetSharing.Check), and says what it did. history: also against every
    // build since release, from Steam, after a QR sign-in, when the builds here leave bytes the mod still ships.
    public static AssetSharing.Checked Run(string modFolder, InstallLayout? install, string? outFolder, bool history = false)
    {
        using var cancel = new CancellationTokenSource();
        var check = AssetSharing.Check(modFolder, outFolder, null, Context(install, history, cancel));
        Console.Error.WriteLine(check.Result.Summary);
        Console.Error.WriteLine($"Report: {check.ReportPath}");
        return check;
    }

    public static AssetSharing.Context Context(InstallLayout? install, bool history, CancellationTokenSource cancel)
    {
        if (history) Console.CancelKeyPress += (_, e) => { e.Cancel = true; cancel.Cancel(); };
        return new(install, history ? new SteamDownloadCommand.TerminalPrompt() : null, Console.Error.WriteLine, cancel.Token, SteamLogins.Default);
    }

    static SteamDepotSession SignIn(CancellationTokenSource cancel)
    {
        Console.CancelKeyPress += (_, e) => { e.Cancel = true; cancel.Cancel(); };
        return SteamDepotSession.SignInAsync(new SteamDownloadCommand.TerminalPrompt(), cancel.Token, SteamLogins.Default).GetAwaiter().GetResult();
    }

    // assets-rebuild --steam yes: signs in to Steam (a saved sign-in, else a QR code for the Steam app; SteamLogins) and rebuilds the mod
    // from this PC's builds plus what it needs of the builds it lacks, fetched from Steam.
    public static AssetFetch.Fetched RebuildFromSteam(string modFolder, InstallLayout? install, bool recipe = true)
    {
        using var cancel = new CancellationTokenSource();
        var session = SignIn(cancel);
        try
        {
            var steam = SteamBuilds.For(session, install, cancel.Token);
            var specBuilds = AssetFetch.SpecNeedsSteam(modFolder) ? AssetFetch.FetchSpec(modFolder, steam, Console.Error.WriteLine) : [];
            if (recipe) return AssetFetch.Rebuild(modFolder, install, steam, Console.Error.WriteLine);
            return new(steam.Store.Downloaded, steam.Store.FromCache, steam.Store.FromLocal, specBuilds);
        }
        finally { session.DisposeAsync().AsTask().GetAwaiter().GetResult(); }
    }
}
