using System.Security.Cryptography;
using System.Text;
using HellForge.Core.GameData;

namespace HellForge.Core.AssetMigration;

// Which resources (type and name) every build has, and in which archive: read from the archives' tables alone, so a
// build costs its tables, not its content. An archive whose files are unchanged since an earlier build (the same
// manifest SHA-1s) is not read again. It answers "is this resource one of the game's, in any build since release?"
// for every build, while the content index answers "are these bytes the game's?" for the builds it holds.
public sealed class NameMap
{
    const string Magic = "HFNM";
    const int Version = 1;

    public List<string> Builds { get; } = [];
    // Each distinct archive table, by its hash: the archive's name and its resource keys.
    readonly List<(string Name, ResourceKey[] Keys)> tables = [];
    readonly Dictionary<string, int> tableIds = new(StringComparer.Ordinal);
    // Per build, the tables it has.
    readonly List<int[]> buildTables = [];
    Dictionary<ResourceKey, List<int>>? byKey;
    public long TablesRead { get; private set; }
    public long TablesReused { get; private set; }

    public int ArchiveCount => tables.Count;

    // Whether an archive with this TableHash is already in the map (its table need not be read).
    public bool Knows(string archiveHash) => tableIds.ContainsKey("file:" + archiveHash);

    public void Add(IGameResources game, Action<int>? progress = null)
    {
        if (Builds.Contains(game.Build)) throw new InvalidOperationException($"Build {game.Build} is already in the name map.");
        var ids = new List<int>();
        foreach (var (archive, keys) in game.Tables())
        {
            var known = game.TableHash(archive);
            int id;
            if (known is not null && tableIds.TryGetValue("file:" + known, out id)) TablesReused++;
            else
            {
                var list = keys().ToArray();
                TablesRead++;
                // An archive known only by its table (no manifest hash) is keyed by the table's own hash.
                var hash = known is not null ? "file:" + known : "table:" + archive + ":" + TableHash(list);
                if (!tableIds.TryGetValue(hash, out id))
                {
                    id = tables.Count;
                    tables.Add((archive, list));
                    tableIds[hash] = id;
                }
            }
            ids.Add(id);
            if (ids.Count % 500 == 0) progress?.Invoke(ids.Count);
        }
        progress?.Invoke(ids.Count);
        Builds.Add(game.Build);
        buildTables.Add([.. ids]);
        byKey = null;
    }

    static string TableHash(ResourceKey[] keys)
    {
        var bytes = new byte[16 * keys.Length];
        for (var i = 0; i < keys.Length; i++)
        {
            BitConverter.TryWriteBytes(bytes.AsSpan(16 * i), keys[i].Type);
            BitConverter.TryWriteBytes(bytes.AsSpan(16 * i + 8), keys[i].Name);
        }
        return Convert.ToHexString(SHA256.HashData(bytes))[..32];
    }

    // The builds (and the archive in each) that have this resource.
    public IEnumerable<(string Build, string Archive)> Find(ResourceKey key)
    {
        if (byKey is null)
        {
            byKey = [];
            for (var t = 0; t < tables.Count; t++)
                foreach (var k in tables[t].Keys)
                {
                    if (!byKey.TryGetValue(k, out var list)) byKey[k] = list = [];
                    if (list.Count == 0 || list[^1] != t) list.Add(t);
                }
        }
        if (!byKey.TryGetValue(key, out var found)) yield break;
        for (var b = 0; b < Builds.Count; b++)
            foreach (var t in buildTables[b])
                if (found.Contains(t)) yield return (Builds[b], tables[t].Name);
    }

    public bool Has(ResourceKey key)
    {
        if (byKey is null) _ = Find(key).Any();
        return byKey!.ContainsKey(key);
    }

    public void Save(string path)
    {
        var temp = path + ".tmp";
        using (var w = new BinaryWriter(File.Create(temp), Encoding.UTF8))
        {
            w.Write(Encoding.ASCII.GetBytes(Magic));
            w.Write(Version);
            w.Write(tables.Count);
            foreach (var (hash, id) in tableIds.OrderBy(t => t.Value))
            {
                w.Write(hash);
                w.Write(tables[id].Name);
                w.Write(tables[id].Keys.Length);
                foreach (var k in tables[id].Keys)
                {
                    w.Write(k.Type);
                    w.Write(k.Name);
                }
            }
            w.Write(Builds.Count);
            for (var b = 0; b < Builds.Count; b++)
            {
                w.Write(Builds[b]);
                w.Write(buildTables[b].Length);
                foreach (var t in buildTables[b]) w.Write(t);
            }
        }
        File.Move(temp, path, true);
    }

    public static NameMap Load(string path)
    {
        using var r = new BinaryReader(File.OpenRead(path), Encoding.UTF8);
        if (Encoding.ASCII.GetString(r.ReadBytes(4)) != Magic || r.ReadInt32() != Version) throw new InvalidDataException($"{path} is not a HellForge name map.");
        var map = new NameMap();
        var count = r.ReadInt32();
        for (var i = 0; i < count; i++)
        {
            var hash = r.ReadString();
            var name = r.ReadString();
            var keys = new ResourceKey[r.ReadInt32()];
            for (var k = 0; k < keys.Length; k++) keys[k] = new ResourceKey(r.ReadUInt64(), r.ReadUInt64());
            map.tableIds[hash] = map.tables.Count;
            map.tables.Add((name, keys));
        }
        var builds = r.ReadInt32();
        for (var b = 0; b < builds; b++)
        {
            map.Builds.Add(r.ReadString());
            var ids = new int[r.ReadInt32()];
            for (var t = 0; t < ids.Length; t++) ids[t] = r.ReadInt32();
            map.buildTables.Add(ids);
        }
        return map;
    }
}
