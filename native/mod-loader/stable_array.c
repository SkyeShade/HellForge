/* Arrays whose entries never move, grown as they fill (registry.c's weapons and entities, their texts, network types and
 * unit copies; fields.c's definition blocks). Included by hellforge_modloader.c.
 *
 * Other threads read these entries without a lock once the count that shows an entry is published (a MemoryBarrier
 * before the count: the game thread's text lookup, the apply switch, the watch loop), and fields.c holds an entry's
 * address across a nested registration. A realloc would move the entries under them. So address space for `ceiling`
 * entries is reserved once, at the first entry, and pages are committed as the array grows: an entry's address never
 * changes, and the memory used follows the entries there are. The ceiling bounds address space only (65,536 weapons
 * and entities reserve about 50 MB of it); no game structure is sized by it. Committed pages are zero, as the static
 * arrays these replace were. Writers of one array are serialised by its callers, as before. */

typedef struct StableArray { uint8_t* base; size_t stride, ceiling, committed; } StableArray;

/* Room for entries [0, count): 1, or 0 past the ceiling or out of memory (nothing changed then). */
static int StableRoom(StableArray* a, size_t count)
{
    if (count <= a->committed) return 1;
    if (count > a->ceiling || a->stride == 0) return 0;
    if (a->base == NULL)
    {
        uint8_t* base = (uint8_t*)VirtualAlloc(NULL, a->ceiling * a->stride, MEM_RESERVE, PAGE_READWRITE);
        if (base == NULL) return 0;
        a->base = base;
    }
    size_t want = a->committed ? a->committed * 2 : 64;
    if (want < count) want = count;
    if (want > a->ceiling) want = a->ceiling;
    if (VirtualAlloc(a->base, want * a->stride, MEM_COMMIT, PAGE_READWRITE) == NULL) return 0;
    a->committed = want;
    return 1;
}
