using System.Buffers.Binary;
using System.Globalization;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using HellForge.Core.GameData;

namespace HellForge.Core;

// A HellForge mod: a folder in <installation>/mods with a hellforge-mod.json manifest, or a .zip of one (ModArchives), and optionally
//   code    a DLL written against the HellForge mod SDK (native/mod-sdk/hellforge_mod.h), loaded into the game by the mod loader;
//   data/   the game's own mod format (<bundle>.patch_<n>, .gpu_resources, .stream), staged into game/data per session;
//   hellforge-data.json  a data spec: patches HellForge builds from the player's own game data (GameData/ModDataBuilder.cs)
//           and stages like data/, so the mod ships no game bytes;
//   native  engine edits HellForge applies in the running game's memory (docs/modding/data.md "Native edits in the manifest"):
//     networkFieldTypes  widen a named network field type (only widen; kind, bits and min must be as expected);
//     componentWrites    set u32 fields of an entity's component record, only from their expected vanilla value.
// A gameplay mod changes how the game plays; friends must run the same gameplay mods to play together.
public sealed record NetworkFieldTypeEdit(string Name, int ExpectKind, int ExpectBits, int ExpectMin, int Bits);
public sealed record ComponentFieldEdit(string Name, int Offset, uint Expect, uint Value);
public sealed record ComponentWrite(string Component, ulong Resource, IReadOnlyList<ComponentFieldEdit> Fields);
public sealed record ModManifest(string Id, string Name, string Version, string Description, bool Gameplay, string Folder,
    IReadOnlyList<NetworkFieldTypeEdit> NetworkFieldTypes, IReadOnlyList<ComponentWrite> ComponentWrites, IReadOnlyList<string> DataFiles)
{
    public bool HasNative => NetworkFieldTypes.Count > 0 || ComponentWrites.Count > 0;
    // The mod's code DLL (a file name in its folder), loaded by the HellForge mod loader.
    public string? CodeDll { get; init; }
    public bool HasCode => CodeDll != null;
    // A source mod (C or C++ under src/): HellForge builds it before each session (ModBuilds) and loads that build, BuiltDll;
    // a prebuilt mod's DLL is the one in its folder.
    public bool HasSource { get; init; }
    public string? BuiltDll { get; init; }
    public string? DllPath => BuiltDll ?? (CodeDll != null && !HasSource ? Path.Combine(Folder, CodeDll) : null);
    public IReadOnlyList<StatsEdit> Stats { get; init; } = [];
    public bool NeedsLoader => HasCode || Stats.Count > 0 || ComponentWrites.Count > 0;
    // Off in the installation's hellforge-mods.json (the launcher's Mods page): listed, but not loaded.
    public bool Enabled { get; init; } = true;
    // The zip in the mods folder this mod was unpacked from (its Folder is then under mods/.unpacked), or null.
    public string? Archive { get; init; }
    // The mod has a data spec (hellforge-data.json), whose patches are generated per installation (ModDataCache).
    public bool HasDataSpec { get; init; }
    // Why HellForge does not check all of the data spec's output (DataSpecs.Unchecked: a "replace", or a type outside the
    // reviewed ones), or null: mod sync takes such a spec as raw data (ModSets.ClassOf).
    public string? UncheckedSpec { get; init; }
    // The folder of the spec's generated patches for this session, set by ModDataCache.Prepare.
    public string? GeneratedData { get; init; }
    // A cosmetic pack (CosmeticPacks): its options, the player's choice, and the folders that choice stages.
    public CosmeticPacks.Pack? Pack { get; init; }
    public CosmeticPacks.Choice PackChoice { get; init; } = CosmeticPacks.Choice.Default;
    public IReadOnlyList<string> PackFolders { get; init; } = [];
    // The Mods page's groups: gameplay mods (friends need the same ones; the same order is up to them) and cosmetic ones.
    public bool Cosmetic => !Gameplay;
    // The picture the Mods page shows for it (a manifest's "icon", a pack's IconPath): a file in its folder, or null.
    public string? Icon { get; init; }
    // A patch of it holds Lua scripts (ModCatalog.HoldsLua): an HD2Runtime or Bingus Shared Loader mod, or one made for them.
    public bool Lua { get; init; }
    // Where players get it (the manifest's "url", an https:// address: ModCatalog.WebAddress), or null. The Mods page links
    // it, and mod sync's prompt offers it (GET IT) to a joiner who has to install the mod themselves.
    public string? Url { get; init; }
}

public static partial class ModCatalog
{
    public const string ManifestName = "hellforge-mod.json";
    // The manifest name of mods made before the rename (HD2 Beyond The Stars), still read.
    public const string LegacyManifestName = "bts-mod.json";

    // A folder's manifest, under the current or the legacy name; null when it has none.
    public static string? ManifestIn(string folder) =>
        File.Exists(Path.Combine(folder, ManifestName)) ? Path.Combine(folder, ManifestName)
        : File.Exists(Path.Combine(folder, LegacyManifestName)) ? Path.Combine(folder, LegacyManifestName) : null;
    // The mod SDK version this HellForge provides (native/mod-sdk/hellforge_mod.h).
    public const int SdkVersion = 15;

    [GeneratedRegex("^[A-Za-z0-9_.-]{1,64}\\.dll$")]
    private static partial Regex DllName();

    [GeneratedRegex("^[a-z0-9][a-z0-9._-]{0,63}$")]
    private static partial Regex ModId();
    public static bool IsModId(string id) => ModId().IsMatch(id);

    // A mod's web address ("url" in its manifest; a mod set's entry from a host): https:// only, at most MaxUrl characters,
    // a host name with a dot and the default port, and nothing a browser or the shell could read as something else (no user
    // name or password, spaces, quotes, backslashes, control or non-ASCII characters). The address as given, or null.
    public const int MaxUrl = 200;
    public static string? WebAddress(string? text)
    {
        if (text == null || text.Length is < 12 or > MaxUrl || !text.StartsWith("https://", StringComparison.Ordinal)) return null;
        if (text.Any(c => c <= ' ' || c >= 0x7F || c is '"' or '\\' or '\'' or '<' or '>' or '`' or '^' or '{' or '}' or '|')) return null;
        return Uri.TryCreate(text, UriKind.Absolute, out var uri) && uri.Scheme == Uri.UriSchemeHttps && uri.HostNameType == UriHostNameType.Dns &&
            uri.Host.Contains('.') && uri.UserInfo.Length == 0 && uri.IsDefaultPort ? text : null;
    }

    [GeneratedRegex("^[0-9a-f]{16}\\.patch_([0-9]{1,4})(\\.gpu_resources|\\.stream)?$")]
    public static partial Regex PatchFile();

    [GeneratedRegex("^[a-z0-9_]{1,64}$")]
    private static partial Regex FieldTypeName();

    // Lua mods (HD2Runtime, Bingus Shared Loader and the mods made for them) ship as game patches holding a lua resource.
    // HellForge stages them like any patch; HD2Runtime runs on HellForge's Lua bridge there (docs/lua-bridge.md), so its
    // mods' changes are HellForge live edits. They are the player's own: client-only, never in the modlist code.

    // A patch file (<archive>.patch_<n>) whose resource table lists the lua type. Only the table at its start is read.
    public static bool HoldsLua(string patchFile)
    {
        var head = new byte[72 + 32 * 256];
        int read;
        using (var stream = new FileStream(patchFile, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
            read = stream.ReadAtLeast(head, head.Length, throwOnEndOfStream: false);
        if (read < 72) return false;
        var types = BinaryPrimitives.ReadInt32LittleEndian(head.AsSpan(4));
        for (var t = 0; t < types && 72 + 32 * t + 32 <= read; t++)
            if (BinaryPrimitives.ReadUInt64LittleEndian(head.AsSpan(72 + 32 * t + 8)) == ResourceTypes.Lua) return true;
        return false;
    }

    // Every mod folder with a manifest, by id, with its on/off state. Mods that fail validation are reported, not
    // listed. Only enabled mods are checked against each other and loaded (Active).
    public static (List<ModManifest> Mods, List<string> Problems) Load(string modsFolder, EntityProfile? profile = null)
    {
        var mods = new List<ModManifest>();
        var problems = new List<string>();
        if (!Directory.Exists(modsFolder)) return (mods, problems);
        profile ??= EntityProfile.Current;
        var sources = Directory.GetDirectories(modsFolder).Order(StringComparer.OrdinalIgnoreCase)
            .Where(folder => !Path.GetFileName(folder).StartsWith('.') && (ManifestIn(folder) != null || CosmeticPacks.IsPack(folder)))
            .Select(folder => (Folder: folder, Archive: (string?)null)).ToList();
        foreach (var (archive, root, problem) in ModArchives.Sync(modsFolder))
            if (problem != null) problems.Add($"{Path.GetFileName(archive)}: {problem}");
            else sources.Add((root!, Path.GetFileName(archive)));
        var settings = ModSettings.Read(modsFolder);
        foreach (var (folder, archive) in sources)
        {
            try { mods.Add(LoadOne(folder, archive, settings, profile)); }
            catch (Exception error) when (error is InvalidDataException or JsonException or IOException)
            {
                problems.Add($"{archive ?? Path.GetFileName(folder)}: {error.Message}");
            }
        }
        foreach (var duplicate in mods.GroupBy(m => m.Id).Where(g => g.Count() > 1))
            problems.Add($"Two mods use the id {duplicate.Key}; neither is loaded.");
        mods.RemoveAll(m => mods.Count(other => other.Id == m.Id) > 1);
        // Network configuration conflicts retain their existing behavior. Stats follow mod order, last edit wins.
        RemoveNativeConflicts(mods, problems);
        var ordered = Ordered(mods, settings.Order);
        StatsConflicts(ordered, problems);
        problems.AddRange(LuaRuntimes.Problems(ordered.Where(m => m.Enabled)));
        return (ordered, problems);
    }

    // A mod folder (or a cosmetic pack, with its saved choice), from the zip archive when it was unpacked from one, and
    // switched off when the Mods page has it off.
    static ModManifest LoadOne(string folder, string? archive, ModSettings.State settings, EntityProfile profile)
    {
        ModManifest mod;
        if (ManifestIn(folder) == null)
        {
            mod = CosmeticPacks.Parse(folder, CosmeticPacks.Choice.Default);
            if (settings.Choices.TryGetValue(mod.Id, out var choice)) mod = CosmeticPacks.Parse(folder, choice);
        }
        else mod = Parse(folder, profile);
        mod = mod with { Archive = archive };
        // Local Stats (the in-game editor's saved changes) is always on: the Mods page does not list it
        return settings.Disabled.Contains(mod.Id) && mod.Id != LocalStatsId ? mod with { Enabled = false } : mod;
    }

    // Of two enabled mods that edit the same native value, the second by id is not loaded.
    static void RemoveNativeConflicts(List<ModManifest> mods, List<string> problems)
    {
        var claimed = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var mod in mods.Where(m => m.Enabled).OrderBy(m => m.Id, StringComparer.Ordinal).ToList())
        {
            var keys = mod.NetworkFieldTypes.Select(t => "type:" + t.Name).ToList();
            var conflict = keys.FirstOrDefault(claimed.ContainsKey);
            if (conflict != null)
            {
                problems.Add($"{mod.Id} edits the same native value as {claimed[conflict]}; it is not loaded.");
                mods.Remove(mod);
                continue;
            }
            foreach (var key in keys) claimed[key] = mod.Id;
        }
    }

    // Stats two enabled mods both edit: reported, the one that loads later applying.
    static void StatsConflicts(List<ModManifest> ordered, List<string> problems)
    {
        var claimed = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var mod in LoadOrder(ordered).Where(m => m.Enabled))
            foreach (var edit in StatsData.Edits(mod))
            {
                var key = StatsData.ConflictKey(edit);
                if (claimed.TryGetValue(key, out var previous))
                    problems.Add($"{mod.Id}: stats conflict on {edit.Label} with {previous}; {mod.Id} applies later.");
                claimed[key] = mod.Id;
            }
    }

    // The Mods page's order, top first: the gameplay mods, then the cosmetic ones, each in the saved order; mods new to it
    // go to the top of their group (by id). The bottom mod loads first (LoadOrder), as in the community's managers. Local
    // Stats (the in-game editor's saved changes) stays on top: what the player edits applies last.
    public static List<ModManifest> Ordered(IEnumerable<ModManifest> mods, IReadOnlyList<string> topFirst)
    {
        var index = topFirst.Select((id, i) => (id, i)).GroupBy(x => x.id).ToDictionary(g => g.Key, g => g.First().i, StringComparer.Ordinal);
        return mods.OrderBy(m => m.Cosmetic).ThenBy(m => m.Id != LocalStatsId).ThenBy(m => index.ContainsKey(m.Id)).ThenBy(m => index.GetValueOrDefault(m.Id))
            .ThenBy(m => m.Id, StringComparer.Ordinal).ToList();
    }

    // The order a session loads and stages mods in: the Mods page's from the bottom up (the top mod's data wins, and its
    // code loads last).
    public static List<ModManifest> LoadOrder(IEnumerable<ModManifest> topFirst) => topFirst.Reverse().ToList();

    public static List<ModManifest> Active(IEnumerable<ModManifest> mods) => mods.Where(m => m.Enabled).ToList();

    // A manifest of the wrong shape (a value of another JSON kind where an object, list or number belongs) is an
    // InvalidDataException, as any other bad manifest: a downloaded one comes from another machine (mod sync).
    public static ModManifest Parse(string folder, EntityProfile profile)
    {
        try { return ParseManifest(folder, profile); }
        catch (Exception error) when (error is InvalidOperationException or KeyNotFoundException or FormatException or OverflowException)
        {
            throw new InvalidDataException("the manifest is malformed: " + error.Message);
        }
    }

    static ModManifest ParseManifest(string folder, EntityProfile profile)
    {
        using var document = JsonDocument.Parse(File.ReadAllText(ManifestIn(folder) ?? Path.Combine(folder, ManifestName)));
        var root = document.RootElement;
        if (root.ValueKind != JsonValueKind.Object) throw new InvalidDataException("the manifest is not a JSON object.");
        var (id, name, version) = ReadIdentity(root);
        var description = root.TryGetProperty("description", out var text) && text.ValueKind == JsonValueKind.String ? text.GetString()! : "";
        var gameplay = !root.TryGetProperty("gameplay", out var flag) || flag.ValueKind != JsonValueKind.False;

        var (types, writes) = ReadNative(root, profile);
        var (code, source) = ReadCode(root, folder);
        var (data, lua) = ReadDataFiles(folder);
        // A data spec is checked here; what it names in the game data when it is built (ModDataCache).
        var dataSpec = DataSpecs.Load(folder);
        var spec = dataSpec != null;
        var stats = StatsData.Load(folder, profile);
        if (stats.Count > 0 || writes.Count > 0) gameplay = true;
        // A mod of Lua patches alone is the player's own: client-only, outside the modlist code.
        else if (lua && code == null && types.Count == 0 && !spec) gameplay = false;
        if (types.Count == 0 && writes.Count == 0 && data.Count == 0 && code == null && !spec && stats.Count == 0) throw new InvalidDataException("The mod changes nothing.");
        var icon = ReadIcon(root, folder);
        // "url": where players get the mod; one that is not an https:// address (WebAddress) is left out
        var url = root.TryGetProperty("url", out var urlElement) && urlElement.ValueKind == JsonValueKind.String ? WebAddress(urlElement.GetString()) : null;
        return new ModManifest(id, name, version, description, gameplay, Path.GetFullPath(folder), types, writes, data)
            { CodeDll = code, HasSource = source, HasDataSpec = spec, UncheckedSpec = dataSpec == null ? null : DataSpecs.Unchecked(dataSpec), Stats = stats, Icon = icon, Lua = lua, Url = url };
    }

    // The manifest's format, id, name and version.
    static (string Id, string Name, string Version) ReadIdentity(JsonElement root)
    {
        if (Integer(root, "format") != 1) throw new InvalidDataException("format must be 1.");
        var id = Text(root, "id");
        if (!ModId().IsMatch(id)) throw new InvalidDataException("id must be 1-64 lowercase letters, digits, '.', '_' or '-'.");
        var name = Text(root, "name");
        var version = Text(root, "version");
        if (name.Length is 0 or > 80 || version.Length is 0 or > 32) throw new InvalidDataException("name and version are required.");
        return (id, name, version);
    }

    // The manifest's native edits: the network field types it widens and its component writes.
    static (List<NetworkFieldTypeEdit> Types, List<ComponentWrite> Writes) ReadNative(JsonElement root, EntityProfile profile)
    {
        var types = new List<NetworkFieldTypeEdit>();
        var writes = new List<ComponentWrite>();
        if (!root.TryGetProperty("native", out var native)) return (types, writes);
        if (native.ValueKind != JsonValueKind.Object) throw new InvalidDataException("native must be an object.");
        if (native.TryGetProperty("networkFieldTypes", out var list))
            foreach (var item in Items(list, "native.networkFieldTypes")) types.Add(ReadFieldType(item));
        if (native.TryGetProperty("componentWrites", out var components))
            foreach (var item in Items(components, "native.componentWrites")) writes.Add(ReadComponentWrite(item, profile));
        return (types, writes);
    }

    static NetworkFieldTypeEdit ReadFieldType(JsonElement item)
    {
        var typeName = Text(item, "name");
        if (!FieldTypeName().IsMatch(typeName)) throw new InvalidDataException("A network field type name is malformed.");
        var expect = item.TryGetProperty("expect", out var pinned) && pinned.ValueKind == JsonValueKind.Object ? pinned
            : throw new InvalidDataException($"{typeName}: expect is required.");
        var edit = new NetworkFieldTypeEdit(typeName, Integer(expect, "kind"), Integer(expect, "bits"), Integer(expect, "min"), Integer(item, "bits"));
        // Only an integer field may be widened, never past 31 bits (32-bit ints are not range checked).
        if (edit.ExpectKind != 1 || edit.ExpectBits is < 1 or > 31 || edit.Bits <= edit.ExpectBits || edit.Bits > 31)
            throw new InvalidDataException($"{typeName}: only an integer type can be widened, to at most 31 bits.");
        return edit;
    }

    static ComponentWrite ReadComponentWrite(JsonElement item, EntityProfile profile)
    {
        var component = Text(item, "component");
        if (!profile.Components.TryGetValue(component, out var schema))
            throw new InvalidDataException($"{component} is not a component HellForge can edit for this game build.");
        var resourceText = Text(item, "resource");
        if (!resourceText.StartsWith("0x", StringComparison.Ordinal) ||
            !ulong.TryParse(resourceText[2..], NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out var resource) || resource == 0)
            throw new InvalidDataException("A component write needs a resource like 0x056DE1C5E21E723E.");
        var fields = new List<ComponentFieldEdit>();
        foreach (var field in Items(item.TryGetProperty("fields", out var listed) ? listed : default, component + " fields"))
        {
            var edit = new ComponentFieldEdit(Text(field, "name"), Integer(field, "offset"), (uint)Integer(field, "expect"), (uint)Integer(field, "value"));
            if (edit.Offset < 0 || edit.Offset % 4 != 0 || edit.Offset + 4 > schema.Stride)
                throw new InvalidDataException($"{component} field {edit.Name}: offset outside the record.");
            fields.Add(edit);
        }
        if (fields.Count == 0 || fields.Select(f => f.Offset).Distinct().Count() != fields.Count)
            throw new InvalidDataException($"{component}: fields are missing or repeated.");
        return new ComponentWrite(component, resource, fields);
    }

    // The manifest's code DLL, and whether HellForge builds it from the mod's sources; (null, false) without one.
    static (string? Code, bool Source) ReadCode(JsonElement root, string folder)
    {
        if (!root.TryGetProperty("code", out var codeElement)) return (null, false);
        var dll = Text(codeElement, "dll");
        var sdk = Integer(codeElement, "sdk");
        if (sdk < 1 || sdk > SdkVersion) throw new InvalidDataException($"needs HellForge mod SDK {sdk}; this HellForge provides SDK {SdkVersion}.");
        if (!DllName().IsMatch(dll)) throw new InvalidDataException("code.dll must be a DLL file name in the mod folder.");
        // A mod with its sources is built by HellForge (ModBuilds); "prebuilt": true keeps a shipped DLL instead.
        var prebuilt = codeElement.TryGetProperty("prebuilt", out var keep) && keep.ValueKind == JsonValueKind.True;
        var source = !prebuilt && ModKit.Sources(folder).Count > 0;
        if (!source)
        {
            if (!File.Exists(Path.Combine(folder, dll))) throw new InvalidDataException($"{dll} is missing, and the mod has no sources (src/*.c or *.cpp) to build it from.");
            // A prebuilt DLL names the game build it was made for; one made for another build is refused.
            if (codeElement.TryGetProperty("game", out var game) && game.ValueKind == JsonValueKind.String && game.GetString() != SupportedBuild.Current.BuildId)
                throw new InvalidDataException($"{dll} was built for game build {game.GetString()}; HellForge runs build {SupportedBuild.Current.BuildId}. Ask its author for a new build.");
        }
        return (dll, source);
    }

    // The mod's data/ patch files, in order, and whether one of them holds Lua.
    static (List<string> Data, bool Lua) ReadDataFiles(string folder)
    {
        var data = new List<string>();
        var lua = false;
        var dataFolder = Path.Combine(folder, "data");
        if (!Directory.Exists(dataFolder)) return (data, lua);
        foreach (var file in Directory.GetFiles(dataFolder).Order(StringComparer.Ordinal))
        {
            var fileName = Path.GetFileName(file);
            if (!PatchFile().IsMatch(fileName)) throw new InvalidDataException($"data/{fileName} is not a game patch file.");
            if (PatchFile().Match(fileName).Groups[2].Length == 0 && HoldsLua(file)) lua = true;
            data.Add(fileName);
        }
        return (data, lua);
    }

    // "icon": a PNG or JPEG in the mod folder, for the Mods page; one that is missing or outside the folder is left out
    static string? ReadIcon(JsonElement root, string folder)
    {
        if (!(root.TryGetProperty("icon", out var iconElement) && iconElement.ValueKind == JsonValueKind.String && iconElement.GetString() is { Length: > 0 } iconPath))
            return null;
        var full = Path.GetFullPath(Path.Combine(folder, iconPath.Replace('\\', '/')));
        return LocalInstall.Within(full, folder) && File.Exists(full) ? full : null;
    }

    // What friends compare before playing together: every gameplay mod's id, version and exact files. "none" without any.
    // Editor and build leftovers (dot folders and files such as .idea, *.user, a CMake build folder) are not part of it.
    // Nor is data generated from a spec (it is kept outside the mod folder): the spec is, with the data builder's version;
    // with the supported game build, which every player has, they determine the generated bytes.
    // Nor are the files a bundled mod keeps for its player (BundledMods.ShippedFiles: the in-game editor's settings, presets,
    // icons and fonts). Nor are the files a zipped mod writes into its unpacked folder (ModArchives.Shipped: its zip's files
    // are what counts, so a mod saving key bindings or a cache beside itself does not split friends). Nor is the Local Stats mod (the editors' saved changes): in co-op everyone plays with the host's (the
    // backend shares them), so friends with different saved changes still play together.
    public const string LocalStatsId = "zz.local.stats";
    public static string Digest(IEnumerable<ModManifest> mods) => Modlist(mods).Digest;

    // The digest, and its mods one by one so a refused join can say which differ (ModlistDiff): each gameplay mod's id,
    // name, version and a hash of its part of the digest.
    public static (string Digest, List<ModFingerprint> Mods) Modlist(IEnumerable<ModManifest> mods)
    {
        var compared = Compared(mods).ToList();
        var lines = string.Concat(compared.Select(m => m.Lines));
        var digest = lines.Length == 0 ? "none" : Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(lines)))[..32].ToLowerInvariant();
        return (digest, [.. compared.Select(m => new ModFingerprint(m.Mod.Id, m.Mod.Name, m.Mod.Version,
            Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(m.Lines)))[..16].ToLowerInvariant()))]);
    }

    // Each compared mod with its counted files (ModSets.Files) and its lines (ModSets.Block): the digest and the
    // fingerprints come from these lines, and mod sync's content hash is their SHA-256 (ModSets.Hash).
    internal static IEnumerable<(ModManifest Mod, List<ModFile> Files, string Lines)> Compared(IEnumerable<ModManifest> mods)
    {
        // by id, whatever the load order: friends need the same gameplay mods, not the same order. A different order is
        // the players' to sort out (which mod wins a conflict); what mods add is numbered by mod id, the same for everyone.
        foreach (var mod in mods.Where(Counted).OrderBy(m => m.Id, StringComparer.Ordinal))
        {
            var files = ModSets.Files(mod);
            yield return (mod, files, ModSets.Block(mod, files));
        }
    }

    // The mods the digest counts: gameplay mods except Local Stats.
    public static bool Counted(ModManifest mod) => mod.Gameplay && mod.Id != LocalStatsId;

    // The files the digest counts in a mod (relative, '/'), in order, with the SHA-256 it takes from the mod's zip (null:
    // read the file). A zipped mod by what its zip ships, not the files it writes into its unpacked folder as it runs (key
    // bindings, a cache: each player's own); a zip that cannot be read again is compared by its folder. A bundled mod (the
    // in-game editor) by the files HellForge ships for it, not what the player saved beside them. A source mod by its
    // sources: the DLL in its folder (an IDE's build) is not what loads (ModBuilds).
    internal static IEnumerable<(string Path, string? Sha256)> DigestFiles(ModManifest mod)
    {
        var zipped = mod.Archive != null ? ModArchives.Shipped(mod.Archive, mod.Folder) : null;
        var shipped = BundledMods.ShippedFiles(mod.Folder);
        var files = zipped?.Keys ?? Directory.GetFiles(mod.Folder, "*", SearchOption.AllDirectories).Select(f => Path.GetRelativePath(mod.Folder, f).Replace('\\', '/'));
        return files.Where(f => !IsWorkspaceFile(f))
            .Where(f => shipped == null || shipped.Contains(f))
            .Where(f => !mod.HasSource || !SourceBuildOutput(mod, f))
            .Order(StringComparer.Ordinal)
            .Select(f => (f, zipped?[f]));
    }

    internal static bool SourceBuildOutput(ModManifest mod, string relative) =>
        relative == mod.CodeDll || relative.EndsWith(".lib", StringComparison.OrdinalIgnoreCase) || relative.EndsWith(".pdb", StringComparison.OrdinalIgnoreCase);

    // A mod's file, read while another program may have it open (a spreadsheet kept beside the mod, open in Excel:
    // File.ReadAllBytes is refused then, issue 18).
    internal static byte[] ReadShared(string path)
    {
        using var stream = OpenShared(path);
        var bytes = new byte[stream.Length];
        stream.ReadExactly(bytes);
        return bytes;
    }

    internal static FileStream OpenShared(string path) => new(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);

    // An editor's files, not the mod's: dot files and folders, an IDE's build folder and user settings, and Office's lock
    // (~$name) and temporary (.tmp) files, which it holds open with no sharing at all.
    internal static bool IsWorkspaceFile(string relative)
    {
        var parts = relative.Split('/');
        return parts.Any(p => p.StartsWith('.') || p.StartsWith("~$", StringComparison.Ordinal)) || parts[0] == "build" ||
            relative.EndsWith(".user", StringComparison.OrdinalIgnoreCase) || relative.EndsWith(".tmp", StringComparison.OrdinalIgnoreCase);
    }

    // A list's items, each an object.
    static IEnumerable<JsonElement> Items(JsonElement list, string name) =>
        list.ValueKind == JsonValueKind.Array && list.EnumerateArray().All(i => i.ValueKind == JsonValueKind.Object) ? list.EnumerateArray()
            : throw new InvalidDataException($"{name} must be a list of objects.");

    static string Text(JsonElement element, string name) =>
        element.ValueKind == JsonValueKind.Object && element.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String
            ? value.GetString()! : throw new InvalidDataException($"{name} is required.");

    static int Integer(JsonElement element, string name) =>
        element.ValueKind == JsonValueKind.Object && element.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.Number && value.TryGetInt64(out var number) && number is >= int.MinValue and <= uint.MaxValue
            ? unchecked((int)number) : throw new InvalidDataException($"{name} must be an integer.");
}

// The plan the HellForge mod loader (native/mod-loader) reads: the build's pins, the entity profile and the code mods in
// load order, as tab-separated lines (no JSON parser in the loader). Written per session next to its logs.
// Mods kept as zips: each <mods>/<name>.zip is unpacked into <mods>/.unpacked/<name>-<content hash> (once per content)
// and loaded from there; the manifest may be at the zip's root or in its single top-level folder. A zip that is removed
// or replaced takes its unpacked copy with it at the next load (a copy still in use, such as a code mod's DLL in a
// running game, is removed at a later load). No entry may be written outside the unpacked folder (no absolute or parent
// paths); a zip's size is the player's business.
public static class ModArchives
{
    public const string UnpackedFolder = ".unpacked";

    public static List<(string Archive, string? Root, string? Problem)> Sync(string modsFolder)
    {
        var results = new List<(string, string?, string?)>();
        var cache = Path.Combine(modsFolder, UnpackedFolder);
        var keep = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var archives = Directory.GetFiles(modsFolder, "*.zip").Order(StringComparer.OrdinalIgnoreCase).ToList();
        LoadHashes(cache);
        foreach (var archive in archives)
        {
            try
            {
                var hash = ContentHash(archive);
                var target = Path.Combine(cache, Path.GetFileNameWithoutExtension(archive) + "-" + hash);
                keep.Add(Path.GetFileName(target));
                if (!Directory.Exists(target)) Unpack(archive, target);
                results.Add((archive, ManifestRoot(target), null));
            }
            catch (Exception error) when (error is InvalidDataException or IOException or UnauthorizedAccessException)
            {
                results.Add((archive, null, error.Message));
            }
        }
        if (archives.Count > 0) SaveHashes(cache, archives);
        if (Directory.Exists(cache))
            foreach (var stale in Directory.GetDirectories(cache).Where(d => !keep.Contains(Path.GetFileName(d))))
                try { Directory.Delete(stale, recursive: true); }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        return results;
    }

    // A zip's content hash, kept while its size and time are unchanged: the Mods page loads the mods on every change, and
    // hashing a pack of hundreds of MB each time made it lag. Kept across runs too (mods/.unpacked/.zip-hashes.json): the
    // launcher and each session's mod staging are new processes, and read every zip again (678 MB on a tester's PC).
    static readonly System.Collections.Concurrent.ConcurrentDictionary<string, (long Length, DateTime Written, string Hash)> hashes = new(StringComparer.OrdinalIgnoreCase);
    const string HashesName = ".zip-hashes.json";
    static readonly HashSet<string> hashesLoaded = new(StringComparer.OrdinalIgnoreCase);

    static string ContentHash(string archive)
    {
        var info = new FileInfo(archive);
        if (hashes.TryGetValue(info.FullName, out var known) && known.Length == info.Length && known.Written == info.LastWriteTimeUtc) return known.Hash;
        string hash;
        using (var stream = File.OpenRead(archive)) hash = Convert.ToHexStringLower(SHA256.HashData(stream))[..12];
        hashes[info.FullName] = (info.Length, info.LastWriteTimeUtc, hash);
        return hash;
    }

    // The saved hashes of the zips in a mods folder, read once per process (a broken or missing file: none).
    static void LoadHashes(string cache)
    {
        lock (hashesLoaded) if (!hashesLoaded.Add(cache)) return;
        try
        {
            if (System.Text.Json.Nodes.JsonNode.Parse(File.ReadAllText(Path.Combine(cache, HashesName))) is not System.Text.Json.Nodes.JsonObject saved) return;
            foreach (var (path, entry) in saved)
                if (entry is System.Text.Json.Nodes.JsonObject e && (long?)e["length"] is long length && (long?)e["written"] is long written && (string?)e["hash"] is { Length: 12 } hash)
                    hashes.TryAdd(path, (length, new DateTime(written, DateTimeKind.Utc), hash));
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException or FormatException) { }
    }

    // The hashes of the zips there now, written whole (another process writing at the same time: the last one stays).
    static void SaveHashes(string cache, IEnumerable<string> archives)
    {
        var saved = new System.Text.Json.Nodes.JsonObject();
        foreach (var archive in archives)
            if (hashes.TryGetValue(Path.GetFullPath(archive), out var known))
                saved[Path.GetFullPath(archive)] = new System.Text.Json.Nodes.JsonObject { ["length"] = known.Length, ["written"] = known.Written.Ticks, ["hash"] = known.Hash };
        try
        {
            Directory.CreateDirectory(cache);
            var path = Path.Combine(cache, HashesName);
            var partial = path + "." + Guid.NewGuid().ToString("N")[..8] + ".partial";
            File.WriteAllText(partial, saved.ToJsonString());
            File.Move(partial, path, overwrite: true);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }

    // The files a zipped mod ships, by their path in its folder (the one holding its manifest), with their SHA-256: what
    // friends compare (ModCatalog.Digest), whatever the mod writes into its unpacked folder as it runs. Kept while the
    // zip's size and time are unchanged. Null when the zip cannot be read.
    static readonly System.Collections.Concurrent.ConcurrentDictionary<string, (long Length, DateTime Written, IReadOnlyDictionary<string, string> Files)> shipped = new(StringComparer.OrdinalIgnoreCase);

    // archive: the zip's name in the mods folder (ModManifest.Archive); folder: the mod's, in mods/.unpacked.
    internal static IReadOnlyDictionary<string, string>? Shipped(string archive, string folder)
    {
        // the folder's place in the zip: its root (mods/.unpacked/<zip>-<hash>), or its one top-level folder (ManifestRoot)
        var full = Path.GetFullPath(folder);
        static bool InCache(string? path) => path != null && string.Equals(Path.GetFileName(Path.GetDirectoryName(path)), UnpackedFolder, StringComparison.OrdinalIgnoreCase);
        var root = InCache(full) ? full : InCache(Path.GetDirectoryName(full)) ? Path.GetDirectoryName(full)! : null;
        if (root == null) return null;
        var prefix = root == full ? "" : Path.GetFileName(full) + "/";
        var cache = Path.GetDirectoryName(root)!;
        var info = new FileInfo(Path.Combine(Path.GetDirectoryName(cache)!, archive));
        var key = info.FullName + "|" + full;
        LoadShipped(cache);
        if (shipped.TryGetValue(key, out var known) && known.Length == info.Length && known.Written == info.LastWriteTimeUtc) return known.Files;
        try
        {
            var files = new Dictionary<string, string>(StringComparer.Ordinal);
            using (var zip = ZipFile.OpenRead(info.FullName))
                foreach (var entry in zip.Entries)
                {
                    var name = entry.FullName.Replace('\\', '/');
                    if (name.EndsWith('/') || !name.StartsWith(prefix, StringComparison.Ordinal)) continue;
                    using var stream = entry.Open();
                    files[name[prefix.Length..]] = Convert.ToHexString(SHA256.HashData(stream));
                }
            shipped[key] = (info.Length, info.LastWriteTimeUtc, files);
            SaveShipped(cache);
            return files;
        }
        catch (Exception error) when (error is InvalidDataException or IOException or UnauthorizedAccessException) { return null; }
    }

    // The files zipped mods ship, kept across runs (mods/.unpacked/.zip-files.json) as the zips' hashes are: each session
    // start computed the modlist code by decompressing every gameplay zip again (127 MB for one of a tester's mods).
    const string ShippedName = ".zip-files.json";
    static readonly HashSet<string> shippedLoaded = new(StringComparer.OrdinalIgnoreCase);

    static void LoadShipped(string cache)
    {
        lock (shippedLoaded) if (!shippedLoaded.Add(cache)) return;
        try
        {
            if (System.Text.Json.Nodes.JsonNode.Parse(File.ReadAllText(Path.Combine(cache, ShippedName))) is not System.Text.Json.Nodes.JsonObject saved) return;
            foreach (var (key, entry) in saved)
                if (entry is System.Text.Json.Nodes.JsonObject e && (long?)e["length"] is long length && (long?)e["written"] is long written &&
                    e["files"] is System.Text.Json.Nodes.JsonObject list)
                    shipped.TryAdd(key, (length, new DateTime(written, DateTimeKind.Utc),
                        list.Where(f => f.Value is not null).ToDictionary(f => f.Key, f => (string)f.Value!, StringComparer.Ordinal)));
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException or FormatException) { }
    }

    // The zips of this mods folder that still are there, written whole.
    static void SaveShipped(string cache)
    {
        var mods = Path.GetDirectoryName(cache)! + Path.DirectorySeparatorChar;
        var saved = new System.Text.Json.Nodes.JsonObject();
        foreach (var (key, entry) in shipped)
        {
            var zip = key[..key.IndexOf('|')];
            if (!zip.StartsWith(mods, StringComparison.OrdinalIgnoreCase) || !File.Exists(zip)) continue;
            var files = new System.Text.Json.Nodes.JsonObject();
            foreach (var (name, hash) in entry.Files) files[name] = hash;
            saved[key] = new System.Text.Json.Nodes.JsonObject { ["length"] = entry.Length, ["written"] = entry.Written.Ticks, ["files"] = files };
        }
        try
        {
            var path = Path.Combine(cache, ShippedName);
            var partial = path + "." + Guid.NewGuid().ToString("N")[..8] + ".partial";
            File.WriteAllText(partial, saved.ToJsonString());
            File.Move(partial, path, overwrite: true);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }

    internal static void Unpack(string archive, string target)
    {
        using var zip = ZipFile.OpenRead(archive);
        // A staging folder of its own, so the launcher and a starting session can unpack the same zip at once.
        var staging = target + ".partial-" + Guid.NewGuid().ToString("N")[..8];
        var root = Path.GetFullPath(staging) + Path.DirectorySeparatorChar;
        try
        {
            foreach (var entry in zip.Entries)
            {
                var name = entry.FullName.Replace('\\', '/');
                if (name.StartsWith('/') || name.Contains(':') || name.Split('/').Any(part => part == ".."))
                    throw new InvalidDataException($"the zip contains an unsafe path ({entry.FullName}).");
                var destination = Path.GetFullPath(Path.Combine(staging, name));
                if (!destination.StartsWith(root, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException($"the zip contains an unsafe path ({entry.FullName}).");
                if (name.EndsWith('/')) { Directory.CreateDirectory(destination); continue; }
                Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
                entry.ExtractToFile(destination, overwrite: false);
            }
            try { Directory.Move(staging, target); }
            catch (IOException) when (Directory.Exists(target)) { Directory.Delete(staging, recursive: true); }
        }
        catch
        {
            try { Directory.Delete(staging, recursive: true); } catch (IOException) { }
            throw;
        }
    }

    // The folder holding hellforge-mod.json: the zip's root, or its single top-level folder.
    internal static string ManifestRoot(string unpacked)
    {
        static bool Root(string folder) => ModCatalog.ManifestIn(folder) != null || CosmeticPacks.IsPack(folder);
        if (Root(unpacked)) return unpacked;
        var folders = Directory.GetDirectories(unpacked);
        if (folders.Length == 1 && Directory.GetFiles(unpacked).Length == 0 && Root(folders[0])) return folders[0];
        var single = folders.Length == 1 && Directory.GetFiles(unpacked).Length == 0 ? folders[0] : unpacked;
        throw new InvalidDataException(NotAModReason(single) ??
            $"no {ModCatalog.ManifestName} (or a cosmetic pack's {CosmeticPacks.ManifestName}) at the archive's root or in its one top-level folder.");
    }

    // Why an archive that holds no mod was added, when it is a known kind of not-a-mod: HD2Runtime's mod template (a
    // starter for writing HD2Runtime mods, which players took for HD2Runtime itself: 0.1.8 reports), or an HD2Runtime
    // mod's unbuilt sources. Null when it is neither.
    internal static string? NotAModReason(string folder)
    {
        var descriptor = Path.Combine(folder, "hd2runtime.json");
        if (!File.Exists(descriptor) || CosmeticPacks.IsPack(folder)) return null;
        string name = "", guid = "";
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(descriptor));
            if (document.RootElement.TryGetProperty("name", out var n) && n.ValueKind == JsonValueKind.String) name = n.GetString()!;
            if (document.RootElement.TryGetProperty("guid", out var g) && g.ValueKind == JsonValueKind.String) guid = g.GetString()!;
        }
        catch (Exception error) when (error is JsonException or IOException) { }
        if (guid == "auto" || name == "My HD2 Mod")
            return "this is HD2Runtime's mod template, a starter for writing HD2Runtime mods, not a mod to play. To run HD2Runtime, add " +
                "its runtime zip (HD2Runtime-<version>-runtime.zip) and Bingus Shared Loader instead.";
        return $"this is the source of an HD2Runtime mod{(name.Length > 0 ? $" ({name})" : "")}, not a built one: add the mod's released zip (with " +
            $"{CosmeticPacks.ManifestName}), or build it with its build.cmd first.";
    }
}

public static class ModLoaderPlan
{
    // [[helldivers2.exe+0x1A10268]] is the engine's loaded network configuration (HD2Runtime deposit-limit research).
    const string NetworkConfig = "helldivers2.exe\t1a10268";

    public static string Text(IReadOnlyList<ModManifest> codeMods, string logs, EntityProfile profile, string buildId)
    {
        static string Field(string value) => value.Contains('\t') || value.Contains('\n') || value.Contains('\r')
            ? throw new InvalidDataException("A path in the mod loader plan contains a tab or line break.") : value;
        var lines = new List<string> { "build\t" + buildId, "logs\t" + Field(logs), "network_config\t" + NetworkConfig,
            $"entity_region\t{profile.EntityRegionSize}\t{profile.MapHeader.ToLowerInvariant()}" };
        foreach (var (name, c) in profile.Components.OrderBy(c => c.Key, StringComparer.Ordinal))
            lines.Add($"component\t{name}\t{c.Offset}\t{c.Header.ToLowerInvariant()}\t{c.Index}\t{c.Indices}\t{c.Records}\t{c.RecordOffset}\t{c.Stride}");
        lines.Add("stats_schema\t" + StatsSchema.Fingerprint);
        foreach (var key in StatsSchema.Observed) lines.Add("observe\t" + key);
        // in the order given, a session's load order (ModCatalog.LoadOrder): code loads and stats apply in it, the last winning
        var mods = codeMods.Where(m => m.Enabled && m.NeedsLoader).ToList();
        if (mods.Count > 64 || mods.Sum(m => StatsData.Edits(m).Count()) > StatsData.MaxEdits)
            throw new InvalidDataException("Mod loader plan exceeds its mod or stats edit capacity.");
        foreach (var mod in mods)
        {
            if (mod.HasCode && mod.DllPath == null) throw new InvalidDataException($"{mod.Id} is a source mod that was not built for this session.");
            lines.Add($"mod\t{mod.Id}\t{Field(mod.Folder)}\t{(mod.HasCode ? Field(mod.DllPath!) : "-")}");
            foreach (var e in StatsData.Edits(mod))
                lines.Add(FormattableString.Invariant($"stat\t{mod.Id}\t{e.Kind}\t{(e.Component.Length == 0 ? "-" : e.Component)}\t{e.Resource:X16}\t{e.Record}\t{e.Offset}\t{e.Storage}\t{e.Value:X8}\t{(e.Expect.HasValue ? e.Expect.Value.ToString("X8", CultureInfo.InvariantCulture) : "-")}\t{(e.Own ? 1 : 0)}\t{(e.Shared ? 1 : 0)}\t{(e.Unverified ? 1 : 0)}\t{Field(e.Label)}"));
        }
        if (Encoding.UTF8.GetByteCount(string.Join("\n", lines)) >= (4 << 20) - 1) throw new InvalidDataException("Mod loader plan exceeds 4 MiB.");
        return string.Join("\n", lines) + "\n";
    }
}

// The Mods page's settings: <installation>/mods/hellforge-mods.json {"format":1,"disabled":[ids],"order":[ids],"packs":{...},
// "blocked":[ids],"trusted":[TrustedMod],"share":true,"showEditedStats":false,"listPublic":true,"hostStats":false}. Mods are on by default, so a mod
// dropped into the folder loads without any setup.
public static class ModSettings
{
    public const string FileName = "hellforge-mods.json";
    // The switches of installations from before the rename, read until the next change writes FileName.
    public const string LegacyFileName = "bts-mods.json";

    // What this process wrote last: the Mods page tells its own saves from another program's (HellForge.Cli, a mod manager) by it.
    public static string? LastWritten { get; private set; }

    // What the Mods page keeps: mods switched off, the order (top first; the bottom mod loads first, as in the
    // community's managers), and each cosmetic pack's choice. Mod sync (ModSyncPlan): mods whose lobbies the player never
    // wants to see or join (Blocked, by id), and whether joiners may download this player's mods when hosting (Share).
    // The Galactic Map's public lobbies: whether lobbies whose host plays with edited stats (which joiners take) are shown,
    // and whether this player's own lobby is listed for other HellForge players while it is Public (ListPublic, on by
    // default; off, a Public lobby still admits whoever reaches it, but no stranger can find it).
    // HostStats: hosting, whether friends play with this player's editor changes (the in-game editor's Local Stats, mod
    // content values and arm swaps) instead of their own (off by default: everyone keeps their own; StatsShare).
    // Trusted: mods the player trusted on mod sync's prompt (TRUST), each by its exact content (id and content hash), with
    // its name and version, the Steam friend who hosted it and when: they download as the verified list's do (ModSyncPlan).
    public sealed record State(HashSet<string> Disabled, List<string> Order, Dictionary<string, CosmeticPacks.Choice> Choices)
    {
        public HashSet<string> Blocked { get; } = new(StringComparer.Ordinal);
        public List<TrustedMod> Trusted { get; } = [];
        public bool Trusts(string id, string hash) => Trusted.Any(t => t.Id == id && t.Hash == hash);
        public bool Share { get; set; } = true;
        public bool ShowEditedStats { get; set; }
        public bool ListPublic { get; set; } = true;
        public bool HostStats { get; set; }
        // Off by default: a stranger's traffic only reaches this machine when the player turns this on. Friends are
        // always accepted. Off, this machine's lobby is not listed in the lobby directory, a search shows friends'
        // listings only, and a join to a stranger's lobby is refused with the reason (docs/multiplayer.md).
        public bool PlayWithStrangers { get; set; }
    }

    public static State Read(string modsFolder)
    {
        var path = Path.Combine(modsFolder, FileName);
        if (!File.Exists(path)) path = Path.Combine(modsFolder, LegacyFileName);
        var state = new State(new HashSet<string>(StringComparer.Ordinal), [], new Dictionary<string, CosmeticPacks.Choice>(StringComparer.Ordinal));
        try
        {
            if (!File.Exists(path)) return state;
            using var document = JsonDocument.Parse(File.ReadAllText(path));
            var root = document.RootElement;
            AddTexts(root, "disabled", state.Disabled);
            AddTexts(root, "blocked", state.Blocked);
            ReadTrusted(root, state);
            ReadSwitches(root, state);
            ReadOrder(root, state);
            ReadPacks(root, state);
        }
        catch (Exception error) when (error is JsonException or IOException) { }
        return state;
    }

    // The texts of the list root[name], if it is one, into a set.
    static void AddTexts(JsonElement root, string name, HashSet<string> into)
    {
        if (root.TryGetProperty(name, out var list) && list.ValueKind == JsonValueKind.Array)
            foreach (var e in list.EnumerateArray()) if (e.ValueKind == JsonValueKind.String) into.Add(e.GetString()!);
    }

    static void ReadTrusted(JsonElement root, State state)
    {
        if (root.TryGetProperty("trusted", out var trusted) && trusted.ValueKind == JsonValueKind.Array)
            foreach (var e in trusted.EnumerateArray())
                if (TrustedMod.Read(e) is { } mod && !state.Trusts(mod.Id, mod.Hash)) state.Trusted.Add(mod);
    }

    // The switches: each changes from its default only when the file says the other value.
    static void ReadSwitches(JsonElement root, State state)
    {
        if (root.TryGetProperty("share", out var share) && share.ValueKind == JsonValueKind.False) state.Share = false;
        if (root.TryGetProperty("showEditedStats", out var edited) && edited.ValueKind == JsonValueKind.True) state.ShowEditedStats = true;
        if (root.TryGetProperty("listPublic", out var listed) && listed.ValueKind == JsonValueKind.False) state.ListPublic = false;
        if (root.TryGetProperty("hostStats", out var hostStats) && hostStats.ValueKind == JsonValueKind.True) state.HostStats = true;
        if (root.TryGetProperty("playWithStrangers", out var strangers) && strangers.ValueKind == JsonValueKind.True) state.PlayWithStrangers = true;
    }

    static void ReadOrder(JsonElement root, State state)
    {
        if (root.TryGetProperty("order", out var order) && order.ValueKind == JsonValueKind.Array)
            foreach (var e in order.EnumerateArray()) if (e.ValueKind == JsonValueKind.String && !state.Order.Contains(e.GetString()!)) state.Order.Add(e.GetString()!);
    }

    static void ReadPacks(JsonElement root, State state)
    {
        if (root.TryGetProperty("packs", out var packs) && packs.ValueKind == JsonValueKind.Object)
            foreach (var pack in packs.EnumerateObject()) state.Choices[pack.Name] = ReadChoice(pack.Value);
    }

    // A cosmetic pack's choice: its options switched off, and each option's sub-option.
    static CosmeticPacks.Choice ReadChoice(JsonElement pack)
    {
        var off = pack.TryGetProperty("off", out var o) && o.ValueKind == JsonValueKind.Array
            ? o.EnumerateArray().Where(x => x.ValueKind == JsonValueKind.String).Select(x => x.GetString()!).ToHashSet(StringComparer.Ordinal) : [];
        var sub = new Dictionary<string, string>(StringComparer.Ordinal);
        if (pack.TryGetProperty("sub", out var s) && s.ValueKind == JsonValueKind.Object)
            foreach (var item in s.EnumerateObject()) if (item.Value.ValueKind == JsonValueKind.String) sub[item.Name] = item.Value.GetString()!;
        return new(off, sub);
    }

    public static HashSet<string> Disabled(string modsFolder) => Read(modsFolder).Disabled;

    public static void SetEnabled(string modsFolder, string id, bool enabled) => Change(modsFolder, state =>
    {
        if (enabled) state.Disabled.Remove(id);
        else state.Disabled.Add(id);
    });

    // The Mods page's order, top first.
    public static void SetOrder(string modsFolder, IEnumerable<string> topFirst) => Change(modsFolder, state =>
    {
        state.Order.Clear();
        state.Order.AddRange(topFirst.Distinct(StringComparer.Ordinal));
    });

    public static void SetChoice(string modsFolder, string id, CosmeticPacks.Choice choice) => Change(modsFolder, state => state.Choices[id] = choice);

    // Mod sync's blocklist: a lobby running a blocked mod is hidden and never joined (ModSyncPlan).
    public static void SetBlocked(string modsFolder, string id, bool blocked) => Change(modsFolder, state =>
    {
        if (blocked) state.Blocked.Add(id);
        else state.Blocked.Remove(id);
    });

    // Mod sync's trusted mods: one the player trusted (that exact content) downloads from a Steam friend as a verified one
    // does; untrusted (the Mods page), it is the player's to install again.
    public static void SetTrusted(string modsFolder, TrustedMod mod) => Change(modsFolder, state =>
    {
        state.Trusted.RemoveAll(t => t.Id == mod.Id && t.Hash == mod.Hash);
        state.Trusted.Add(mod);
    });

    public static void Untrust(string modsFolder, string id, string hash) => Change(modsFolder, state => state.Trusted.RemoveAll(t => t.Id == id && t.Hash == hash));

    // Hosting: whether joiners who lack this player's mods may download them from this machine.
    public static void SetShare(string modsFolder, bool share) => Change(modsFolder, state => state.Share = share);

    // The Galactic Map: whether public lobbies whose host plays with edited stats are shown.
    public static void SetShowEditedStats(string modsFolder, bool show) => Change(modsFolder, state => state.ShowEditedStats = show);

    // Hosting: whether this player's Public lobbies are listed on other HellForge players' Galactic Map.
    public static void SetListPublic(string modsFolder, bool list) => Change(modsFolder, state => state.ListPublic = list);
    // Multiplayer: whether people who are not Steam friends can reach this machine through the lobby directory.
    public static void SetPlayWithStrangers(string modsFolder, bool allow) => Change(modsFolder, state => state.PlayWithStrangers = allow);
    public static bool PlayWithStrangers(string modsFolder) => Read(modsFolder).PlayWithStrangers;

    // Hosting: whether friends play with this player's editor changes instead of their own.
    public static void SetHostStats(string modsFolder, bool host) => Change(modsFolder, state => state.HostStats = host);

    static void Change(string modsFolder, Action<State> change)
    {
        var state = Read(modsFolder);
        change(state);
        Directory.CreateDirectory(modsFolder);
        var path = Path.Combine(modsFolder, FileName);
        var root = new JsonObject
        {
            ["format"] = 1,
            ["disabled"] = new JsonArray([.. state.Disabled.Order(StringComparer.Ordinal).Select(id => (JsonNode?)id)]),
            ["order"] = new JsonArray([.. state.Order.Select(id => (JsonNode?)id)]),
            ["packs"] = new JsonObject(state.Choices.OrderBy(c => c.Key, StringComparer.Ordinal).Select(c => KeyValuePair.Create(c.Key, (JsonNode?)new JsonObject
            {
                ["off"] = new JsonArray([.. c.Value.Off.Order(StringComparer.Ordinal).Select(o => (JsonNode?)o)]),
                ["sub"] = new JsonObject(c.Value.Sub.OrderBy(s => s.Key, StringComparer.Ordinal).Select(s => KeyValuePair.Create(s.Key, (JsonNode?)s.Value)))
            }))),
            ["blocked"] = new JsonArray([.. state.Blocked.Order(StringComparer.Ordinal).Select(id => (JsonNode?)id)]),
            ["trusted"] = new JsonArray([.. state.Trusted.Select(t => (JsonNode?)t.ToJson())]),
            ["share"] = state.Share,
            ["showEditedStats"] = state.ShowEditedStats,
            ["listPublic"] = state.ListPublic,
            ["hostStats"] = state.HostStats,
            ["playWithStrangers"] = state.PlayWithStrangers
        };
        var text = root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n";
        File.WriteAllText(path + ".tmp", text);
        LastWritten = text;
        File.Move(path + ".tmp", path, overwrite: true);
        File.Delete(Path.Combine(modsFolder, LegacyFileName));
    }
}

// A mod the player trusted on mod sync's prompt: its id and content hash (ModSets.ContentHash: that exact content, no
// other version or bytes), its name and version as the host listed them, the Steam friend who hosted it (their name, then
// their SteamID64) and when (UTC, ISO 8601). In hellforge-mods.json's "trusted"; an entry that is malformed is left out.
public sealed record TrustedMod(string Id, string Hash, string Name, string Version, string From, string Since)
{
    public JsonObject ToJson() => new()
    {
        ["id"] = Id, ["hash"] = Hash, ["name"] = Name, ["version"] = Version, ["from"] = From, ["since"] = Since
    };

    public static TrustedMod? Read(JsonElement e)
    {
        if (e.ValueKind != JsonValueKind.Object) return null;
        string Text(string name, int max) => e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String && v.GetString()!.Length <= max &&
            !v.GetString()!.Any(char.IsControl) ? v.GetString()! : "";
        var mod = new TrustedMod(Text("id", 64), Text("hash", 64), Text("name", 80), Text("version", 32), Text("from", 120), Text("since", 40));
        return ModCatalog.IsModId(mod.Id) && ModSets.Hex(mod.Hash, 64) ? mod : null;
    }
}

// Data mods for one session: their patch files are copied into game/data, renumbered so each bundle's patches stay
// contiguous after any the player installed there by hand, and listed in temp/staged-mods.json so the next session
// removes exactly them first. Game files from Steam are never touched. A mod's generated data (ModDataCache.Prepare) is
// staged after its data/ folder, the same way.
public static class ModStaging
{
    const string RecordName = "staged-mods.json";

    // What the last Stage did: files copied, and files kept from the session before (the same source, unchanged).
    public static (int Copied, int Kept) LastCounts { get; private set; }

    // The previous session's staged files stay where they are what this one stages (same name, and the same size and
    // time as the file they come from: a copy keeps its source's time): a session start copied and deleted every patch
    // again, ~3 GB for a tester's packs, and kept the drive busy for minutes. Only new or changed patches are copied;
    // those no longer staged are removed.
    public static IReadOnlyList<string> Stage(InstallLayout install, IReadOnlyList<ModManifest> mods)
    {
        var data = Path.Combine(install.Game, "data");
        var previous = Previous(install);
        var plan = Plan(mods, data, previous);
        var wanted = plan.Select(p => p.Target).ToHashSet(StringComparer.Ordinal);
        var left = new HashSet<string>(previous, StringComparer.Ordinal); /* staged here before and still in game/data */
        var staged = new List<string>();
        int copied = 0, kept = 0;
        try
        {
            foreach (var name in previous.Where(n => !wanted.Contains(n)))
            {
                var path = Path.Combine(data, name);
                if (File.Exists(path)) File.Delete(path);
                left.Remove(name);
            }
            foreach (var (source, target) in plan)
            {
                var path = Path.Combine(data, target);
                if (left.Contains(target) && Same(source, path)) { staged.Add(target); left.Remove(target); kept++; continue; }
                if (left.Remove(target) && File.Exists(path)) File.Delete(path);
                File.Copy(source, path, overwrite: false);
                staged.Add(target);
                copied++;
            }
        }
        finally
        {
            Directory.CreateDirectory(install.Temp);
            /* a stage cut short: what it placed, and the last session's files it did not get to, for the next one to remove */
            staged.AddRange(left.Where(n => File.Exists(Path.Combine(data, n))));
            File.WriteAllText(Path.Combine(install.Temp, RecordName), JsonSerializer.Serialize(staged));
            LastCounts = (copied, kept);
        }
        return staged;
    }

    // Each mod's patch files (its data/, its pack's chosen folders, its generated data) with the names they are staged
    // under: each bundle's patches renumbered after the ones already in game/data, in load order.
    static List<(string Source, string Target)> Plan(IReadOnlyList<ModManifest> mods, string data, HashSet<string> previous)
    {
        var next = new Dictionary<string, int>(StringComparer.Ordinal);
        int NextIndex(string bundle)
        {
            if (!next.TryGetValue(bundle, out var index))
            {
                index = 0;
                /* after the bundle's own patches (the game's or the player's); the last session's staged ones do not count */
                while (!previous.Contains($"{bundle}.patch_{index}") && File.Exists(Path.Combine(data, $"{bundle}.patch_{index}"))) index++;
            }
            next[bundle] = index + 1;
            return index;
        }
        var plan = new List<(string Source, string Target)>();
        foreach (var mod in mods)
        {
            if (mod.HasDataSpec && mod.GeneratedData == null) throw new InvalidDataException($"{mod.Id}: its data has not been generated.");
            foreach (var (folder, files) in Sources(mod))
                foreach (var group in files.GroupBy(f => (Bundle: f[..16], Index: int.Parse(ModCatalog.PatchFile().Match(f).Groups[1].Value, CultureInfo.InvariantCulture)))
                    .OrderBy(g => g.Key.Bundle, StringComparer.Ordinal).ThenBy(g => g.Key.Index))
                {
                    var index = NextIndex(group.Key.Bundle);
                    foreach (var file in group.Order(StringComparer.Ordinal))
                        plan.Add((Path.Combine(folder, file), $"{group.Key.Bundle}.patch_{index}{ModCatalog.PatchFile().Match(file).Groups[2].Value}"));
                }
        }
        return plan;
    }

    // A mod's folders of patches and their files: its data/, its pack's chosen folders, its generated data.
    static List<(string Folder, IEnumerable<string> Files)> Sources(ModManifest mod)
    {
        var sources = new List<(string Folder, IEnumerable<string> Files)> { (Path.Combine(mod.Folder, "data"), mod.DataFiles) };
        foreach (var folder in mod.PackFolders) sources.Add((folder, CosmeticPacks.PatchFiles(folder)));
        if (mod.GeneratedData != null)
            sources.Add((mod.GeneratedData, Directory.GetFiles(mod.GeneratedData).Select(f => Path.GetFileName(f)).Where(f => ModCatalog.PatchFile().IsMatch(f))));
        return sources;
    }

    // The last session's staged files (patch file names only).
    static HashSet<string> Previous(InstallLayout install)
    {
        var record = Path.Combine(install.Temp, RecordName);
        try
        {
            return File.Exists(record)
                ? (JsonSerializer.Deserialize<List<string>>(File.ReadAllText(record)) ?? []).Where(n => ModCatalog.PatchFile().IsMatch(n)).ToHashSet(StringComparer.Ordinal)
                : new HashSet<string>(StringComparer.Ordinal);
        }
        catch (JsonException) { return new HashSet<string>(StringComparer.Ordinal); }
    }

    // A staged file still is its source: the same size and last write time (a copy keeps its source's time).
    static bool Same(string source, string staged)
    {
        var from = new FileInfo(source);
        var to = new FileInfo(staged);
        return to.Exists && from.Exists && to.Length == from.Length && to.LastWriteTimeUtc == from.LastWriteTimeUtc;
    }

    public static void Unstage(InstallLayout install)
    {
        var record = Path.Combine(install.Temp, RecordName);
        if (!File.Exists(record)) return;
        var data = Path.Combine(install.Game, "data");
        foreach (var name in JsonSerializer.Deserialize<List<string>>(File.ReadAllText(record)) ?? [])
            if (ModCatalog.PatchFile().IsMatch(name) && File.Exists(Path.Combine(data, name))) File.Delete(Path.Combine(data, name));
        File.Delete(record);
    }
}

// Where the game keeps entity component records, for the supported build (entity-profile.json, exported from
// HD2Runtime's reviewed schema by scripts/ExportEntityProfile.py).
public sealed record ComponentSchema(long Offset, string Header, int Index, int Indices, int Records, int RecordOffset, int Stride, uint Type);
public sealed record EntityProfile(string ExeSha256, string GameDllSha256, long EntityRegionSize, string MapHeader, int MapRows,
    IReadOnlyDictionary<string, ComponentSchema> Components)
{
    static readonly Lazy<EntityProfile> current = new(() =>
    {
        using var stream = typeof(EntityProfile).Assembly.GetManifestResourceStream("HellForge.Core.entity-profile.json")
            ?? throw new InvalidDataException("The entity profile is not embedded.");
        return JsonSerializer.Deserialize(stream, CoreJson.Default.EntityProfile)
            ?? throw new InvalidDataException("The entity profile is empty.");
    });
    public static EntityProfile Current => current.Value;
}

// The engine's IdString32 for names in its network configuration: the upper half of MurmurHash64A (seed 0).
public static class EngineHash
{
    public static uint Id32(string name) => (uint)(Murmur64A(Encoding.UTF8.GetBytes(name)) >> 32);

    public static ulong Murmur64A(ReadOnlySpan<byte> data)
    {
        const ulong m = 0xC6A4A7935BD1E995;
        var h = (ulong)data.Length * m;
        var whole = data.Length / 8 * 8;
        for (var i = 0; i < whole; i += 8)
        {
            var k = BitConverter.ToUInt64(data.Slice(i, 8)) * m;
            k ^= k >> 47;
            h = (h ^ (k * m)) * m;
        }
        if (whole != data.Length)
        {
            ulong tail = 0;
            for (var i = data.Length - 1; i >= whole; i--) tail = (tail << 8) | data[i];
            h = (h ^ tail) * m;
        }
        h ^= h >> 47;
        h *= m;
        return h ^ (h >> 47);
    }
}
