"""Export the HD2R Editor's Spawning pages (Mission Difficulty, Spawn Rosters, Spawn Compositions, Demolition Templates) for HellForge's editor:
the enemy spawn director's rows with the fields HellForge's engine writes (native/mod-loader/director.c), and what the
summoning enemies call up (native/mod-loader/enemy_summons.c). Metadata only: names, labels, row ids, family ids,
offsets, storage and ranges; no game values.

Usage: py scripts/ExportDirector.py <HD2Runtime 0.40.1> mappings/director.json

Reads HD2Runtime's domains/enemy_director.lua as literals (never executed): its difficulty, roster and composition rows,
each with its fields (offset, storage, range, whether the HD2R Editor lets it change) and metadata (an enemy's name, a
composition's faction, family, role and groups). Rows are named as the engine finds them:
  rosters       (roster << 16) | row, the roster being difficulty.c's kRosters order (by its descriptor's RVA); the ten
                weights as EnemyRoster offsets 0..36, and level 11's as EnemyRosterLevel11 (with a mod that adds it)
  compositions  (roster << 16) | (family - 1 << 12) | row, family being HD2Runtime's (1 to 8); named by their role (the
                family's spawn channel, FAMILY_ROLES) and the enemies their groups draw. Each group's enemy family is a
                pick of the faction roster's families (a roster row's +0, its family id); the first empty group is
                offered too, to add enemies
  difficulty    its level (1 to 10; 11 with a mod that adds it), as DifficultySettings rows: the HD2R Editor's three
                budgets and HellForge's own objective, patrol, traveller and straggler fields
  demolition    a demolition template's index (1 to 9; "none" and "Count" have no field), its force as a DemolitionTemplate
                row's +4
  summons       per enemy (mappings/entities.json's key), its summoning ability's EnemySummon row: the family it calls
                up (+0) and how many each call (+4)
A field the HD2R Editor locks (an authored value outside its write range) is left out; authored values are read only to
decide that, and a row's family id from its bytes. The HD2R Editor's preset key of each field is kept
("ed|composition/<key>|<field>").
Credit: HD2R Editor, copyright 2026 SkyeShade (https://github.com/SkyeShade/HD2Runtime-Editor), whose pages this follows.
"""
import hashlib
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
sys.dont_write_bytecode = True
from ExportEntities import parse_at, human  # noqa: E402

# difficulty.c kRosters, in its order: a roster by its descriptor's RVA.
ROSTERS = {0x21CBD60: 0, 0x21CE910: 1, 0x21CBCB0: 2, 0x21CB3C0: 3, 0x21CB890: 4}
FACTIONS = {'terminids': 0, 'illuminate': 1, 'automatons': 2}
SECTIONS = {'composition.weight': 'Selection', 'composition.cost_multiplier': 'Selection', 'composition.post_schedule_duration': 'Selection',
            'composition.minimum_tech_level': 'Eligibility', 'composition.maximum_tech_level': 'Eligibility',
            'composition.minimum_players': 'Eligibility', 'composition.maximum_players': 'Eligibility'}
# HellForge's own difficulty fields beside the HD2R Editor's three (mappings/components.json DifficultySettings), each
# triplet Terminids, Automatons, Illuminate. The director copies the row every frame (0x94FE02, to rbp+0x2B0) and reads:
#   patrols     +0x2A0 interval, +0x2AC the most living patrol enemies, before the HiveMind's progress and player factors
#               (up to about 2.9 times at level 10 with four players). A patrol's SpawnSquad (flags 0x5C, bit 6) skips the
#               game's 448-enemy gate (0x952EB0), and no array is sized by the cap: only frame rate bounds it
#   travellers  +0x2E0 interval, +0x2EC the most alive (spawn reason 7, Family 5: 0x95AAE0)
#   stragglers  +0x2F8 interval (0x95B250 sets its timer from it, rbp+0x5A8), +0x310 the most sent at once (rbp+0x5C0;
#               one squad), +0x31C the cap on living map guards and stragglers together (rbp+0x5CC, against the
#               director's +0x618 and +0x630) under which Family 6 tops them up (spawn reason 8). +0x304 is left out
PATROL_CAP_NOTE = ('The most living patrol enemies of this faction; the mission scales it by its progress and the player count '
                   '(up to about 2.9 times at difficulty 10 with four players). Nothing else limits patrols: hundreds cost frame rate.')
TRAVELLER_NOTE = ('Travellers roam the map (Family 5 compositions: Shriekers and Dragonroaches, Gunships, War Striders): '
                  'the most alive at once, and the seconds between them.')
STRAGGLER_NOTE = ("Stragglers top up the map's guards (Family 6 compositions): while fewer guards and stragglers than the cap "
                  'are alive, the director sends up to the batch each interval.')
DIFFICULTY_EXTRA = [
    ('difficulty.optional_objectives', 'Optional objectives', 40, 'u32', 0, 10, 'Objectives', None),
    ('difficulty.major_objectives', 'Secondary objectives', 44, 'u32', 0, 10, 'Objectives', None),
    ('difficulty.patrol_interval_terminids', 'Patrol interval: Terminids', 672, 'f32', 1, 3600, 'Patrols', None),
    ('difficulty.patrol_interval_automatons', 'Patrol interval: Automatons', 676, 'f32', 1, 3600, 'Patrols', None),
    ('difficulty.patrol_interval_illuminate', 'Patrol interval: Illuminate', 680, 'f32', 1, 3600, 'Patrols', None),
    ('difficulty.patrol_cap_terminids', 'Patrol cap: Terminids', 684, 'u32', 0, 1000, 'Patrols', PATROL_CAP_NOTE),
    ('difficulty.patrol_cap_automatons', 'Patrol cap: Automatons', 688, 'u32', 0, 1000, 'Patrols', PATROL_CAP_NOTE),
    ('difficulty.patrol_cap_illuminate', 'Patrol cap: Illuminate', 692, 'u32', 0, 1000, 'Patrols', PATROL_CAP_NOTE),
    ('difficulty.traveller_interval_terminids', 'Traveller interval: Terminids', 736, 'f32', 1, 3600, 'Travellers', TRAVELLER_NOTE),
    ('difficulty.traveller_interval_automatons', 'Traveller interval: Automatons', 740, 'f32', 1, 3600, 'Travellers', TRAVELLER_NOTE),
    ('difficulty.traveller_interval_illuminate', 'Traveller interval: Illuminate', 744, 'f32', 1, 3600, 'Travellers', TRAVELLER_NOTE),
    ('difficulty.traveller_cap_terminids', 'Traveller cap: Terminids', 748, 'u32', 0, 100, 'Travellers', TRAVELLER_NOTE),
    ('difficulty.traveller_cap_automatons', 'Traveller cap: Automatons', 752, 'u32', 0, 100, 'Travellers', TRAVELLER_NOTE),
    ('difficulty.traveller_cap_illuminate', 'Traveller cap: Illuminate', 756, 'u32', 0, 100, 'Travellers', TRAVELLER_NOTE),
    ('difficulty.straggler_interval_terminids', 'Straggler interval: Terminids', 760, 'f32', 1, 3600, 'Stragglers', STRAGGLER_NOTE),
    ('difficulty.straggler_interval_automatons', 'Straggler interval: Automatons', 764, 'f32', 1, 3600, 'Stragglers', STRAGGLER_NOTE),
    ('difficulty.straggler_interval_illuminate', 'Straggler interval: Illuminate', 768, 'f32', 1, 3600, 'Stragglers', STRAGGLER_NOTE),
    ('difficulty.straggler_batch_terminids', 'Straggler batch: Terminids', 784, 'u32', 0, 32, 'Stragglers', STRAGGLER_NOTE),
    ('difficulty.straggler_batch_automatons', 'Straggler batch: Automatons', 788, 'u32', 0, 32, 'Stragglers', STRAGGLER_NOTE),
    ('difficulty.straggler_batch_illuminate', 'Straggler batch: Illuminate', 792, 'u32', 0, 32, 'Stragglers', STRAGGLER_NOTE),
    ('difficulty.straggler_cap_terminids', 'Guard and straggler cap: Terminids', 796, 'u32', 0, 448, 'Stragglers', STRAGGLER_NOTE),
    ('difficulty.straggler_cap_automatons', 'Guard and straggler cap: Automatons', 800, 'u32', 0, 448, 'Stragglers', STRAGGLER_NOTE),
    ('difficulty.straggler_cap_illuminate', 'Guard and straggler cap: Illuminate', 804, 'u32', 0, 448, 'Stragglers', STRAGGLER_NOTE),
]
DIFFICULTY_UNITS = {'Travellers': 'seconds', 'Stragglers': 'seconds'}
# A composition family's role: the spawn reason its consumer hands SpawnSquad (0x953580; the reasons' names at
# 0x2142590): 1 patrols (0x959950, reason 1), 2 guard posts (0x94E580, reason 0; every row empty in this build), 3 map
# guards (the guardforce budget, 0x90E250), 4 reinforcements (breaches and drops: 0x957E10, DispatchReinforcement),
# 5 travellers (0x95AAE0, reason 7), 6 stragglers (0x95B250, reason 8), 7 convoys (0x868840, reason 5: the Automatons'
# Factory Striders and siege engines), 8 air support (0x95C110, reason 9: the Illuminate's attack ships).
FAMILY_ROLES = {1: 'patrol', 2: 'guard', 3: 'map', 4: 'reinforcement', 5: 'traveller', 6: 'straggler', 7: 'convoy', 8: 'air_support'}
ROLE_NAMES = {'patrol': 'Patrol', 'guard': 'Guard post', 'map': 'Map guard', 'reinforcement': 'Reinforcement', 'traveller': 'Traveller',
              'straggler': 'Straggler', 'convoy': 'Convoy', 'air_support': 'Air support'}
GROUP_KEY_NOTE = ("The enemy family the group draws from: one of its enemies by spawn weight at the mission's difficulty. "
                  'A family with no weight there spawns nothing. Applies from the next mission.')
EMPTY_GROUP_NOTE = 'An empty group: pick a family and a count to add enemies to the composition.'
# The summoning abilities (native/mod-loader/enemy_summons.c): the enemy whose page shows it (entities.json), the
# ability, the section, the count's label and the note.
SUMMONS = [
    ('Impaler', 333, 'Summons: tentacles', 'Enemies per strike',
     "The Impaler's strike calls up its tentacles from the ground: this many of this family each strike."),
    ('warrior_big_tier2', 463, 'Summons: warriors', 'Enemies per call (three calls)',
     'The Alpha Commander digs warriors out of the ground in front of it, in three calls: this many of this family each call. '
     "The ability is the game's 463: any enemy that runs it summons the same."),
]
SUMMON_NOTE = " Picked from the mission faction's roster by spawn weight at its difficulty. Only the host's choice counts."
LABELS = {'difficulty.encounter_points': 'Encounter points', 'difficulty.guardforce_points': 'Guard force points',
          'difficulty.shared_spawner_cooldown': 'Spawner cooldown'}
UNITS = {'difficulty.shared_spawner_cooldown': 'seconds', 'composition.post_schedule_duration': 'seconds',
         'composition.group_emission': 'seconds'}
DIFFICULTY_NOTE = 'Applies from the next mission; only the host\'s values decide what spawns.'
DEMOLITION_NOTE = ('Shared by every destruction stage that names this template. Stages are passed in order: an earlier '
                   'unmet stage can keep a later one from being reached.')


def rows(runtime):
    out = {}
    for line in (runtime / 'domains' / 'enemy_director.lua').read_text(encoding='utf-8-sig').splitlines():
        m = re.match(r'D\.rows\[("(?:\\.|[^"\\])*")\]=\(function\(\)return ', line)
        if m:
            out[json.loads(m.group(1))] = parse_at(line, m.end())[0]
    return out


def field(key, label, kind, record, offset, storage, low, high, section, hd2r=None, note=None, unit=None):
    f = {'key': key, 'label': label, 'section': section, 'kind': kind, 'record': record, 'offset': offset, 'storage': storage,
         'min': low, 'max': high, 'unit': unit, 'hd2r': hd2r, 'note': note}
    return {k: v for k, v in f.items() if v is not None}


def species(name):
    """An enemy's name without its variant: "Hunter - Hunter Tier 1" is a Hunter."""
    return (name or '').split(' - ')[0].strip() or name


def family_id(hex_bytes, at):
    """A row's u32 (a family id) at `at`, from its bytes as hex."""
    return int.from_bytes(bytes.fromhex(hex_bytes[at * 2:at * 2 + 8]), 'little')


def families(rows_by_name, rosters):
    """Each primary roster's families in its order: faction -> [(id, label)], a label being its enemies' names, with
    their variants where two families would read the same or one variant is not its name (an id where still the same)."""
    out = {}
    for faction, roster in FACTIONS.items():
        names, variants = {}, {}
        for r in rows_by_name.values():
            if r['kind'] != 'roster' or ROSTERS[r['descriptor']['rva']] != roster:
                continue
            label, key = rosters[r['key']]['label'], family_id(r['hex'], 0)
            for into, part in ((names, species(label)), (variants, label.split(' - ')[-1].strip())):
                if part not in into.setdefault(key, []):
                    into[key].append(part)
        labels = {k: ' / '.join(v) for k, v in names.items()}
        same = {v for v in labels.values() if list(labels.values()).count(v) > 1}
        for k, v in list(labels.items()):
            if v in same or (len(variants[k]) == 1 and variants[k] != names[k]):
                more = ' +%d' % (len(variants[k]) - 3) if len(variants[k]) > 3 else ''
                labels[k] = '%s (%s%s)' % (v, ', '.join(variants[k][:3]), more)
        twice = {v for v in labels.values() if list(labels.values()).count(v) > 1}
        out[faction] = [(k, '%s %08X' % (v, k) if v in twice else v) for k, v in labels.items()]
    return out


def units(meta, labels):
    """What a composition's groups draw, for its name: "Hunter, Scavenger", "empty" for none."""
    seen = []
    for g in sorted(meta.get('groups') or [], key=lambda g: g['slot']):
        for n in g.get('candidateNames') or [labels.get(g.get('family'), 'unidentified')]:
            if species(n) not in seen:
                seen.append(species(n))
    if not seen:
        return 'empty'
    return ', '.join(seen[:3]) + (' +%d' % (len(seen) - 3) if len(seen) > 3 else '')


def composition_fields(r, meta, record, options):
    """A composition's fields: Selection and Eligibility as the HD2R Editor has them, then each group with its family
    (a pick of `options`, a faction of "families"), count and emission time, and the first empty group to add enemies."""
    editable = [f for f in r.get('fields') or [] if f.get('editable') is not False]
    groups = {g['slot']: ', '.join(g.get('candidateNames') or g.get('candidates') or []) for g in meta.get('groups') or []}
    keys = [family_id(r['hex'], slot * 12) for slot in range(8)]
    empty = next((slot for slot in range(8) if keys[slot] == 0), None)
    fields, by_slot = [], {}
    for f in editable:
        m = re.match(r'composition\.group_(\d+)_(count|emission_time)$', f['id'])
        hd2r = 'ed|composition/%s|%s' % (r['key'], f['id'])
        if m:
            by_slot.setdefault(int(m.group(1)), {})[m.group(2)] = (f, hd2r)
            continue
        note = 'Difficulty levels; 11 only with a mod that adds it.' if f['id'].endswith('tech_level') else None
        high = 11 if f['id'] in ('composition.maximum_tech_level', 'composition.minimum_tech_level') else f['max']
        fields.append(field(f['id'], human(f['id']), 'EnemyComposition', record, f['offset'], f['storage'], f['min'], high,
                            SECTIONS.get(f['id'], 'Other'), hd2r, note, UNITS.get(f['id'])))
    for slot in range(8):
        if keys[slot] == 0 and slot != empty:
            continue
        section = 'Group %d · %s' % (slot + 1, groups.get(slot) or ('empty' if keys[slot] == 0 else 'unnamed'))
        key = field('composition.group_%d_key' % slot, 'Enemy family', 'EnemyComposition', record, slot * 12, 'u32', None, None,
                    section, note=EMPTY_GROUP_NOTE + ' ' + GROUP_KEY_NOTE if keys[slot] == 0 else GROUP_KEY_NOTE)
        key['families'] = options
        fields.append(key)
        own = by_slot.get(slot, {})
        if keys[slot] == 0:
            own = {'count': ({'id': 'composition.group_%d_count' % slot, 'offset': slot * 12 + 4, 'storage': 'u32', 'min': 0, 'max': 32}, None),
                   'emission_time': ({'id': 'composition.group_%d_emission_time' % slot, 'offset': slot * 12 + 8, 'storage': 'f32',
                                      'min': 0, 'max': 3600}, None)}
        for part, label, note, unit in (('count', 'Count', "The groups total at most 32 enemies (the squad queue's capacity).", None),
                                        ('emission_time', 'Emission time', None, 'seconds')):
            if part in own:
                f, hd2r = own[part]
                fields.append(field(f['id'], label, 'EnemyComposition', record, f['offset'], f['storage'], f['min'], f['max'], section,
                                    hd2r, note, unit))
    return fields


def difficulty_fields(r, level):
    editable = [f for f in r.get('fields') or [] if f.get('editable') is not False]
    fields = [field(f['id'], LABELS.get(f['id'], human(f['id'])), 'DifficultySettings', level, f['offset'], f['storage'], f['min'], f['max'],
                    'Budgets', 'ed|difficulty/%d|%s' % (level, f['id']), DIFFICULTY_NOTE, UNITS.get(f['id'])) for f in editable]
    for k, label, off, st, lo, hi, section, note in DIFFICULTY_EXTRA:
        unit = DIFFICULTY_UNITS.get(section) if st == 'f32' else None
        fields.append(field(k, label, 'DifficultySettings', level, off, st, lo, hi, section,
                            note=(note + ' ' if note else '') + DIFFICULTY_NOTE, unit=unit))
    return fields


def roster_fields(r, record):
    editable = [f for f in r.get('fields') or [] if f.get('editable') is not False]
    fields = [field(f['id'], 'Weight at difficulty %s' % f['id'].rsplit('_', 1)[1], 'EnemyRoster', record, f['offset'] - 0x30, f['storage'],
                    f['min'], f['max'], 'Spawn weights', 'ed|roster/%s|%s' % (r['key'], f['id'])) for f in editable]
    fields.append(field('spawn.weight_11', 'Weight at difficulty 11', 'EnemyRosterLevel11', record, 0, 'f32', 0, 10000, 'Spawn weights',
                        note='Only with a mod that adds difficulty 11 (it starts as difficulty 10\'s).'))
    return fields


def summons(options):
    """Per enemy, its summoning ability's row: the family it calls up and how many each call."""
    out = {}
    for enemy, ability, section, count, note in SUMMONS:
        family = field('summon.family', 'Summoned enemy family', 'EnemySummon', ability, 0, 'u32', None, None, section, note=note + SUMMON_NOTE)
        family['families'] = options
        out[enemy] = [family, field('summon.count', count, 'EnemySummon', ability, 4, 'u32', 0, 16, section,
                                    note='0 summons none; at most 16 a call. ' + note + SUMMON_NOTE)]
    return out


def export(runtime):
    difficulty, rosters, compositions, demolition = {}, {}, {}, {}
    rows_by_name = rows(runtime)
    for name, r in rows_by_name.items():
        if r['kind'] == 'roster':
            meta = r.get('metadata') or {}
            roster, row = ROSTERS[r['descriptor']['rva']], meta['row']
            rosters[r['key']] = {'label': meta.get('fullDisplayName') or meta.get('displayName') or human(meta.get('path', name).split('/')[-1]),
                                 'faction': meta.get('faction') or meta.get('roster'), 'roster': roster, 'row': row,
                                 'fields': roster_fields(r, (roster << 16) | row)}
    picks = families(rows_by_name, rosters)
    options = {faction: faction for faction in picks}
    for name, r in rows_by_name.items():
        kind = r['kind']
        meta = r.get('metadata') or {}
        editable = [f for f in r.get('fields') or [] if f.get('editable') is not False]
        if kind == 'difficulty':
            level = int(r['key'])
            difficulty[str(level)] = {'label': 'Difficulty %d · %s' % (level, meta.get('displayName') or level),
                                      'fields': difficulty_fields(r, level)}
        elif kind == 'composition':
            roster = FACTIONS[meta['faction']]
            family, row = meta['family'], meta['row']
            record = (roster << 16) | ((family - 1) << 12) | row
            role = FAMILY_ROLES[family]
            labels = dict(picks[meta['faction']])
            for g in meta.get('groups') or []:
                g['family'] = family_id(r['hex'], g['slot'] * 12)
            compositions[r['key']] = {'label': '%s: %s' % (ROLE_NAMES[role], units(meta, labels)), 'faction': meta['faction'],
                                      'family': family, 'row': row, 'role': role,
                                      'fields': composition_fields(r, meta, record, options[meta['faction']])}
        elif kind == 'demolition_template' and editable:
            fields = [field(f['id'], 'Force required', 'DemolitionTemplate', r['index'], f['offset'], f['storage'], f['min'], f['max'],
                            'Demolition', 'ed|demolition_template/%s|%s' % (r['key'], f['id']), DEMOLITION_NOTE) for f in editable]
            demolition[r['key']] = {'label': human(r['key'].replace('_', ' ')), 'fields': fields}
    # level 11: the moved table's row, with a mod that adds it (its fields as level 10's, the HD2R Editor having none)
    if '10' in difficulty:
        difficulty['11'] = {'label': 'Difficulty 11 · with a mod that adds it',
                            'fields': [dict(f, record=11, **({'hd2r': None} if 'hd2r' in f else {})) for f in difficulty['10']['fields']]}
        for f in difficulty['11']['fields']:
            f.pop('hd2r', None)
            f['note'] = 'Only with a mod that adds difficulty 11. ' + f['note']
    return difficulty, rosters, compositions, demolition, summons('terminids'), {f: [[k, label] for k, label in v] for f, v in picks.items()}


def main(runtime, out):
    runtime = Path(runtime)
    source = hashlib.sha256((runtime / 'domains' / 'enemy_director.lua').read_bytes().replace(b'\r\n', b'\n')).hexdigest().upper()
    difficulty, rosters, compositions, demolition, summoned, picks = export(runtime)
    order = lambda d: dict(sorted(d.items(), key=lambda kv: [int(p) if p.isdigit() else p for p in re.split(r'(\d+)', kv[0])]))
    document = {'format': 1, 'build': '25480438', 'source': source,
                'note': "The HD2R Editor's Spawning pages: the spawn director's rows and the fields HellForge's engine writes "
                        '(scripts/ExportDirector.py, native/mod-loader/director.c).',
                'difficulty': order(difficulty), 'rosters': order(rosters), 'compositions': order(compositions),
                'demolition': order(demolition), 'summons': summoned,
                'families': picks}
    Path(out).write_text(json.dumps(document, ensure_ascii=False, indent=1) + '\n', encoding='utf-8', newline='\n')
    print('%d difficulties, %d roster rows, %d compositions, %d demolition templates, %d summons: %d fields' % (len(difficulty), len(rosters),
          len(compositions), len(demolition), len(summoned),
          sum(len(t['fields']) for d in (difficulty, rosters, compositions, demolition) for t in d.values())))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
