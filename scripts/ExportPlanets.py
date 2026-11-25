"""Export the galaxy's planets for the launcher's Planets page and the planet modifiers (HellForge.Core PlanetModifiers):
each planet's name, sector, biome, where it lies on the map, its supply lines, its full health and who holds it in the
frozen war.

Usage: py -I scripts/ExportPlanets.py <helldivers-2/json planets/planets.json> data/planets.json

Positions, supply lines (waypoints), full health and owners come from the frozen war (src/HellForge.LocalBackend/war-data.json), sectors from
src/HellForge.LocalBackend/planet-sectors.json (the map's, as the all-planets war uses them). Names and biomes are labels
from the community data at https://github.com/helldivers-2/json (MIT, data/licences/helldivers-2-json-LICENSE.txt). A
planet the frozen war has and the community data lacks is named by its index.
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BACKEND = ROOT / 'src' / 'HellForge.LocalBackend'


def main(community_path, out_path):
    war = json.loads((BACKEND / 'war-data.json').read_text(encoding='utf-8'))
    sectors = json.loads((BACKEND / 'planet-sectors.json').read_text(encoding='utf-8'))['sectors']
    community = json.loads(Path(community_path).read_text(encoding='utf-8'))
    owners = {p['index']: p['owner'] for p in war['status']['planetStatus']}
    homes = {i for h in war['warInfo'].get('homeWorlds', []) for i in h['planetIndices']}
    planets = []
    for info in sorted(war['warInfo']['planetInfos'], key=lambda p: p['index']):
        index = info['index']
        named = community.get(str(index), {})
        planets.append({
            'index': index,
            'name': named.get('name') or 'Planet %d' % index,
            'sector': sectors.get(str(index)) or named.get('sector') or '',
            'biome': named.get('biome') or '',
            'x': round(info['position']['x'], 6),
            'y': round(info['position']['y'], 6),
            'maxHealth': info['maxHealth'],
            'owner': owners.get(index, info.get('initialOwner', 1)),
            'home': index in homes,
            'links': sorted(info.get('waypoints', [])),
        })
    out = {
        'scope': ('The galaxy\'s planets for the planet modifiers, by index: labels and map positions, no game values. '
                  'Positions, full health and owners from the frozen war (war-data.json), sectors from planet-sectors.json, '
                  'names and biomes from the community data at https://github.com/helldivers-2/json (MIT, Copyright (c) 2024 '
                  'Helldivers 2 Community; data/licences/helldivers-2-json-LICENSE.txt). Made by scripts/ExportPlanets.py.'),
        'planets': planets,
    }
    # one planet a line
    lines = ',\n  '.join(json.dumps(p, ensure_ascii=False) for p in planets)
    text = '{\n "scope": %s,\n "planets": [\n  %s\n ]\n}\n' % (json.dumps(out['scope'], ensure_ascii=False), lines)
    Path(out_path).write_text(text, encoding='utf-8', newline='\n')
    print(len(planets), 'planets,', sum(1 for p in planets if p['name'].startswith('Planet ')), 'unnamed')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
