"""Export the reference catalogue (projectile donors, explosions, status effects, ammunition) and the package loader's
pins from HD2Runtime's generated domain tables. Metadata only: type ids, labels, package ids and code pins, no game data.

Usage: py scripts/ExportReferences.py <HD2Runtime> mappings/references.json native/mod-loader/reference_data.h

mappings/references.json feeds the editor's pickers (HD2R Editor's payload swaps); reference_data.h gives the loader
the same rows with the packages each needs (refs.c) and the native package system's pins (packages.c).
"""
import json
import re
import sys
from pathlib import Path

NUMBER = re.compile(r'-?0x[0-9a-fA-F]+|-?\d+(\.\d+)?([eE][-+]?\d+)?|true|false|nil')
NAME = re.compile(r'([A-Za-z_]\w*)\s*=')
ESCAPES = {'n': '\n', 't': '\t', '\\': '\\', '"': '"', "'": "'"}


def read_table(path):
    """A generated Lua data table ('return {...}'), read without executing Lua."""
    text = Path(path).read_text(encoding='utf-8')
    pos = [text.index('return') + 6]

    def ws():
        while True:
            while pos[0] < len(text) and text[pos[0]] in ' \t\r\n':
                pos[0] += 1
            if text.startswith('--', pos[0]):
                while pos[0] < len(text) and text[pos[0]] != '\n':
                    pos[0] += 1
                continue
            return

    def value():
        ws()
        c = text[pos[0]]
        if c == '{':
            return table()
        if c in '"\'':
            pos[0] += 1
            out = []
            while text[pos[0]] != c:
                if text[pos[0]] == '\\':
                    pos[0] += 1
                    out.append(ESCAPES.get(text[pos[0]], text[pos[0]]))
                else:
                    out.append(text[pos[0]])
                pos[0] += 1
            pos[0] += 1
            return ''.join(out)
        m = NUMBER.match(text, pos[0])
        if not m:
            raise ValueError(f'{path}: unexpected {text[pos[0]:pos[0] + 20]!r}')
        pos[0] = m.end()
        t = m.group(0)
        if t in ('true', 'false'):
            return t == 'true'
        if t == 'nil':
            return None
        if t.lower().startswith(('0x', '-0x')):
            return int(t, 16)
        return float(t) if ('.' in t or 'e' in t.lower()) else int(t)

    def table():
        pos[0] += 1
        arr, obj = [], {}
        while True:
            ws()
            if text[pos[0]] == '}':
                pos[0] += 1
                break
            if text[pos[0]] == '[':
                pos[0] += 1
                k = value()
                ws(); pos[0] += 1  # ]
                ws(); pos[0] += 1  # =
                obj[str(k)] = value()
            else:
                m = NAME.match(text, pos[0])
                if m:
                    pos[0] = m.end()
                    obj[m.group(1)] = value()
                else:
                    arr.append(value())
            ws()
            if text[pos[0]] in ',;':
                pos[0] += 1
        if arr and obj:
            raise ValueError(f'{path}: mixed table')
        return arr if arr or not obj else obj

    return value()


def main(runtime, out_json, out_header):
    domains = Path(runtime) / 'domains'
    outputs = read_table(domains / 'attack_outputs.lua')
    explosions = read_table(domains / 'explosion_catalogue.lua')['explosions']
    residency = read_table(domains / 'package_residency.lua')
    statuses = read_table(domains / 'status_catalog.lua')['statuses']
    entities = read_table(domains / 'entity_authoring.lua')
    authoring = read_table(domains / 'status_effect_authoring.lua')['statuses']
    payload = read_table(domains / 'payload_options.lua')
    dependencies, packages = residency['dependencies'], residency['packages']
    if residency.get('build') != 'F5FEE03DCFDB':  # helldivers2.exe's SHA-256 prefix for build 25480438
        raise ValueError('package_residency is for another build')

    used = {}

    def package(key):
        dependency = dependencies.get(key)
        if dependency is None:
            return None
        pid = dependency['package']
        entry = packages.get(pid)
        used[pid] = entry['name'] if entry else dependency['label']
        return pid

    # Projectile donors: every projectile attack output whose type and package are known. A type several things fire
    # takes the first owner's package (any one of them carries what it needs).
    projectiles = {}
    for output in outputs['outputs'].values():
        if output['family'] != 'projectile':
            continue
        settings = output.get('referenceSettings') or {}
        ptype = settings.get('recordType')
        key = output.get('dependencyKey')
        pid = package(key) if key else None
        if ptype is None or pid is None:
            continue
        entry = projectiles.setdefault(ptype, {'type': ptype, 'label': output['owner']['name'], 'owners': [],
                                               'class': output.get('compatibilityClass'), 'package': pid})
        if output['owner']['name'] not in entry['owners']:
            entry['owners'].append(output['owner']['name'])
        entry.setdefault('kinds', []).append(output['owner']['kind'])
    for entry in projectiles.values():
        entry['owner'] = entry.pop('kinds')[0]

    # Explosions: every catalogued type; the packages its effects need (none: always resident; a mission's effects
    # package: resident in missions, never loaded).
    explosion_rows = []
    for key, x in explosions.items():
        p = x.get('package')
        pids, mission = [], False
        if p:
            mission = bool(p.get('mission'))
            keys = p.get('keys') or [p['key']]
            if not mission:
                for k in keys:
                    pid = package(k)
                    if pid is None:
                        raise ValueError(f'explosion {key}: package {k} is not in the catalogue')
                    pids.append(pid)
        explosion_rows.append({'type': x['type'], 'id': key, 'label': x['label'], 'family': x['family'],
                               'packages': pids, 'mission': mission, 'shrapnel': bool(x.get('shrapnel')),
                               'damage': (x.get('damage') or {}).get('type')})
    explosion_rows.sort(key=lambda r: r['type'])
    if len({r['type'] for r in explosion_rows}) != len(explosion_rows):
        raise ValueError('explosion types are not unique')

    # HD2R Editor's payload options (domains/payload_options): which explosions a payload slot offers, their group and
    # warnings, and the options a field excludes.
    for r in explosion_rows:
        option = payload['options'].get(r['id'])
        if option:
            r['payload'] = {'group': option['group'], 'warnings': option['warnings']}
    payload_info = {
        'groups': [{'id': g['id'], 'label': g['label']} for g in payload['groups']],
        'warnings': payload['warnings'],
        'excluded': {},
    }
    by_id = {r['id']: r['type'] for r in explosion_rows}
    for field, f in sorted(payload['fields'].items()):
        if f.get('excluded'):
            kind, owner, key = field.split(':', 2)
            payload_info['excluded'][f'weapons/{owner}/{key}'] = sorted(by_id[x] for x in f['excluded'])

    # Status effects: every one (the Status Effects pages: duration and, for 25, the damage each tick deals); a damage
    # type can apply the attachable ones (HD2R Editor offers those; tiers other than player_attack are unverified on a
    # player's attack).
    names = {}
    for key, s in statuses.items():
        names.setdefault(s['name'].replace('_', ' '), []).append(key)
    status_rows = []
    for key, s in statuses.items():
        label = s['name'].replace('_', ' ')
        if len(names[label]) > 1:
            label += f' ({key.replace("_", " ")})'
        tick = (authoring.get(key, {}).get('targets', {}).get('damage') or {}).get('damageType')
        status_rows.append({'type': s['nativeType'], 'id': key, 'label': label, 'family': s['family'],
                            'attachable': bool(s.get('attachable')), 'tier': s.get('tier'), 'damage': tick})
    status_rows.sort(key=lambda r: r['type'])

    # Programmable ammunition (ProjectileWeapon +576): the weapons that fire a function projectile of their own.
    programmable = {}
    for name in ('player_weapon_authoring', 'support_weapon_authoring'):
        def walk(o, path):
            if isinstance(o, dict):
                if o.get('semanticFieldId') == 'function_ammo.projectile' and o.get('editable') and (o.get('currentDefault') or {}).get('projectileType'):
                    programmable[path[1]] = {'projectile': o['currentDefault']['projectileType'], 'note': o.get('acknowledgementReason') or ''}
                for k, v in o.items():
                    walk(v, path + [k])
            elif isinstance(o, list):
                for v in o:
                    walk(v, path)
        walk(read_table(domains / f'{name}.lua'), [])

    # Ammunition: the six weapons whose shot is their default ammunition's (an entity delta), not ProjectileWeapon +0.
    # Its rows reach other weapons only when those equip the same ammunition (research/ammunition-projectiles: the item's
    # defaultOf against unlockListedBy): "optionalFor", which the editor names as conditional (bug-report-0035).
    research = json.loads((Path(runtime) / 'research' / 'ammunition-projectiles-F5FEE03DCFDB.json').read_text(encoding='utf-8'))
    rows = {w['weapon']: w for w in research['weapons']}
    ammunition = []
    for weapon, a in sorted(outputs['ammunition'].items()):
        if a['backing']['kind'] != 'entity_delta' or a['backing']['offset'] != 0:
            raise ValueError(f'ammunition {weapon}: not a projectile entity delta')
        r = rows[weapon]
        if r['ammunitionItem'] != a['item'] or r['projectileType'] != a['currentDefault']['projectileType']:
            raise ValueError(f'ammunition {weapon}: the research names another item or projectile')
        item = next(i for i in r['ammunitionItems'] if i['item'] == a['item'])
        ammunition.append({'weapon': weapon, 'item': a['item'], 'resource': a['resource'], 'record': a['settingsIndex'],
                           'component': a['component'], 'offset': a['dataOffset'], 'projectile': a['currentDefault']['projectileType'],
                           'damage': r['damageInfo']['recordType'],
                           'optionalFor': sorted(set(item['unlockListedBy']) - set(item['defaultOf']))})

    # Weapons whose shot can be swapped (HD2R Editor's hosts): ProjectileWeapon +0 ("component"), or their default
    # ammunition's delta ("ammunition"); the others' reasons, for the editor's "not offered" note.
    # The hosts list, then every source HD2Runtime 0.40 classifies ACTIVE_DIRECT (and, for support weapons, writable):
    # ProjectileWeapon +0, or for rounds-fed weapons WeaponRounds +64 ("rounds").
    hosts = {name: {'mechanism': h['mechanism'], 'class': h.get('class')} for name, h in sorted(outputs['hosts'].items())}
    refused = {}
    sources = [(name, roles) for name, roles in outputs['sources'].items()]
    sources += [(name, {'primary': r}) for name, r in outputs['supportSources'].items() if isinstance(r, dict)]
    for name, roles in sorted(sources):
        if name in hosts:
            continue
        direct = [r for r in roles.values() if r.get('status') == 'ACTIVE_DIRECT' and r.get('writable', True)]
        # Where WeaponRounds +64 and ProjectileWeapon +0 carry the same projectile, the shot takes WeaponRounds +64 when
        # the weapon has no magazine or heat (none of these do): the type resolver (game.dll 0x7456D0) checks heat (flag
        # bit 9), then a magazine (bit 7), then rounds (bit 8, +0x40), and every live WeaponRounds weapon in the decoded
        # snapshots (SG-8 Punisher, SG-88 Break-Action Shotgun) has bit 8.
        direct = direct or [r for r in roles.values() if r.get('status') == 'AMBIGUOUS' and r.get('member') == 'WeaponRounds +64' and
                            r.get('candidatesAgree') and r.get('reason', '').startswith('Both WeaponRounds PrimaryProjectileType and ProjectileWeapon +0')]
        if direct and direct[0].get('member') == 'ProjectileWeapon +0':
            hosts[name] = {'mechanism': 'component', 'class': direct[0].get('compatibilityClass')}
        elif direct and direct[0].get('member') == 'WeaponRounds +64':
            hosts[name] = {'mechanism': 'rounds', 'class': direct[0].get('compatibilityClass')}
        else:
            r = roles.get('primary') or next(iter(roles.values()))
            if r.get('reason'):
                refused[name] = r['reason'] if r.get('status') != 'ACTIVE_DIRECT' else 'HD2Runtime marks its projectile not writable: ' + r['reason']
    hosts = dict(sorted(hosts.items()))

    # Mounted weapons a vehicle's slot can hold (HD2R Editor's arm swap): each one's entity resource and the package it
    # needs (dependencies 'mounted_weapon/<id>'; none listed: it needs none); each vehicle slot's vanilla occupant and
    # the weapons HD2Runtime allows there (MountComponentData slot k at +24k).
    mounted = {}
    for mid, m in sorted(entities['mountedWeapons'].items()):
        mounted[mid] = {'resource': m['resource'], 'name': m.get('displayName') or mid, 'family': m.get('attackFamily'),
                        'package': package('mounted_weapon/' + mid)}
    mount_slots = []
    for vname, v in sorted(entities['vehicles'].items()):
        for f in v['fields']:
            t = f.get('target') or {}
            if t.get('path') == 'mount' and f.get('type') == 'mounted_weapon_reference' and f.get('editable'):
                b = f['backing']
                allowed = [f['currentDefault']] + [a for a in f.get('allowedValues') or [] if a != f['currentDefault']]
                mount_slots.append({'vehicle': vname, 'resource': b.get('resource') or v['resource'], 'record': b['recordIndex'],
                                    'slot': b['offset'] // 24, 'default': f['currentDefault'], 'allowed': [a for a in allowed if a in mounted]})
    # An exosuit's shield arm (the EXO-55's; vehicle_weapon_authoring mountedShield): HD2Runtime offers no swap for its
    # slot. Offered here, untested, with the weapons the vehicle's other arm takes, the shield (its mount entity) first.
    # Its entry goes after every mounted weapon, so the editor's saved choices (by index) keep their meaning.
    for key, w in sorted(read_table(domains / 'vehicle_weapon_authoring.lua')['weapons'].items()):
        chain = w.get('mountChain') or {}
        vname, slot = w.get('vehicle'), chain.get('slot', w.get('slot'))
        other = next((s for s in mount_slots if s['vehicle'] == vname and s['slot'] != slot), None)
        if not w.get('mountedShield') or other is None or any(s['vehicle'] == vname and s['slot'] == slot for s in mount_slots):
            continue
        sid = 'vehicle-shield/' + key
        mounted[sid] = {'resource': chain.get('mountPath') or w['attackResource'], 'name': key, 'family': 'shield', 'package': None}
        mount_slots.append({'vehicle': vname, 'resource': other['resource'], 'record': other['record'], 'slot': slot, 'default': sid,
                            'allowed': [sid] + other['allowed'], 'untested': 'HD2Runtime offers no swap for the shield arm: swapping it is untested '
                                                                           '(the shield may keep or lose its behaviour).'})

    # Hellpod payloads (HD2R Editor's pod rows; HD2Runtime pod_payload_authoring): what a call-in's pod opens with, a
    # rack entity's HellpodRackComponentData slots 1-4 (+64 per slot: the item's entity resource) and its count of spawned
    # slots (+556). Each writable rack with the stratagems that call it in, its slots' vanilla items and the pickups a
    # live test proved in exactly that slot; the reviewed pickups (support weapons, backpacks, ammo, stims, grenades,
    # supplies) with the package each needs (dependencies 'pickup/<id>').
    pods = read_table(domains / 'pod_payload_authoring.lua')
    pickups = {}
    for pid_, pk in sorted(pods['pickups'].items(), key=lambda kv: (kv[1]['category'], kv[1]['name'])):
        pickups[pid_] = {'resource': pk['resource'], 'name': pk['name'], 'category': pk['category'], 'compatibility': pk['compatibility'],
                         'alwaysResident': bool(pk.get('alwaysResident')), 'package': package('pickup/' + pid_)}
    pod_racks = []
    for rname, rk in sorted(pods['racks'].items()):
        if not rk.get('writable'):
            continue
        slots = []
        for k in range(1, int(pods['authoredSlots']) + 1):
            sl = rk['slots'].get(str(k)) or {}
            current = sl.get('current', 'empty')
            slots.append({'slot': k, 'default': current if current in pickups else 'empty', 'active': bool(sl.get('active')),
                          'tested': sorted(x for x in (sl.get('live') or {}) if x in pickups)})
        pod_racks.append({'rack': rname, 'resource': rk['resource'], 'record': rk['recordIndex'], 'shared': bool(rk.get('shared')),
                          'consumers': [c['name'] for c in rk['consumers']], 'spawnCount': rk['spawnCount'], 'slots': slots})

    # Whole-row copies (HD2R Editor's G-109 Urchin "Pulse explosion"): a field that is an explosion row's settings as raw
    # bytes, set by copying another catalogued explosion's over it. The row, the bytes, its own explosion and the
    # explosions HD2Runtime allows there, by type.
    row_copies = []
    for tname, th in sorted(read_table(domains / 'throwable_authoring.lua')['throwables'].items()):
        for path, tgt in sorted((th.get('targets') or {}).items()):
            for fid, f in sorted((tgt.get('fields') or {}).items()):
                b = f.get('backing') or {}
                if b.get('storage') != 'bytes' or b.get('settings') != 'explosion' or not f.get('allowedValues'):
                    continue
                types = [explosions[x]['type'] for x in f['allowedValues'] if x in explosions]
                # the rows the same copy goes into with it (the Urchin's final discharge: HD2Runtime swaps both pulse rows)
                also = [r['recordType'] for r in f.get('payloadRecipients') or [] if r.get('recordType') != b['recordType']]
                row_copies.append({'category': 'throwables', 'target': tname, 'key': fid, 'label': f.get('displayName') or fid,
                                   'table': 'ExplosionSettings', 'row': b['recordType'], 'offset': b['offset'], 'size': b['width'],
                                   'default': explosions[f['currentDefault']]['type'] if f.get('currentDefault') in explosions else b['recordType'],
                                   'allowed': types, 'also': also})

    # Packages mods load for sounds no catalogue entry carries (HfAbility.package): the Launch ICBM objective's, whose bank
    # obj_gen_icbm holds the ICBM launch sequence (HD2Runtime research/sound-events: the bank ships in this package).
    used.setdefault('0x20BDEC2A51065E1F', 'packages/content/gen_icbm (the Launch ICBM objective: its launch sounds)')

    loader = residency['loader']
    engine = loader['engine']
    data = {
        'format': 1, 'build': '25480438',
        'source': 'HD2Runtime 0.40.1 domains/attack_outputs, explosion_catalogue, status_catalog, package_residency',
        'packages': dict(sorted(used.items())),
        'projectiles': sorted(projectiles.values(), key=lambda r: r['type']),
        'explosions': explosion_rows, 'statuses': status_rows, 'ammunition': ammunition, 'hosts': hosts, 'refused': refused,
        'payload': payload_info, 'programmable': dict(sorted(programmable.items())),
        'mountedWeapons': mounted, 'mountSlots': mount_slots, 'pickups': pickups, 'podRacks': pod_racks, 'rowCopies': row_copies,
        # HD2R Editor's explosion pages: the fields every catalogued explosion shows (its row, or its damage type's)
        'explosionFields': [{'id': f['id'], 'backing': f['backing'], 'offset': f['offset'], 'storage': f['storage'], 'min': f.get('min'),
                             'max': f.get('max'), 'unit': f.get('unit')} for f in read_table(domains / 'explosion_catalogue.lua')['fields']],
    }
    Path(out_json).write_text(json.dumps(data, indent=2, ensure_ascii=False) + '\n', encoding='utf-8', newline='\n')

    ids = sorted(used)
    index = {pid: i + 1 for i, pid in enumerate(ids)}

    def pin(name, hexbytes):
        b = bytes.fromhex(hexbytes)
        rows = [', '.join(f'0x{v:02x}' for v in b[i:i + 16]) for i in range(0, len(b), 16)]
        return f'static const uint8_t {name}[{len(b)}] = {{\n    ' + ',\n    '.join(rows) + '};\n'

    q = engine['queue']
    h = ['/* Generated by scripts/ExportReferences.py from HD2Runtime 0.40.1 (domains/package_residency, attack_outputs,',
         ' * explosion_catalogue, status_catalog) for game build 25480438; do not edit. Type ids, package ids and code pins only.',
         ' * Included by packages.c and refs.c. */', '',
         '/* game.dll: the RefcountedPackageSystem (request: void(system, const uint64_t* ids, uint32_t count)) */',
         f'#define PKG_REQUEST 0x{loader["requestRva"]:X}u',
         f'#define PKG_SYSTEM 0x{loader["instanceGlobalRva"]:X}u',
         f'#define PKG_MAP_ENTRIES_AT {loader["refcountMap"]["entries"]}u',
         f'#define PKG_MAP_CAPACITY_AT {loader["refcountMap"]["capacity"]}u',
         f'#define PKG_MAP_STRIDE {loader["refcountMap"]["entryStride"]}u',
         '/* helldivers2.exe: the engine\'s resource manager, its package list and load queue */',
         f'#define PKG_EXE_ENGINE 0x{engine["managerGlobalRva"]:X}u',
         f'#define PKG_EXE_HAS_LOADED 0x{engine["hasLoadedRva"]:X}u',
         f'#define PKG_EXE_FIND 0x{engine["findPackageRva"]:X}u',
         f'#define PKG_MANAGER 0x{engine["packageManager"]:X}u',
         f'#define PKG_RESOURCES 0x{engine["resourceManager"]:X}u',
         f'#define PKG_LIST 0x{engine["list"]:X}u',
         f'#define PKG_LIST_COUNT 0x{engine["listCount"]:X}u',
         f'#define PKG_ID 0x{engine["packageId"]:X}u',
         f'#define PKG_PARTS 0x{engine["parts"]:X}u',
         f'#define PKG_PART_COUNT 0x{engine["partCount"]:X}u',
         f'#define PKG_PART_STATE 0x{engine["partState"]:X}u',
         f'#define PKG_LOADED {engine["loadedState"]}u',
         f'#define PKG_QUEUE_HEAD 0x{q["head"]:X}u',
         f'#define PKG_QUEUE_TAIL 0x{q["tail"]:X}u',
         f'#define PKG_QUEUE_ENTRIES 0x{q["entries"]:X}u',
         f'#define PKG_QUEUE_STRIDE {q["stride"]}u',
         f'#define PKG_QUEUE_CAPACITY {q["capacity"]}u',
         f'#define PKG_QUEUE_FLAG 0x{q["loadFlag"]:X}u', '',
         pin('kPackageRequestPin', loader['requestProof']),
         pin('kPackageHasLoadedPin', engine['hasLoadedProof']),
         pin('kPackageFindPin', engine['findProof'] if 'findProof' in engine else engine['findPackageProof']),
         'typedef struct RefPackage { uint64_t id; const char* name; } RefPackage;',
         '/* The packages references need (index + 1 in the rows below; 0: none) */',
         'static const RefPackage kRefPackages[] = {']
    for pid in ids:
        name = used[pid].replace('\\', '/').replace('"', "'")
        h.append(f'    {{0x{int(pid, 16):016X}ull, "{name}"}},')
    h += ['};', '',
          'typedef struct RefRow { uint32_t type; uint16_t package, package2; uint8_t mission, shrapnel; } RefRow;',
          '/* Projectile types other weapons fire, with the package of one that fires it */',
          'static const RefRow kRefProjectiles[] = {']
    for r in data['projectiles']:
        h.append(f'    {{{r["type"]}u, {index[r["package"]]}, 0, 0, 0}}, /* {r["label"]} */')
    h += ['};', '/* Explosion types: the packages their effects need; mission: a mission\'s effects package (not loaded) */',
          'static const RefRow kRefExplosions[] = {']
    for r in explosion_rows:
        p = [index[x] for x in r['packages']] + [0, 0]
        h.append(f'    {{{r["type"]}u, {p[0]}, {p[1]}, {int(r["mission"])}, {int(r["shrapnel"])}}}, /* {r["label"]} */')
    mids = list(mounted)
    h += ['};', '/* Mounted weapons a vehicle slot can hold: entity resource, package (index + 1; 0 none) */',
          'typedef struct RefMount { uint64_t resource; uint16_t package; } RefMount;', 'static const RefMount kRefMounts[] = {']
    for mid in mids:
        m = mounted[mid]
        h.append(f'    {{0x{int(m["resource"], 16):016X}ull, {index[m["package"]] if m["package"] else 0}}}, /* {mid} */')
    h += ['};', '/* Vehicle mount slots: the vehicle entity, the slot, its vanilla occupant and the weapons allowed there (kRefMountAllowed) */',
          'typedef struct RefMountSlot { uint64_t vehicle; uint32_t slot; uint16_t first, count; } RefMountSlot;', 'static const RefMountSlot kRefMountSlots[] = {']
    allowed_list = []
    for s in mount_slots:
        h.append(f'    {{0x{int(s["resource"], 16):016X}ull, {s["slot"]}, {len(allowed_list)}, {len(s["allowed"])}}}, /* {s["vehicle"]} slot {s["slot"]} */')
        allowed_list += [mids.index(a) for a in s['allowed']]
    h += ['};', '/* the first of a slot\'s is its vanilla occupant */', 'static const uint16_t kRefMountAllowed[] = {']
    h += ['    ' + ', '.join(str(i) for i in allowed_list[k:k + 24]) + ',' for k in range(0, len(allowed_list), 24)]
    h += ['};', '/* Pickups a hellpod slot can hold: entity resource, package (index + 1; 0 none) */',
          'typedef struct RefPickup { uint64_t resource; uint16_t package; } RefPickup;', 'static const RefPickup kRefPickups[] = {']
    for pid_, pk in pickups.items():
        h.append(f'    {{0x{int(pk["resource"], 16):016X}ull, {index[pk["package"]] if pk["package"] else 0}}}, /* {pk["name"]} */')
    h += ['};', '/* Hellpod rack slots HellForge may change: the rack entity, the slot (0-3: +64 each), its vanilla item (0: empty) */',
          'typedef struct RefPodSlot { uint64_t rack; uint32_t slot; uint64_t vanilla; } RefPodSlot;', 'static const RefPodSlot kRefPodSlots[] = {']
    for r in pod_racks:
        for sl in r['slots']:
            vanilla = int(pickups[sl['default']]['resource'], 16) if sl['default'] != 'empty' else 0
            h.append(f'    {{0x{int(r["resource"], 16):016X}ull, {sl["slot"] - 1}, 0x{vanilla:016X}ull}}, /* {r["rack"]} slot {sl["slot"]} */')
    h += ['};', '/* Settings rows a copy of another row may replace: table, row, bytes, the rows allowed as donors (kRefRowDonors), and',
          ' * `with`: the row whose copy this one follows (0: a row mods name; else it takes the same donor whenever that row does) */',
          'typedef struct RefRowCopy { const char* table; uint32_t row, offset, size; uint16_t first, count; uint32_t with; } RefRowCopy;',
          'static const RefRowCopy kRefRowCopies[] = {']
    donors = []
    for r in row_copies:
        h.append(f'    {{"{r["table"]}", {r["row"]}u, {r["offset"]}u, {r["size"]}u, {len(donors)}, {len(r["allowed"])}, 0u}}, /* {r["target"]}: {r["label"]} */')
        for row in r.get('also') or []:
            h.append(f'    {{"{r["table"]}", {row}u, {r["offset"]}u, {r["size"]}u, {len(donors)}, {len(r["allowed"])}, {r["row"]}u}}, /* {r["target"]}: {r["label"]}, with it */')
        donors += r['allowed']
    h += ['    {0}};' if not row_copies else '};', 'static const uint32_t kRefRowDonors[] = {', '    ' + ', '.join(f'{d}u' for d in donors or [0]) + '};']
    h += ['/* Status effects a damage type can apply */', 'static const uint32_t kRefStatuses[] = {']
    h.append('    ' + ', '.join(f'{r["type"]}u' for r in status_rows if r['attachable']) + '};')
    Path(out_header).write_text('\n'.join(h) + '\n', encoding='utf-8', newline='\n')
    print(f'{len(data["projectiles"])} projectile donors, {len(explosion_rows)} explosions '
          f'({sum(1 for r in explosion_rows if "payload" in r)} payload options), {len(status_rows)} statuses, {len(programmable)} programmable, '
          f'{len(ammunition)} ammunition weapons, {len(hosts)} swap hosts, {len(pickups)} pickups for {len(pod_racks)} pod racks, {len(ids)} packages')


if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
