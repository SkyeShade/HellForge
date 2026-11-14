# Projectiles and damage

<!-- diataxis: how-to -->

A weapon fires projectiles, and each projectile deals a damage type. Both are rows in tables the game keeps: a
projectile row holds speed, gravity, lifetime, effects and the damage id it deals; a damage row holds standard and
durable damage, armour penetration and status effects. `add_damage` and `add_projectile` (SDK 3) copy a vanilla row
under an id of its own, so changing yours changes no vanilla weapon.

`examples/mods/talon-liberator` adds all three: a damage type with five times the LAS-58 Talon's damage, a projectile
that deals it with a blue look, and a Liberator that fires it with the LAS-16 Sickle's sound.

```c
HfDamage heavy = {sizeof heavy};
heavy.base = HF_DAMAGE_LAS58_TALON;
heavy.name = "bolt_damage"; /* SDK 6: short names are the mod's ("skyeshade.talon-liberator:bolt_damage") */
heavy.scale = 5.0f;
uint32_t damage = api->add_damage(mod, &heavy);

HfProjectile bolt = {sizeof bolt};
bolt.base = HF_PROJECTILE_LAS58_TALON;
bolt.name = "bolt";
bolt.damage = damage;
bolt.spawn_effect = api->resource_hash("skyeshade/talon_liberator/assault_rifle_talon/bolt_blue");
bolt.flight_effect = api->resource_hash("skyeshade/talon_liberator/assault_rifle_talon/bolt_flight_blue");
uint32_t projectile = api->add_projectile(mod, &bolt);

HfWeapon rifle = {sizeof rifle};
rifle.base = HF_ITEM_AR23_LIBERATOR;
rifle.unit = "skyeshade/talon_liberator/assault_rifle_talon";
rifle.designation = "AR-23T";
rifle.name = "Liberator Talon";
rifle.description = "...";
rifle.projectile = projectile;
rifle.sound_from = HF_ITEM_LAS16_SICKLE; /* its banks are in data/ with the rifle's model */
uint32_t weapon = api->add_weapon(mod, &rifle);
```

- `scale` multiplies the base's standard and durable damage (0 means 1); the rest stays the base's. New damage ids are
  650 and up.
- A projectile's `damage` is any damage id (0 keeps the base's). `visual_from` takes another projectile type's look,
  such as `HF_PROJECTILE_AR23_LIBERATOR`. New projectile types are 351 and up.
- A weapon's `projectile` is any projectile type, vanilla or added.
- Add damage before the projectiles that deal it, and projectiles before the weapons that fire them.

The catalogue names 295 explosions and 342 damage ids as `HF_EXPLOSION_` and `HF_DAMAGE_` constants, so a round's blast
or a weapon's damage is one autocompletion away. They are all in the [SDK reference](../sdk-reference.md).

## Contents

- [Explosions](#explosions)
- [Beams and arcs](#beams-and-arcs)
- [Sprays](#sprays)
- [Changing your rows further](#changing-your-rows-further)
- [Ammunition](#ammunition)
- [Looks](#looks)
- [Room and co-op](#room-and-co-op)

## Explosions

`add_explosion` (SDK 17) copies a vanilla explosion under a type of its own, with its own damage and radii. A projectile
then goes off with it through `impact_explosion` (a hit) and `expiry_explosion` (the end of its flight).

```c
HfExplosion blast = {sizeof blast};
blast.base = HF_EXPLOSION_R36_ERUPTOR_IMPACT;
blast.name = "big_blast";
blast.damage = damage;       /* 0 keeps the base's */
blast.outer_radius = 12.0f;
uint32_t explosion = api->add_explosion(mod, &blast);

HfProjectile round = {sizeof round};
round.base = HF_PROJECTILE_R36_ERUPTOR;
round.name = "big_round";
round.impact_explosion = explosion;
uint32_t projectile = api->add_projectile(mod, &round);
```

- `HfExplosion` takes `base`, `name`, `damage`, `inner_radius`, `outer_radius`, `shockwave_radius` (metres) and `arc`, the
  arc it chains. Each left at 0 keeps the base's. New explosion types are 423 and up.
- `impact_explosion` and `expiry_explosion` take any explosion type, vanilla or added (0 keeps the base's). Add an
  explosion before the projectiles that use it.
- An explosion's effect and sound stay the base's.
- A vanilla blast's sound comes with the weapon that fires it. HellForge loads the weapon package of a round that
  explodes, so its sound plays when that weapon is not in the loadout. If the package does not load, your mod's log says
  so and the blast is silent.

## Beams and arcs

A beam or arc weapon's record names only a type. Its damage, radius, length, range and chains are in that type's row,
which every weapon of the type shares. So a beam or arc weapon with stats of its own needs a type of its own.

```c
HfBeam ray = {sizeof ray};
ray.base = HF_WEAPON_BEAM_LAS5_SCYTHE;
ray.name = "long_ray";
ray.damage = damage;
ray.length = 80.0f;
uint32_t beam = api->add_beam(mod, &ray);

HfWeapon scythe = {sizeof scythe};
scythe.base = HF_ITEM_LAS5_SCYTHE; /* a base that fires a beam */
scythe.beam = beam;
```

- `add_beam` (SDK 17) takes an `HfBeam`: `base`, `name`, `damage`, `radius` and `length` (metres). New beam types are 31
  and up.
- `add_arc` (SDK 17) takes an `HfArc`: `base`, `name` and `damage`. New arc types are 17 and up.
- `HfWeapon.beam` and `HfWeapon.arc` name the type it fires, vanilla or added. Its base must fire one: a beam weapon's
  or an arc weapon's own record holds the type. `HfExplosion.arc` chains an arc from an explosion.
- `HF_WEAPON_BEAM_` and `HF_ARC_` constants name the vanilla types. Left at 0, each field keeps the base's.

## Sprays

A spray weapon (the flamethrowers, the Sterilizer) keeps its damage in its own `SprayWeapon` record, not in a shared row.
So a spray of your own needs no new type: give the weapon a damage of its own.

```c
HfDamage hot = {sizeof hot};
hot.base = HF_DAMAGE_FLAM40_FLAMETHROWER;
hot.name = "hot_flame";
hot.scale = 2.0f;

HfWeapon flamer = {sizeof flamer};
flamer.base = HF_ITEM_FLAM40_FLAMETHROWER; /* a base that sprays */
flamer.spray_damage = api->add_damage(mod, &hot);
```

- `HfWeapon.spray_damage` (SDK 20) names the damage its spray deals, vanilla or added; 0 keeps the base's. The weapon
  gets a `SprayWeapon` record of its own with that damage, so the base and other sprays don't change. A copy of the
  Sterilizer's damage (`HF_DAMAGE_TX41_STERILIZER`) on a flamethrower deals the Sterilizer's damage and status.
- Its base must spray. A weapon that doesn't keeps its own damage, and its log says so.
- The flames' look and reach come from the base's particle effect, which stays the base's.
- `SprayWeapon.damage`, `projectile` and `link_radius` are in stats files and `hf_set` like any other field. An enemy
  spray throws `projectile` and deals that projectile's damage; every player spray has 0 there.

## Changing your rows further

After `HF_EVENT_PROJECTILES_READY`, `projectile_row(type)` and `damage_row(id)` return your rows to change further.
They return addresses: cast them to `HfProjectileSettings*` and `HfDamageInfo*`, and keep each row's id as it is.
`explosion_row`, `beam_row` and `arc_row` do the same for the new types (152, 112 and 104 bytes), whose first 4 bytes are
the type and must stay.
`hf_set` reaches the rows too. The compendium's Pelican EMS and Gas turrets give their rounds an impact explosion this
way (`src/pelicans.c`), and `hf_set(mod, type, ProjectileSettings, impact_explosion, HF_U32(82))` makes a projectile's
hits burst into the Orbital Gas Strike's cloud. HellForge loads the explosion's packages.

## Ammunition

Attachments can set a weapon's projectile when it is assembled (the Liberator's default ammunition does). A weapon given
`projectile` drops the default ammunition from its own customisation, so it fires what you named.

## Looks

A projectile looks the way its particle effects (`spawn_effect`, `flight_effect`) draw it. The colours are curves inside
the effect, so a recoloured effect comes as data ([data specs](data.md#data-specs)). `examples/mods/talon-liberator`
turns the Talon's bolt blue.

## Room and co-op

There is room for 1,024 new projectile types and 512 new damage types across all mods, and for 512 each of explosions,
beams and arcs. In co-op, every machine fires its own copy of a hand-held weapon's shots from its own data; with the same
mods, that is the same projectile.

Some shots also travel between players by type (synced fire, reloads, projectiles added to the manager), in a network
field of 9 bits: types up to 511, so the first 161 new ones. A larger set widens it to 11 bits, on every machine together,
as all of them run the same mods. If the field is not the one HellForge knows, only the first 161 go in and the log names
the rest; weapons that fire those fire their base's.

A set of more than 89 explosion types widens a co-op network field by one bit. Every player runs the same mods, so all of
them widen it together. If the field is not the one HellForge knows, only the first 89 go in and the log names the rest.

## Where to go next

- [Content](content.md): the weapons that fire these, and the rest of what mods add.
- [SDK reference](../sdk-reference.md): every `HF_PROJECTILE_`, `HF_DAMAGE_`, `HF_EXPLOSION_`, `HF_WEAPON_BEAM_` and `HF_ARC_` constant.
