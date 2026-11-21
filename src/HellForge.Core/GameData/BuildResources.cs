namespace HellForge.Core.GameData;

// The resources a data spec takes from past builds ("source": {"build": ...}, cut content): fetched once from Steam
// with the player's own sign-in (HellForge.SteamDownload.AssetFetch), and kept in the mod's own folder,
// .hellforge-build-resources/<depot>_<manifest>/, a dot folder that neither friends' modlists, mod sync nor packages
// count. Each resource is its parts, <type>-<name>.main, .gpu and .stream, with a .none file for one that build does not
// have (a unit without physics). The spec's builder reads them from there (ModDataBuilder); a mod whose resources are not
// all there is pending and does not load (ModManifest.AssetsPending).
public static class BuildResources
{
    public const string FolderName = ".hellforge-build-resources";

    static string Folder(string modFolder, string build) => Path.Combine(modFolder, FolderName, build.Replace(':', '_'));
    static string Stem(ResourceKey key) => $"{key.Type:x16}-{key.Name:x16}";

    // The resources the spec reads from each past build: each copied or included one, and a unit copy's physics.
    public static Dictionary<string, HashSet<ResourceKey>> Wanted(DataSpec spec)
    {
        var wanted = new Dictionary<string, HashSet<ResourceKey>>(StringComparer.Ordinal);
        foreach (var resource in spec.Patches.SelectMany(p => p.Resources).Where(r => r.Build != null))
        {
            if (!wanted.TryGetValue(resource.Build!, out var keys)) wanted[resource.Build!] = keys = [];
            switch (resource)
            {
                case CopyResource copy:
                    keys.Add(new(copy.Type, copy.From));
                    if (copy.Type == ResourceTypes.Unit) keys.Add(new(ResourceTypes.Physics, copy.From));
                    break;
                case IncludeResources include:
                    foreach (var id in include.Ids) keys.Add(new(include.Type, id));
                    break;
            }
        }
        return wanted;
    }

    // The past builds the spec needs resources of that the mod folder does not hold yet.
    public static List<string> Missing(string modFolder, DataSpec spec) =>
        [.. Wanted(spec).Where(w => w.Value.Any(k => !Known(modFolder, w.Key, k))).Select(w => w.Key)];

    static bool Known(string modFolder, string build, ResourceKey key)
    {
        var stem = Path.Combine(Folder(modFolder, build), Stem(key));
        return File.Exists(stem + ".main") || File.Exists(stem + ".none");
    }

    // A resource of a past build, as fetched; null when that build does not have it. Throws IOException when it was not
    // fetched (the mod is pending until it is).
    public static ResourceParts? Read(string modFolder, string build, ResourceKey key)
    {
        var stem = Path.Combine(Folder(modFolder, build), Stem(key));
        if (File.Exists(stem + ".none")) return null;
        if (!File.Exists(stem + ".main")) throw new FileNotFoundException($"{key} of build {build} is not fetched yet (the Mods page's GET THEM FROM STEAM fetches it).");
        static byte[] Part(string path) => File.Exists(path) ? File.ReadAllBytes(path) : [];
        return new(Part(stem + ".main"), Part(stem + ".gpu"), Part(stem + ".stream"));
    }

    // Keeps a fetched resource (or that the build has none): each part written whole, then moved into place, the main
    // part last, since it is what says the resource is there.
    public static void Write(string modFolder, string build, ResourceKey key, ResourceParts? parts)
    {
        var folder = Folder(modFolder, build);
        Directory.CreateDirectory(folder);
        var stem = Path.Combine(folder, Stem(key));
        void Put(string path, byte[] bytes)
        {
            var partial = path + "." + Guid.NewGuid().ToString("N")[..8] + ".partial";
            File.WriteAllBytes(partial, bytes);
            File.Move(partial, path, overwrite: true);
        }
        if (parts == null)
        {
            Put(stem + ".none", []);
            return;
        }
        if (parts.Gpu.Length > 0) Put(stem + ".gpu", parts.Gpu);
        if (parts.Stream.Length > 0) Put(stem + ".stream", parts.Stream);
        Put(stem + ".main", parts.Main);
    }

    // What the cache key of a spec's build takes from its past builds' resources: their sizes (their bytes come from
    // Steam by the build's manifest, so the same spec reads the same bytes).
    public static string Stamp(string modFolder, DataSpec spec)
    {
        var lines = new System.Text.StringBuilder();
        foreach (var (build, keys) in Wanted(spec).OrderBy(w => w.Key, StringComparer.Ordinal))
            foreach (var key in keys.OrderBy(k => k.Type).ThenBy(k => k.Name))
            {
                var stem = Path.Combine(Folder(modFolder, build), Stem(key));
                var size = File.Exists(stem + ".none") ? -1 : File.Exists(stem + ".main") ? new FileInfo(stem + ".main").Length : -2;
                lines.Append(build).Append('/').Append(Stem(key)).Append(' ').Append(size).Append('\n');
            }
        return lines.ToString();
    }
}
