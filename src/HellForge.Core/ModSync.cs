using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using HellForge.Core.GameData;

namespace HellForge.Core;

// Mod sync (docs/mods.md "Playing with other mods"): a join refused for different gameplay mods carries the host's mod
// set (ModSet); the joiner's backend plans the difference (ModSyncPlan), downloads what it may from the host's backend
// into the synced store (SyncedStore) and restarts the session with the host's set (ModSyncSession). What a joiner takes
// from a host depends on the mod's class, which the joiner reads from the bytes it received, never from the host:
//   content  what HellForge checks itself: data specs it checks all of (built from the joiner's own game files: icons, and
//            copies and includes of the reviewed types with only HellForge's own edits; DataSpecs.Unchecked), schema
//            stats (named fields within their reviewed ranges) and network field types (integer widenings): from any host;
//   code     a DLL, sources HellForge compiles, or a patch holding Lua: only when VerifiedMods lists its exact content, or
//            the player trusted that exact content (ModSettings' Trusted: TRUST on the prompt, for a Steam friend's mod);
//   raw      bytes HellForge cannot check, which reach the game's own parsers or any field: binary data patches (data/),
//            data specs with "replace" or other resource types, raw stats lines and componentWrites: as code;
//   bundled  a mod HellForge ships (the in-game editor): never downloaded; a different one is another HellForge version.
// Every host gets the same rule, a Steam friend too; and code and raw mods come only from Steam friends (a host serves them
// only to its friends: a stranger could not take them unverified anyway, and they may be a modder's unreleased sources).
// Trust changes only which list lets a hash through: every byte is checked against it as before.
public enum ModClass { Content, Code, Bundled, Raw }

// A file of a mod as the digest counts it: its path in the mod folder ('/'), size and SHA-256 (upper-case hex).
public sealed record ModFile(string Path, long Size, string Sha256);

// One gameplay mod of a mod set. Hash is its content hash: the SHA-256 (lower-case hex) of the lines the modlist digest
// takes from it (ModSets.Block). Order is its place in the session's load order (0 loads first). Folder and Files are
// this machine's own and never leave it (ModSet.ToJson(local: false)). Url is where players get the mod (its manifest's
// "url", ModCatalog.WebAddress; never a bundled mod's), or null: the prompt's GET IT.
public sealed record ModSetEntry(string Id, string Name, string Version, string Hash, long Bytes, ModClass Class, int Order)
{
    public string? Folder { get; init; }
    public IReadOnlyList<ModFile> Files { get; init; } = [];
    public string? Url { get; init; }
}

// The gameplay mods a session runs, with the modlist digest they make. Written per session (mods-stage --modset) for the
// backend, which shares it with joiners whose mods differ.
public sealed record ModSet(string Digest, IReadOnlyList<ModSetEntry> Mods)
{
    public const int MaxMods = 256;

    // The set of mods given in load order (bottom of the Mods page first): the mods the digest counts, from the digest's
    // own lines (ModCatalog.Compared), so the set's digest is the modlist digest.
    public static ModSet Of(IReadOnlyList<ModManifest> loadOrder)
    {
        var entries = new List<ModSetEntry>();
        var lines = new StringBuilder();
        var counted = loadOrder.Where(ModCatalog.Counted).ToList();
        foreach (var (mod, files, block) in ModCatalog.Compared(counted))
        {
            lines.Append(block);
            var kind = ModSets.ClassOf(mod);
            entries.Add(new ModSetEntry(mod.Id, mod.Name, mod.Version, ModSets.Hash(block), files.Sum(f => f.Size), kind, counted.IndexOf(mod))
                { Folder = mod.Folder, Files = files, Url = kind == ModClass.Bundled ? null : mod.Url });
        }
        var digest = lines.Length == 0 ? "none" : Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(lines.ToString())))[..32].ToLowerInvariant();
        return new ModSet(digest, entries);
    }

    public ModSetEntry? Find(string id, string hash) => Mods.FirstOrDefault(m => m.Id == id && m.Hash == hash);

    // The mods as a refused join names them (ModCatalog.Modlist's fingerprints: the content hash's first 16 characters).
    public List<ModFingerprint> Fingerprints() => [.. Mods.Select(m => new ModFingerprint(m.Id, m.Name, m.Version, m.Hash[..16]))];

    // local: with each mod's folder and files (the session's own file); otherwise what a joiner is sent.
    public JsonObject ToJson(bool local)
    {
        var mods = new JsonArray();
        foreach (var mod in Mods)
        {
            var item = new JsonObject
            {
                ["id"] = mod.Id, ["name"] = mod.Name, ["version"] = mod.Version, ["hash"] = mod.Hash, ["bytes"] = mod.Bytes,
                ["class"] = ModSets.ClassName(mod.Class), ["order"] = mod.Order
            };
            if (mod.Url != null) item["url"] = mod.Url;
            if (local)
            {
                item["folder"] = mod.Folder;
                item["files"] = new JsonArray([.. mod.Files.Select(f => (JsonNode?)new JsonObject { ["path"] = f.Path, ["size"] = f.Size, ["sha256"] = f.Sha256 })]);
            }
            mods.Add((JsonNode?)item);
        }
        return new JsonObject { ["format"] = 1, ["digest"] = Digest, ["mods"] = mods };
    }

    public void Write(string path)
    {
        File.WriteAllText(path + ".partial", ToJson(local: true).ToJsonString());
        File.Move(path + ".partial", path, overwrite: true);
    }

    // A set from JSON: the session's own file (local: folders and files kept) or a host's (local: false), which comes
    // from another machine and is checked field by field.
    public static ModSet Parse(string json, bool local)
    {
        JsonNode? root;
        try { root = JsonNode.Parse(json); }
        catch (JsonException error) { throw new InvalidDataException("The mod set is not JSON: " + error.Message); }
        if (root is not JsonObject set || (int?)set["format"] != 1) throw new InvalidDataException("The mod set has no format 1.");
        var digest = (string?)set["digest"] ?? "";
        if (digest != "none" && !ModSets.Hex(digest, 32)) throw new InvalidDataException("The mod set's digest is malformed.");
        if (set["mods"] is not JsonArray list || list.Count > MaxMods) throw new InvalidDataException("The mod set's mods are missing or too many.");
        var mods = new List<ModSetEntry>();
        foreach (var node in list) mods.Add(ParseEntry(node, local));
        if (mods.Select(m => m.Id).Distinct(StringComparer.Ordinal).Count() != mods.Count) throw new InvalidDataException("The mod set names a mod twice.");
        return new ModSet(digest, mods);
    }

    static ModSetEntry ParseEntry(JsonNode? node, bool local)
    {
        if (node is not JsonObject item) throw new InvalidDataException("A mod set entry is not an object.");
        string Text(string name, int max) => item[name] is JsonValue v && v.TryGetValue<string>(out var s) && s.Length > 0 && s.Length <= max && !s.Any(char.IsControl)
            ? s : throw new InvalidDataException($"A mod set entry's {name} is missing or malformed.");
        long Number(string name, long max) => item[name] is JsonValue v && v.TryGetValue<long>(out var n) && n >= 0 && n <= max
            ? n : throw new InvalidDataException($"A mod set entry's {name} is missing or malformed.");
        var id = Text("id", 64);
        if (!ModCatalog.IsModId(id)) throw new InvalidDataException("A mod set entry's id is malformed.");
        var hash = Text("hash", 64);
        if (!ModSets.Hex(hash, 64)) throw new InvalidDataException($"{id}: its content hash is malformed.");
        var kind = ModSets.ClassNamed(Text("class", 16));
        // a url that is not a web address HellForge would open (or a bundled mod's) is left out, not the set
        var url = kind == ModClass.Bundled || item["url"] is not JsonValue address || !address.TryGetValue<string>(out var given) ? null : ModCatalog.WebAddress(given);
        var entry = new ModSetEntry(id, Text("name", 80), Text("version", 32), hash, Number("bytes", 1L << 40), kind, (int)Number("order", MaxMods)) { Url = url };
        if (!local) return entry;
        var files = new List<ModFile>();
        foreach (var file in item["files"] as JsonArray ?? [])
            files.Add(new ModFile((string?)file?["path"] ?? "", (long?)file?["size"] ?? -1, (string?)file?["sha256"] ?? ""));
        return entry with { Folder = (string?)item["folder"], Files = files };
    }
}

public static class ModSets
{
    // The files of a mod as the digest counts them, hashed. A zipped mod's hashes are its zip's (ModArchives.Shipped), its
    // sizes those of its unpacked folder: a file the mod changed there as it ran is sent as it is now and fails the
    // joiner's check, rather than counting differently.
    public static List<ModFile> Files(ModManifest mod) =>
        [.. ModCatalog.DigestFiles(mod).Select(file =>
        {
            var full = Path.Combine(mod.Folder, file.Path);
            if (file.Sha256 != null) return new ModFile(file.Path, File.Exists(full) ? new FileInfo(full).Length : 0, file.Sha256);
            using var stream = ModCatalog.OpenShared(full); // a file another program has open (issue 18)
            return new ModFile(file.Path, stream.Length, Convert.ToHexString(SHA256.HashData(stream)));
        })];

    // The lines the modlist digest takes from one mod: id and version, the data builder's version for a data spec (with
    // the supported game build, which every player has, it determines the generated bytes), then each file and its hash.
    public static string Block(ModManifest mod, IEnumerable<ModFile> files)
    {
        var lines = new StringBuilder();
        lines.Append(mod.Id).Append('|').Append(mod.Version).Append('\n');
        if (mod.HasDataSpec) lines.Append("data-builder|").Append(ModDataBuilder.Version).Append('\n');
        foreach (var file in files) lines.Append(file.Path).Append('|').Append(file.Sha256).Append('\n');
        return lines.ToString();
    }

    public static string Hash(string block) => Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(block)));

    // A mod's content hash, read from its folder now.
    public static string ContentHash(ModManifest mod) => Hash(Block(mod, Files(mod)));

    // Code runs in the game: a DLL (or sources HellForge builds into one), or a patch with Lua scripts. Raw is what reaches
    // the game unchecked: binary data patches, componentWrites, raw stats lines (StatsData's "raw" section, the only
    // stats edits that carry an expected vanilla value rather than a reviewed range), and a data spec HellForge does not
    // check all of (DataSpecs.Unchecked: a "replace" writes any 8 bytes into a copied resource; a type outside the
    // reviewed ones, lua among them, is not HellForge's to judge).
    public static ModClass ClassOf(ModManifest mod) =>
        BundledMods.ShippedFiles(mod.Folder) != null ? ModClass.Bundled
        : mod.HasCode || mod.Lua ? ModClass.Code
        : mod.DataFiles.Count > 0 || mod.ComponentWrites.Count > 0 || mod.UncheckedSpec != null || mod.Stats.Any(e => e.Expect != null) ? ModClass.Raw
        : ModClass.Content;

    // Downloads only when the verified list names its exact content, or the player trusted it.
    public static bool NeedsVerified(ModClass kind) => kind is ModClass.Code or ModClass.Raw;

    // What a download already shows before the rest of its bytes: the host's file list, its manifest, its stats file and
    // its data spec (null when it lists none). Code or Raw when they show it, null otherwise: the whole mod decides then
    // (ClassOf).
    public static ModClass? Declared(IEnumerable<string> paths, string manifestJson, string? statsJson, string? specJson = null)
    {
        using var manifest = JsonDocument.Parse(manifestJson);
        var root = manifest.RootElement;
        if (root.ValueKind != JsonValueKind.Object) return null;
        if (root.TryGetProperty("code", out _)) return ModClass.Code;
        if (paths.Any(p => p.StartsWith("data/", StringComparison.OrdinalIgnoreCase))) return ModClass.Raw;
        if (root.TryGetProperty("native", out var native) && native.ValueKind == JsonValueKind.Object && native.TryGetProperty("componentWrites", out _))
            return ModClass.Raw;
        if (specJson != null)
        {
            // a spec that does not parse is left to the whole mod's check, which refuses it
            try { if (DataSpecs.Unchecked(DataSpecs.FromJson(specJson)) != null) return ModClass.Raw; }
            catch (InvalidDataException) { }
        }
        if (statsJson == null) return null;
        using var stats = JsonDocument.Parse(statsJson);
        return stats.RootElement.ValueKind == JsonValueKind.Object && stats.RootElement.TryGetProperty("raw", out _) ? ModClass.Raw : null;
    }

    public static string ClassName(ModClass kind) => kind switch
    {
        ModClass.Code => "code", ModClass.Bundled => "bundled", ModClass.Raw => "raw", _ => "content"
    };

    public static ModClass ClassNamed(string name) => name switch
    {
        "content" => ModClass.Content, "code" => ModClass.Code, "bundled" => ModClass.Bundled, "raw" => ModClass.Raw,
        _ => throw new InvalidDataException($"{name} is not a mod class.")
    };

    // Why a mod of this class needs the verified list, for the player.
    public static string Unchecked(ModClass kind) => kind == ModClass.Raw
        ? "carries data HellForge cannot check (data patches, raw stats, component writes or a data spec's replaced values)"
        : "runs code (a DLL, sources or Lua)";

    internal static bool Hex(string text, int length) => text.Length == length && text.All(char.IsAsciiHexDigitLower);
}

// A mod's content hash as a player's HellForge takes it (HellForge.Cli mod-hash, for the verified list): a folder by its
// counted files; a zip as the catalogue loads it from a mods folder, by what it ships (ModArchives), from a copy in a
// temporary mods folder of its own (the zip's own folder is not touched).
public static class ModHash
{
    public sealed record Hashed(string Id, string Name, string Version, string Hash, ModClass Class, int Files, string? Url);

    public static Hashed Of(string path, EntityProfile? profile = null)
    {
        if (!path.EndsWith(".zip", StringComparison.OrdinalIgnoreCase)) return Of(ModCatalog.Parse(path, profile ?? EntityProfile.Current));
        var temp = Path.Combine(Path.GetTempPath(), "hellforge-mod-hash-" + Guid.NewGuid().ToString("N")[..8]);
        try
        {
            Directory.CreateDirectory(temp);
            File.Copy(path, Path.Combine(temp, Path.GetFileName(path)));
            var (loaded, problems) = ModCatalog.Load(temp, profile);
            return loaded.Count == 1 ? Of(loaded[0])
                : throw new InvalidDataException(problems.Count > 0 ? string.Join("\n", problems) : $"{Path.GetFileName(path)} holds no mod.");
        }
        finally
        {
            try { Directory.Delete(temp, recursive: true); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        }
    }

    public static Hashed Of(ModManifest mod) =>
        new(mod.Id, mod.Name, mod.Version, ModSets.ContentHash(mod), ModSets.ClassOf(mod), ModSets.Files(mod).Count, mod.Url);
}

// HellForge's verified list (data/verified-mods.json, embedded in Core): code and raw mods whose exact content a HellForge
// maintainer reviewed, which mod sync may download from a Steam friend. A version of a mod is listed by its content hash
// (ModSets.ContentHash; HellForge.Cli mod-hash prints it, docs/modding/tools.md "The verified list"), so any other bytes
// under the same id and version are not verified.
//   {"format":1,"mods":[{"id":"...","name":"...","version":"...","hash":"<64 hex>","reviewed":"<date>, <by whom>"}]}
public static class VerifiedMods
{
    public sealed record Entry(string Id, string Name, string Version, string Hash, string Reviewed);

    static readonly Lazy<IReadOnlyList<Entry>> embedded = new(() =>
    {
        using var stream = typeof(VerifiedMods).Assembly.GetManifestResourceStream("HellForge.Core.verified-mods.json")
            ?? throw new InvalidDataException("The verified mod list is not embedded.");
        using var reader = new StreamReader(stream);
        return Parse(reader.ReadToEnd());
    });

    public static IReadOnlyList<Entry> All => embedded.Value;

    public static bool Contains(string id, string hash) => All.Any(e => e.Id == id && e.Hash == hash);

    public static IReadOnlyList<Entry> Parse(string json)
    {
        var root = JsonNode.Parse(json) as JsonObject ?? throw new InvalidDataException("The verified mod list is not an object.");
        if ((int?)root["format"] != 1 || root["mods"] is not JsonArray mods) throw new InvalidDataException("The verified mod list has no format 1 or no mods.");
        var entries = new List<Entry>();
        foreach (var node in mods)
        {
            var entry = new Entry((string?)node?["id"] ?? "", (string?)node?["name"] ?? "", (string?)node?["version"] ?? "", (string?)node?["hash"] ?? "",
                (string?)node?["reviewed"] ?? "");
            if (!ModCatalog.IsModId(entry.Id) || !ModSets.Hex(entry.Hash, 64) || entry.Version.Length == 0 || entry.Reviewed.Length == 0)
                throw new InvalidDataException($"A verified mod entry is malformed: {entry.Id}.");
            entries.Add(entry);
        }
        return entries;
    }
}

// Where the joiner finds a mod it may already have, by id and content hash: an installed mod (on or off, a folder or a
// zip) or one the synced store holds. The installed ones are the catalogue's (ModCatalog.Load, as the Mods page reads it
// while a session runs), so a zipped mod is hashed by its zip as the host's is; each is hashed only when asked about, once.
public sealed class ModLocator(string modsFolder, EntityProfile? profile = null)
{
    readonly Dictionary<string, List<(string Folder, string Hash)>> byId = new(StringComparer.Ordinal);
    // The store's answers, each with its record's time: hashed again only when the record changed (stored again or gone).
    readonly Dictionary<(string Id, string Hash), (DateTime Stamp, string? Folder)> synced = [];
    List<ModManifest>? installed;

    public string? Find(string id, string hash)
    {
        if (FindSynced(id, hash) is { } stored) return stored;
        if (!byId.TryGetValue(id, out var found))
        {
            found = [];
            installed ??= Directory.Exists(modsFolder) ? ModCatalog.Load(modsFolder, profile).Mods : [];
            foreach (var mod in installed.Where(m => m.Id == id))
                try { found.Add((mod.Folder, ModSets.ContentHash(mod))); }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
            byId[id] = found;
        }
        return found.FirstOrDefault(f => f.Hash == hash).Folder;
    }

    string? FindSynced(string id, string hash)
    {
        if (!ModCatalog.IsModId(id) || !ModSets.Hex(hash, 64)) return null;
        DateTime stamp;
        try { stamp = File.GetLastWriteTimeUtc(SyncedStore.RecordIn(modsFolder, id, hash)); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return null; }
        lock (synced)
            if (synced.TryGetValue((id, hash), out var known) && known.Stamp == stamp) return known.Folder;
        var folder = SyncedStore.Find(modsFolder, id, hash, profile);
        lock (synced) synced[(id, hash)] = (stamp, folder);
        return folder;
    }
}

// What joining a host's lobby takes, mod by mod (each host mod in id order, then this machine's mods the host lacks).
//   have      this machine has it (installed, on or off, or synced before): Folder;
//   download  a content mod, or a verified or trusted code or raw mod from a Steam friend, the host shares;
//   manual    the host does not share, or a code or raw mod whose host is not a Steam friend: the player installs it
//             themselves (its Url, when it has one, says where);
//   blocked   on the player's blocklist;
//   update    a mod HellForge ships differs: one of the two players runs another HellForge version;
//   off       this machine's own gameplay mod the host does not run: left out of the synced session;
//   trust     a code or raw mod HellForge has not verified, from a Steam friend who shares: it downloads once the player
//             trusts that exact content (TRUST on the prompt), or the player installs it themselves.
public enum SyncAction { Have, Download, Manual, Blocked, Update, Off, Trust }
// Trust: nothing else stands in the way, but mods wait for the player's trust.
public enum SyncVerdict { Ready, Download, Unjoinable, Blocked, Trust }
public sealed record SyncItem(ModSetEntry Mod, SyncAction Action, string? Folder);
// What lets a code or raw mod download: HellForge's verified list (VerifiedMods.Contains when null) and the player's
// trusted mods (ModSettings' Trusted; none when null), both by id and exact content hash, and whether the host is a Steam
// friend (only friends serve code and raw mods, so only a friend's can be trusted).
public sealed record SyncTrust(Func<string, string, bool>? Verified = null, bool HostIsFriend = true, Func<string, string, bool>? Trusted = null);

public sealed record ModSyncPlan(IReadOnlyList<SyncItem> Items)
{
    public SyncVerdict Verdict =>
        Items.Any(i => i.Action == SyncAction.Blocked) ? SyncVerdict.Blocked
        : Items.Any(i => i.Action is SyncAction.Manual or SyncAction.Update) ? SyncVerdict.Unjoinable
        : Items.Any(i => i.Action == SyncAction.Trust) ? SyncVerdict.Trust
        : Items.Any(i => i.Action == SyncAction.Download) ? SyncVerdict.Download : SyncVerdict.Ready;

    public long DownloadBytes => Items.Where(i => i.Action == SyncAction.Download).Sum(i => i.Mod.Bytes);

    // own: this session's mod set (the joiner's); find: an installed or synced mod by id and hash (ModLocator.Find);
    // trust: what lets a code or raw mod download (SyncTrust; its defaults when null).
    public static ModSyncPlan Make(ModSet host, bool hostShares, ModSet own, Func<string, string, string?> find, IReadOnlySet<string> blocked,
        SyncTrust? trust = null)
    {
        var verified = trust?.Verified ?? VerifiedMods.Contains;
        var hostIsFriend = trust?.HostIsFriend ?? true;
        var trusted = trust?.Trusted ?? ((_, _) => false);
        var items = new List<SyncItem>();
        foreach (var mod in host.Mods.OrderBy(m => m.Id, StringComparer.Ordinal))
        {
            if (blocked.Contains(mod.Id)) { items.Add(new(mod, SyncAction.Blocked, null)); continue; }
            var folder = own.Find(mod.Id, mod.Hash)?.Folder ?? find(mod.Id, mod.Hash);
            var action = folder != null ? SyncAction.Have
                : mod.Class == ModClass.Bundled ? SyncAction.Update
                : !hostShares || ModSets.NeedsVerified(mod.Class) && !hostIsFriend ? SyncAction.Manual
                : ModSets.NeedsVerified(mod.Class) && !verified(mod.Id, mod.Hash) && !trusted(mod.Id, mod.Hash) ? SyncAction.Trust
                : SyncAction.Download;
            items.Add(new(mod, action, folder));
        }
        foreach (var mod in own.Mods.Where(m => host.Mods.All(h => h.Id != m.Id)).OrderBy(m => m.Id, StringComparer.Ordinal))
            items.Add(new(mod, SyncAction.Off, mod.Folder));
        return new ModSyncPlan(items);
    }

    public static string ActionName(SyncAction action) => action.ToString().ToLowerInvariant();
}

// Mods downloaded from hosts: <mods>/.synced/<id>@<first 16 of the content hash>/, which normal sessions never load
// (ModCatalog.Load skips dot folders); only a synced session (ModSyncSession) does. A download is written into
// .synced/.partial/<same name>-<attempt>/ and moved in only after SyncedStore.Install has checked all of it.
public static class SyncedStore
{
    public const string FolderName = ".synced";
    // Beside a synced mod's files; a dot file, so the digest and the content hash leave it out.
    public const string RecordName = ".hellforge-synced.json";
    public const int MaxFiles = 10000;
    // How many synced mods the store keeps (Prune): the ones a session played or downloaded most recently.
    public const int Keep = 20;
    static readonly TimeSpan PartialAge = TimeSpan.FromDays(1);

    public static string Root(string modsFolder) => Path.Combine(modsFolder, FolderName);
    static string Name(string id, string hash) => id + "@" + hash[..16];
    public static string RecordIn(string modsFolder, string id, string hash) => Path.Combine(Root(modsFolder), Name(id, hash), RecordName);

    // The store's folder of a mod, when it holds that exact one: its record names the hash, and its files rehash to it now
    // (the record only says what was checked when it was stored; a file changed since makes it another mod).
    public static string? Find(string modsFolder, string id, string hash, EntityProfile? profile = null)
    {
        if (!ModCatalog.IsModId(id) || !ModSets.Hex(hash, 64)) return null;
        var folder = Path.Combine(Root(modsFolder), Name(id, hash));
        try
        {
            var record = Path.Combine(folder, RecordName);
            if (!File.Exists(record) || (string?)JsonNode.Parse(File.ReadAllText(record))?["hash"] != hash) return null;
            var mod = ModCatalog.Parse(folder, profile ?? EntityProfile.Current);
            if (mod.Id != id || ModSets.ContentHash(mod) != hash) return null;
            // The store stays writable while a session runs (the backend downloads into it): a file the mod does not count
            // would load without being compared, so a copy with one is not this mod.
            var counted = ModSets.Files(mod).Select(f => f.Path).ToHashSet(StringComparer.Ordinal);
            return Directory.EnumerateFiles(folder, "*", SearchOption.AllDirectories).Select(f => Path.GetRelativePath(folder, f).Replace('\\', '/'))
                .All(f => f == RecordName || counted.Contains(f)) ? folder : null;
        }
        catch (Exception error) when (error is JsonException or InvalidDataException or IOException or UnauthorizedAccessException or InvalidOperationException) { return null; }
    }

    // Deletes the store's copy of a mod (the player took back their trust in it); none there is nothing to do.
    public static void Forget(string modsFolder, string id, string hash)
    {
        if (!ModCatalog.IsModId(id) || !ModSets.Hex(hash, 64)) return;
        var folder = Path.Combine(Root(modsFolder), Name(id, hash));
        if (Directory.Exists(folder)) Directory.Delete(folder, recursive: true);
    }

    // A stored mod a session plays now: its record's "used", which Prune goes by.
    public static void Touch(string folder)
    {
        var record = Path.Combine(folder, RecordName);
        try
        {
            if (JsonNode.Parse(File.ReadAllText(record)) is not JsonObject node) return;
            node["used"] = DateTime.UtcNow.ToString("o", CultureInfo.InvariantCulture);
            File.WriteAllText(record + ".tmp", node.ToJsonString());
            File.Move(record + ".tmp", record, overwrite: true);
        }
        catch (Exception error) when (error is JsonException or IOException or UnauthorizedAccessException) { }
    }

    // Deletes the stored mods beyond the `keep` used most recently (a record's "used", else when it was downloaded), never
    // one of `inUse` (the starting session's folders), and downloads left half-done for more than a day. Returns what it
    // removed, by folder name.
    public static List<string> Prune(string modsFolder, IEnumerable<string> inUse, int keep = Keep)
    {
        var store = Root(modsFolder);
        var removed = new List<string>();
        if (!Directory.Exists(store)) return removed;
        var kept = inUse.Select(f => Path.GetFullPath(f).TrimEnd('\\', '/')).ToHashSet(StringComparer.OrdinalIgnoreCase);
        var partials = Path.Combine(store, PartialFolder);
        if (Directory.Exists(partials))
            foreach (var partial in Directory.GetDirectories(partials).Where(d => DateTime.UtcNow - Directory.GetLastWriteTimeUtc(d) > PartialAge))
                if (Delete(partial)) removed.Add(PartialFolder + "/" + Path.GetFileName(partial));
        var stored = Directory.GetDirectories(store).Where(d => !Path.GetFileName(d).StartsWith('.'))
            .Select(d => (Folder: d, Used: LastUsed(d))).OrderByDescending(m => m.Used).ToList();
        foreach (var (folder, _) in stored.Skip(keep).Where(m => !kept.Contains(Path.GetFullPath(m.Folder).TrimEnd('\\', '/'))))
            if (Delete(folder)) removed.Add(Path.GetFileName(folder));
        return removed;

        static bool Delete(string folder)
        {
            try { Directory.Delete(folder, recursive: true); return true; }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return false; } // in use: the next start
        }
    }

    static DateTime LastUsed(string folder)
    {
        try
        {
            var node = JsonNode.Parse(File.ReadAllText(Path.Combine(folder, RecordName)));
            foreach (var key in new[] { "used", "synced" })
                if (node?[key] is JsonValue value && value.TryGetValue<string>(out var text) &&
                    DateTime.TryParse(text, CultureInfo.InvariantCulture, DateTimeStyles.AdjustToUniversal | DateTimeStyles.AssumeUniversal, out var time)) return time;
        }
        catch (Exception error) when (error is JsonException or IOException or UnauthorizedAccessException) { }
        return DateTime.MinValue; // no record: the first to go
    }

    public const string PartialFolder = ".partial";

    // An empty folder of its own to download a mod into: each attempt has its own, so two never write into one.
    public static string Partial(string modsFolder, string id, string hash)
    {
        if (!ModCatalog.IsModId(id) || !ModSets.Hex(hash, 64)) throw new InvalidDataException("A synced mod's id or hash is malformed.");
        var folder = Path.Combine(Root(modsFolder), PartialFolder, Name(id, hash) + "-" + Guid.NewGuid().ToString("N")[..8]);
        Directory.CreateDirectory(folder);
        return folder;
    }

    // Deletes a download that is not kept; one still held open is left for the next Clean.
    public static void Discard(string partial)
    {
        try { if (Directory.Exists(partial)) Directory.Delete(partial, recursive: true); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }

    // The downloads' folder emptied: what an earlier session left there (it stopped mid-download). Only while no download
    // runs (a backend's start).
    public static void Clean(string modsFolder) => Discard(Path.Combine(Root(modsFolder), PartialFolder));

    // A file a host listed, inside the download folder; refused unless its path is one the digest would count and that
    // Windows reads as written (no device names, no trailing dots or spaces, no streams).
    public static string Target(string partial, string relative)
    {
        if (!SafePath(relative)) throw new InvalidDataException($"The host listed an unsafe path: {Visible(relative)}.");
        var root = Path.GetFullPath(partial) + Path.DirectorySeparatorChar;
        var full = Path.GetFullPath(Path.Combine(partial, relative));
        return full.StartsWith(root, StringComparison.OrdinalIgnoreCase) ? full : throw new InvalidDataException($"The host listed an unsafe path: {Visible(relative)}.");
    }

    static readonly HashSet<string> Devices = new(["CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$",
        .. Enumerable.Range(0, 10).SelectMany(n => new[] { "COM" + n, "LPT" + n }), "COM¹", "COM²", "COM³", "LPT¹", "LPT²", "LPT³"], StringComparer.OrdinalIgnoreCase);

    public static bool SafePath(string relative)
    {
        if (relative.Length is 0 or > 240 || ModCatalog.IsWorkspaceFile(relative)) return false;
        foreach (var part in relative.Split('/'))
        {
            if (part.Length == 0 || part.EndsWith('.') || part.EndsWith(' ') || part.StartsWith(' ')) return false;
            if (part.Any(c => c < 32 || c is '\\' or ':' or '*' or '?' or '"' or '<' or '>' or '|')) return false;
            // Windows reads "con .txt" and "nul .json" as the devices too: the spaces before the extension do not count
            if (Devices.Contains(part.Split('.')[0].TrimEnd(' '))) return false;
        }
        return true;
    }

    static string Visible(string text) => string.Concat(text.Take(80).Select(c => char.IsControl(c) ? '?' : c));

    // Checks a finished download against what the plan expected and moves it into the store; returns its folder. It
    // must be a mod with that id and version, the same content hash read from its own files, and content, or code or raw
    // data verified lets through (the verified list, or the player's trust; the class read from its own files, whatever the
    // host said). Throws InvalidDataException (or OperationCanceledException when cancelled), with the download deleted
    // whatever went wrong.
    public static string Install(string modsFolder, ModSetEntry expected, string partial, Func<string, string, bool>? verified = null,
        EntityProfile? profile = null, CancellationToken cancel = default)
    {
        try
        {
            cancel.ThrowIfCancellationRequested();
            Verify(expected, partial, verified ?? VerifiedMods.Contains, profile ?? EntityProfile.Current);
            cancel.ThrowIfCancellationRequested();
            File.WriteAllText(Path.Combine(partial, RecordName), new JsonObject { ["hash"] = expected.Hash, ["id"] = expected.Id, ["version"] = expected.Version,
                ["synced"] = DateTime.UtcNow.ToString("o", CultureInfo.InvariantCulture) }.ToJsonString());
            var target = Path.Combine(Root(modsFolder), Name(expected.Id, expected.Hash));
            if (Directory.Exists(target)) Directory.Delete(target, recursive: true);
            Directory.Move(partial, target);
            return target;
        }
        catch
        {
            Discard(partial);
            throw;
        }
    }

    static void Verify(ModSetEntry expected, string partial, Func<string, string, bool> verified, EntityProfile profile)
    {
        ModManifest mod;
        try { mod = ModCatalog.Parse(partial, profile); }
        catch (JsonException error) { throw new InvalidDataException($"{expected.Id}: its manifest is not valid JSON ({error.Message})."); }
        catch (Exception error) when (error is not (InvalidDataException or OperationCanceledException))
        {
            throw new InvalidDataException($"{expected.Id}: it could not be read as a mod ({error.Message}).");
        }
        if (mod.Id != expected.Id || mod.Version != expected.Version || !mod.Gameplay)
            throw new InvalidDataException($"{expected.Id}: the files are another mod ({mod.Id} {mod.Version}{(mod.Gameplay ? "" : ", cosmetic")}).");
        var files = ModSets.Files(mod);
        if (files.Count > MaxFiles || ModSets.Hash(ModSets.Block(mod, files)) != expected.Hash)
            throw new InvalidDataException($"{expected.Id}: the files do not match the host's mod.");
        // Only the counted files may be there: anything else would load without being compared.
        var counted = files.Select(f => f.Path).ToHashSet(StringComparer.Ordinal);
        if (Directory.GetFiles(partial, "*", SearchOption.AllDirectories).Any(f => !counted.Contains(Path.GetRelativePath(partial, f).Replace('\\', '/'))))
            throw new InvalidDataException($"{expected.Id}: it holds files the host did not list.");
        var kind = ModSets.ClassOf(mod);
        if (kind == ModClass.Bundled) throw new InvalidDataException($"{expected.Id} is a mod HellForge ships; it is never downloaded.");
        if (ModSets.NeedsVerified(kind) && !verified(mod.Id, expected.Hash))
            throw new InvalidDataException($"{expected.Id} {ModSets.Unchecked(kind)} and is neither on HellForge's verified list nor trusted.");
    }
}

// The offer as a session's backend writes it for launchers (<run>/backend/mod-sync.json), and the player's answer a
// launcher writes back (<run>/backend/mod-sync-answer.json), which the backend takes once a second. For sessions whose
// game has no in-game editor to show the MOD SYNC prompt; the answer is the one the prompt sends (sync_answer).
public sealed record ModSyncOffer(int Version, string State, string Host, long Bytes, long Done, string Message, IReadOnlyList<ModSyncOffer.Mod> Mods)
{
    public sealed record Mod(string Id, string Name, string Version, string Class, string Action, long Bytes, string? Url = null);

    public const string FileName = "mod-sync.json", AnswerFileName = "mod-sync-answer.json";
    public static readonly IReadOnlySet<string> Answers = new HashSet<string>(StringComparer.Ordinal) { "accept", "decline", "restart", "block", "trust", "open" };
    // the answers about one mod, which name it
    static readonly IReadOnlySet<string> ModAnswers = new HashSet<string>(StringComparer.Ordinal) { "block", "trust", "open" };

    // The offer in a session's backend folder; null when there is none or it cannot be read (being written).
    public static ModSyncOffer? Read(string backendFolder)
    {
        try
        {
            if (JsonNode.Parse(File.ReadAllText(Path.Combine(backendFolder, FileName))) is not JsonObject root) return null;
            static string Text(JsonNode? node) => node is JsonValue value && value.TryGetValue<string>(out var text) ? text : "";
            static long Number(JsonNode? node) => node is JsonValue value && value.TryGetValue<long>(out var number) ? number : 0;
            var mods = root["mods"] is JsonArray list
                ? list.OfType<JsonObject>().Select(m => new Mod(Text(m["id"]), Text(m["name"]), Text(m["version"]), Text(m["class"]), Text(m["action"]), Number(m["bytes"]),
                    ModCatalog.WebAddress(Text(m["url"]) is { Length: > 0 } url ? url : null))).ToList()
                : [];
            return new ModSyncOffer((int)Number(root["version"]), Text(root["state"]), Text(root["host"]), Number(root["bytes"]), Number(root["done"]), Text(root["message"]), mods);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException) { return null; }
    }

    // The player's answer to the offer of that version: written whole (a temporary file, then a rename).
    public static void Answer(string backendFolder, int version, string answer, string? modId = null)
    {
        if (!Answers.Contains(answer) || ModAnswers.Contains(answer) != (modId != null) || modId != null && !ModCatalog.IsModId(modId))
            throw new InvalidDataException($"{answer} {modId} is not an answer to a mod sync offer.");
        var path = Path.Combine(backendFolder, AnswerFileName);
        File.WriteAllText(path + ".tmp", new JsonObject { ["version"] = version, ["answer"] = answer, ["mod"] = modId }.ToJsonString());
        File.Move(path + ".tmp", path, overwrite: true);
    }

    // The backend's side: the waiting answer as a syncanswer line's parts, and the file gone; null when there is none or
    // it is not an answer (it is deleted either way).
    public static IReadOnlyList<string>? TakeAnswer(string backendFolder)
    {
        var path = Path.Combine(backendFolder, AnswerFileName);
        if (!File.Exists(path)) return null;
        try
        {
            var text = File.ReadAllText(path);
            File.Delete(path);
            var root = JsonNode.Parse(text) as JsonObject;
            var version = root?["version"] is JsonValue v && v.TryGetValue<int>(out var number) && number >= 0 ? number : -1;
            var answer = root?["answer"] is JsonValue a && a.TryGetValue<string>(out var word) ? word : "";
            var mod = root?["mod"] is JsonValue m && m.TryGetValue<string>(out var id) ? id : null;
            if (version < 0 || !Answers.Contains(answer) || ModAnswers.Contains(answer) != (mod != null) || mod != null && !ModCatalog.IsModId(mod)) return null;
            return mod == null ? ["syncanswer", version.ToString(CultureInfo.InvariantCulture), answer] : ["syncanswer", version.ToString(CultureInfo.InvariantCulture), answer, mod];
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException) { return null; }
    }
}

// The session after a sync: the host's gameplay mods in the host's load order, found by id and content hash, then this
// player's own cosmetic mods and Local Stats. <installation>/temp/mod-sync/session.json, written by the joiner's backend
// once everything is here; used once, by the session the launcher starts with --mod-sync.
public sealed record ModSyncSession(string Host, string HostName, string Connection, string Digest, IReadOnlyList<ModSyncSession.Mod> Mods)
{
    public sealed record Mod(string Id, string Hash, int Order);
    public const string FolderName = "mod-sync", FileName = "session.json";

    public static string PathIn(InstallLayout install) => System.IO.Path.Combine(install.Temp, FolderName, FileName);

    public void Write(string path)
    {
        Directory.CreateDirectory(System.IO.Path.GetDirectoryName(path)!);
        var root = new JsonObject
        {
            ["format"] = 1, ["host"] = Host, ["hostName"] = HostName, ["connection"] = Connection, ["digest"] = Digest,
            ["created"] = DateTime.UtcNow.ToString("o", CultureInfo.InvariantCulture),
            ["mods"] = new JsonArray([.. Mods.Select(m => (JsonNode?)new JsonObject { ["id"] = m.Id, ["hash"] = m.Hash, ["order"] = m.Order })])
        };
        File.WriteAllText(path + ".partial", root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
        File.Move(path + ".partial", path, overwrite: true);
    }

    public static ModSyncSession Read(string path)
    {
        var root = JsonNode.Parse(File.ReadAllText(path)) as JsonObject ?? throw new InvalidDataException("The mod sync file is not an object.");
        if ((int?)root["format"] != 1) throw new InvalidDataException("The mod sync file has no format 1.");
        var mods = new List<Mod>();
        foreach (var node in root["mods"] as JsonArray ?? [])
        {
            var mod = new Mod((string?)node?["id"] ?? "", (string?)node?["hash"] ?? "", (int?)node?["order"] ?? -1);
            if (!ModCatalog.IsModId(mod.Id) || !ModSets.Hex(mod.Hash, 64) || mod.Order < 0) throw new InvalidDataException("A mod in the mod sync file is malformed.");
            mods.Add(mod);
        }
        return new ModSyncSession((string?)root["host"] ?? "", (string?)root["hostName"] ?? "", (string?)root["connection"] ?? "", (string?)root["digest"] ?? "", mods);
    }

    // The session's mods in load order (bottom of the Mods page first), from the player's catalogue (ModCatalog.Load) and
    // the store; null with the reason when a mod is missing or the result's digest is not the host's.
    public (List<ModManifest>? LoadOrder, string? Problem) Resolve(string modsFolder, IReadOnlyList<ModManifest> catalog, EntityProfile? profile = null)
    {
        var locator = new ModLocator(modsFolder, profile);
        var host = new List<ModManifest>();
        foreach (var mod in Mods.OrderBy(m => m.Order))
        {
            var listed = catalog.FirstOrDefault(m => m.Id == mod.Id);
            var folder = locator.Find(mod.Id, mod.Hash);
            if (folder == null) return (null, $"{mod.Id} is no longer here.");
            ModManifest found;
            try
            {
                found = listed != null && Same(listed.Folder, folder) ? listed with { Enabled = true }
                    : ModCatalog.Parse(folder, profile ?? EntityProfile.Current);
            }
            catch (Exception error) when (error is InvalidDataException or JsonException or IOException) { return (null, $"{mod.Id}: {error.Message}"); }
            host.Add(found);
        }
        // The player's own: cosmetic mods first (the bottom of their Mods page), Local Stats last (it stays on top).
        var own = ModCatalog.LoadOrder(catalog).Where(m => m.Enabled && !ModCatalog.Counted(m) && host.All(h => h.Id != m.Id)).ToList();
        var loadOrder = own.Where(m => m.Id != ModCatalog.LocalStatsId).Concat(host).Concat(own.Where(m => m.Id == ModCatalog.LocalStatsId)).ToList();
        var digest = ModSet.Of(loadOrder).Digest;
        return digest == Digest ? (loadOrder, null) : (null, "the mods found do not make the host's modlist code.");
    }

    static bool Same(string a, string b) => string.Equals(System.IO.Path.GetFullPath(a).TrimEnd('\\', '/'), System.IO.Path.GetFullPath(b).TrimEnd('\\', '/'), StringComparison.OrdinalIgnoreCase);
}
