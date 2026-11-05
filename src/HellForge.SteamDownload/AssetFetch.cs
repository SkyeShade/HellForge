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

    // The chunk cache is kept within this: a rebuild needs only the chunks its parts sit in, and they are fetched again
    // only if they were trimmed.
    public const long ChunkCacheLimit = 4L << 30;

    static string Size(long bytes) => bytes >= 1e6 ? $"{bytes / 1e6:F1} MB" : $"{Math.Max(1, bytes / 1000)} KB";
}
