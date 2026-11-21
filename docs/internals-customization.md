# How attachment values and weapon picks work

<!-- diataxis: explanation -->

This page is for people working on HellForge itself. It explains how an attachment gets values of its own and how
`set_weapon_pick` puts an item in a weapon's slot. The mod's side is in
[modding/content.md](modding/content.md#ammunition-of-your-own).

## An attachment's values

An attachment is a copy of a vanilla item whose entity deltas are copied under its own key (`attachments.c`). Each
`HfAttachment.set` entry becomes a write {component, offset, bytes} on that copy:

- A write inside one of the base's deltas of the same component goes over the copied bytes.
- Any other write is appended after that component's copied deltas. The game copies a setting's deltas in order, so
  the appended one wins where they overlap.
- A write to a component the base's deltas don't touch gets a component setting of its own in the copy.

The grown storage is measured with these writes first, so a copy with values never writes past its arrays.

## Weapon picks

The game keeps the player's customizations in its ItemCustomizationManager (game.dll+0x347CEF8, +0x178A4EC). It holds
512 entries of 0x94 bytes: the weapon's id, a state, the template the Armory shows (`CYCLE WEAPON CONFIG`), and three
templates of ten picks, one for each slot number.

| Piece | game.dll | Use |
| --- | --- | --- |
| EquipCustomizationItem | 0x1035370 | stores a pick; for an entry in state 0 or 1 it copies the weapon's picks into the local player's loadout and sends it |
| The loadout push | 0x1034180 | the record's loadout at +0x38 and the send that tells the other machines |
| The customize screen's reads | 0x1493770, 0x1494590 | read the same entries, so the screen shows a mod's pick |

- `set_weapon_pick` only queues a request. The game thread makes it through EquipCustomizationItem, so the pick takes
  the game's own path to the save and the loadout.
- Requests are taken in any game state. EquipCustomizationItem itself does not check the state; in a mission it
  changes the saved pick and the loadout, not the weapon already built.
- EquipCustomizationItem's first bytes are pinned; with other bytes the calls stay off and the log says so.
- A weapon with no template yet (the game's defaults, -1) gets the pick in its first template.

## A mod weapon's ammunition

`HfWeapon.ammunition` is written into the weapon's own customization record (component 271) as it loads
(`registry_load.c`). The item becomes the slot-6 pair among the defaults, and slot 6 joins the slots the player may
change, so a pick there is the weapon's own.

## Where to go next

- [internals.md](internals.md): the rest of how HellForge implements mods.
- [sdk-reference.md](sdk-reference.md): every SDK call, component, table and constant.
