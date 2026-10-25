"""Export the HD2R Editor's Spawning pages (Mission Difficulty, Spawn Rosters, Spawn Compositions, Demolition Templates) for HellForge's editor:
the enemy spawn director's rows with the fields HellForge's engine writes (native/mod-loader/director.c). Metadata only:
names, labels, row ids, offsets, storage and ranges; no game values.

Usage: py scripts/ExportDirector.py <HD2Runtime 0.40.1> mappings/director.json

Reads HD2Runtime's domains/enemy_director.lua as literals (never executed): its difficulty, roster and composition rows,
each with its fields (offset, storage, range, whether the HD2R Editor lets it change) and metadata (an enemy's name, a
composition's faction, family, role and groups). Rows are named as the engine finds them:
  rosters       (roster << 16) | row, the roster being difficulty.c's kRosters order (by its descriptor's RVA); the ten
                weights as EnemyRoster offsets 0..36, and level 11's as EnemyRosterLevel11 (with a mod that adds it)
  compositions  (roster << 16) | (family - 1 << 12) | row, family being HD2Runtime's (1 to 8)
  difficulty    its level (1 to 10; 11 with a mod that adds it), as DifficultySettings rows: the HD2R Editor's three
                budgets and HellForge's own objective and patrol fields
  demolition    a demolition template's index (1 to 9; "none" and "Count" have no field), its force as a DemolitionTemplate
                row's +4
A field the HD2R Editor locks (an authored value outside its write range) is left out; authored values are read only to
decide that. The HD2R Editor's preset key of each field is kept ("ed|composition/<key>|<field>").
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
# HellForge's own difficulty fields beside the HD2R Editor's three (mappings/components.json DifficultySettings)
DIFFICULTY_EXTRA = [
    ('difficulty.optional_objectives', 'Optional objectives', 40, 'u32', 0, 10, 'Objectives'),
    ('difficulty.major_objectives', 'Secondary objectives', 44, 'u32', 0, 10, 'Objectives'),
    ('difficulty.patrol_interval_terminids', 'Patrol interval: Terminids', 672, 'f32', 1, 3600, 'Patrols'),
    ('difficulty.patrol_interval_automatons', 'Patrol interval: Automatons', 676, 'f32', 1, 3600, 'Patrols'),
    ('difficulty.patrol_interval_illuminate', 'Patrol interval: Illuminate', 680, 'f32', 1, 3600, 'Patrols'),
    ('difficulty.patrol_cap_terminids', 'Patrol cap: Terminids', 684, 'u32', 0, 128, 'Patrols'),
    ('difficulty.patrol_cap_automatons', 'Patrol cap: Automatons', 688, 'u32', 0, 128, 'Patrols'),
    ('difficulty.patrol_cap_illuminate', 'Patrol cap: Illuminate', 692, 'u32', 0, 128, 'Patrols'),
]
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


def export(runtime):
    difficulty, rosters, compositions, demolition = {}, {}, {}, {}
    for name, r in rows(runtime).items():
        kind = r['kind']
        meta = r.get('metadata') or {}
        editable = [f for f in r.get('fields') or [] if f.get('editable') is not False]
        if kind == 'difficulty':
            level = int(r['key'])
            fields = [field(f['id'], LABELS.get(f['id'], human(f['id'])), 'DifficultySettings', level, f['offset'], f['storage'], f['min'], f['max'],
                            'Budgets', 'ed|difficulty/%d|%s' % (level, f['id']), DIFFICULTY_NOTE, UNITS.get(f['id'])) for f in editable]
            fields += [field(k, label, 'DifficultySettings', level, off, st, lo, hi, section, note=DIFFICULTY_NOTE)
                       for k, label, off, st, lo, hi, section in DIFFICULTY_EXTRA]
            difficulty[str(level)] = {'label': 'Difficulty %d \u00b7 %s' % (level, meta.get('displayName') or level), 'fields': fields}
        elif kind == 'roster':
            roster = ROSTERS[r['descriptor']['rva']]
            row = meta['row']
            record = (roster << 16) | row
            fields = [field(f['id'], 'Weight at difficulty %s' % f['id'].rsplit('_', 1)[1], 'EnemyRoster', record, f['offset'] - 0x30, f['storage'],
                            f['min'], f['max'], 'Spawn weights', 'ed|roster/%s|%s' % (r['key'], f['id'])) for f in editable]
            fields.append(field('spawn.weight_11', 'Weight at difficulty 11', 'EnemyRosterLevel11', record, 0, 'f32', 0, 10000, 'Spawn weights',
                                note='Only with a mod that adds difficulty 11 (it starts as difficulty 10\'s).'))
            rosters[r['key']] = {'label': meta.get('fullDisplayName') or meta.get('displayName') or human(meta.get('path', name).split('/')[-1]),
                                 'faction': meta.get('faction') or meta.get('roster'), 'roster': roster, 'row': row, 'fields': fields}
        elif kind == 'composition':
            roster = FACTIONS[meta['faction']]
            family, row = meta['family'], meta['row']
            record = (roster << 16) | ((family - 1) << 12) | row
            groups = {g['slot']: ', '.join(g.get('candidateNames') or g.get('candidates') or []) for g in meta.get('groups') or []}
            fields = []
            for f in editable:
                m = re.match(r'composition\.group_(\d+)_(count|emission_time)$', f['id'])
                if m:
                    slot = int(m.group(1))
                    section = 'Group %d \u00b7 %s' % (slot + 1, groups.get(slot) or 'unnamed')
                    label = 'Count' if m.group(2) == 'count' else 'Emission time'
                    note = 'The groups total at most 32 enemies (the squad queue\'s capacity).' if m.group(2) == 'count' else None
                    unit = None if m.group(2) == 'count' else 'seconds'
                else:
                    section, label, note, unit = SECTIONS.get(f['id'], 'Other'), human(f['id']), None, UNITS.get(f['id'])
                    if f['id'].endswith('tech_level'):
                        note = 'Difficulty levels; 11 only with a mod that adds it.'
                high = 11 if f['id'] == 'composition.maximum_tech_level' or f['id'] == 'composition.minimum_tech_level' else f['max']
                fields.append(field(f['id'], label, 'EnemyComposition', record, f['offset'], f['storage'], f['min'], high, section,
                                    'ed|composition/%s|%s' % (r['key'], f['id']), note, unit))
            compositions[r['key']] = {'label': '%s \u00b7 Family %d \u00b7 Row %d' % (human(meta['faction']), family, row),
                                      'faction': meta['faction'], 'family': family, 'role': meta.get('role'), 'fields': fields}
        elif kind == 'demolition_template' and editable:
            fields = [field(f['id'], 'Force required', 'DemolitionTemplate', r['index'], f['offset'], f['storage'], f['min'], f['max'],
                            'Demolition', 'ed|demolition_template/%s|%s' % (r['key'], f['id']), DEMOLITION_NOTE) for f in editable]
            demolition[r['key']] = {'label': human(r['key'].replace('_', ' ')), 'fields': fields}
    # level 11: the moved table's row, with a mod that adds it (its fields as level 10's, the HD2R Editor having none)
    if '10' in difficulty:
        difficulty['11'] = {'label': 'Difficulty 11 \u00b7 with a mod that adds it',
                            'fields': [dict(f, record=11, **({'hd2r': None} if 'hd2r' in f else {})) for f in difficulty['10']['fields']]}
        for f in difficulty['11']['fields']:
            f.pop('hd2r', None)
            f['note'] = 'Only with a mod that adds difficulty 11. ' + DIFFICULTY_NOTE
    return difficulty, rosters, compositions, demolition


def main(runtime, out):
    runtime = Path(runtime)
    source = hashlib.sha256((runtime / 'domains' / 'enemy_director.lua').read_bytes().replace(b'\r\n', b'\n')).hexdigest().upper()
    difficulty, rosters, compositions, demolition = export(runtime)
    order = lambda d: dict(sorted(d.items(), key=lambda kv: [int(p) if p.isdigit() else p for p in re.split(r'(\d+)', kv[0])]))
    document = {'format': 1, 'build': '25480438', 'source': source,
                'note': "The HD2R Editor's Spawning pages: the spawn director's rows and the fields HellForge's engine writes "
                        '(scripts/ExportDirector.py, native/mod-loader/director.c).',
                'difficulty': order(difficulty), 'rosters': order(rosters), 'compositions': order(compositions),
                'demolition': order(demolition)}
    Path(out).write_text(json.dumps(document, ensure_ascii=False, indent=1) + '\n', encoding='utf-8', newline='\n')
    print('%d difficulties, %d roster rows, %d compositions, %d demolition templates: %d fields' % (len(difficulty), len(rosters),
          len(compositions), len(demolition), sum(len(t['fields']) for d in (difficulty, rosters, compositions, demolition) for t in d.values())))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
