/* HellForge mod SDK: customization items of a mod's own, for add_attachment (SDK 17; docs/modding/content.md) and
 * add_pattern (SDK 18: patterns for weapons, vehicles, the hellpod and the Pelican; docs/modding/patterns.md). Included by
 * hellforge_mod.h. */
#pragma once
#include <stdint.h>

/* A new weapon attachment (SDK 17, add_attachment): a copy of a vanilla customization item (a magazine, muzzle, optic...)
 * under an id of its own, with the base's slot, stat changes and look in the armory, offered on weapons mods added.
 * Zero-initialize it and set `size`. */
typedef struct HfAttachment
{
    uint32_t size;      /* sizeof(HfAttachment) */
    uint32_t base;      /* the vanilla item it copies (its id, e.g. a magazine the base weapon takes) */
    const char* name;   /* its id is the thin hash of this; unique, prefixed with your name and mod */
    /* A magazine's, muzzle's or optic's model: a unit resource the mod's data provides, drawn at the weapon's attach_
     * joint in place of the base's. 0 keeps the base's (and is required for other slots). */
    uint64_t unit;
    /* Its text in the armory (UTF-8); NULL keeps the base's. An upper-case copy of the name is made. */
    const char* name_text;
    const char* description;
    /* The weapons offering it: loadout ids add_weapon returned (up to 16). Their base must take the base item's slot. */
    const uint32_t* weapons;
    uint32_t weapon_count;
} HfAttachment;

/* One texture a pattern's base names, and the texture that takes its place (resource name hashes). */
typedef struct HfTextureSwap
{
    uint64_t from;
    uint64_t to;
} HfTextureSwap;

#define HF_PATTERN_SWAPS 16
#define HF_PATTERN_COLOURS 8

/* A new pattern (SDK 18, add_pattern; docs/modding/patterns.md): a copy of a vanilla pattern under an id of its own, with
 * the base's look until its textures are swapped. The base says where it goes: a weapon paint (the armory's pattern slot)
 * is offered on weapons, and a vehicle bay, hellpod or Pelican skin joins every collection that has the base (all four
 * exosuits for an exosuit skin, the FRVs and tanks for an FRV one, the hellpod and its weapon rack together). The
 * player holds it from the start. Zero-initialize it and set `size`. */
typedef struct HfPattern
{
    uint32_t size;               /* sizeof(HfPattern) */
    uint32_t base;               /* the vanilla pattern it copies, by id: a weapon paint or a unit skin (not a default) */
    const char* name;            /* its id is the thin hash of this; unique, prefixed with your name and mod */
    const char* name_text;       /* its name in the armory or hangar (UTF-8); NULL keeps the base's */
    const char* description;     /* a weapon pattern's description; NULL keeps the base's */
    /* A unit skin's pattern colour: a pattern_lut texture (3 x 1, RGBA16F) your mod's data provides, put in place of
     * every pattern_lut the base sets. 0 keeps the base's. A weapon paint sets material_luts only: swap those. */
    uint64_t pattern_lut;
    /* Textures the base names (each material's material_lut, pattern_masks_array or decal_sheet), replaced by yours:
     * up to HF_PATTERN_SWAPS. The mod's log lists the base's textures once the pattern is in the game. */
    const HfTextureSwap* swaps;
    uint32_t swap_count;
    /* The swatch shown on its card: 1 to HF_PATTERN_COLOURS linear RGB colours; NULL keeps the base's. */
    const float (*colours)[3];
    uint32_t colour_count;
    /* A weapon pattern's weapons: vanilla loadout ids or ones add_weapon returned (up to 16). NULL: every weapon that
     * offers the base paint, mods' copies of them included. Not read for unit skins. */
    const uint32_t* weapons;
    uint32_t weapon_count;
} HfPattern;
