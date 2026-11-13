/* HellForge mod SDK 19: screens in the game's own UI (ui_open, ui_set, ui_on, ui_close; docs/modding/ui.md).
 *
 * A screen is XAML, parsed by the game's NoesisGUI, so the game's styles, fonts and themed controls are there: text
 * styles {StaticResource Headerstyle} and Body_Medium, colours such as BrandYellow, and stock controls (Button,
 * CheckBox, Slider...) already in the game's look. Name the elements your code talks to (x:Name). Every call may come
 * from any thread: HellForge runs it on the game thread at the next update. Included by hellforge_mod.h. */
#pragma once
#include <stdint.h>

/* ui_open kinds */
#define HF_UI_OVERLAY 0u /* over the game, on the ship and in missions; no cursor of its own (not for clicking) */
#define HF_UI_MENU 1u    /* a menu, as the game's own: the cursor shows, the player's movement and camera are held, Back
                          * (Esc) closes it. One mod menu at a time; it opens once no game menu is open. */

/* A click on a named element (ui_on): the screen and the element's name. With no element (ui_on(mod, screen, NULL, ...))
 * it is called when the screen closes (ui_close, or Back on a menu), with element "". On the game thread. */
typedef void (*HfUiCallback)(struct HfMod* mod, int screen, const char* element, void* user);
