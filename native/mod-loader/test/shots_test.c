/* Per-player projectile swaps (shots.c): the lobby's rules from the pipe, and the type each shot fires by who fires it:
 * a friend's swap on their gun, this machine's own swap only on its player's, a mode's type left alone, shots by no
 * player and rules not ready yet untouched. Included by stats_test.c. */

static uint32_t ShotFakeWielder(uint32_t weapon) { return weapon == 11 ? 100 : weapon == 22 ? 200 : weapon == 33 ? 999 : 0; }

static void ShotsFixture(void)
{
    char line[512], reply[256];
    const char* why;
    g_shotLocalCount = 0; /* earlier fixtures' projectile writes noted this machine's swaps */
    /* the pipe: rules replace the last, "-" none, malformed refused */
    strcpy(line, "shots\t1\t110000100000022:968211C0033DCE64=142,110000100000022:A955C4EA6F6D4203=263");
    LiveRequest(line, reply, sizeof reply);
    assert(!strcmp(reply, "1\tok\tnow") && g_shotRuleCount == 2 && g_shotsActive && g_shotRules[0].peer == 0x110000100000022ull &&
        g_shotRules[0].resource == 0x968211C0033DCE64ull && g_shotRules[0].type == 142 && !g_shotRules[0].ready);
    strcpy(line, "shots\t2\tnot-a-rule"); LiveRequest(line, reply, sizeof reply); assert(!strncmp(reply, "2\trefused\t", 10) && g_shotRuleCount == 2);
    assert(!ShotsSetRules("1:2=0", &why) && !ShotsSetRules("1:2=x", &why) && g_shotRuleCount == 2);
    strcpy(line, "shots\t3\t-"); LiveRequest(line, reply, sizeof reply); assert(!strcmp(reply, "3\tok\tnow") && g_shotRuleCount == 0 && !g_shotsActive);

    /* who fires what: this machine's player (avatar 100, peer L) and a friend (avatar 200, peer F) */
    const uint64_t L = 0x110000100000011ull, F = 0x110000100000022ull, liberator = 0x968211C0033DCE64ull, onetwo = 0xA955C4EA6F6D4203ull;
    g_shotWielder = ShotFakeWielder;
    g_shotPlayers[0] = (ShotPlayer){L, 100, 1}; g_shotPlayers[1] = (ShotPlayer){F, 200, 0}; g_shotPlayerCount = 2;
    uint8_t mine[16] = {0}, theirs[16] = {0}, turret[16] = {0}, theirTwo[16] = {0};
    uint32_t w;
    memcpy(mine, &liberator, 8); w = 11; memcpy(mine + 8, &w, 4);
    memcpy(theirs, &liberator, 8); w = 22; memcpy(theirs + 8, &w, 4);
    memcpy(turret, &liberator, 8); w = 33; memcpy(turret + 8, &w, 4);
    memcpy(theirTwo, &onetwo, 8); w = 22; memcpy(theirTwo + 8, &w, 4);
    assert(ShotType(334, theirs) == 334); /* no rules: nothing changes (solo, or a host sharing its stats) */
    char rules[128];
    snprintf(rules, sizeof rules, "%llx:%llx=142,%llx:%llx=263", (unsigned long long)F, (unsigned long long)liberator, (unsigned long long)F,
        (unsigned long long)onetwo);
    assert(ShotsSetRules(rules, &why));
    assert(ShotType(334, theirs) == 334); /* not ready yet: its row and packages unchecked */
    g_shotRules[0].ready = g_shotRules[1].ready = 1;
    g_shotRules[0].native = 334; g_shotRules[1].native = 330;
    assert(ShotType(334, theirs) == 142);   /* the friend's Liberator fires the friend's swap */
    assert(ShotType(334, mine) == 334);     /* this machine's player's own: the game's choice */
    assert(ShotType(263, theirs) == 263);   /* not the weapon's normal round (a mode's): left alone */
    assert(ShotType(334, turret) == 334);   /* no player's: left alone */
    assert(g_shotUnowned == (LONG64)liberator && g_shotUnownedWielder == 999); /* and noted for the log */
    assert(ShotType(330, theirTwo) == 263);
    /* this machine swapped the Liberator to 142 itself: a friend without a swap fires the Liberator's own round here */
    snprintf(rules, sizeof rules, "%llx:%llx=263", (unsigned long long)F, (unsigned long long)onetwo);
    assert(ShotsSetRules(rules, &why));
    g_shotRules[0].ready = 1; g_shotRules[0].native = 330;
    ShotsNoteLocal(liberator, SHOT_NORMAL, 334, 142);
    assert(ShotType(142, theirs) == 334 && ShotType(142, mine) == 142);
    ShotsNoteLocal(liberator, SHOT_NORMAL, 142, 150); /* changed again: its base stays the first */
    assert(g_shotLocalCount == 1 && g_shotLocal[0].base == 334 && g_shotLocal[0].now == 150 && ShotType(150, theirs) == 334);

    /* a round from the ammunition (stock 276, "/276"): a magazine chambers this machine's type, so a friend's copy fires
     * this machine's swap unless told; this machine's own swap is its own rule (the loader does not tie the delta to the
     * weapon) */
    g_shotLocalCount = 0;
    snprintf(rules, sizeof rules, "%llx:%llx=142/276,%llx:%llx=150/276", (unsigned long long)F, (unsigned long long)liberator, (unsigned long long)L,
        (unsigned long long)liberator);
    assert(ShotsSetRules(rules, &why) && g_shotRules[0].stock == 276 && g_shotRules[0].native == 276 && !g_shotRules[0].rounds);
    g_shotRules[0].ready = g_shotRules[1].ready = 1;
    assert(ShotType(150, theirs) == 142 && ShotType(276, theirs) == 142); /* the friend's swap, over this machine's or the stock */
    assert(ShotType(150, mine) == 150 && ShotType(99, theirs) == 99);    /* mine; another ammunition's type left alone */
    snprintf(rules, sizeof rules, "%llx:%llx=150/276", (unsigned long long)L, (unsigned long long)liberator);
    assert(ShotsSetRules(rules, &why));
    g_shotRules[0].ready = 1;
    assert(ShotType(150, theirs) == 276 && ShotType(150, mine) == 150);  /* a friend without one: the stock round */
    /* one weapon, a swap per round, each by its stock type (a charge weapon's partial and full charge); this machine's own
     * swap of the partial round stands for that round on a friend's gun */
    snprintf(rules, sizeof rules, "%llx:%llx=500/129,%llx:%llx=600/296,%llx:%llx=700/129", (unsigned long long)F, (unsigned long long)liberator,
        (unsigned long long)F, (unsigned long long)liberator, (unsigned long long)L, (unsigned long long)liberator);
    assert(ShotsSetRules(rules, &why));
    g_shotRules[0].ready = g_shotRules[1].ready = g_shotRules[2].ready = 1;
    assert(ShotType(129, theirs) == 500 && ShotType(296, theirs) == 600 && ShotType(999, theirs) == 999);
    assert(ShotType(700, theirs) == 500 && ShotType(700, mine) == 700 && ShotType(296, mine) == 296);
    /* a record this machine swapped: a friend's rule for it names the stock round, read here as the swap */
    g_shotLocalCount = 0;
    ShotsNoteLocal(onetwo, SHOT_NORMAL, 330, 142);
    snprintf(rules, sizeof rules, "%llx:%llx=263", (unsigned long long)F, (unsigned long long)onetwo);
    assert(ShotsSetRules(rules, &why));
    g_shotRules[0].ready = 1; g_shotRules[0].native = 330; /* as the tick sets it: the record reads 142, this machine's swap of 330 */
    assert(ShotType(142, theirTwo) == 263 && ShotType(330, theirTwo) == 263);
    g_shotLocalCount = 0;
    /* a rounds feed's ("/r": WeaponRounds +64 is its normal round); malformed suffixes refused */
    assert(ShotsSetRules("1:2=142/r", &why) && g_shotRules[0].rounds && g_shotRules[0].stock == 0);
    assert(!ShotsSetRules("1:2=142/", &why) && !ShotsSetRules("1:2=142/0", &why) && !ShotsSetRules("1:2=142/rx", &why));
    /* a programmable round's ("/f": ProjectileWeapon +0x240, only while its function is on): its own slot, apart from the
     * normal round's; no programmable round keeps the shot's type */
    g_shotLocalCount = 0;
    snprintf(rules, sizeof rules, "%llx:%llx=142/f", (unsigned long long)F, (unsigned long long)liberator);
    assert(ShotsSetRules(rules, &why) && g_shotRules[0].slot == SHOT_FUNCTION && !g_shotRules[0].rounds && !g_shotRules[0].stock);
    g_shotRules[0].ready = 1; g_shotRules[0].native = 36;
    assert(ShotFunctionType(36, theirs) == 142 && ShotFunctionType(36, mine) == 36 && ShotFunctionType(0, theirs) == 0);
    assert(ShotType(36, theirs) == 36);                                   /* not the normal round's rule */
    ShotsNoteLocal(liberator, SHOT_FUNCTION, 36, 150);                    /* this machine's own programmable swap */
    assert(g_shotLocalCount == 1 && ShotFunctionType(150, mine) == 150 && ShotFunctionType(150, theirs) == 142 && ShotType(150, theirs) == 150);
    assert(ShotsSlotOf("ProjectileWeaponComponentData", 576) == SHOT_FUNCTION && ShotsSlotOf("ProjectileWeaponComponentData", 0) == SHOT_NORMAL &&
        ShotsSlotOf("WeaponRoundsComponentData", 64) == SHOT_NORMAL && ShotsSlotOf("WeaponRoundsComponentData", 68) < 0);

    assert(ShotsSetRules("-", &why));
    g_shotLocalCount = 0; g_shotPlayerCount = 0;
    g_shotWielder = ShotWielderOf;
}
