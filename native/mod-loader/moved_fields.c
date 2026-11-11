/* The fields of the game's per-player managers that a mod may move, for game.dll build 25480438 (docs/modding/hooks.md
 * "Fields a mod moved"). A mod that moves one of these arrays rewrites every instruction that addresses it, and the game
 * follows. HellForge reads each offset from one such instruction (the field's anchor) and requires its other sites to
 * agree, so it follows any mover that moved the array consistently. Pins over these displacements leave them open
 * (MfPinMatches): they are checked through the field instead. Included by hellforge_modloader.c before events.c. */

enum
{
    MF_PLAYER_COUNT, MF_PLAYER_PEERS, MF_PLAYER_LIFE, MF_PLAYER_DESCRIPTORS, MF_PLAYER_NETWORK, MF_PLAYER_CAP,
    MF_CUSTOM_COUNT, MF_CUSTOM_MAP, MF_CUSTOM_DESCRIPTORS, MF_CUSTOM_APPLIED, MF_CUSTOM_PENDING, MF_CUSTOM_CAP, MF_CUSTOM_BLOCK,
    MF_AVATAR_MAP, MF_AVATAR_DESCRIPTORS, MF_AVATAR_CONTEXT, MF_AVATAR_PLAYER_OBJECT,
    MF_SCORE_MAP, MF_SCORE_RECORDS, MF_SCORE_CAP, MF_FIELDS
};
#define MF_FAR 0x4000000 /* no manager of the game reaches 64 MB */
#define MF_MAX_SLOTS 16  /* HellForge's own per-player arrays (EVT_MAX_PLAYERS) */

/* A field: what it is, its vanilla value, its alignment and range. */
typedef struct MfField { const char* what; int32_t vanilla; uint32_t align; int32_t low, high; } MfField;
static const MfField kMfFields[MF_FIELDS] = {
    {"the player list's count", 0x84, 4, 0x10, MF_FAR}, {"the player list's peers", 0x2C8, 8, 0x10, MF_FAR},
    {"the player list's lifecycles", 0x2E0, 4, 0x10, MF_FAR}, {"the player list's descriptors", 0xE8, 8, 0x10, MF_FAR},
    {"the player list's network ids", 0x3A8, 4, 0x10, MF_FAR}, {"the player list's capacity", 4, 1, 1, MF_MAX_SLOTS},
    {"the customization records' count", 0x8E8, 4, 0x10, MF_FAR}, {"the customization map", 0x930, 8, 0x10, MF_FAR},
    {"the customization descriptors", 0x948, 8, 0x10, MF_FAR}, {"the applied customization records", 0x96C, 4, 0x10, MF_FAR},
    {"the pending customization records", 0xA7C, 4, 0x10, MF_FAR}, {"the customization records' capacity", 4, 1, 1, MF_MAX_SLOTS},
    {"the customization manager's place", 0xE9FC98, 8, INT32_MIN, INT32_MAX}, {"the avatar map", 0xF8, 8, 0x10, MF_FAR},
    {"the avatar descriptors", 0x110, 8, 0x10, MF_FAR}, {"the avatars' action contexts", 0x53E170, 8, 0x10, MF_FAR},
    {"the avatars' player objects", 0x5478A4, 4, 0x10, MF_FAR}, {"the score map", 0x37D58, 8, 0x10, MF_FAR},
    {"the score records", 0x37D98, 8, 0x10, MF_FAR}, {"the score records' capacity", 4, 1, 1, MF_MAX_SLOTS},
};

/* An instruction that addresses a field: the code before its value (pinned: the opcode, registers, scale and the
 * stride's multiply where it comes first) and the value's width (4 a displacement, 1 a capacity's imm8). The value is
 * the field plus `add`. Each field's first site is its anchor. */
typedef struct MfSite { uint8_t field, size, width; uint32_t rva; int32_t add; uint8_t code[14]; } MfSite;
#define MF_SITE(field, rva, width, add, ...) {field, sizeof((uint8_t[]){__VA_ARGS__}), width, rva, add, {__VA_ARGS__}}
static const MfSite kMfSites[] = {
    /* the player manager [game + 0x3326468]: its local player lookup 0x62C710 and its add-a-player 0x540850/0x540920 */
    MF_SITE(MF_PLAYER_COUNT, 0x62C721u, 4, 0, 0x41, 0x8b, 0xc0, 0x44, 0x8b, 0x91),
    MF_SITE(MF_PLAYER_PEERS, 0x5409CAu, 4, 0, 0x48, 0x6b, 0xc2, 0x38, 0x0f, 0x57, 0xc0, 0x0f, 0x11, 0x84, 0x18),
    MF_SITE(MF_PLAYER_PEERS, 0x62C72Bu, 4, 0, 0x45, 0x85, 0xd2, 0x74, 0x17, 0x4c, 0x8d, 0x89),
    MF_SITE(MF_PLAYER_LIFE, 0x6094D7u, 4, 0, 0x48, 0x6b, 0xc2, 0x38, 0x42, 0x83, 0xbc, 0x20),
    MF_SITE(MF_PLAYER_DESCRIPTORS, 0x62C751u, 4, 0, 0x48, 0x8b, 0x8c, 0xc1),
    MF_SITE(MF_PLAYER_DESCRIPTORS, 0x5408FBu, 4, 0, 0x48, 0x89, 0x94, 0xcb),
    MF_SITE(MF_PLAYER_NETWORK, 0xACC665u, 4, 0, 0x48, 0xc1, 0xe0, 0x05, 0x4d, 0x03, 0xe6, 0x42, 0x8b, 0x94, 0x30),
    MF_SITE(MF_PLAYER_CAP, 0x54086Au, 1, 0, 0x41, 0x8d, 0x40, 0x01, 0x83, 0xf8), MF_SITE(MF_PLAYER_CAP, 0x54093Au, 1, 0, 0xff, 0xc0, 0x83, 0xf8),
    MF_SITE(MF_PLAYER_CAP, 0x60CA9Au, 1, 0, 0xff, 0xc0, 0x83, 0xf8), MF_SITE(MF_PLAYER_CAP, 0x60CBB7u, 1, 0, 0x41, 0x8d, 0x40, 0x01, 0x83, 0xf8),
    /* the customization manager [game + 0x33264F8]: its add-a-record 0x540660, its writers 0x874530 and the attribute
     * reader 0x11D9E00 (the passive and perk pins) */
    MF_SITE(MF_CUSTOM_COUNT, 0x54068Cu, 4, 0, 0x80, 0x3a, 0x00, 0x8b, 0x83),
    MF_SITE(MF_CUSTOM_MAP, 0x540719u, 4, 0, 0x48, 0x8d, 0x8b), MF_SITE(MF_CUSTOM_MAP, 0x11D9F1Du, 4, 0, 0x4d, 0x8b, 0xb1),
    MF_SITE(MF_CUSTOM_MAP, 0x11D9EFFu, 4, 8, 0x45, 0x8b, 0x91), MF_SITE(MF_CUSTOM_MAP, 0x11D9F24u, 4, 0xC, 0x41, 0x8b, 0xa9),
    MF_SITE(MF_CUSTOM_MAP, 0x11D9F06u, 4, 0x10, 0x41, 0x8b, 0xb1),
    MF_SITE(MF_CUSTOM_DESCRIPTORS, 0x540720u, 4, 0, 0x48, 0x89, 0xbc, 0xd3), MF_SITE(MF_CUSTOM_DESCRIPTORS, 0x87455Du, 4, 0, 0x49, 0x8b, 0x84, 0xc8),
    MF_SITE(MF_CUSTOM_APPLIED, 0x5406C8u, 4, 0, 0x48, 0x6b, 0xc2, 0x44, 0x0f, 0x57, 0xc0, 0x0f, 0x11, 0x84, 0x18),
    MF_SITE(MF_CUSTOM_APPLIED, 0x54060Eu, 4, 0, 0x0f, 0x11, 0x84, 0x18), MF_SITE(MF_CUSTOM_APPLIED, 0x874565u, 4, 0, 0x4d, 0x8d, 0xb8),
    MF_SITE(MF_CUSTOM_APPLIED, 0x874DFCu, 4, 0x38, 0x48, 0x8d, 0x9f), MF_SITE(MF_CUSTOM_APPLIED, 0x8781F7u, 4, 0xC, 0x42, 0x8b, 0x94, 0x17),
    MF_SITE(MF_CUSTOM_APPLIED, 0x11D9F81u, 4, 0x3C, 0x43, 0x8b, 0x84, 0x0b), MF_SITE(MF_CUSTOM_APPLIED, 0x11D9FDAu, 4, 0x38, 0x43, 0x8b, 0x84, 0x0b),
    MF_SITE(MF_CUSTOM_PENDING, 0x874556u, 4, 0, 0x4d, 0x8d, 0xa0), MF_SITE(MF_CUSTOM_PENDING, 0x8761AFu, 4, 0xC, 0x44, 0x89, 0x84, 0x3e),
    MF_SITE(MF_CUSTOM_PENDING, 0x11DA410u, 4, 0xC, 0x43, 0x8b, 0x84, 0x0f), MF_SITE(MF_CUSTOM_PENDING, 0x11DA609u, 4, 4, 0x43, 0x8b, 0x84, 0x0f),
    MF_SITE(MF_CUSTOM_CAP, 0x5405E7u, 1, 0, 0x41, 0x8d, 0x40, 0x01, 0x83, 0xf8), MF_SITE(MF_CUSTOM_CAP, 0x54067Au, 1, 0, 0xff, 0xc0, 0x83, 0xf8),
    MF_SITE(MF_CUSTOM_CAP, 0x87BD3Au, 1, 0, 0xff, 0xc0, 0x83, 0xf8), MF_SITE(MF_CUSTOM_CAP, 0x87BE14u, 1, 0, 0x41, 0x8d, 0x40, 0x01, 0x83, 0xf8),
    MF_SITE(MF_CUSTOM_BLOCK, 0x568EBAu, 4, 0, 0x48, 0x8d, 0x83), /* its place in the component world: only its global is read */
    /* the avatar manager [game + 0x3326D20]: its ability use 0x97EE60, 0x11B0000, the busy check 0xA431A8 and 0xA432F8 */
    MF_SITE(MF_AVATAR_MAP, 0x97EE6Fu, 4, 0, 0x49, 0x8d, 0x8e), MF_SITE(MF_AVATAR_MAP, 0x11D9E41u, 4, 0, 0x4d, 0x8b, 0xb3),
    MF_SITE(MF_AVATAR_DESCRIPTORS, 0x289F97u, 4, 0, 0x48, 0x8b, 0x8c, 0xda),
    MF_SITE(MF_AVATAR_CONTEXT, 0x11B008Du, 4, 0, 0x48, 0x69, 0xc1, 0x38, 0x12, 0x00, 0x00, 0x48, 0x8d, 0x8d),
    MF_SITE(MF_AVATAR_CONTEXT, 0x97EE8Eu, 4, 0, 0x49, 0x8d, 0xbe),
    MF_SITE(MF_AVATAR_CONTEXT, 0xA431AAu, 4, 0x718, 0x48, 0x69, 0xc1, 0x38, 0x12, 0x00, 0x00, 0x42, 0x8b, 0x84, 0x10),
    MF_SITE(MF_AVATAR_CONTEXT, 0xA4330Bu, 4, 0x718, 0x4a, 0x85, 0x8c, 0x18), MF_SITE(MF_AVATAR_CONTEXT, 0xA43313u, 4, 0x710, 0x4a, 0x8b, 0x94, 0x18),
    MF_SITE(MF_AVATAR_PLAYER_OBJECT, 0x11D9ED1u, 4, 0, 0x48, 0x6b, 0xc1, 0x78, 0x48, 0x8d, 0x4c, 0x24, 0x50, 0x42, 0x8b, 0x94, 0x18),
    /* the score manager [game + 0x3326AE0]: its add-a-record 0x53E130 and the stats poll's lookup 0x62C764 */
    MF_SITE(MF_SCORE_MAP, 0x53E186u, 4, 0, 0x48, 0x8d, 0x8b), MF_SITE(MF_SCORE_MAP, 0x62C78Cu, 4, 0, 0x49, 0x8b, 0xbb),
    MF_SITE(MF_SCORE_RECORDS, 0x53E16Au, 4, 0, 0x48, 0x8d, 0x04, 0x92, 0x0f, 0x11, 0x84, 0xc3),
    MF_SITE(MF_SCORE_CAP, 0x53E147u, 1, 0, 0x41, 0x8d, 0x40, 0x01, 0x83, 0xf8),
};
#define MF_SITES (sizeof kMfSites / sizeof kMfSites[0])
static volatile LONG g_mfNoted[MF_FIELDS];

/* A site's value less its add, when the code before it reads as reviewed; 0 not. */
static int MfSiteValue(uintptr_t game, const MfSite* s, int32_t* value)
{
    uint8_t code[20];
    if (SafeRead(game + s->rva, code, (size_t)s->size + s->width) || memcmp(code, s->code, s->size)) return 0;
    int32_t raw = 0;
    if (s->width == 1) raw = code[s->size];
    else memcpy(&raw, code + s->size, 4);
    *value = (int32_t)((int64_t)raw - s->add);
    return 1;
}

static int MfRefuse(int field, const MfSite* s, const char* why, int32_t value)
{
    if (InterlockedExchange(&g_mfNoted[field], 1) == 0)
        Note("moved fields: %s %s at game.dll +0x%X (0x%X); what reads it is off", kMfFields[field].what, why, s->rva, (uint32_t)value);
    return 0;
}

/* A field's offset (or capacity): its anchor's value, when every site of the field reads as reviewed and agrees and the
 * value is plausible; 0 (noted once) when not. */
static int MfFieldOf(uintptr_t game, int field, int32_t* out)
{
    const MfField* f = &kMfFields[field];
    const MfSite* anchor = NULL;
    int32_t value = 0, other;
    for (size_t i = 0; i < MF_SITES; i++)
    {
        const MfSite* s = &kMfSites[i];
        if (s->field != field) continue;
        if (!MfSiteValue(game, s, &other)) return MfRefuse(field, s, "is not the reviewed instruction", 0);
        if (anchor == NULL) { anchor = s; value = other; }
        else if (other != value) return MfRefuse(field, s, "disagrees with its anchor", other + s->add);
    }
    if (anchor == NULL) return 0;
    if (value < f->low || value > f->high || (uint32_t)value % f->align) return MfRefuse(field, anchor, "is not a plausible offset", value);
    *out = value;
    return 1;
}
static uint32_t MfOffset(uintptr_t game, int field, int* ok)
{
    int32_t v = 0;
    if (!MfFieldOf(game, field, &v)) *ok = 0;
    return (uint32_t)v;
}

/* Whether code at game + rva reads as `bytes`, the moved fields' values in it left out and checked through their field
 * instead (the passive, perk, event and action pins). */
static int MfPinMatches(uintptr_t game, uint32_t rva, const uint8_t* bytes, size_t size)
{
    uint8_t got[256];
    if (size > sizeof got || SafeRead(game + rva, got, size)) return 0;
    for (size_t i = 0; i < MF_SITES; i++)
    {
        const MfSite* s = &kMfSites[i];
        uint32_t from = s->rva + s->size, to = from + s->width;
        if (to <= rva || from >= rva + size) continue;
        int32_t value;
        if (!MfFieldOf(game, s->field, &value)) return 0;
        for (uint32_t at = from; at < to; at++) if (at >= rva && at < rva + size) got[at - rva] = bytes[at - rva];
    }
    return !memcmp(got, bytes, size);
}
