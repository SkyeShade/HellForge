using HellForge.Core;

namespace HellForge.LocalBackend;

// registered-items.json is written by the session's game, so a player can put any ids in it. An entry whose own id, or an
// id it makes (a weapon's or a pattern's children, a stratagem's child), is the game's or Reserved is refused, and the
// log names its mod. A mod's ids are hashes of its names, so an honest entry never collides. A copy is held only when the
// player may hold what it copies, followed through other mods' weapons (registry_load.c PendingBase) to the game's.
public sealed partial class RegisteredItems
{
    // Level 10's unlock item, the base of every level a mod adds (difficulty.c kDifficultyItems).
    public const uint LevelTenItem = 0xA0AE653A;

    // Ids no entry may take or make: paid, warbond-gated or a child of either (PaidContentSet.IsReserved); null checks
    // only the game's catalogue (tests, the development backend).
    public Func<uint, bool>? Reserved { get; set; }

    bool Taken(uint id, Vanilla? game) => game?.Ids.Contains(id) == true || Reserved?.Invoke(id) == true;

    List<Item> Checked(IReadOnlyList<Item> loaded)
    {
        var game = Game();
        List<Item> kept = [];
        foreach (var item in loaded)
            if (Refusal(item, game) is { } why) Report(item, why);
            else kept.Add(item);
        return kept;
    }

    // A level is difficulty.c's: level 10's copy under the top half of Murmur64A("<name>#item").
    string? Refusal(Item item, Vanilla? game)
    {
        if (item.IsDifficulty && (item.CopyOf != LevelTenItem || item.Name.Length == 0 || item.MixId != EngineHash.Id32(item.Name + "#item")))
            return "it is not a level copied from level 10 under its name's id";
        if (Taken(item.MixId, game)) return "its id is the game's or a paid item's";
        if (item.IsStratagem && game?.StratagemItems.ContainsKey(item.Stable) == true) return $"its stable id {item.Stable} is a game stratagem's";
        IEnumerable<uint> made = item.IsStratagem ? [WeaponChild(item.Stable, item.MixId)]
            : item.IsWeapon ? (game?.ChildIds.GetValueOrDefault(item.CopyOf) ?? []).Select(child => Rehome(child, item.CopyOf, item.MixId)) : [];
        return made.Where(id => Taken(id, game)).Select(id => $"its child's id {id} is the game's or a paid item's").FirstOrDefault();
    }

    List<ModPattern> Checked(IReadOnlyList<ModPattern> patterns, IReadOnlyList<Item> admitted)
    {
        var game = Game();
        List<ModPattern> kept = [];
        foreach (var p in patterns)
        {
            IEnumerable<uint> made = p.IsWeapon ? p.Weapons.Concat(Offering(game, p.CopyOf, admitted)).Select(w => WeaponChild(p.MixId, w))
                : game?.Skins.GetValueOrDefault(p.CopyOf) is { MixId: not 0 } skin && SkinParent(game, p.CopyOf, skin) is { } parent ? [WeaponChild(p.MixId, parent)] : [];
            if (made.Prepend(p.MixId).Any(id => Taken(id, game))) ReportPattern(p, "its id or a child's is the game's or a paid item's");
            else kept.Add(p);
        }
        return kept;
    }

    List<ModAttachment> Checked(IReadOnlyList<ModAttachment> attachments, IReadOnlyList<Item> admitted)
    {
        var game = Game();
        List<ModAttachment> kept = [];
        foreach (var a in attachments)
            if (a.Weapons.Select(w => WeaponChild(a.MixId, w)).Prepend(a.MixId).Any(id => Taken(id, game)))
                ReportEntry(a.Mod, "attachment", a.MixId, "its id or a child's is the game's or a paid item's");
            else kept.Add(a);
        return kept;
    }

    void ReportEntry(string mod, string kind, uint mixId, string why)
    {
        lock (reported)
            if (!reported.Add(mixId ^ 0xA77AC4u)) return;
        Log($"{(mod.Length > 0 ? mod + "'s " : "")}{kind} {mixId} is left out: {why}.");
    }

    // A premium mod the player does not own, or a copy of a premium base they do not own; a stratagem whose base cannot be
    // found yet stays out too, until the game has loaded its stratagems. A weapon copying a weapon a locked mod added is a
    // copy of that mod's content, so it is locked too.
    bool Locked(Item item, Session current)
    {
        if (LockedMods.Contains(item.Mod)) return true;
        if (OwnsBase == null || item.IsDifficulty) return false;
        if (item.IsStratagem)
            return Game()?.StratagemItems.TryGetValue(item.CopyOf, out var parent) != true || !OwnsBase(parent);
        return GameBase(item, current) is not { } at || !OwnsBase(at);
    }

    // The game's weapon at the end of a copy chain, as the loader follows it (registry_load.c PendingBase: 8 copies at
    // most, then no base). A longer chain, a cycle or an end that is not the game's has none, or one through a locked mod.
    uint? GameBase(Item item, Session current)
    {
        var at = item;
        for (var depth = 0; depth < MaxCopies && current.Admitted.FirstOrDefault(o => o.IsWeapon && o != at && o.MixId == at.CopyOf) is { } next; depth++)
        {
            if (LockedMods.Contains(next.Mod)) return null;
            at = next;
        }
        return Game()?.Ids.Contains(at.CopyOf) == true ? at.CopyOf : null;
    }

    const int MaxCopies = 8;

    // An attachment or a weapon pattern is held when the player may hold one of the game's children it copies: their
    // owners are the weapons they are offered on. A unit pattern follows its base skin's item.
    bool AttachmentLocked(ModAttachment a) => LockedMods.Contains(a.Mod) || OwnsBase != null && !OwnsAnyChild(a.CopyOf);

    bool PatternLocked(PatternPlan plan)
    {
        var p = plan.Pattern;
        if (LockedMods.Contains(p.Mod)) return true;
        if (OwnsBase == null) return false;
        return p.IsWeapon ? !OwnsAnyChild(p.CopyOf) : Game()?.Skins.GetValueOrDefault(p.CopyOf) is not { MixId: not 0 } skin || !OwnsBase(skin.MixId);
    }

    bool OwnsAnyChild(uint itemId) => Game()?.ItemChildren.GetValueOrDefault(itemId) is { } children && children.Any(OwnsBase!);
}
