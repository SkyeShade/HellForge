using System.Globalization;
using System.Text.Json.Nodes;

namespace HellForge.Core;

// Changing a running session's mods without a restart (docs/mods.md "Changing mods while you play"). The session's
// backend compares the mods the session runs (SessionMods, written by mods-stage) with the mods it would run now (the
// Mods page's, or a host's after a mod sync), and sends the in-game loader the change (LiveModPlan) over the live pipe
// (HellForge-Engine native/mod-loader/live_mods.c). Stats, componentWrites and "own" records, network field types, code
// that declares "live": true (SDK 16; source mods built for the change), and game data (the change's hot package,
// LiveData, built with the source mods by HellForge.Cli mods-live) change live. Code without "live", Lua mods, cosmetic
// packs, a new order and a mod built on premium content the player does not own (LiveModPlan.WithPremium: a restart locks
// its items) wait for the next session, as does whatever the loader refuses (numbered content a restart has to number, a
// difficulty with larger maps).

// A mod as the session runs it: its content hash and pack choice tell whether the mod on disk is still the one that runs.
// Live: it can be taken out while the game runs; Data: it put patches in the game's data (a live change restores them).
public sealed record SessionMod(string Id, string Version, string Folder, string Hash, string Choice, bool Loader, bool Live, bool Data = false);

// The session's mods in load order, and the mods its load order named that did not load (a source mod that did not
// build, a data spec that could not be made), so they do not count as added. SyncedWith: the host whose mods it plays
// after a mod sync ("" its own).
public sealed record SessionMods(IReadOnlyList<SessionMod> Mods, IReadOnlyList<SessionMod> Skipped, string SyncedWith)
{
    public const string FileName = "session-mods.json";

    public static SessionMod Of(ModManifest mod) =>
        new(mod.Id, mod.Version, mod.Folder, ModSets.ContentHash(mod), ChoiceText(mod), mod.NeedsLoader, LiveModPlan.Why(mod) == null, LiveModPlan.HasData(mod));

    public static SessionMods Of(IReadOnlyList<ModManifest> loadOrder, IReadOnlyList<ModManifest> active, string syncedWith) =>
        new([.. active.Select(Of)], [.. loadOrder.Where(m => active.All(a => a.Id != m.Id)).Select(Of)], syncedWith);

    // A cosmetic pack's choice as text: which options are off and which variants are picked ("" for other mods).
    public static string ChoiceText(ModManifest mod) => mod.Pack == null ? "" :
        string.Join(',', mod.PackChoice.Off.Order(StringComparer.Ordinal)) + ";" +
        string.Join(',', mod.PackChoice.Sub.OrderBy(p => p.Key, StringComparer.Ordinal).Select(p => p.Key + "=" + p.Value));

    public void Write(string path)
    {
        static JsonArray List(IEnumerable<SessionMod> mods) => [.. mods.Select(m => (JsonNode?)new JsonObject
        {
            ["id"] = m.Id, ["version"] = m.Version, ["folder"] = m.Folder, ["hash"] = m.Hash, ["choice"] = m.Choice, ["loader"] = m.Loader, ["live"] = m.Live,
            ["data"] = m.Data
        })];
        var root = new JsonObject { ["format"] = 1, ["syncedWith"] = SyncedWith, ["mods"] = List(Mods), ["skipped"] = List(Skipped) };
        File.WriteAllText(path + ".partial", root.ToJsonString());
        File.Move(path + ".partial", path, overwrite: true);
    }

    public static SessionMods Read(string path)
    {
        var root = JsonNode.Parse(File.ReadAllText(path)) as JsonObject ?? throw new InvalidDataException("The session's mods file is not an object.");
        if ((int?)root["format"] != 1) throw new InvalidDataException("The session's mods file has no format 1.");
        static List<SessionMod> List(JsonNode? node)
        {
            var mods = new List<SessionMod>();
            foreach (var m in node as JsonArray ?? [])
            {
                var mod = new SessionMod((string?)m?["id"] ?? "", (string?)m?["version"] ?? "", (string?)m?["folder"] ?? "", (string?)m?["hash"] ?? "",
                    (string?)m?["choice"] ?? "", (bool?)m?["loader"] ?? false, (bool?)m?["live"] ?? false, (bool?)m?["data"] ?? false);
                if (!ModCatalog.IsModId(mod.Id) || !ModSets.Hex(mod.Hash, 64)) throw new InvalidDataException("A mod in the session's mods file is malformed.");
                mods.Add(mod);
            }
            return mods;
        }
        return new SessionMods(List(root["mods"]), List(root["skipped"]), (string?)root["syncedWith"] ?? "");
    }
}

public enum LiveModAction { Add, Remove, Replace }

// One mod's part of a change. Mod: the mod as it will run (null for a removal); Was: as it ran (null for an addition).
public sealed record LiveModChange(string Id, string Name, LiveModAction Action, ModManifest? Mod, SessionMod? Was);

// What the session's tools made for a change before the loader takes it (HellForge.Cli mods-live): each source mod's
// build (its DLL), and the change's game data: the hot package and its overrides, as the loader's lines.
public sealed record LivePrepared(IReadOnlyDictionary<string, string> Builds, IReadOnlyList<string[]> Data);

// A change of the session's mods: Live when the loader can take all of it now; otherwise Blocking says, mod by mod, what
// waits for the next session. Target: the mods the session runs after it, in load order.
public sealed record LiveModPlan(IReadOnlyList<LiveModChange> Changes, IReadOnlyList<string> Blocking, IReadOnlyList<ModManifest> Target)
{
    public bool Live => Changes.Count > 0 && Blocking.Count == 0;
    public bool Empty => Changes.Count == 0 && Blocking.Count == 0;

    // Whether it puts patches in the game's data (its data/ folder, or a data spec built for the session).
    public static bool HasData(ModManifest mod) => mod.HasDataSpec || mod.DataFiles.Count > 0;

    // Why a mod cannot change while the game runs, or null.
    public static string? Why(ModManifest mod) =>
        mod.HasCode && !mod.Live ? "it runs code that does not declare \"live\": true" :
        mod.Lua ? "it holds Lua scripts" :
        mod.Pack != null ? "it is a cosmetic pack" : null;

    // Whether the change needs the session's tools first: a source mod's build, or game data to package.
    public bool NeedsPreparation => Changes.Any(c => c.Mod != null && (c.Mod.HasSource || HasData(c.Mod)) || c.Was?.Data == true);

    // A change's target from its mods' folders (HellForge.Cli mods-live --target): the mods the Mods page lists as it
    // loads them (cosmetic packs and archives among them, which have no manifest of their own there), a host's mod
    // downloaded by mod sync (mods/.synced, which the page does not list) from its manifest.
    public static List<ModManifest> TargetOf(string modsFolder, IEnumerable<string> folders)
    {
        var listed = ModCatalog.Load(modsFolder).Mods.ToDictionary(m => Path.GetFullPath(m.Folder), StringComparer.OrdinalIgnoreCase);
        return [.. folders.Where(f => f.Length > 0).Select(f => listed.TryGetValue(Path.GetFullPath(f), out var mod) ? mod : ModCatalog.Parse(f, EntityProfile.Current))];
    }

    public static LiveModPlan Make(SessionMods current, IReadOnlyList<ModManifest> target)
    {
        var changes = new List<LiveModChange>();
        var blocking = new List<string>();
        var was = current.Mods.ToDictionary(m => m.Id, StringComparer.Ordinal);
        var skipped = current.Skipped.ToDictionary(m => m.Id, StringComparer.Ordinal);
        var now = target.Select(m => m.Id).ToHashSet(StringComparer.Ordinal);
        void Change(LiveModAction action, string id, string name, ModManifest? mod, SessionMod? old)
        {
            var why = mod != null ? Why(mod) : null;
            why ??= old != null && !old.Live ? "it ran code that did not declare \"live\", Lua or a cosmetic pack" : null;
            if (why != null) blocking.Add($"{name}: {why}");
            else changes.Add(new LiveModChange(id, name, action, mod, old));
        }
        foreach (var mod in target)
        {
            if (!was.TryGetValue(mod.Id, out var old))
            {
                // a mod that did not load at the session's start, unchanged since, is not new
                if (skipped.TryGetValue(mod.Id, out var notLoaded) && notLoaded.Hash == ModSets.ContentHash(mod)) continue;
                Change(LiveModAction.Add, mod.Id, mod.Name, mod, null);
            }
            // Local Stats is the editors' save file: what they save, they have already changed live
            else if (mod.Id != ModCatalog.LocalStatsId && (old.Hash != ModSets.ContentHash(mod) || old.Choice != SessionMods.ChoiceText(mod)))
                Change(LiveModAction.Replace, mod.Id, mod.Name, mod, old);
        }
        foreach (var old in current.Mods.Where(m => !now.Contains(m.Id))) Change(LiveModAction.Remove, old.Id, old.Id, null, old);
        // the mods that stay keep their order: stats apply in it, and data patches were staged in it
        var stayed = current.Mods.Where(m => now.Contains(m.Id)).Select(m => m.Id).ToList();
        var stays = target.Where(m => was.ContainsKey(m.Id)).Select(m => m.Id).ToList();
        if (!stayed.SequenceEqual(stays, StringComparer.Ordinal)) blocking.Add("The mods' order changed");
        return new LiveModPlan(changes, blocking, target);
    }

    // The plan with the mods it adds or changes that build on premium content this player does not own (PremiumMods) held
    // back: a session's start loads such a mod with its weapons and stratagems locked (the backend's --premium-locked), which
    // a live change cannot do, so they wait for a restart. The mods that stay keep the start's lock. owned is read only when
    // a mod the change brings declares premium content.
    public LiveModPlan WithPremium(Func<PremiumContent> owned)
    {
        var premium = Changes.Select(c => c.Mod).OfType<ModManifest>().Where(m => m.Premium.Count > 0).ToList();
        if (premium.Count == 0) return this;
        var ownership = owned();
        List<string> held = [.. premium.Select(m => (m.Name, Missing: ownership.Missing(m.Premium))).Where(m => m.Missing.Count > 0)
            .Select(m => $"{m.Name}: it uses {string.Join(", ", m.Missing.Select(PremiumContent.Describe))}, which this Steam account has not synced, so it needs a restart")];
        return held.Count == 0 ? this : this with { Blocking = [.. Blocking, .. held] };
    }

    // What the loader is told a mod carries ("stats,code,live,net"; LiveModsRequest's kinds), or null when it carries
    // nothing the loader takes (a data mod alone: its data is the session's lines).
    static string? Kinds(ModManifest mod)
    {
        var kinds = new List<string>();
        if (StatsData.Edits(mod).Any()) kinds.Add("stats");
        if (mod.HasCode) kinds.AddRange(["code", "live"]);
        if (mod.NetworkFieldTypes.Count > 0) kinds.Add("net");
        return kinds.Count > 0 ? string.Join(',', kinds) : null;
    }

    // The loader's lines, verb then fields (the live pipe adds the sequence number): the loader's mods after the change in
    // load order, then each change with its stat and network type lines, then the change's game data. txn: the change's
    // id; sync: mod sync applying a host's set; prepared: what mods-live made (needed when NeedsPreparation).
    public IEnumerable<(string Verb, string[] Fields)> Lines(string txn, bool sync, string digest, LivePrepared? prepared = null)
    {
        if (NeedsPreparation && prepared == null) throw new InvalidDataException("The change needs its builds and game data first (mods-live).");
        yield return ("modbegin", [txn, sync ? "sync" : "solo", digest]);
        var index = 0;
        foreach (var mod in Target.Where(m => m.NeedsLoader))
            yield return ("modorder", [txn, (index++).ToString(CultureInfo.InvariantCulture), mod.Id]);
        foreach (var line in Changes.SelectMany(c => ChangeLines(c, txn, prepared))) yield return line;
        foreach (var line in prepared?.Data ?? []) yield return (line[0], [txn, .. line[1..]]);
        yield return ("modcommit", [txn]);
    }

    // One mod's lines: its removal, or its addition or new version with its stat and network type lines (none for a mod
    // the loader never sees: its data and the digest change).
    static IEnumerable<(string Verb, string[] Fields)> ChangeLines(LiveModChange change, string txn, LivePrepared? prepared)
    {
        var loads = change.Mod?.NeedsLoader == true;
        var loaded = change.Was?.Loader == true;
        if (loaded && (change.Action == LiveModAction.Remove || !loads)) { yield return ("modremove", [txn, change.Id]); yield break; }
        if (!loads) yield break;
        var mod = change.Mod!;
        var dll = !mod.HasCode ? "-" : prepared?.Builds.GetValueOrDefault(mod.Id) ?? mod.DllPath ??
            throw new InvalidDataException($"{mod.Id} is a source mod that was not built for this change.");
        yield return (loaded ? "modreplace" : "modadd", [txn, mod.Id, Kinds(mod)!, ModSets.ContentHash(mod)[..16], mod.Folder, dll]);
        foreach (var edit in StatsData.Edits(mod)) yield return ("modstat", [txn, .. ModLoaderPlan.StatFields(mod, edit)]);
        foreach (var type in mod.NetworkFieldTypes) yield return ("modnet", [txn, .. ModLoaderPlan.NetFields(mod, type)]);
    }
}
