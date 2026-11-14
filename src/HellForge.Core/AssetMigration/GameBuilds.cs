using HellForge.Core.GameData;

namespace HellForge.Core.AssetMigration;

// A build of the game's data on this PC: HellForge's own game copy (the supported build) or HELLDIVERS 2 as Steam has it
// installed. Label is how recipes and indexes name the build: "<data depot>:<manifest>" (553853 holds the game's data),
// the same on every PC that has it and what Steam fetches it by. Build is Steam's build id, for people to read.
public sealed record LocalBuild(string Label, string Build, string DataFolder, string What);

public static class GameBuilds
{
    public const uint DataDepot = 553853;

    public static string Label(uint depot, string manifest) => $"{depot}:{manifest}";

    // The builds this PC has: the installation's game copy (when given and set up) first, then Steam's, without the same
    // build twice. Recipes and the index name a build by its data manifest, so a copy and a Steam install of one build
    // are one build.
    public static List<LocalBuild> Local(InstallLayout? install, string? steamRoot = null)
    {
        var found = new List<LocalBuild>();
        if (install != null && File.Exists(Path.Combine(install.Game, "data", GameDataReader.IndexFile)))
        {
            var pinned = SupportedBuild.Current;
            if (pinned.Depots.FirstOrDefault(d => d.Id == DataDepot) is { } depot)
                found.Add(new(Label(DataDepot, depot.ManifestId), pinned.BuildId, Path.Combine(install.Game, "data"), $"HellForge's game copy (build {pinned.BuildId})"));
        }
        if (Steam(steamRoot) is { } steam && found.All(b => b.Label != steam.Label)) found.Add(steam);
        return found;
    }

    // HELLDIVERS 2 as Steam has it installed, labelled by the data manifest its app manifest says is installed; null when
    // Steam does not have the game, or its app manifest does not say.
    public static LocalBuild? Steam(string? steamRoot = null)
    {
        SteamGame? game;
        try { game = SteamLocator.FindGame(steamRoot); }
        catch (Exception error) when (error is InvalidDataException or IOException or UnauthorizedAccessException) { return null; }
        if (game == null) return null;
        try
        {
            var app = ValveData.Parse(File.ReadAllText(game.ManifestPath)).Children["AppState"];
            if (!app.Children.TryGetValue("InstalledDepots", out var depots) || !depots.Children.TryGetValue(DataDepot.ToString(), out var data) ||
                !data.Values.TryGetValue("manifest", out var manifest) || manifest.Length == 0) return null;
            var folder = Path.Combine(game.Report.Source, "data");
            if (!File.Exists(Path.Combine(folder, GameDataReader.IndexFile)) && !Directory.Exists(folder)) return null;
            return new(Label(DataDepot, manifest), game.Report.Pin.BuildId, folder, $"HELLDIVERS 2 in Steam (build {game.Report.Pin.BuildId})");
        }
        catch (Exception error) when (error is InvalidDataException or IOException or UnauthorizedAccessException or KeyNotFoundException) { return null; }
    }

    // A build's data, read as its layout is: bundles.nxa (Steam's packed data since February 2026), the loose archives of
    // older builds (one file per archive, named by hash, .gpu_resources and .stream beside it), or both (the transition
    // builds of December 2025 to January 2026).
    public static IGameResources Open(IDataFiles files, string label)
    {
        if (!files.Exists(GameDataReader.IndexFile)) return new LooseGameResources(files, label);
        return files.List().Any(n => n.Length == 16 && !n.Contains('.')) ? new MixedGameResources(files, label) : new GameResources(files, label);
    }

    public static IGameResources Open(LocalBuild build) => Open(new FolderFiles(build.DataFolder), build.Label);
}
