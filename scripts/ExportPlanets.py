"""Export the galaxy's planets for the launcher's Planets page and the planet modifiers (HellForge.Core PlanetModifiers):
each planet's name, sector, biome, where it lies on the map, its supply lines, its full health and who holds it in the
frozen war.

Usage: py -I scripts/ExportPlanets.py <helldivers-2/json planets/planets.json> <planet-preview-map.json> <planet-hologram-materials.json>
       data/planets.json

Positions, supply lines (waypoints), full health and owners come from the frozen war (src/HellForge.LocalBackend/war-data.json), sectors from
src/HellForge.LocalBackend/planet-sectors.json (the map's, as the all-planets war uses them). Names and biomes are labels
from the community data at https://github.com/helldivers-2/json (MIT, data/licences/helldivers-2-json-LICENSE.txt). A
planet the frozen war has and the community data lacks is named by its index.

`preview` is the hash of the planet's picture, the 460x148 banner of its biome that the game keeps as a texture (and in
its UI atlas): its planet data record's PlanetPreviewImage (LevelGenerationPlanetData +560), the record named by the
high 32 bits of warInfo's settingsHash (Murmur64A of "planet_NNN"). The map file holds those hashes only, read from a
plain copy of generated_planet_data (HD2Mods research/planet-modifiers/planet-art); a planet it lacks takes the picture
most planets of its community biome have. The launcher decodes the picture from the player's own game data.

`hologram` is the hash of the planet's galaxy map material (LevelGenerationPlanetData +552, HologramPlanetMaterial), from
the same plain copy (planet-modifiers/planet-icons); the launcher draws the planet's icon from it (GameData.PlanetIcons).

`unused` (only when true) marks a planet of the war data that no sector of the map has: planet-sectors.json's "TBD",
264 NEW INSIGHT and 265 WAYWARD, placeholders at Super Earth's spot. The planet modifiers leave them out (PlanetCatalog),
so the launcher's map, its planet list and Custom planets do not offer them, and the all-planets war gives them no
campaign (OpenWar). They stay in the file, which is read by index.
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BACKEND = ROOT / 'src' / 'HellForge.LocalBackend'
UNUSED_SECTOR = 'TBD'


def by_planet(map_path, field, community):
    """Each planet's hash (hex) from a map file's field, and for a planet the map lacks the one most of its community biome's have."""
    rows = json.loads(Path(map_path).read_text(encoding='utf-8'))['planets']
    by_index = {r['index']: r[field][2:].lower() for r in rows if r.get(field)}
    votes = {}
    for index, preview in by_index.items():
        biome = community.get(str(index), {}).get('biome')
        votes.setdefault(biome, []).append(preview)
    by_biome = {b: max(set(v), key=v.count) for b, v in votes.items() if b}
    return lambda index: by_index.get(index) or by_biome.get(community.get(str(index), {}).get('biome'), '')


def main(community_path, map_path, hologram_path, out_path):
    war = json.loads((BACKEND / 'war-data.json').read_text(encoding='utf-8'))
    sectors = json.loads((BACKEND / 'planet-sectors.json').read_text(encoding='utf-8'))['sectors']
    community = json.loads(Path(community_path).read_text(encoding='utf-8'))
    preview = by_planet(map_path, 'previewImage', community)
    hologram = by_planet(hologram_path, 'hologramMaterial', community)
    owners = {p['index']: p['owner'] for p in war['status']['planetStatus']}
    homes = {i for h in war['warInfo'].get('homeWorlds', []) for i in h['planetIndices']}
    planets = []
    for info in sorted(war['warInfo']['planetInfos'], key=lambda p: p['index']):
        index = info['index']
        named = community.get(str(index), {})
        sector = sectors.get(str(index)) or named.get('sector') or ''
        planet = {
            'index': index,
            'name': named.get('name') or 'Planet %d' % index,
            'sector': sector,
            'biome': named.get('biome') or '',
            'x': round(info['position']['x'], 6),
            'y': round(info['position']['y'], 6),
            'maxHealth': info['maxHealth'],
            'owner': owners.get(index, info.get('initialOwner', 1)),
            'home': index in homes,
            'links': sorted(info.get('waypoints', [])),
            'preview': preview(index),
            'hologram': hologram(index),
        }
        if sector == UNUSED_SECTOR:
            planet['unused'] = True
        planets.append(planet)
    out = {
        'scope': ('The galaxy\'s planets for the planet modifiers, by index: labels and map positions, no game values. '
                  'Positions, full health and owners from the frozen war (war-data.json), sectors from planet-sectors.json, '
                  'names and biomes from the community data at https://github.com/helldivers-2/json (MIT, Copyright (c) 2024 '
                  'Helldivers 2 Community; data/licences/helldivers-2-json-LICENSE.txt); preview and hologram: the hashes of each planet\'s picture and galaxy map material in '
                  'the game data (no art); unused: a placeholder of the war data that is on no sector of the map (TBD), left out of '
                  'the planet modifiers. Made by scripts/ExportPlanets.py.'),
        'planets': planets,
    }
    # one planet a line
    lines = ',\n  '.join(json.dumps(p, ensure_ascii=False) for p in planets)
    text = '{\n "scope": %s,\n "planets": [\n  %s\n ]\n}\n' % (json.dumps(out['scope'], ensure_ascii=False), lines)
    Path(out_path).write_text(text, encoding='utf-8', newline='\n')
    print(len(planets), 'planets,', sum(1 for p in planets if p['name'].startswith('Planet ')), 'unnamed,', sum(1 for p in planets if not p['preview']), 'without a picture,', sum(1 for p in planets if not p['hologram']), 'without a material')


if __name__ == '__main__':
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    main(*sys.argv[1:5])
