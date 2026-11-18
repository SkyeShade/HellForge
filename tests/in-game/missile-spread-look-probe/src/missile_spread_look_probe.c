/* Missile spread look probe (in-game test only, never shipped; tests/in-game/missile-spread-look.json). Settles which way
 * the spread turns a missile. The AR-23 Liberator (dev11's primary) fires the MLS-4X Commando's missile (raw stats on
 * its ProjectileWeapon +0x28, with the Commando's package held), and the stats give that missile a veer of 45 degrees
 * at direction 0 and a curve of 90 degrees a second about the curve's first axis only (curve_pitch). The probe follows
 * the first missiles in the SeekingMissile manager (rows of 0xC0 at +0x48: +0x24 age, +0x38 rotation, +0x88 the spread
 * flag; instances of 0x38 at +0x50: +0x24 the launch rotation) and logs their heading against the launch frame (local Y
 * forward, X right, Z up): "up" and "side" angles at ages around 0.03, 0.3, 0.5, 0.65 and 0.8 s (they hit before 1.2 s),
 * then a verdict for direction 0 (up, down or a side) and for the curve's axis (up plane: pitch; sideways: yaw; none:
 * roll). The primary is the player's, so the AR-23 Liberator is dev11's loadout; another install needs its own raw edit.
 * "PROBE <check> ok" or "PROBE <check> FAIL <why>" per check. */
#define WIN32_LEAN_AND_MEAN
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "hellforge_mod.h"
#include "hellforge_components.h"
#include "hellforge_globals.h"

static HfMod* g_mod;
static const HfApi* hf;
#define LOG(...) hf->log(g_mod, __VA_ARGS__)

#define COMMANDO_PACKAGE 0xC0052E5B38E18C33ull /* packages/generated/loadout/laser_guided_missile_launcher */
#define COMMANDO_MISSILE 0x527470C9D5F2B3C7ull /* its missile entity (SeekingMissile record 10) */
#define TRACKED 3

typedef struct { float x, y, z; } V3;
typedef struct Tracked
{
    uintptr_t id;          /* the entity data pointer the manager lists (+0x38), so a moved row is still found */
    V3 forward, right, up; /* the launch frame */
    int sample;            /* the next age mark */
    float up_at[5], side_at[5];
    int done;
} Tracked;
static Tracked g_tracked[TRACKED];
static int g_count, g_package, g_armed, g_verdicts;
static const float kMarks[5] = {0.03f, 0.3f, 0.5f, 0.65f, 0.8f};

static uintptr_t Pointer(uintptr_t at) { uintptr_t v = 0; return at != 0 && hf->read(at, &v, 8) == 0 ? v : 0; }
static int Floats(uintptr_t at, float* out, int n) { return at != 0 && hf->read(at, out, (size_t)n * 4) == 0; }

static V3 Rotate(const float q[4], V3 v)
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    V3 t = {2 * (y * v.z - z * v.y), 2 * (z * v.x - x * v.z), 2 * (x * v.y - y * v.x)};
    return (V3){v.x + w * t.x + (y * t.z - z * t.y), v.y + w * t.y + (z * t.x - x * t.z), v.z + w * t.z + (x * t.y - y * t.x)};
}
static float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float Deg(float r) { return r * 57.29578f; }

/* The primary the player holds (the stats' raw edits make the AR-23 Liberator fire the Commando's missile, before its
 * entity is made: the field applies on respawn); Tick checks it. */
static uint64_t g_primary;
static void Weapon(HfMod* mod, const HfWeaponEvent* event, void* user)
{
    (void)mod; (void)user;
    if (event->selection == 1 && event->resource != 0) g_primary = event->resource;
}

static void Arm(void)
{
    const HfProjectileWeapon* weapon = hf_record(g_mod, g_primary, HfProjectileWeapon);
    g_armed = 1;
    if (weapon == NULL) LOG("PROBE primary FAIL no ProjectileWeapon record for %016llx", (unsigned long long)g_primary);
    else if (weapon->projectile_entity != COMMANDO_MISSILE)
        LOG("PROBE primary FAIL %016llx fires %016llx", (unsigned long long)g_primary, (unsigned long long)weapon->projectile_entity);
    else LOG("PROBE primary ok: %016llx fires %016llx", (unsigned long long)g_primary, (unsigned long long)weapon->projectile_entity);
}

static void Verdict(const Tracked* t, int n)
{
    /* the veer, at its end (0.5 s): direction 0 should be straight up */
    float up = t->up_at[2], side = t->side_at[2];
    const char* way = fabsf(up) < 10 && fabsf(side) < 10 ? "none" : fabsf(side) > fabsf(up) ? (side > 0 ? "right" : "left") : (up > 0 ? "up" : "down");
    if (!strcmp(way, "up")) LOG("PROBE veer direction 0 ok: up (missile %d: up %.1f, side %.1f degrees at 0.5 s)", n, up, side);
    else LOG("PROBE veer direction 0 FAIL it went %s (missile %d: up %.1f, side %.1f degrees at 0.5 s)", way, n, up, side);
    /* the curve, from 0.5 to 0.8 s: pitch keeps it in the up plane */
    float dup = t->up_at[4] - t->up_at[2], dside = t->side_at[4] - t->side_at[2];
    const char* axis = fabsf(dup) < 5 && fabsf(dside) < 5 ? "roll (heading unchanged)" : fabsf(dup) > 2 * fabsf(dside) ? "pitch" : fabsf(dside) > 2 * fabsf(dup) ? "yaw" : "mixed";
    if (!strcmp(axis, "pitch")) LOG("PROBE curve first axis ok: pitch (missile %d: up %+.1f, side %+.1f degrees from 0.5 to 0.8 s)", n, dup, dside);
    else LOG("PROBE curve first axis FAIL it is %s (missile %d: up %+.1f, side %+.1f degrees from 0.5 to 0.8 s)", axis, n, dup, dside);
}

static void Follow(void)
{
    HfSeekingMissileManager* manager = hf_global_SeekingMissileManager(g_mod);
    if (manager == NULL) return;
    uintptr_t m = (uintptr_t)manager, ids = Pointer(m + 0x38), rows = Pointer(m + 0x48), instances = Pointer(m + 0x50);
    uint32_t count = 0;
    if (ids == 0 || rows == 0 || instances == 0 || hf->read(m + 0xC, &count, 4)) return;
    for (uint32_t i = 0; i < count && i < 256; i++)
    {
        uintptr_t id = Pointer(ids + (uintptr_t)i * 8), row = rows + (uintptr_t)i * 0xC0;
        Tracked* t = NULL;
        for (int k = 0; k < g_count; k++) if (g_tracked[k].id == id) t = &g_tracked[k];
        float age = 0, rot[4], launch[4];
        uint8_t flag = 0;
        if (!Floats(row + 0x24, &age, 1) || !Floats(row + 0x38, rot, 4) || hf->read(row + 0x88, &flag, 1)) continue;
        if (t == NULL)
        {
            if (g_count >= TRACKED || age > 0.1f || !Floats(instances + (uintptr_t)i * 0x38 + 0x24, launch, 4)) continue;
            t = &g_tracked[g_count++];
            memset(t, 0, sizeof *t);
            t->id = id;
            t->forward = Rotate(launch, (V3){0, 1, 0});
            t->right = Rotate(launch, (V3){1, 0, 0});
            t->up = Rotate(launch, (V3){0, 0, 1});
            LOG("PROBE missile %d seen at age %.3f, spread flag %u, launch forward (%.2f %.2f %.2f), launch up (%.2f %.2f %.2f)",
                g_count - 1, age, flag, t->forward.x, t->forward.y, t->forward.z, t->up.x, t->up.y, t->up.z);
        }
        if (t->done || t->sample >= 5 || age < kMarks[t->sample]) continue;
        V3 f = Rotate(rot, (V3){0, 1, 0});
        float up = Deg(atan2f(Dot(f, t->up), Dot(f, t->forward))), side = Deg(atan2f(Dot(f, t->right), Dot(f, t->forward)));
        t->up_at[t->sample] = up;
        t->side_at[t->sample] = side;
        LOG("PROBE missile %d at age %.3f: up %.1f, side %.1f degrees off its launch line (world forward %.2f %.2f %.2f)",
            (int)(t - g_tracked), age, up, side, f.x, f.y, f.z);
        if (++t->sample == 5) { t->done = 1; if (g_verdicts++ == 0) Verdict(t, (int)(t - g_tracked)); }
    }
}

static void Tick(HfMod* mod, float dt, void* user)
{
    (void)mod; (void)dt; (void)user;
    uint32_t state = hf->game_state(g_mod);
    if (!g_package && (state == HF_STATE_SHIP || state == HF_STATE_MISSION))
    {
        int s = hf->load_package(g_mod, COMMANDO_PACKAGE);
        if (s == HF_PACKAGE_RESIDENT) { g_package = 1; LOG("PROBE package ok: the Commando's package is resident"); }
        else if (s == 0) { g_package = -1; LOG("PROBE package FAIL refused"); }
    }
    if (state == HF_STATE_MISSION && g_package == 1 && !g_armed && g_primary != 0) Arm();
    if (state == HF_STATE_MISSION) Follow();
}

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* api, HfMod* mod)
{
    hf = api;
    g_mod = mod;
    if (api->version < 16) { api->log(mod, "PROBE sdk FAIL the loader is SDK %u", api->version); return 1; }
    LOG("PROBE sdk ok");
    LOG(hf_on_game_thread(mod, Tick, NULL) == 0 ? "PROBE game_thread ok" : "PROBE game_thread FAIL refused");
    LOG(hf_on_weapon(mod, Weapon, NULL, 0) == 0 ? "PROBE weapon ok" : "PROBE weapon FAIL refused");
    return 0;
}
