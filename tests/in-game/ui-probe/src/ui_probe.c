/* UI probe (in-game test only, never shipped; tests/in-game/ui19.json): SDK 19's screens in the game's own UI. Opens
 * ui/panel.xaml (the game's styles) as an overlay, sets its text every second for a minute, listens to its button,
 * and checks the refusals. One PROBE line per check in its log. */
#include <stdio.h>
#include "hellforge_mod.h"

static int g_screen;
static float g_clock;
static int g_seconds;

static void Clicked(HfMod* mod, int screen, const char* element, void* user)
{
    (void)user;
    HF_API(mod)->log(mod, "PROBE click %s on screen %d", element, screen);
    HF_API(mod)->ui_set(mod, screen, "Body", "Text", "clicked");
}

static void Tick(HfMod* mod, float dt, void* user)
{
    (void)user;
    const HfApi* hf = HF_API(mod);
    if ((g_clock += dt) < 1.0f || g_seconds >= 60) return;
    g_clock = 0;
    char text[64];
    snprintf(text, sizeof text, "updated by ui_set: %d s", ++g_seconds);
    int queued = hf->ui_set(mod, g_screen, "Body", "Text", text) == 0;
    if (g_seconds <= 2) hf->log(mod, "PROBE set %d %s", g_seconds, queued ? "ok" : "FAIL");
    if (g_seconds == 3) hf->ui_set(mod, g_screen, "Title", "Foreground", "#FFE900");
    if (g_seconds == 4) hf->ui_set(mod, g_screen, "Nobody", "Text", "x"); /* logged: no element of that name */
    if (g_seconds == 5) hf->ui_set(mod, g_screen, "Title", "NoSuchProperty", "x"); /* logged: no such property */
}

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* hf, HfMod* mod)
{
    if (hf->version < 19 || hf->size < offsetof(HfApi, ui_close) + sizeof(void*)) { hf->log(mod, "PROBE sdk FAIL %u", hf->version); return 1; }
    hf->log(mod, "PROBE sdk ok");
    g_screen = hf->ui_open(mod, "ui/panel.xaml", HF_UI_OVERLAY);
    hf->log(mod, "PROBE open %s %d", g_screen ? "ok" : "FAIL", g_screen);
    hf->log(mod, "PROBE on %s", hf->ui_on(mod, g_screen, "Ok", Clicked, NULL) == 0 ? "ok" : "FAIL");
    int refused = hf->ui_open(mod, "../outside.xaml", HF_UI_OVERLAY) == 0 && hf->ui_open(mod, "ui/missing.xaml", HF_UI_OVERLAY) == 0 &&
        hf->ui_open(mod, "<Grid/>", 7) == 0 && hf->ui_set(mod, 999, "Body", "Text", "x") != 0;
    hf->log(mod, "PROBE refusals %s", refused ? "ok" : "FAIL");
    return hf->on_game_thread(mod, Tick, NULL) == 1 ? 0 : 1;
}
