/* content() after the other kinds (attachments.c PatternContent): each weapon pattern in the game, in the order mods
 * added them, as HF_KIND_WEAPON_PATTERN with its paint item, base, name, title and swatch. Attachments that are not
 * patterns and refused patterns are left out, and an index past the last answers 0. Included by stats_test.c. */

static Attachment g_pcSaved[4];

static void PcAdd(int at, HfMod* mod, const char* name, const char* title, uint32_t id, int pattern, int state, uint32_t colours)
{
    Attachment* a = &g_attachments[at];
    memset(a, 0, sizeof *a);
    a->mod = mod;
    snprintf(a->name, sizeof a->name, "%s", name);
    snprintf(a->title, sizeof a->title, "%s", title);
    a->id = id;
    a->base = 1561483157u; /* Arctic Camo */
    a->pattern = pattern;
    a->state = state;
    a->colourCount = colours;
    for (uint32_t k = 0; k < colours; k++) a->colours[k][0] = a->colours[k][1] = a->colours[k][2] = 0.25f * (float)(k + 1);
}

static void PatternContentFixture(void)
{
    static HfMod mod;
    snprintf(mod.id, sizeof mod.id, "test.patterns");
    snprintf(mod.folder, sizeof mod.folder, "test-patterns");
    int savedCount = g_attachmentCount;
    memcpy(g_pcSaved, g_attachments, sizeof g_pcSaved);
    PcAdd(0, &mod, "test.patterns:muzzle", "", 0x11u, 0, 1, 0);    /* an attachment, not a pattern */
    PcAdd(1, &mod, "test.patterns:tiger", "Tiger", 0x22u, 1, 1, 2);
    PcAdd(2, &mod, "test.patterns:refused", "Refused", 0x33u, 1, -1, 0);
    PcAdd(3, &mod, "test.patterns:plain", "", 0x44u, 1, 0, 0);       /* not in the game yet: listed too */
    g_attachmentCount = 4;

    HfContent c;
    uint32_t index = 0;
    assert(PatternContent(&index, sizeof c, &c) && c.size == sizeof c && c.kind == HF_KIND_WEAPON_PATTERN);
    assert(c.id == 0x22u && c.base == 1561483157u && strcmp(c.mod, "test.patterns") == 0 && strcmp(c.folder, "test-patterns") == 0);
    assert(strcmp(c.name, "test.patterns:tiger") == 0 && strcmp(c.title, "Tiger") == 0);
    assert(c.colour_count == 2 && c.colours != NULL && c.colours[1][0] == 0.5f);
    index = 1;
    assert(PatternContent(&index, sizeof c, &c) && c.id == 0x44u && c.title == NULL && c.colours == NULL && c.colour_count == 0);
    index = 2;
    assert(!PatternContent(&index, sizeof c, &c) && index == 0); /* the caller's index counts down past the patterns */

    memcpy(g_attachments, g_pcSaved, sizeof g_pcSaved);
    g_attachmentCount = savedCount;
}
