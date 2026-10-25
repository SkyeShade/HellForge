"""Name every stats-schema field in mappings/components.json (so the live editor and the SDK check one layout).

Usage: py scripts/ImportSchemaMappings.py [<HD2Runtime 0.40.1>]

For each field of src/HellForge.Core/stats-schema.json (component records and settings tables) whose bytes no mapping
names, adds a field to its component (one the mappings lack is added from src/HellForge.Core/entity-profile.json),
named after the schema key's last segment, unverified, its source the schema key; its live timing is the component's.
A field that would overlap a mapped one of another type is reported, not added.

With HD2Runtime's checkout, also the HD2R Editor's Enemies fields of components the mappings have (scripts/ExportEntities.py:
an entity's main fields, each damage zone's, the HiveMind's director multipliers), named after the field id's last
segment and a damage zone's by its slot ("zone_12_health"); run ExportEntities.py again afterwards to offer them.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SIZES = {'u8': 1, 'u16': 2, 'u32': 4, 'i32': 4, 'f32': 4, 'u64': 8, 'bytes': 1}
TABLES = {'ProjectileSettings', 'DamageInfo', 'ExplosionSettings', 'StatusEffectSettings', 'StratagemDefinition', 'ArcSettings', 'BeamSettings',
          'MeleeHitVolume'}
# Tables the mappings may not have yet: added from the schema (the loader finds their rows; native/mod-loader/stats-tables.c).
NEW_TABLES = {'MeleeHitVolume': {'stride': 248, 'meaning': "a melee hit volume (static in game.dll, a record by its RVA; HD2Runtime melee_authoring)",
                                 'applies': 'now', 'appliesSource': "copied when a melee attack starts: the next attack uses it (HD2Runtime melee_authoring lifecycle)"}}


def name_of(component):
    n = re.sub(r'ComponentData$', '', component)
    return n


def main(runtime=None):
    schema = json.loads((ROOT / 'src/HellForge.Core/stats-schema.json').read_text(encoding='utf-8'))
    profile = json.loads((ROOT / 'src/HellForge.Core/entity-profile.json').read_text(encoding='utf-8'))['components']
    path = ROOT / 'mappings/components.json'
    m = json.loads(path.read_text(encoding='utf-8'))
    comps = m['components']
    by_component = {c['component']: n for n, c in comps.items()}
    added, conflicts, new_components, refs = 0, [], [], 0
    seen = set()
    # the vehicles' fields (mappings/vehicles.json) as schema fields
    vehicles = json.loads((ROOT / 'mappings/vehicles.json').read_text(encoding='utf-8'))['vehicles']
    extra = {name: {'fields': {f['key']: {'kind': 'component', 'component': f['component'], 'offset': f['offset'], 'storage': f['storage'],
                                          'name': f['label']} for f in v['fields']}} for name, v in vehicles.items()}
    # the weapons' melee fields (mappings/melee.json, added to the schema at load): hit volumes and melee components
    melee = json.loads((ROOT / 'mappings/melee.json').read_text(encoding='utf-8'))['weapons']
    melee_extra = {name: {'fields': {('%s:%s' % (z['key'], f['id'])): {'kind': f['kind'], 'component': f.get('component', ''), 'offset': f['offset'],
                                                                        'storage': f['storage'], 'name': f['label']}
                                     for z in w['zones'] + [{'key': 'tuning', 'fields': w['tuning']}] for f in z['fields']}} for name, w in melee.items()}
    # the fields HellForge adds where the catalogues stop short (mappings/parity.json, added to the schema at load)
    parity = json.loads((ROOT / 'mappings/parity.json').read_text(encoding='utf-8'))['targets']
    parity_extra = {f'{category}/{name}': {'fields': {f['key']: dict({'kind': f['kind'], 'component': f['component'], 'offset': f['offset'],
                                                                      'storage': f['storage'], 'name': f['label']}, **({'ref': f['ref']} if f.get('ref') else {}))
                                                      for f in fields}}
                    for category, targets in parity.items() for name, fields in targets.items()}
    # the HD2R Editor's Enemies fields the mappings lack, for components they have (scripts/ExportEntities.py)
    entity_extra = {}
    if runtime:
        sys.path.insert(0, str(Path(__file__).parent))
        sys.dont_write_bytecode = True
        from ExportEntities import export  # noqa: E402
        unmapped = export(Path(runtime))[3]
        entity_extra = {'entities': {'fields': {('%s@%s+%d' % (u['key'], u['component'], u['offset'])): {
            'kind': 'component' if u['kind'] == 'component' else u['component'], 'component': u['component'], 'offset': u['offset'],
            'storage': u['storage'], 'name': u['label'], 'id': u['key']} for u in unmapped}}}
    for cat, targets in list(schema['targets'].items()) + [('vehicles-extra', extra), ('melee-extra', melee_extra), ('parity-extra', parity_extra),
                                                           ('entities-extra', entity_extra)]:
        for tname, t in targets.items():
            for key, f in t['fields'].items():
                kind = f['kind']
                component = f['component'] if kind == 'component' else kind if kind in TABLES else None
                if component is None:
                    continue
                if component not in by_component and component in NEW_TABLES:
                    t = NEW_TABLES[component]
                    comps[component] = {'component': component, 'table': True, 'stride': t['stride'], 'layout': '25480438', 'meaning': t['meaning'],
                                        'applies': t['applies'], 'appliesVerified': False, 'appliesSource': t['appliesSource'], 'fields': {}}
                    by_component[component] = component
                    new_components.append(component)
                if component not in by_component:
                    if kind != 'component' or component not in profile:
                        continue
                    pc = profile[component]
                    n = name_of(component)
                    comps[n] = {'component': component, 'index': pc['index'], 'stride': pc['stride'], 'layout': '25480438',
                                'meaning': f'{component} (fields from the stats schema)', 'applies': 'respawn', 'appliesVerified': False,
                                'appliesSource': 'a component record: read when the entity is made', 'fields': {}}
                    by_component[component] = n
                    new_components.append(n)
                c = comps[by_component[component]]
                off, size = f['offset'], SIZES[f['storage']]
                ident = (component, off, f['storage'])
                if ident in seen:
                    continue
                seen.add(ident)
                covered = None
                for fn, fv in c['fields'].items():
                    fsize = SIZES[fv['type']] * fv.get('count', 1)
                    if off < fv['offset'] + fsize and fv['offset'] < off + size:
                        covered = (fn, fv)
                        break
                if covered:
                    fn, fv = covered
                    if fv['type'] != f['storage'] or (off - fv['offset']) % SIZES[fv['type']]:
                        conflicts.append(f'{component} +{off} {f["storage"]} ({cat}/{tname} {key}) overlaps {fn} +{fv["offset"]} {fv["type"]}')
                    elif f.get('ref') and fv.get('ref') != f['ref'] and fv['offset'] == off and fv.get('count', 1) == 1:
                        if fv.get('ref'):
                            conflicts.append(f'{component} +{off}: the schema says {f["ref"]} ({key}), the mappings {fv["ref"]}')
                        else:
                            fv['ref'] = f['ref']  # a reference the mappings named as a plain number
                            refs += 1
                    continue
                if off + size > c['stride']:
                    conflicts.append(f'{component} +{off}: outside its {c["stride"]}-byte record')
                    continue
                if cat == 'entities-extra':
                    key = f['id']  # its HD2Runtime field id, without the entry's address suffix
                last = key.split('.')
                # a slot of a list ("presentation.traits.1", "fire_rate.modes.x"): named with its list
                last = '_'.join(last[-2:]) if len(last) > 1 and (last[-1].isdigit() or last[-1] in ('x', 'y', 'z')) else last[-1]
                zoned = cat == 'vehicles-extra' or (cat in ('parity-extra', 'entities-extra') and key.startswith('zone'))  # "zone_3_armor"
                if cat in ('parity-extra', 'entities-extra') and key.startswith('zone.'):
                    key = 'zone_%d%s' % ((off - 520) // 552, key[4:])  # a page's single zone, by its number
                base = re.sub(r'[^a-z0-9_]', '_', (key.replace('.', '_') if zoned else last).lower()) or 'field'
                if base[0].isdigit():
                    base = 'f_' + base
                field = base
                i = 2
                while field in c['fields']:
                    field = f'{base}_{i}'
                    i += 1
                c['fields'][field] = {'offset': off, 'type': f['storage'], 'meaning': f.get('name') or key, 'verified': False,
                                      'source': f'HD2Runtime enemy catalogue {f["id"]}' if cat == 'entities-extra' else f'stats-schema {key}'}
                if f.get('ref'):
                    c['fields'][field]['ref'] = f['ref']
                added += 1
    for c in comps.values():
        c['fields'] = dict(sorted(c['fields'].items(), key=lambda kv: kv[1]['offset']))
    path.write_text(json.dumps(m, indent=2, ensure_ascii=False) + '\n', encoding='utf-8', newline='\n')
    print(f'{added} fields added, {refs} references named, {len(new_components)} components added {new_components}, {len(conflicts)} conflicts')
    for c in conflicts:
        print('  ' + c)


if __name__ == '__main__':
    if len(sys.argv) > 2:
        sys.exit(__doc__)
    main(*sys.argv[1:])
