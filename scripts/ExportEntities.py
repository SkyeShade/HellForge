"""Export the HD2R Editor's Enemies pages (Terminids, Automatons, Illuminate, Structures, Other Entities) for HellForge's
editor: every entity HD2Runtime's enemy catalogue lists, with the fields HellForge's engine can write. Metadata only:
names, labels, sections, resources, records, offsets, storage and ranges; no game values.

Usage: py scripts/ExportEntities.py <HD2Runtime 0.40.1> mappings/entities.json [blocked.json]

Reads HD2Runtime's generated enemy domains as literals (never executed): domains/enemy_authoring.lua (the original
catalogue and the field schema) merged with domains/enemy_controls.lua (the expanded one: every entity class, its
zones, attacks and components), as domains/enemy_writes.lua merges them, and schemas/enemy_fields.json for units. Rows
follow the HD2R Editor (1.0.0, src/editor/enemies.lua entity_rows): a field with a numeric authored value is a row, keyed
'<path>:<zone or attack>:<field id>' (its preset key is 'en|<entity>|<that>'); a field HD2Runtime does not let the
editor change, or whose authored value lies outside its write range or precision, is locked. Authored values are read
only to decide that; none is written.

A row is editable here when HellForge's engine maps its bytes: the component (or settings table) and the exact offset in
mappings/components.json. With a third path, the rest are written there by entity and reason: the work list for
mapping more (artifacts/, not the repository).
Credit: HD2R Editor, copyright 2026 SkyeShade (https://github.com/SkyeShade/HD2Runtime-Editor), whose adapter this follows.
"""
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SETTINGS = {'damage': 'DamageInfo', 'projectile': 'ProjectileSettings', 'explosion': 'ExplosionSettings', 'arc': 'ArcSettings',
            'beam': 'BeamSettings'}
LIMITS = {'u8': (0, 255), 'u16': (0, 65535), 'u32': (0, 4294967295), 'i8': (-128, 127), 'i16': (-32768, 32767),
          'i32': (-2147483648, 2147483647)}
# The HD2R Editor's sections (enemies.lua TITLES), by a field id's first word.
TITLES = {'unit': 'Size & Geometry', 'spawner': 'Spawning', 'hive': 'Director Multipliers', 'transport': 'Transport',
          'breach': 'Bug Breach', 'shield': 'Shield', 'jammer': 'Stratagem Jammer', 'behavior': 'Behavior', 'entity': 'Health & Armor',
          'gore': 'Destruction'}
NOTES = {
    'unit.scale_min': 'Creation-time scale: applies to newly spawned units. Physics and hit geometry need gameplay testing.',
    'jammer.radius': 'Positive 3D radius. Native zero or negative values bypass the distance check; they do not disable the jammer.',
    'jammer.initially_enabled': 'Initialization only. Does not switch off a jammer that already exists.',
}
WORDS = {'ap': 'AP', 'rpm': 'RPM', 'hp': 'HP', 'id': 'ID', 'ui': 'UI', 'fov': 'FOV', 'aoe': 'AoE', 'ems': 'EMS', 'mg': 'MG'}

# ------------------------------------------------------------------------------------------- Lua literals --
_TOK = re.compile(r'\s*(?:--[^\n]*\n\s*)*("(?:\\.|[^"\\])*"|0x[0-9A-Fa-f]+|-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?|true|false|nil|'
                  r'[A-Za-z_][A-Za-z_0-9]*|[{}\[\]=,;])')
_DEC = re.compile(r'\\(\d{1,3})')


def parse_at(src, pos):
    """The Lua table literal starting at src[pos] ('{'), as Python data, and where it ends. Never executes anything."""
    def take():
        nonlocal pos
        m = _TOK.match(src, pos)
        if not m:
            raise ValueError('bad literal at %d: %r' % (pos, src[pos:pos + 60]))
        pos = m.end()
        return m.group(1)

    def peek():
        m = _TOK.match(src, pos)
        return m.group(1) if m else None

    def value(t=None):
        t = take() if t is None else t
        if t != '{':
            if t in ('nil', 'true', 'false'):
                return {'nil': None, 'true': True, 'false': False}[t]
            if t.startswith('"'):
                return json.loads(_DEC.sub(lambda m: '\\u%04x' % int(m.group(1)), t))
            return int(t, 16) if t.lower().startswith('0x') else json.loads(t)
        arr, mp = [], {}
        t = take()
        while t != '}':
            if t == '[':
                k = value()
                if take() != ']' or take() != '=':
                    raise ValueError('bad keyed entry at %d' % pos)
                mp[k] = value()
            elif re.match(r'[A-Za-z_]', t) and t not in ('true', 'false', 'nil') and peek() == '=':
                take()
                mp[t] = value()
            else:
                arr.append(value(t))
            t = take()
            if t in (',', ';'):
                t = take()
            elif t != '}':
                raise ValueError('missing separator at %d' % pos)
        if arr and mp:
            for i, v in enumerate(arr, 1):
                mp.setdefault(i, v)
            return mp
        return mp if mp else arr

    return value(), pos


def returned(path):
    """A generated 'return {...}' file."""
    src = Path(path).read_text(encoding='utf-8-sig')
    return parse_at(src, re.search(r'\breturn\s*{', src).end() - 1)[0]


def controls(path):
    """domains/enemy_controls.lua: its header table, and each 'D.enemies["name"]=(function() local e={...} ...
    e.fields=function()return {...} end ... end)()' line's entry with its fields."""
    lines = Path(path).read_text(encoding='utf-8-sig').splitlines()
    header = parse_at(lines[1], lines[1].index('{'))[0]
    enemies = {}
    for line in lines:
        m = re.match(r'D\.enemies\[("(?:\\.|[^"\\])*")\]=', line)
        if not m:
            continue
        entry, end = parse_at(line, line.index('{', m.end()))
        at = line.index('e.fields=function()return', end) + len('e.fields=function()return')
        entry['fields'] = parse_at(line, line.index('{', at))[0]
        enemies[json.loads(m.group(1))] = entry
    header['enemies'] = enemies
    return header


def lst(x):
    return x if isinstance(x, list) else [x[k] for k in sorted(k for k in x if isinstance(k, int))] if isinstance(x, dict) else []


def merged(runtime):
    """enemy_writes.lua's database: the original catalogue, each expanded entry's names, zones, attacks and fields
    added (a field the original has already is kept)."""
    database = returned(runtime / 'domains' / 'enemy_authoring.lua')
    expanded = controls(runtime / 'domains' / 'enemy_controls.lua')
    for key, schema in expanded['schema'].items():
        database['schema'].setdefault(key, schema)
    for name, extra in expanded['enemies'].items():
        entry = database['enemies'].setdefault(name, dict(extra, fields=[]))
        for key in ('displayName', 'gameName', 'variantLabel', 'fullDisplayName', 'components', 'mountedEntities'):
            entry[key] = extra.get(key)
        entry['attacks'] = lst(entry.get('attacks'))
        have = {a['id'] for a in entry['attacks']}
        entry['attacks'] += [a for a in lst(extra.get('attacks')) if a['id'] not in have]
        for zone in lst(entry.get('zones')):
            for new in lst(extra.get('zones')):
                if zone['id'] == new['id']:
                    zone.update({k: new.get(k) for k in ('name', 'nameCandidates', 'actors')})
        entry['fields'] = lst(entry.get('fields'))
        ident = {(f.get('id'), f.get('zone'), f.get('path'), f.get('attack')) for f in entry['fields']}
        entry['fields'] += [f for f in lst(extra['fields']) if (f.get('id'), f.get('zone'), f.get('path'), f.get('attack')) not in ident]
    return database


# ---------------------------------------------------------------------------------------------- the export --
def human(s):
    words = [WORDS.get(w, w) for w in str(s or '').split('.')[-1].split('_') if w]
    text = ' '.join(words)
    return text[:1].upper() + text[1:]


def category(entry):
    if entry.get('kind') == 'structure':
        return 'structures'
    if entry.get('kind') == 'enemy' and entry.get('faction') in ('terminids', 'automatons', 'illuminate'):
        return entry['faction']
    return 'other'


def representable(value, storage, integer):
    """The HD2R Editor's precision lock (catalog.lua): a value it cannot show and put back exactly is not editable."""
    if integer:
        return value % 1 == 0
    rounded = float('%.3f' % value)
    if rounded == value:
        return True
    if storage == 'f32':
        try:
            if struct.pack('<f', rounded) == struct.pack('<f', value):
                return True
        except OverflowError:
            pass
    return abs(rounded - value) <= 1e-9 * max(1, abs(value))


SIZES = {'u8': 1, 'u16': 2, 'u32': 4, 'i32': 4, 'f32': 4, 'u64': 8}


def mapped():
    """HellForge's engine fields: component or settings table name -> the (offset, storage) it maps, every element of an
    array field included (SdkMappings.FieldAt: a field of that type at the offset or inside an array of it)."""
    data = json.loads((REPO / 'mappings' / 'components.json').read_text(encoding='utf-8'))['components']
    return {c['component']: {(f['offset'] + i * SIZES.get(f['type'], 1), f['type']) for f in c['fields'].values() for i in range(f.get('count', 1))}
            for c in data.values()}


def export(runtime):
    database = merged(runtime)
    units = {f['id']: f.get('unit') for f in json.loads((runtime / 'schemas' / 'enemy_fields.json').read_text(encoding='utf-8'))['fields']}
    engine = mapped()
    profile = json.loads((REPO / 'src' / 'HellForge.Core' / 'entity-profile.json').read_text(encoding='utf-8'))['components']
    entities, editable, blocked_count, unmapped = {}, 0, 0, []
    for name, entry in sorted(database['enemies'].items()):
        zones = {z['id']: z for z in lst(entry.get('zones'))}
        attacks = {a['id']: a for a in lst(entry.get('attacks'))}
        fields, blocked = [], {}
        for f in entry['fields']:
            authored = f.get('currentDefault')
            if not isinstance(authored, (int, float)) or isinstance(authored, bool):
                continue  # not a row in the HD2R Editor either
            schema = database['schema'][f['id']]
            backing = {}
            if f['backing'].get('kind') != 'settings' and entry.get('health'):
                backing.update(entry['health'])
            backing.update(f['backing'])
            storage = backing.get('storage')
            low, high = f.get('min', schema.get('min')), f.get('max', schema.get('max'))
            if storage in LIMITS:
                low = LIMITS[storage][0] if low is None else max(low, LIMITS[storage][0])
                high = LIMITS[storage][1] if high is None else min(high, LIMITS[storage][1])
            part = f.get('zone') or f.get('attack') or ''
            key = '%s:%s:%s' % (f['path'], part, f['id'])
            if f['path'] == 'damage_zone':
                zone = zones.get(f['zone'], {})
                section = '%s (%s)' % (zone.get('wikiZone') or human(zone.get('name') or f['zone']), f['zone'])
            elif f['path'] == 'attack':
                section = '%s \u00b7 %s' % (human(f['attack']), human(attacks.get(f['attack'], {}).get('role')))
            else:
                section = TITLES.get(f['id'].split('.')[0]) or human(f['id'].split('.')[0])
            if backing.get('kind') == 'settings' and backing.get('settings') in SETTINGS and backing.get('recordType') is not None:
                kind, component, resource, record = SETTINGS[backing['settings']], '', '0x0', backing['recordType']
            elif backing.get('component') and backing.get('recordIndex') is not None:
                kind, component, resource, record = 'component', backing['component'], entry['resource'], backing['recordIndex']
            else:
                kind = None
            reason = None
            if f.get('editable') is False:
                reason = 'The HD2R Editor shows it for inspection only'
            elif kind is None or backing.get('offset') is None:
                reason = "HD2Runtime names no native address for it"
            elif not representable(authored, storage, schema.get('type') == 'integer' or (schema.get('type') != 'number' and storage in LIMITS)):
                reason = 'Its authored value cannot be shown and restored exactly'
            elif (low is not None and authored < low) or (high is not None and authored > high):
                reason = 'Its authored value lies outside the write range'
            elif (component or kind) not in engine:
                reason = "HellForge's engine has no %s mapping yet" % (component or kind)
                # a component the entity profile locates (src/HellForge.Core/entity-profile.json): the import adds it
                if kind == 'component' and component in profile:
                    unmapped.append({'kind': kind, 'component': component, 'offset': backing['offset'], 'storage': storage,
                                     'key': f['id'], 'zone': f.get('zone') if f['path'] == 'damage_zone' else None, 'label': human(f['id'])})
            elif (backing['offset'], storage) not in engine[component or kind]:
                reason = "HellForge's engine does not map %s +%d yet" % (component or kind, backing['offset'])
                # a field of a component the engine knows: scripts/ImportSchemaMappings.py names it from this
                unmapped.append({'kind': kind, 'component': component or kind, 'offset': backing['offset'], 'storage': storage,
                                 'key': f['id'], 'zone': f.get('zone') if f['path'] == 'damage_zone' else None, 'label': human(f['id'])})
            if reason:
                blocked[key] = reason
                continue
            note = ' '.join(t for t in (NOTES.get(f['id']), f.get('lifecycle') or schema.get('lifecycle')) if t) or None
            # A component field is on the entity's own resource; a settings row has none. Empty properties are left out.
            field = {'key': key, 'label': human(f['id']), 'section': section, 'kind': kind, 'component': component or None,
                     'record': record, 'offset': backing['offset'], 'storage': storage, 'min': low, 'max': high,
                     'unit': schema.get('unit') or units.get(f['id']),
                     'shared': schema.get('shared') is True or backing.get('uniqueOwner') is False or kind != 'component',
                     # proven in game: an enemy class's main health and zone armor (HD2Runtime sdk/LiveEvidenceCatalog.json)
                     'verified': entry.get('kind') == 'enemy' and f['id'] in ('entity.health', 'zone.armor') and not entry.get('requiresAcknowledgement'),
                     'note': note}
            fields.append({k: v for k, v in field.items() if v is not None})
        if not fields and not blocked:
            continue
        label = entry.get('fullDisplayName') or entry.get('displayName') or human(entry.get('wikiName') or name)
        reasons = {}
        for key, reason in sorted(blocked.items()):
            reasons.setdefault(reason, []).append(key)
        entities[name] = {'label': label, 'category': category(entry), 'kind': entry.get('kind'), 'faction': entry.get('faction'),
                          'resource': entry['resource'], 'fields': fields, 'blocked': reasons}
        editable += len(fields)
        blocked_count += len(blocked)
    return entities, editable, blocked_count, unmapped


def write(path, head, entities, part):
    """One entity's header, then a line per entry of `part` (its fields, or its blocked reasons): diffable, and a fraction
    of an indented file. Still one JSON document."""
    def compact(value):
        return json.dumps(value, ensure_ascii=False, separators=(',', ':'))
    lines = ['{' + compact(head)[1:-1] + ',"entities":{']
    for i, (name, e) in enumerate(entities.items()):
        header = {k: v for k, v in e.items() if k not in ('fields', 'blocked')} if part == 'fields' else {}
        entries = e[part]
        if part == 'fields':
            lines.append(' %s:{%s"fields":[' % (compact(name), compact(header)[1:-1] + ',' if header else ''))
            lines += ['  %s%s' % (compact(f), ',' if j < len(entries) - 1 else '') for j, f in enumerate(entries)]
            lines.append(' ]}' + (',' if i < len(entities) - 1 else ''))
        else:
            lines.append(' %s:{' % compact(name))
            lines += ['  %s:%s%s' % (compact(r), compact(keys), ',' if j < len(entries) - 1 else '') for j, (r, keys) in enumerate(entries.items())]
            lines.append(' }' + (',' if i < len(entities) - 1 else ''))
    lines.append('}}')
    text = '\n'.join(lines) + '\n'
    json.loads(text)
    Path(path).write_text(text, encoding='utf-8', newline='\n')


def main(runtime, out, blocked_out=None):
    runtime = Path(runtime)
    # The domains by their bytes: a checkout's VERSION file is not always the release its domains came from.
    sources = {n: hashlib.sha256((runtime / 'domains' / (n + '.lua')).read_bytes().replace(b'\r\n', b'\n')).hexdigest().upper()
               for n in ('enemy_authoring', 'enemy_controls')}
    entities, editable, blocked, _ = export(runtime)
    out = Path(out)
    write(out, {'format': 1, 'build': '25480438', 'sources': sources,
                'note': "The HD2R Editor's Enemies pages: entities and the fields HellForge's engine can write (scripts/ExportEntities.py). "
                        'Keys are HD2R Editor row ids. Blocked fields: run the script with a third path.'}, entities, 'fields')
    # The rest, by why: the work list for mapping more (not part of the app).
    if blocked_out:
        write(Path(blocked_out), {'format': 1, 'build': '25480438', 'sources': sources,
              'note': "HD2R Editor Enemies rows HellForge's editor does not offer yet, by entity and reason (scripts/ExportEntities.py)."},
              {n: e for n, e in entities.items() if e['blocked']}, 'blocked')
    by = {}
    for e in entities.values():
        by[e['category']] = by.get(e['category'], 0) + 1
    print('%d entities %s: %d editable fields, %d blocked' % (len(entities), by, editable, blocked))


if __name__ == '__main__':
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    main(*sys.argv[1:])
