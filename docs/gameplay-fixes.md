# Fixes to the game's own behaviour

<!-- diataxis: explanation -->

For contributors and testers: places where the mod loader changes what the game does on build 25480438, because the
game gets it wrong, and why each change is safe. Performance fixes are on [their own page](performance.md), crash guards
on [theirs](crash-guards.md).

## Explosion knockback and cover

Behind a rock or a wall, an explosion's damage stopped but its knockback still threw the Helldiver. HellForge makes the
knockback wait for the same line-of-sight check as the damage (`native/mod-loader/explosion_cover.c`).

### What the game does

An explosion's hit on a Helldiver goes through ProduceDamage (game.dll 0x129DD30). It makes a damage event and, when the
blast pushes hard enough, a knockback.

- The hit zone's `explosion_verification_mode` (DamageableZoneInfo +0x150: none, outer radius, all) says whether the
  damage needs a line of sight. A Helldiver's zones ask for it outside the blast's inner radius.
- Such a damage event leaves the damage queue for the explosion system's line-of-sight queue (`[0x346D558] + 0x9828`,
  256 entries of 0xB0 bytes). The next frame, a ray from the blast to the hit decides (0x13C6A50, called from
  ExplosionGather 0x13C6180). Clear, the event goes back to the damage queue; blocked, it is dropped.
- The knockback goes into the knockback queue (`[0x347CF38] + 0x155120`, 0x40 bytes each, count at +0x201130) in the
  same call. Nothing checks it, so the Helldiver was thrown with no damage taken.

An in-game test showed it with only a test mod on, and no other HellForge code in that path. Behind a rock the game's
check blocked the damage, and the Helldiver still landed 7 m away, knocked down.

### What changes

- A knockback queued by a call that also queued its damage for a line of sight is held, not left in its queue.
- Before each gather, HellForge runs the game's own check on the entries that gather decides, as the gather does (either
  ray clear). The knockback goes into its queue only when its damage gets through.
- Hits the game does not check keep their knockback at once: inside the inner radius, zones without the mode, a full
  line-of-sight queue. Close blasts still knock you down through thin walls, as their damage still hurts.

### Co-op and limits

- Each machine does it for the explosions it simulates, as the game runs the check there. A knockback for another
  player's Helldiver is sent to its owner as before, so no machine waits for another.
- The knockback comes one frame later than it would have.
- Every instruction it relies on is pinned. Another build keeps the game's knockback, with a line in `modloader.log`.
- `HELLFORGE_EXPLOSION_COVER=off` turns it off for a session. `modloader.log` counts the knockbacks held, let through,
  stopped by cover and gone stale (their entry never decided, as when a mission ends).
