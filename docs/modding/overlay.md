# Screens over the game

<!-- diataxis: reference -->
A code mod draws its own screens over the game with SDK 17: `add_screen` adds one, and its key or `screen_show` opens and
closes it. HellForge's overlay host draws them into the game's frames, on Direct3D 11 and 12, in SDR and HDR. The types
are in `hellforge_overlay.h`, which `hellforge_mod.h` includes.

## The calls

| Call | What it does |
|---|---|
| `int add_screen(HfMod* mod, const HfScreen* screen)` | Adds a screen, from any thread. Returns its id (1 and up), or 0 when refused. `modloader.log` says why. |
| `void screen_show(HfMod* mod, int id, int open)` | Opens (1) or closes (0) one of the mod's own screens, from any thread. |

A mod's manifest needs `"sdk": 17` for these calls. A screen can be added in `hellforge_mod_init` or later. Drawing
starts once the game's entities are ready.

```c
static int DrawMap(void* user, const HfDraw* d, const HfInput* in, float dt)
{
    if (in->closing) return 0;
    d->backdrop(0.6f, 0.5f, 12);
    d->rect(100, 100, 600, 400, HF_RGBA(30, 30, 30, 230), HF_RGBA(255, 220, 0, 255), 3);
    d->text(120, 140, "TACTICAL MAP", 36, HF_FACE_TITLE, HF_ALIGN_LEFT, HF_RGBA(255, 255, 255, 255));
    return 1;
}

HfScreen map = {sizeof map, "Tactical map", 'M', HF_SCREEN_MODAL, NULL, DrawMap, NULL, NULL};
int id = hf->add_screen(mod, &map);
```

## HfScreen

| Field | Meaning |
|---|---|
| `size` | `sizeof(HfScreen)` |
| `name` | The name in the logs. The host copies it. |
| `key` | The virtual key that opens and closes it (`'M'`, `VK_F7`), or 0 for none. Two screens cannot share a key. |
| `flags` | `HF_SCREEN_MODAL`: while open, the mouse and keys are the screen's and the cursor is free. |
| `user` | Passed back to every callback. |
| `draw` | Called every frame from the opening until it returns 0, on the host's render thread. |
| `opened` | Optional. Called with 1 before the first draw and with 0 after the last. |
| `can_open` | Optional. Asked on the window's thread when the key is pressed. 0 keeps the screen shut and gives the key to the game. |

## Opening and closing

| Step | What happens |
|---|---|
| Opening | The key or `screen_show(id, 1)` asks for it. A modal screen takes the input at once, so the press never reaches the game. |
| First frame | `opened(1)`, then `draw` every frame. |
| Closing | The key again, Esc on a modal screen, or `screen_show(id, 0)`. `input->closing` turns 1 and the game gets the input back that frame. |
| Closed | `draw` returns 0. Then `opened(0)`. |

A screen can animate its closing: it keeps returning 1 while `input->closing` is 1, then returns 0. When the mod is
taken out while the game runs, its screens close. Its next version takes them over by adding a screen on the same key.

## Units

Every position and size is in screen units. The screen is 1080 units high and `width` units wide: 1920 on a 16:9
window, 2520 on 21:9. (0, 0) is the top left. `pixels` is pixels per unit, the window's height divided by 1080.

## HfDraw

`HfDraw` is valid only inside `draw`. Everything is drawn in call order, inside the current clip. Shapes are
anti-aliased.

| Function | What it draws |
|---|---|
| `rect(x0, y0, x1, y1, fill, outline, thickness)` | A box. The outline lies inside the box. |
| `lines(points, count, colour, thickness, closed)` | A line through the points, closed or open. |
| `polygon(points, count, fill)` | A filled polygon. The host splits concave ones into triangles. |
| `circle(x, y, radius, fill, outline, thickness)` | A circle. |
| `text(x, y, utf8, size, face, align, colour)` | One line of text with its cap height centred on `y`. Returns its width. |
| `text_width(utf8, size, face)` | The width `text` would draw. |
| `image_create(width, height, rgba)` | A picture from RGBA8 pixels, rows from the top, sRGB, straight alpha. NULL when it cannot be made. |
| `image_update(image, rgba)` | Replaces all of a picture's pixels. |
| `image_free(image)` | Frees a picture. Pictures otherwise live until the session ends. |
| `image(image, box, part, tint)` | A picture in the `HfRect` `box`, filtered, times `tint`. `part` is the piece to draw, in 0 to 1 of its size; NULL draws all of it. |
| `clip_push(x0, y0, x1, y1)` / `clip_pop()` | Cuts drawing off outside a box. Boxes nest. |
| `backdrop(dim, desaturate, blur)` | The game's frame behind the screen, dimmed, greyed and blurred. |

Colours are `HF_RGBA(r, g, b, a)` in sRGB, as on an SDR screen. In HDR the host converts them, so white is the
display's SDR white.

Text uses the game's fonts: `HF_FACE_BODY` is its regular face and `HF_FACE_TITLE` its medium one. They are baked at
sizes from 10 to 48 units; other sizes scale the nearest. Without the font files, text uses ImGui's own font.

## The backdrop

`backdrop(dim, desaturate, blur)` copies the game's frame once when the screen opens. The copy stays while the screen is
open, so the game's world freezes behind it. `dim` goes from 0 (none) to 1 (black), `desaturate` from 0 to 1, and `blur`
is a radius in units.

| API | How it is made |
|---|---|
| Direct3D 11 | The back buffer is copied into a texture of its format and its mips are made by `GenerateMips`. |
| Direct3D 12 | The back buffer is copied into a texture of its format and the host makes its mips with its own shader. |
| Either, when the copy fails | A dimmed rectangle over the live frame, without blur or greying. `modloader.log` says why. |

The blur reads the mip whose texels match the radius, with a 3x3 tent filter. Colours are decoded from sRGB, PQ or
scRGB first, so dimming looks the same in SDR and HDR.

## HfInput

| Field | Meaning |
|---|---|
| `x`, `y` | The mouse, in units. |
| `wheel` | Wheel notches since the last frame, positive away from the player. |
| `down`, `pressed`, `released` | `HF_MOUSE_LEFT`, `HF_MOUSE_RIGHT` and `HF_MOUSE_MIDDLE`: held, and changed since the last frame. |
| `keys`, `key_count` | Up to 16 virtual keys pressed since the last frame. Held keys do not repeat. |
| `closing` | 1 once the screen was asked to close. |

Only the open modal screen added last gets buttons, wheel and keys. Every other screen gets the mouse position only. A
screen's own key and Esc never show up in `keys`: the host takes them to open and close it.

## Threads and limits

- `draw` and `opened` run on the host's render thread, inside the game's Present. Keep them short: the frame waits.
- `can_open` runs on the window's thread. A call into the game from there is not safe.
- At most 64 screens in a session, and 1024 pictures on Direct3D 12.

## Settings

`HELLFORGE_OVERLAY_HDR` overrides the detected HDR mode: `off`, `pq` or `scrgb`, optionally with the SDR white in nits
(`pq:200`). It is for a display that reports its mode wrongly.

## C++ screens with Dear ImGui

A C++ mod can draw its screen with Dear ImGui 1.91.9b in the host's context. Build ImGui with
`IMGUI_DISABLE_OBSOLETE_FUNCTIONS`, as the host does. Inside `draw`:

1. Get `hf_overlay_imgui` from `hellforge_overlay.dll` (`GetModuleHandleW`, `GetProcAddress`).
2. Call it with `HfOverlayImGui shared = {HF_OVERLAY_IMGUI_LAYOUT};`. It returns 0 when the versions and layouts match.
3. Call `ImGui::SetAllocatorFunctions(shared.alloc, shared.free, shared.alloc_user)` and
   `ImGui::SetCurrentContext((ImGuiContext*)shared.context)`.

`hellforge_overlay_host.h` in `native/overlay` declares it. The host has one font atlas: use the fonts it baked.

## The host

The host is `hellforge_overlay.dll` in `<installation>/app/sdk-shims`, beside the mod loader. The loader loads it at the
first `add_screen` and starts it when the entities are ready. `scripts/Build-Overlay.ps1` builds it, and
`scripts/build-overlay.sh` on Linux.

| Export | What it does |
|---|---|
| `hf_overlay_start(const HfOverlayStart*)` | Hooks the swap chain through the loader's hook chains. Returns 0 when started. |
| `hf_overlay_add_screen(const HfScreen*)` | As `add_screen`, without the mod. |
| `hf_overlay_show(int id, int open)` | As `screen_show`. |
| `hf_overlay_imgui(HfOverlayImGui*)` | The ImGui context and heap, for C++ screens. |
