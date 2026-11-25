"""Export the enemy combat fields players asked the editor for (Discord: enemy stats, projectiles and melee, status
thresholds): how an enemy moves, sees, fights and takes statuses. Metadata only: field keys, labels, record ids,
offsets, ranges and notes; no game values. Also names the fields it offers in mappings/components.json.

Usage: py scripts/ExportEnemyCombat.py <HD2Runtime-0.40.1> <melee map> mappings/enemy-combat.json

Reads mappings/entities.json (the enemy pages) and the pinned build's datalibrary through HD2Runtime 0.40.1's
scripts/migration/build_view (which component record each entity owns, and its bytes to see which entries it has). The
melee map ({entity: {"damage": {DamageInfo id: [AbilityIds]}}}) comes from the enemy melee research pass: an entity's
behaviour (BehaviorComponent +0) picks its AI ability options (game.dll 0x4A0F00, 0x60-byte rows in .s01, +0 the
AbilityId), each ability's tick handler (AbilityTick 0x11509E0) starts melee hit volumes (MeleeHitVolumeStart 0x7D38D0,
a 248-byte template whose +4 is the DamageInfo id), plus its AiEnemy obstacle and charge abilities.

Adds, per enemy page that owns the record:
  movement    AiEnemy: each AI state's move speed in m/s ({speed, locomotion set, random min, random max} at +0x24 patrol,
              +0x64 combat, +0x7C strafe, +0xF4 retreat, +0x114 charge, of the 7 states; 0x7F0180 takes
              the random range when both ends are above 0, else the speed, as the state begins; navigation caps the
              velocity to it, 0x964830), the charge's acceleration (+0x124, set as the charge starts, 0xAA85D0: with the
              charger's fixed speed it is its turn arc) and tracking angle (+0x12C, read each charge tick, 0xAA8680);
              Locomotion +0x1170 / +0x1174 acceleration and deceleration (copied when it spawns, 0x9CD990); Rotation
              +0 / +4 / +8 turn speed, acceleration and deceleration in degrees (copied when it spawns, 0x61D640; a turn
              speed of 0 makes it turn by animation, 0x625280, so it is offered only where it is not 0)
  detection   SensorEye +0 / +4 / +8: the sight ranges ahead, to the sides and behind (game.dll 0x64C3C0 copies them,
              times the eye's scale, into the eye when the enemy is made; a negative side or rear range takes the
              range ahead)
  melee       each melee DamageInfo row: damage, durable damage, AP, demolition, stagger and push (read per hit)
  elements    HealthComponent +0x53FC: 4 {element, multiplier} pairs ApplyDamage (0x9235F0, at 0x9239E2) scales a hit's
              damage of that element by, the first match winning; element 0 matches damage with no element
  statuses    StatusEffectReceiver zone 0 (+8, 33 entries of 0xB0, packed up to the first type 0): per status the enemy
              takes, the buildup it needs (+0x90; ApplyStatus 0x699E40 returns below it) and the buildup that applies it
              for sure (+0x94: above the minimum the chance is buildup / this), the tick damage multiplier (+0xA8,
              0x694C4E); a status pick on the first free entry (an immunity lifted: FUN_00690B00 builds each enemy's
              allowed statuses when it spawns); the duration multipliers (+0xB5C8, 8 {type, scale} pairs, 0x691A7F)
"""
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXTRA = ('SensorEyeComponentData', 'StatusEffectReceiverComponentData', 'AiEnemyComponentData', 'LocomotionComponentData')
FACTIONS = ('terminids', 'automatons', 'illuminate')

# The susceptibility types (game.dll 0x21CB580: the statuses each one covers) the editor offers: the ones player weapons
# and stratagems mostly apply. Those dealing damage while active also show their tick damage multiplier.
STATUSES = {4: 'Fire', 10: 'Acid', 20: 'Electric', 22: 'Stun', 23: 'Illuminate stun', 27: 'Gas', 28: 'Gas confusion'}
TICKING = {4, 10, 20, 27}
ELEMENTS = {1: 'Fire', 2: 'Arc', 3: 'Acid', 4: 'Bleed', 5: 'Gas', 8: 'Tornado'}
ZONE0, ENTRY, ENTRIES, DURATIONS = 8, 0xB0, 33, 0xB5C8
ELEMENT_DAMAGE = 0x53FC
STATES = (('patrol', 'Patrol', 0x24), ('combat', 'Combat', 0x64), ('strafe', 'Strafe', 0x7C), ('retreat', 'Retreat', 0xF4),
          ('charge', 'Charge', 0x114))
MELEE = (('standard_damage', 'Damage', 4, 'i32', 'damage', 100000), ('durable_damage', 'Durable damage', 8, 'i32', 'damage', 100000),
         ('ap_direct', 'AP direct', 12, 'u32', 'armor_class', 1000), ('ap_slight', 'AP slight', 16, 'u32', 'armor_class', 1000),
         ('ap_large', 'AP large', 20, 'u32', 'armor_class', 1000), ('ap_extreme', 'AP extreme', 24, 'u32', 'armor_class', 1000),
         ('demolition', 'Demolition', 28, 'u32', 'force', 1000), ('stagger', 'Stagger', 32, 'u32', 'force', 1000),
         ('push_force', 'Push force', 36, 'u32', 'force', 1000))


def view_of(runtime):
    sys.path.insert(0, str(Path(runtime) / 'scripts'))
    import build_profile  # noqa: E402 (HD2Runtime 0.40.1: the pinned build's datalibrary)
    from migration import build_view  # noqa: E402
    build_view.COMPONENTS = tuple(dict.fromkeys(tuple(build_view.COMPONENTS) + EXTRA))
    return build_view.from_datalibrary(build_profile.datalibrary())


def field(key, label, section, component, resource, record, offset, storage, unit=None, lo=None, hi=None, note=None, shared=False):
    f = {'kind': 'component', 'component': component, 'resource': resource, 'record': record, 'offset': offset,
         'storage': storage, 'min': lo, 'max': hi, 'unit': unit, 'shared': shared, 'key': key, 'label': label, 'section': section}
    if note:
        f['note'] = note
    return f


def slug(name):
    return name.lower().replace(' ', '_')


def options(names, none='None'):
    return [[0, none]] + [[k, v] for k, v in sorted(names.items())]


def record_of(view, component, resource):
    rec = view.record(component, int(resource, 16))
    return None if rec is None else (rec['bytes'], rec['recordIndex'], rec['ownerCount'] > 1)


def movement(view, resource, add):
    sec = 'Movement'
    ai = record_of(view, 'AiEnemyComponentData', resource)
    if ai:
        raw, record, shared = ai
        for key, name, at in STATES:
            speed, _, lo, hi = struct.unpack_from('<fIff', raw, at)
            if lo > 0 and hi > 0:
                for end, label, off in (('min', 'lowest', 8), ('max', 'highest', 12)):
                    add(field('movement::%s.speed_%s' % (key, end), '%s speed, %s' % (name, label), sec, 'AiEnemyComponentData', resource,
                              record, at + off, 'f32', 'meters_per_second', 0.1, 50,
                              'It picks a speed between the lowest and highest each time it starts this behaviour.', shared))
            elif speed > 0:
                add(field('movement::%s.speed' % key, name + ' speed', sec, 'AiEnemyComponentData', resource, record, at, 'f32',
                          'meters_per_second', 0.1, 50, 'How fast it moves while it does this, from the next time it starts it.', shared))
        if struct.unpack_from('<f', raw, 0x114)[0] > 0:
            add(field('movement::charge.acceleration', 'Charge acceleration', sec, 'AiEnemyComponentData', resource, record, 0x124, 'f32',
                      'meters_per_second_squared', 0.1, 100, 'How hard it can steer while it charges: higher turns tighter. From its next charge.',
                      shared))
            add(field('movement::charge.tracking_angle', 'Charge tracking angle', sec, 'AiEnemyComponentData', resource, record, 0x12C, 'f32',
                      'degrees', 0, 180, 'It keeps homing on its target while the target is within this angle of where it faces.', shared))
    loco = record_of(view, 'LocomotionComponentData', resource)
    if loco:
        raw, record, shared = loco
        for off, key, label in ((0x1170, 'acceleration', 'Acceleration'), (0x1174, 'deceleration', 'Deceleration')):
            if struct.unpack_from('<f', raw, off)[0] > 0:
                add(field('movement::locomotion.' + key, label, sec, 'LocomotionComponentData', resource, record, off, 'f32',
                          'meters_per_second_squared', 0.1, 200, 'How fast it gets up to speed or stops. For enemies that spawn after the change.',
                          shared))
    rot = record_of(view, 'RotationComponentData', resource)
    if rot and struct.unpack_from('<f', rot[0], 0)[0] != 0:
        _, record, shared = rot
        for off, key, label, lo, note in ((0, 'turn_speed', 'Turn speed', 1, 'How fast it turns. For enemies that spawn after the change.'),
                                          (4, 'turn_acceleration', 'Turn acceleration', 0, '0: it turns at full speed at once.'),
                                          (8, 'turn_deceleration', 'Turn deceleration', 0, '0: the same as its turn acceleration.')):
            add(field('movement::rotation.' + key, label, sec, 'RotationComponentData', resource, record, off, 'f32',
                      'degrees_per_second' if off == 0 else 'degrees_per_second_squared', lo, 5000, note, shared))


def detection(view, resource, add):
    eye = record_of(view, 'SensorEyeComponentData', resource)
    if eye is None:
        return
    _, record, shared = eye
    for key, label, offset, lo, note in (
        ('eye.range', 'Sight range', 0, 0, 'How far it sees a target ahead of it. For enemies that spawn after the change.'),
        ('eye.side_range', 'Side sight range', 4, -1, 'How far it sees to its sides; -1 uses the sight range.'),
        ('eye.rear_range', 'Rear sight range', 8, -1, 'How far it sees behind it; -1 uses the sight range.'),
    ):
        add(field('sensor::' + key, label, 'Detection', 'SensorEyeComponentData', resource, record, offset, 'f32',
                  'meters', lo, 1000, note, shared))


def melee(name, attacks, users, add):
    for n, damage in enumerate(sorted(attacks, key=int), 1):
        others = len(users[damage]) - 1
        for key, label, offset, storage, unit, hi in MELEE:
            f = {'kind': 'DamageInfo', 'component': '', 'resource': '0x0', 'record': int(damage), 'offset': offset, 'storage': storage,
                 'min': 0, 'max': hi, 'unit': unit, 'shared': True, 'key': 'attack:melee_%d:damage.%s' % (n, key), 'label': label,
                 'section': 'Melee %d · Damage' % n}
            if others and offset == 4:
                f['note'] = 'Shared with %d other enemies.' % others
            add(f)


def statuses(view, resource, add):
    rec = record_of(view, 'StatusEffectReceiverComponentData', resource)
    if rec is None:
        return
    raw, record, shared = rec
    sec, comp = 'Status Effects', 'StatusEffectReceiverComponentData'

    def row(key, label, offset, storage, unit=None, lo=None, hi=None, note=None, opts=None):
        f = field('status::' + key, label, sec, comp, resource, record, offset, storage, unit, lo, hi, note, shared)
        if opts:
            f['options'] = opts
        add(f)

    taken = []
    for k in range(ENTRIES):
        at = ZONE0 + k * ENTRY
        kind = struct.unpack_from('<I', raw, at + 0x8C)[0]
        if kind == 0:
            break
        taken.append(kind)
        if kind not in STATUSES:
            continue
        name = STATUSES[kind]
        row(slug(name) + '.min_buildup', name + ': buildup needed', at + 0x90, 'f32', 'strength', 0, 10000,
            'The status strength hits must build up before it can take hold; the buildup falls by 1 a second.')
        row(slug(name) + '.sure_buildup', name + ': sure at buildup', at + 0x94, 'f32', 'strength', 0, 10000,
            'Past the buildup needed, a hit applies it with a chance of buildup / this, sure at or under the buildup needed.')
        if kind in TICKING:
            row(slug(name) + '.damage_multiplier', name + ': damage taken', at + 0xA8, 'f32', 'ratio', 0, 100,
                'Multiplies the damage it takes from this status while it lasts.')
    else:
        return
    missing = {t: n for t, n in STATUSES.items() if t not in taken}
    if missing:
        free = ZONE0 + len(taken) * ENTRY
        row('added.type', 'Added status', free + 0x8C, 'u32', opts=options(missing),
            note='A status it is immune to, made one it takes, for enemies that spawn after the change. Set the buildup needed '
                 'above 0. Whether it reacts as other enemies do is up to its own animations.')
        row('added.min_buildup', 'Added status: buildup needed', free + 0x90, 'f32', 'strength', 0, 10000)
        row('added.sure_buildup', 'Added status: sure at buildup', free + 0x94, 'f32', 'strength', 0, 10000)
    for i in range(8):
        kind = struct.unpack_from('<I', raw, DURATIONS + i * 8)[0]
        if kind == 0:
            row('duration.type', 'Status lasting longer or shorter', DURATIONS + i * 8, 'u32', opts=options(STATUSES),
                note='A status whose duration on it Duration multiplier scales.')
            row('duration.multiplier', 'Duration multiplier', DURATIONS + i * 8 + 4, 'f32', 'ratio', 0.05, 100,
                'Multiplies how long the status picked above lasts on it.')
            break
        if kind in STATUSES:
            row(slug(STATUSES[kind]) + '.duration_multiplier', STATUSES[kind] + ': duration multiplier', DURATIONS + i * 8 + 4, 'f32',
                'ratio', 0.05, 100, 'Multiplies how long the status lasts on it.')


def elements(view, resource, add):
    rec = record_of(view, 'HealthComponentData', resource)
    if rec is None:
        return
    raw, record, shared = rec
    sec = 'Damage Taken'
    present = {struct.unpack_from('<I', raw, ELEMENT_DAMAGE + i * 8)[0] for i in range(4)}
    for i in range(4):
        at = ELEMENT_DAMAGE + i * 8
        kind = struct.unpack_from('<I', raw, at)[0]
        if kind == 0:
            add(field('elements::added.type', 'Added element', sec, 'HealthComponentData', resource, record, at, 'u32',
                      note='An element whose damage the next row scales.', shared=shared) |
                {'options': options({k: v for k, v in ELEMENTS.items() if k not in present})})
            add(field('elements::added.multiplier', 'Added element: damage taken', sec, 'HealthComponentData', resource, record,
                      at + 4, 'f32', 'ratio', 0, 100, 'While Added element is None this scales every hit with no element: bullets, '
                      'blades and most explosions.', shared))
            break
        if kind in ELEMENTS:
            add(field('elements::%s.multiplier' % slug(ELEMENTS[kind]), ELEMENTS[kind] + ' damage taken', sec,
                      'HealthComponentData', resource, record, at + 4, 'f32', 'ratio', 0, 100,
                      'Multiplies all %s damage it takes, from hits and statuses.' % ELEMENTS[kind].lower(), shared))


# The components.json entries the offered fields need (the live editor and the loader check one layout).
COMPONENTS = {
    'AiEnemy': {'component': 'AiEnemyComponentData', 'index': 302, 'stride': 732, 'meaning': "an enemy's AI behaviours: each state's "
                'movement settings, its charge', 'applies': 'now', 'appliesVerified': False,
                'appliesSource': 'read from the type record (0x511CC0) as a state begins (0x7F0180) and every charge tick (0xAA8680)'},
    'Locomotion': {'component': 'LocomotionComponentData', 'index': 311, 'stride': 4624, 'meaning': "how an enemy's legs move it: eight "
                   'locomotion sets (animation speed bands, turn animations), its acceleration and deceleration', 'applies': 'respawn',
                   'appliesVerified': False, 'appliesSource': 'acceleration and deceleration are copied into the instance at spawn (0x9CD990)'},
    'StatusEffectReceiver': {'component': 'StatusEffectReceiverComponentData', 'index': 307, 'stride': 46600,
                             'meaning': 'the statuses an entity takes: 8 zones of 33 susceptibility entries (0xB0 each, packed up to the '
                                        'first type 0; zone 0 at +8) and 8 duration multipliers at +0xB5C8',
                             'applies': 'now', 'appliesVerified': False,
                             'appliesSource': 'read from the type record at every hit (FUN_00512740, ApplyStatus 0x699E40)'},
}


def mapping_fields():
    out = {'AiEnemy': {}, 'Locomotion': {}, 'StatusEffectReceiver': {}, 'Health': {}}
    for key, name, at in STATES:
        for leaf, off, meaning in (('speed', 0, 'move speed in m/s (used unless both random ends are above 0)'),
                                   ('speed_min', 8, 'random move speed, lowest'), ('speed_max', 12, 'random move speed, highest')):
            out['AiEnemy']['%s_%s' % (key, leaf)] = {'offset': at + off, 'type': 'f32', 'meaning': '%s behaviour: %s' % (name, meaning),
                                                     'source': 'game.dll 0x7F0180 (state entry), 0x964830 (velocity cap)'}
    out['AiEnemy']['charge_acceleration'] = {'offset': 0x124, 'type': 'f32', 'meaning': 'locomotion acceleration while charging (m/s^2)',
                                             'source': 'game.dll 0xAA85D0 (charge start, Locomotion_SetLimitA)'}
    out['AiEnemy']['charge_tracking_angle'] = {'offset': 0x12C, 'type': 'f32', 'meaning': 'it keeps homing while the target is within this '
                                               'many degrees of its facing', 'source': 'game.dll 0xAA8680 (charge tick)'}
    for key, off in (('acceleration', 0x1170), ('deceleration', 0x1174)):
        out['Locomotion'][key] = {'offset': off, 'type': 'f32', 'meaning': key + ' (m/s^2; -1 the default)',
                                  'source': 'game.dll 0x9CD990 (copied at spawn), 0x9C77D0'}
    s = out['StatusEffectReceiver']
    for k in range(ENTRIES):
        at = ZONE0 + k * ENTRY
        s['entry_%d_type' % k] = {'offset': at + 0x8C, 'type': 'u32', 'meaning': 'zone 0 entry %d: susceptibility type (0 ends the list)' % k,
                                  'source': 'game.dll FUN_006992A0 (lookup), FUN_00690B00 (allowed statuses, at spawn)', 'applies': 'respawn',
                                  'appliesSource': "each enemy's allowed statuses are built from the record when it spawns (FUN_00690B00)"}
        s['entry_%d_min_buildup' % k] = {'offset': at + 0x90, 'type': 'f32', 'meaning': 'zone 0 entry %d: buildup below which it never applies' % k,
                                         'source': 'game.dll 0x69A0D6'}
        s['entry_%d_sure_buildup' % k] = {'offset': at + 0x94, 'type': 'f32', 'meaning': 'zone 0 entry %d: chance to apply = buildup / this' % k,
                                          'source': 'game.dll 0x69A124'}
        s['entry_%d_damage_multiplier' % k] = {'offset': at + 0xA8, 'type': 'f32', 'meaning': 'zone 0 entry %d: tick damage multiplier' % k,
                                               'source': 'game.dll 0x694C4E'}
    for i in range(8):
        s['duration_%d_type' % i] = {'offset': DURATIONS + i * 8, 'type': 'u32', 'source': 'game.dll 0x691A7F',
                                     'meaning': 'duration multiplier %d: susceptibility type (0 ends the list)' % i}
        s['duration_%d_multiplier' % i] = {'offset': DURATIONS + i * 8 + 4, 'type': 'f32', 'source': 'game.dll 0x691A7F',
                                           'meaning': 'duration multiplier %d (0 or less is ignored)' % i}
    for i in range(4):
        for leaf, off, meaning in (('type', 0, 'the element (0 matches damage with no element)'), ('multiplier', 4, "multiplier of that element's damage")):
            out['Health']['element_%d_%s' % (i, leaf)] = {'offset': ELEMENT_DAMAGE + i * 8 + off, 'type': 'u32' if off == 0 else 'f32',
                                                          'meaning': 'element damage %d: %s' % (i, meaning), 'applies': 'now',
                                                          'appliesSource': 'read at every hit', 'source': 'game.dll 0x9239E2 (ApplyDamage 0x9235F0)'}
    return out


def update_mappings():
    path = ROOT / 'mappings/components.json'
    m = json.loads(path.read_text(encoding='utf-8'))
    comps = m['components']
    for name, c in COMPONENTS.items():
        comps.setdefault(name, dict(c, layout='25480438', fields={}))
    for name, fields in mapping_fields().items():
        taken = {f['offset'] for f in comps[name]['fields'].values()}
        for key, f in fields.items():
            if f['offset'] not in taken:
                comps[name]['fields'][key] = {k: v for k, v in f.items() if k not in ('applies', 'appliesSource')} | {'verified': False} | \
                    {k: v for k, v in f.items() if k in ('applies', 'appliesSource')}
    with open(path, 'w', encoding='utf-8', newline='\n') as out:
        out.write(json.dumps(m, indent=2, ensure_ascii=False) + '\n')


def main(runtime, melee_map, out):
    entities = json.loads((ROOT / 'mappings/entities.json').read_text(encoding='utf-8'))['entities']
    attacks = {k: v['damage'] for k, v in json.loads(Path(melee_map).read_text(encoding='utf-8')).items()}
    users = {}
    for name, ids in attacks.items():
        for damage in ids:
            users.setdefault(damage, set()).add(name)
    view = view_of(runtime)
    targets = {}
    for name, e in entities.items():
        if e['kind'] != 'enemy' and e.get('faction') not in FACTIONS:
            continue
        fields = []
        movement(view, e['resource'], fields.append)
        detection(view, e['resource'], fields.append)
        melee(name, attacks.get(name, {}), users, fields.append)
        elements(view, e['resource'], fields.append)
        statuses(view, e['resource'], fields.append)
        if fields:
            targets[name] = fields
    update_mappings()
    result = {'format': 1, 'build': '25480438',
              'note': 'Enemy combat fields players asked for (scripts/ExportEnemyCombat.py).',
              'enemies': dict(sorted(targets.items()))}
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(json.dumps(result, indent=1, ensure_ascii=False) + '\n')
    print('Exported', sum(len(v) for v in targets.values()), 'fields on', len(targets), 'enemy pages')


if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
