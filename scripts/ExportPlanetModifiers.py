"""Export the planet modifiers' catalogue (HellForge.Core PlanetModifiers, docs/planet-modifiers.md): the enemies a
planet can be given, its subfactions, and enemy groups that can surge (far more often) or be left out, each with the
galactic war effect definitions the backend serves for them.

Usage: py -I scripts/ExportPlanetModifiers.py data/planet-modifiers.json

Subfactions are type 40 effects (enemy tags), from src/HellForge.LocalBackend/galactic-war-effects.json, with their war
table flags (type 71); their names are the live war's (the community's planetEffects.json, as the War Room shows them).
A surge or a ban is a type 72 effect, the spawn weight of one enemy family in squad compositions: valueTypes [16, 2],
values [family id, percent], 1000 (x10, as the live war's HULK SURGE 1356) for a surge and -100 (left out, as its 1192)
for none (HD2Mods research/galactic-war/effects, "Type 72"). The families are mappings/director.json's (a roster row's
+0): a family joins a group only when every enemy it holds is the group's, so no other enemy surges with it. Three
groups show the live war's card on the planet (type 71: HEAVY ARMOR SURGE 1355, HULK SURGE 1357, DEVASTATOR SURGE 1359,
copied as the live server sent them on 2026-10-14); the others show nothing there. Effect ids from 91000, in this file's
order.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIRST_ID = 91000
SURGE, NONE = 1000, -100
FACTIONS = [('terminids', 2, 'Terminids'), ('automatons', 3, 'Automatons'), ('illuminate', 4, 'Illuminate')]
RACE = {key: race for key, race, _ in FACTIONS}
# The live war's names for the subfactions (community planetEffects.json; the War Room's catalogue).
SUBFACTION_NAMES = {1202: 'Jet Brigade', 1248: 'Incineration Corps', 1360: 'Cyborgs', 1243: 'Predator Strain', 1244: 'Spore Burst Strain',
                    1303: 'Rupture Strain', 1306: 'Dragonroaches', 1307: 'Hive Lords', 1377: 'Mindless Masses', 1380: 'Appropriators',
                    1402: 'Vote Snatchers', 1413: 'Invasion Fleet', 1401: 'Heavy SEAF Presence'}
# key, name, faction, the enemies (mappings/director.json names, without their variant), the live card (or None)
GROUPS = [
    ('hulks', 'Hulks', 'automatons', ['Hulk Scorcher', 'Hulk Bruiser', 'Hulk Obliterator'], 1357),
    ('devastators', 'Devastators', 'automatons', ['Devastator', 'Heavy Devastator', 'Rocket Devastator', 'Conflagration Devastator'], 1359),
    ('tanks', 'Tanks', 'automatons', ['Annihilator Tank', 'Shredder Tank', 'Barrager Tank'], 1355),
    ('berserkers', 'Berserkers', 'automatons', ['Berserker'], None),
    ('factory-striders', 'Factory Striders', 'automatons', ['Factory Strider', 'Vox Engine'], None),
    ('war-striders', 'War Striders', 'automatons', ['War Strider'], None),
    ('gunships', 'Gunships', 'automatons', ['Gunship'], None),
    ('troopers', 'Troopers and Raiders', 'automatons', ['Trooper', 'Raider', 'Marauder', 'Brawler', 'Assault Raider', 'MG Raider', 'Rocket Raider',
                                                         'Scout Raider', 'Commissar', 'Pyro Trooper'], None),
    ('chargers', 'Chargers', 'terminids', ['Charger', 'Charger Behemoth', 'Spore Charger', 'Rupture Charger'], None),
    ('bile-titans', 'Bile Titans', 'terminids', ['Bile Titan', 'Spore Burst Bile Titan'], None),
    ('impalers', 'Impalers', 'terminids', ['Impaler'], None),
    ('stalkers', 'Stalkers', 'terminids', ['Stalker', 'Predator Stalker'], None),
    ('spewers', 'Bile Spewers', 'terminids', ['Bile Spewer', 'Nursing Spewer', 'Rupture Spewer'], None),
    ('commanders', 'Brood Commanders', 'terminids', ['Brood Commander', 'Alpha Commander'], None),
    ('hunters', 'Hunters', 'terminids', ['Hunter', 'Spore Burst Hunter'], None),
    ('warriors', 'Warriors and Hive Guards', 'terminids', ['Warrior', 'Hive Guard', 'Bile Warrior', 'Rupture Warrior', 'Spore Burst Warrior'], None),
    ('scavengers', 'Scavengers and Pouncers', 'terminids', ['Scavenger', 'Bile Spitter', 'Pouncer', 'Spore Burst Scavenger'], None),
    ('shriekers', 'Shriekers', 'terminids', ['Shrieker'], None),
    ('harvesters', 'Harvesters', 'illuminate', ['Harvester'], None),
    ('overseers', 'Overseers', 'illuminate', ['Overseer', 'Elevated Overseer', 'Crescent Overseer'], None),
    ('watchers', 'Watchers', 'illuminate', ['Watcher'], None),
    ('fleshmobs', 'Fleshmobs', 'illuminate', ['Fleshmob'], None),
    ('leviathans', 'Leviathans', 'illuminate', ['Leviathan'], None),
    ('stingrays', 'Stingrays', 'illuminate', ['Stingray'], None),
    ('exomechs', 'Veracitors and Gatekeepers', 'illuminate', ['Veracitor', 'Gatekeeper'], None),
    ('body-horror', 'Crushers and Wretches', 'illuminate', ['Crusher', 'Wretch'], None),
    ('obtruders', 'Obtruders', 'illuminate', ['Obtruder'], None),
    ('voteless', 'Voteless', 'illuminate', ['Voteless'], None),
]
# The live war's cards for three surges (type 71), as its server sent them (2026-10-14 snapshot).
CARDS = [
    {"id": 1355, "gameplayEffectId32": 0, "effectType": 71, "flags": 1, "nameHash": 4084727831, "descriptionFluffHash": 0, "descriptionGamePlayLongHash": 1294895283,
     "descriptionGamePlayShortHash": 1398730330, "valueTypes": [16], "values": [2338177773]},
    {"id": 1357, "gameplayEffectId32": 0, "effectType": 71, "flags": 1, "nameHash": 2054056036, "descriptionFluffHash": 0, "descriptionGamePlayLongHash": 3947262095,
     "descriptionGamePlayShortHash": 2024149994, "valueTypes": [16], "values": [3864345041]},
    {"id": 1359, "gameplayEffectId32": 0, "effectType": 71, "flags": 1, "nameHash": 2317260026, "descriptionFluffHash": 0, "descriptionGamePlayLongHash": 3123640251,
     "descriptionGamePlayShortHash": 304725810, "valueTypes": [16], "values": [1484910334]},
]


def species(label):
    """A director family's enemies: "Charger / Charger Behemoth (Charger Bull) 1A2B3C4D" holds a Charger and a Charger Behemoth."""
    label = re.sub(r'\s+[0-9A-F]{8}$', '', label)
    label = re.sub(r'\s*\([^)]*\)', '', label)
    return [s.strip() for s in label.split(' / ') if s.strip()]


def slug(name):
    return re.sub(r'[^a-z0-9]+', '-', name.lower()).strip('-')


def spawn_weight(effect_id, family, percent):
    return {"id": effect_id, "gameplayEffectId32": 0, "effectType": 72, "flags": 1, "nameHash": 0, "descriptionFluffHash": 0,
            "descriptionGamePlayLongHash": 0, "descriptionGamePlayShortHash": 0, "valueTypes": [16, 2], "values": [family, percent & 0xFFFFFFFF]}


def main(out_path):
    director = json.loads((ROOT / 'mappings' / 'director.json').read_text(encoding='utf-8'))
    shipped = json.loads((ROOT / 'src' / 'HellForge.LocalBackend' / 'galactic-war-effects.json').read_text(encoding='utf-8'))
    definitions = {e['id']: e for e in shipped['effects']}
    effects, next_id = [], FIRST_ID
    subfactions = []
    for s in sorted(shipped['subfactions'], key=lambda s: (s['faction'] or 9, s['id'])):
        name = SUBFACTION_NAMES[s['id']]
        entry = {'key': slug(name), 'name': name, 'faction': s['faction'], 'effect': s['id']}
        if 'flag' in s: entry['flag'] = s['flag']
        subfactions.append(entry)
        effects += [definitions[i] for i in (s['id'], s.get('flag')) if i in definitions]
    groups, used = [], set()
    for key, name, faction, members, card in GROUPS:
        families = [f for f, label in director['families'][faction] if set(species(label)) <= set(members)]
        if not families: sys.exit('no family for ' + key)
        used.update(families)
        group = {'key': key, 'name': name, 'faction': RACE[faction], 'families': families}
        for mode, percent in (('surge', SURGE), ('none', NONE)):
            group[mode] = list(range(next_id, next_id + len(families)))
            effects += [spawn_weight(i, f, percent) for i, f in zip(group[mode], families)]
            next_id += len(families)
        if card: group['card'] = card
        groups.append(group)
    effects += CARDS
    out = {
        'scope': ('The planet modifiers\' catalogue (HellForge.Core PlanetModifiers): the enemies a planet can be given, its '
                  'subfactions (type 40, names the live war\'s) and enemy groups that can surge (type 72 spawn weight x10) or be '
                  'left out (-100), with the galactic war effect definitions served for them. Families from '
                  'mappings/director.json. Made by scripts/ExportPlanetModifiers.py.'),
        'factions': [{'key': key, 'race': race, 'name': name} for key, race, name in FACTIONS],
        'subfactions': subfactions,
        'enemies': groups,
        'effects': effects,
    }
    lines = lambda items: ',\n  '.join(json.dumps(i, ensure_ascii=False) for i in items)
    text = ('{\n "scope": %s,\n "factions": [\n  %s\n ],\n "subfactions": [\n  %s\n ],\n "enemies": [\n  %s\n ],\n "effects": [\n  %s\n ]\n}\n'
            % (json.dumps(out['scope']), lines(out['factions']), lines(subfactions), lines(groups), lines(effects)))
    Path(out_path).write_text(text, encoding='utf-8', newline='\n')
    for g in groups: print('%-16s %d families' % (g['key'], len(g['families'])))
    left = [(fac, '%08X' % f, label) for fac, rows in director['families'].items() for f, label in rows if f not in used]
    print(len(effects), 'effects; families in no group:')
    for row in left: print('  ', *row)


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
