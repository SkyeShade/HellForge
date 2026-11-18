# Screens in the game's own UI

<!-- diataxis: how-to -->

A code mod can show screens made the way the game makes its own: XAML, parsed and drawn by the game's NoesisGUI. The
game's fonts, text styles, colours and themed controls are all there, so a mod's screen looks like part of the game.
The calls are SDK 19 (`ui_open`, `ui_set`, `ui_on`, `ui_close`); the types are in `hellforge_ui.h`, which
`hellforge_mod.h` includes.

For a screen drawn with HellForge's own drawing calls (a map, a graph), see [Screens over the game](overlay.md).

## Contents

- [A first screen](#a-first-screen)
- [Kinds of screen](#kinds-of-screen)
- [XAML](#xaml)
- [Changing a screen from code](#changing-a-screen-from-code)
- [Clicks and closing](#clicks-and-closing)
- [Limits](#limits)

## A first screen

Put the XAML in your mod's folder, say `ui/shop.xaml`, and name the elements your code talks to with `x:Name`:

```xml
<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
      xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" Background="#B0000000">
  <Border HorizontalAlignment="Center" VerticalAlignment="Center" Width="560" Background="#F0101214"
          BorderThickness="0,4,0,0" Padding="36,22,36,26">
    <Border.BorderBrush><SolidColorBrush Color="{StaticResource BrandYellow}"/></Border.BorderBrush>
    <StackPanel>
      <TextBlock x:Name="Title" Style="{StaticResource Headerstyle}" Text="REQUISITION"/>
      <TextBlock x:Name="Body" Style="{StaticResource Body_Medium}" Text="Nothing bought yet."/>
      <Button x:Name="Buy" Content="BUY" Padding="24,8" Margin="0,18,0,0" HorizontalAlignment="Left"/>
    </StackPanel>
  </Border>
</Grid>
```

Then open it and listen to its button:

```c
static void Bought(HfMod* mod, int screen, const char* element, void* user)
{
    HF_API(mod)->ui_set(mod, screen, "Body", "Text", "Bought one.");
}

static void Closed(HfMod* mod, int screen, const char* element, void* user)
{
    HF_API(mod)->log(mod, "the shop closed");
}

/* from hellforge_mod_init, a game event or the game thread */
int shop = hf->ui_open(mod, "ui/shop.xaml", HF_UI_MENU);
hf->ui_on(mod, shop, "Buy", Bought, NULL);
hf->ui_on(mod, shop, NULL, Closed, NULL);
```

The manifest needs `"sdk": 19`. Every call may come from any thread: HellForge queues it and runs it on the game thread
at the next game update, the only thread that may touch the game's UI. Callbacks run on the game thread.

## Kinds of screen

| Kind | What it is |
|---|---|
| `HF_UI_MENU` | A menu, as the game's own: the cursor shows, the player's movement, camera and buttons are held, and Back (Esc) closes it. It opens once no game menu is open or opening, on the ship or in a mission. One mod menu is open at a time; another mod's `ui_open` of a menu is refused while one is up. |
| `HF_UI_OVERLAY` | Over the game, on the ship and in missions, with no cursor of its own: for timers, counters and small panels. It does not take clicks. It stays until the mod closes it. |

A menu takes the game's unused menu slot 40, which the game itself never opens, so no game screen is lost.

## XAML

The XAML is parsed by the game's NoesisGUI 3.2.5, which reads WPF's XAML. `ui_open` takes a file of the mod's folder
(a relative path, no `..`, 4 MB at most) or the XAML text itself, starting with `<`.

The game's global resources are in reach with `{StaticResource ...}`, among them:

| Resource | What it is |
|---|---|
| `Headerstyle` | the game's heading text style |
| `Body_Medium` | its body text style |
| `BrandYellow` | the game's yellow, a `Color` (use it in a `SolidColorBrush`) |

Stock controls (`Button`, `CheckBox`, `Slider`, `TextBox`, `ScrollViewer`...) already have the game's look. A screen has
no code behind it: leave out `x:Class`, and wire it up from C with the calls below.

When the XAML does not parse, `ui_open` still returns an id; the screen is refused when it would open, and the mod's
log says so. The game's Noesis log line says why.

## Changing a screen from code

`ui_set(mod, screen, element, property, value)` sets a property of a named element (`""` or NULL: the screen's root)
from text, the way XAML would read it:

```c
hf->ui_set(mod, shop, "Body", "Text", "3 left");
hf->ui_set(mod, shop, "Title", "Foreground", "#FFE900");
hf->ui_set(mod, shop, "Buy", "Visibility", "Collapsed");
hf->ui_set(mod, shop, "Buy", "IsEnabled", "False");
```

Any dependency property of the element or its base types works, through that property's own type converter. A name
that is not there, a property the element does not have, or text that is not a value of it is logged in the mod's log
(`ui(1): no element named "Nobody"`).

## Clicks and closing

`ui_on(mod, screen, element, callback, user)` calls `callback` when the named element is clicked. It takes anything
with a `Command` property: a `Button`, a `MenuItem`, a `Hyperlink`. Only a menu screen takes clicks.

`ui_on` with element NULL calls `callback` when the screen closes, with element `""`: after `ui_close`, or when the
player presses Back on a menu. A screen that closed is gone; open it again for a new one.

`ui_close(mod, screen)` closes a screen. A menu closes the way the game closes its own, at the end of that UI frame, so
it is safe from inside a click callback.

## Limits

- 64 screens across all mods per session, and 256 click handlers.
- One mod menu at a time.
- Screens are client only: they are not part of mod sync, and each player sees only their own.
- Images and other files referenced from a mod's XAML are not served yet.
- Only the reviewed game build (25480438): on another, screens are refused and `modloader.log` says why.
