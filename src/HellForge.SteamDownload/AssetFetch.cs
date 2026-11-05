using HellForge.Core;
using HellForge.Core.AssetMigration;
using HellForge.Core.GameData;

namespace HellForge.SteamDownload;

// A recipe mod (ModAssets) whose recipe takes content from a build this PC does not have, typically cut content from an
// older build: those builds are read from Steam with the player's own sign-in, only the chunks that hold the parts the
// recipe needs, and the mod is rebuilt from them and the builds this PC has. Nothing is fetched without the player
// asking (the Mods page's button, or assets-rebuild --steam yes).
public static class AssetFetch
{
    public sealed record Fetched(long Downloaded, long FromCache, long FromLocal, IReadOnlyList<string> Builds);

    // Rebuilds the mod in folder from this PC's builds and, for what they lack, the builds the recipe names, from Steam.
    public static Fetched Rebuild(string folder, InstallLayout? install, SteamBuilds steam, Action<string>? log = null)
    {
        var local = GameBuilds.Local(install).Select(GameBuilds.Open).ToList();
        var remote = new List<IGameResources>();
        try
        {
            var missing = ModAssets.MissingFrom(Recipe.LoadRecipe(folder), local);
            foreach (var build in missing)
            {
                log?.Invoke($"Reading {Size(build.Bytes)} of game files from build {build.Build} on Steam...");
                remote.Add(steam.Open(build.Build));
            }
            ModAssets.Ensure(folder, () => [.. local, .. remote], log);
            var store = steam.Store;
            store.Trim(ChunkCacheLimit);
            return new(store.Downloaded, store.FromCache, store.FromLocal, [.. missing.Select(m => m.Build)]);
        }
        finally
        {
            foreach (var game in local.Concat(remote)) (game as IDisposable)?.Dispose();
        }
    }

    // The resources a mod's data spec takes from past builds ("source": {"build": ...}) that its folder does not hold yet,
    // read from those builds on Steam and kept in the mod's folder (BuildResources). The builds asked for, or none.
    public static IReadOnlyList<string> FetchSpec(string folder, SteamBuilds steam, Action<string>? log = null)
    {
        var spec = DataSpecs.Load(folder) ?? throw new InvalidDataException($"{DataSpecs.FileName} is missing.");
        var missing = BuildResources.Missing(folder, spec);
        foreach (var (build, keys) in BuildResources.Wanted(spec).Where(w => missing.Contains(w.Key)))
        {
            log?.Invoke($"Reading {keys.Count} resources of build {build} from Steam...");
            var game = steam.Open(build);
            try
            {
                foreach (var key in keys)
                    BuildResources.Write(folder, build, key, game.Read(key, PartKind.Main) is { } main
                        ? new ResourceParts(main, game.Read(key, PartKind.Gpu) ?? [], game.Read(key, PartKind.Stream) ?? []) : null);
            }
            finally { (game as IDisposable)?.Dispose(); }
        }
        steam.Store.Trim(ChunkCacheLimit);
        return missing;
    }

    // Whether the mod's data spec takes resources from past builds that are not fetched yet.
    public static bool SpecNeedsSteam(string folder) =>
        DataSpecs.Load(folder) is { Builds.Count: > 0 } spec && BuildResources.Missing(folder, spec).Count > 0;

    // The chunk cache is kept within this: a rebuild needs only the chunks its parts sit in, and they are fetched again
    // only if they were trimmed.
    public const long ChunkCacheLimit = 4L << 30;

    static string Size(long bytes) => bytes >= 1e6 ? $"{bytes / 1e6:F1} MB" : $"{Math.Max(1, bytes / 1000)} KB";
}
