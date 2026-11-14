/* Fields a vanilla attachment's entity deltas lack, added by HellForge (stats edits "EntityDelta" at offset 0): the
 * LAS-12 Sai's Improved Cooldown heatsink, whose deltas change the cooling rate but not the heatsink counts, so it took
 * the weapon's own. Included by attachments.c. An attachment's deltas are its key's settings entry: component settings
 * {component, first delta, count} and deltas {record offset, size, data offset} the game copies onto the weapon's records
 * when it is assembled (the Improved Heatcapacity's carry these very members). When an edit sets one (0 keeps the
 * weapon's own), the grown storage (AttGrow) gives the key a settings entry of its own: a copy of its components and
 * deltas with the added member, its value in the data. A changed value is written in place; one set or cleared makes the
 * storage again. No game function of its own: attachments.c's pins cover the lookups it relies on. Every player runs the
 * same stats, so every machine adds the same members. */

#define ATT_EXTRA_MAX 64
#define ATT_HEAT 266u

/* The members an edit may add: WeaponHeat's starting heatsinks, heatsinks from supply and spare heatsinks (u32). */
static const uint32_t kAttExtraFields[][2] = {{ATT_HEAT, 84}, {ATT_HEAT, 88}, {ATT_HEAT, 92}};

typedef struct AttAddedMember
{
    uint64_t key;              /* the attachment's delta key (its resource) */
    uint32_t component, member;
    volatile uint32_t value;   /* the stats edit's; 0: not added */
    uint32_t added;            /* in the grown storage: its data at live */
    uint8_t* live;
} AttAddedMember;
static AttAddedMember g_attExtras[ATT_EXTRA_MAX];
static int g_attExtraCount;

/* The value an edit of the member writes (found or made), or NULL: not one kAttExtraFields allows, or no room. */
static uint32_t* AttExtraSlot(uint64_t key, uint32_t component, uint32_t member)
{
    int allowed = 0;
    for (size_t i = 0; i < sizeof kAttExtraFields / sizeof kAttExtraFields[0]; i++)
        allowed |= kAttExtraFields[i][0] == component && kAttExtraFields[i][1] == member;
    if (!allowed || key == 0) return NULL;
    for (int i = 0; i < g_attExtraCount; i++)
        if (g_attExtras[i].key == key && g_attExtras[i].component == component && g_attExtras[i].member == member) return (uint32_t*)&g_attExtras[i].value;
    if (g_attExtraCount == ATT_EXTRA_MAX) return NULL;
    AttAddedMember* x = &g_attExtras[g_attExtraCount];
    memset(x, 0, sizeof *x);
    x->key = key; x->component = component; x->member = member;
    MemoryBarrier();
    g_attExtraCount++;
    return (uint32_t*)&x->value;
}

/* Whether the key's settings in s set the member already (then nothing is added: the attachment's own value stays). */
static int AttExtraHas(const uint8_t* s, const uint32_t* setting, uint32_t component, uint32_t member)
{
    const uint32_t* comps = *(uint32_t* const*)(s + 0x20);
    const uint32_t* deltas = *(uint32_t* const*)(s + 0x30);
    for (uint32_t c = 0; c < setting[0]; c++)
    {
        const uint32_t* comp = comps + 3u * (setting[1] + c);
        for (uint32_t d = 0; comp[0] == component && d < comp[2]; d++) if (deltas[3u * (comp[1] + d)] == member) return 1;
    }
    return 0;
}

/* Whether extra i goes into a storage made from s: set, its key in s, its member not there already. */
static int AttExtraWanted(const uint8_t* s, int i)
{
    const AttAddedMember* x = &g_attExtras[i];
    const uint32_t* setting = x->value ? AttDeltaSetting(s, x->key) : NULL;
    return setting != NULL && !AttExtraHas(s, setting, x->component, x->member);
}

/* Whether extra i is the first wanted one of its key (each key's copy is made once, with all its extras). */
static int AttExtraLeads(const uint8_t* s, int i)
{
    for (int k = 0; k < i; k++) if (g_attExtras[k].key == g_attExtras[i].key && AttExtraWanted(s, k)) return 0;
    return AttExtraWanted(s, i);
}

/* Whether the grown storage no longer matches the edits: one set or cleared since it was made. */
static int AttExtrasPending(void)
{
    for (int i = 0; i < g_attExtraCount; i++) if ((g_attExtras[i].value != 0) != (g_attExtras[i].added != 0)) return 1;
    return 0;
}

/* No storage made: each set extra counts as considered (2) until its value is cleared or set again. */
static void AttExtrasUnmade(void)
{
    for (int i = 0; i < g_attExtraCount; i++) g_attExtras[i].added = g_attExtras[i].value ? 2 : 0, g_attExtras[i].live = NULL;
}

/* A changed value of an added member, written where the grown storage holds it (the next assembly takes it). */
static void AttExtrasLive(void)
{
    for (int i = 0; i < g_attExtraCount; i++)
    {
        AttAddedMember* x = &g_attExtras[i];
        uint32_t v = x->value;
        if (x->added == 1 && v && memcmp(x->live, &v, 4)) memcpy(x->live, &v, 4);
    }
}

/* What the extras add to a storage made from s: a settings entry per key, its components (and one per component it
 * lacks), its deltas and data with the added members. */
static AttSizes AttExtrasSize(const uint8_t* s)
{
    AttSizes add = {0};
    const uint32_t* comps = *(uint32_t* const*)(s + 0x20);
    const uint32_t* deltas = *(uint32_t* const*)(s + 0x30);
    for (int i = 0; i < g_attExtraCount; i++)
    {
        if (!AttExtraLeads(s, i)) continue;
        const uint32_t* setting = AttDeltaSetting(s, g_attExtras[i].key);
        add.settings++;
        add.comps += setting[0] + 1; /* room for a component it lacks */
        for (uint32_t c = 0; c < setting[0]; c++)
        {
            const uint32_t* comp = comps + 3u * (setting[1] + c);
            add.deltas += comp[2];
            for (uint32_t d = 0; d < comp[2]; d++) add.data += deltas[3u * (comp[1] + d) + 1];
        }
        for (int k = i; k < g_attExtraCount; k++)
            if (g_attExtras[k].key == g_attExtras[i].key && AttExtraWanted(s, k)) { add.deltas++; add.data += 4; }
    }
    return add;
}

/* One key's added members of one component, as deltas after *z's last, their data written: how many. */
static uint32_t AttExtraDeltas(const uint8_t* s, uint64_t key, uint32_t component, const AttArrays* to, AttSizes* z, int from)
{
    uint32_t n = 0;
    for (int k = from; k < g_attExtraCount; k++)
    {
        AttAddedMember* x = &g_attExtras[k];
        if (x->key != key || x->component != component || !AttExtraWanted(s, k)) continue;
        uint32_t v = x->value, *nd = to->deltas + 3u * z->deltas++;
        nd[0] = x->member; nd[1] = 4; nd[2] = z->data;
        memcpy(to->data + z->data, &v, 4);
        x->live = to->data + z->data;
        z->data += 4;
        n++;
    }
    return n;
}

/* Extra i's key (it leads): a settings entry of its own after *z's, its components copied with the added members after
 * each one's own deltas (a WeaponHeat component of its own when it has none), its key pointed at it in the map. */
static void AttExtrasKey(const uint8_t* s, int i, const AttArrays* to, AttSizes* z)
{
    uint64_t key = g_attExtras[i].key;
    const uint32_t* from = AttDeltaSetting(s, key);
    const uint32_t* oldComps = *(uint32_t* const*)(s + 0x20);
    uint32_t first = z->comps, count = from[0], heat = 0;
    for (uint32_t c = 0; c < from[0]; c++)
    {
        uint32_t* nc = AttCopyComponent(s, oldComps + 3u * (from[1] + c), to, z);
        nc[2] += AttExtraDeltas(s, key, nc[0], to, z, i);
        heat |= nc[0] == ATT_HEAT;
    }
    if (!heat)
    {
        uint32_t* nc = to->comps + 3u * z->comps;
        nc[0] = ATT_HEAT; nc[1] = z->deltas;
        nc[2] = AttExtraDeltas(s, key, ATT_HEAT, to, z, i);
        if (nc[2]) { z->comps++; count++; }
    }
    to->settings[2 * z->settings] = count;
    to->settings[2 * z->settings + 1] = first;
    for (uint32_t at = (uint32_t)(key % to->slots), probe = 0; probe < to->slots; probe++, at = at + 1 == to->slots ? 0 : at + 1)
        if (*(const uint64_t*)(to->keys + 16u * at) == key) { memcpy(to->keys + 16u * at + 8, &z->settings, 4); break; }
    z->settings++;
}

/* Each key with added members, made after the copies (AttGrow). */
static void AttExtrasCopy(const uint8_t* s, const AttArrays* to, AttSizes* z)
{
    for (int i = 0; i < g_attExtraCount; i++) g_attExtras[i].added = 0, g_attExtras[i].live = NULL;
    for (int i = 0; i < g_attExtraCount; i++) if (AttExtraLeads(s, i)) AttExtrasKey(s, i, to, z);
    /* 1 added; 2 set but not added (its key is not in the storage, or sets the member itself), so not made again for it */
    for (int i = 0; i < g_attExtraCount; i++) g_attExtras[i].added = g_attExtras[i].live != NULL ? 1 : g_attExtras[i].value ? 2 : 0;
}
