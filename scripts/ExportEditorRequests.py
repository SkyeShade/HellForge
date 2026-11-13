"""Export the fields players asked the editor for that sit on records the exported catalogues never reach (the Discord
list "Missing stuff from the HellForge's editor"). Metadata only: field ids, labels, record ids, offsets, ranges and row
ids; no game values.

Usage: py scripts/ExportEditorRequests.py <HD2Runtime-0.40.1> mappings/editor-requests.json

Reads src/HellForge.Core/stats-schema.json and the pinned build's datalibrary through HD2Runtime 0.40.1's
scripts/migration/build_view (which component records an entity owns, an explosion row's shrapnel projectile). Adds:
  lock-on      a weapon's or seeker grenade's own SensorEye range (+0): how far the FAF-14 Spear, the W.A.S.P., the
               Commando, the Warrant and the Missile Pistol lock on, and how far each one's missile looks for targets
  c4           the B/MD C4 Pack's backpack (rack slot 1): its DepositComponent, the charges it holds, starts with and
               gets from a resupply
  battlement   the E/GL-21 Grenadier Battlement's gun (its MountComponent slot 0): handling, rate of fire and
               ammunition, the fields the GL-21 Grenade Launcher shows, on the gun's own records
  designator   the MS-11 Solo Silo's laser remote (rack slot 1): its sway and ergonomics
  payloads     the FAF-14 Spear's impact payload (its ProjectileWeapon +0 round, the missile's hit row) and the Solo
               Silo missile's detonation and impact payloads (its ExplosiveComponent +0x24 / +0x28)
  shrapnel     each explosion's shrapnel projectile (ExplosionSettings +0x54), for the bomblet fields of its page
"""
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPEAR, SPEAR_ROUND = 'FAF-14 Spear', 225
C4_RACK, SILO_RACK = 0x0AC9CDDBE8C64851, 0xDE18775FA447A9BF
BATTLEMENT = 0x0C8257D1C0255593
BATTLEMENT_KEYS = ['weapon.sway', 'weapon.horizontal_spread', 'weapon.vertical_spread', 'weapon.recoil_drift_horizontal',
                   'weapon.recoil_drift_vertical', 'weapon.recoil_climb_horizontal', 'weapon.recoil_climb_vertical', 'weapon.fire_rate',
                   'weapon.capacity', 'magazine.starting_magazines', 'magazine.magazines_from_supply', 'magazine.spare_magazines',
                   'reload.duration']
EXTRA = ('AbilityWeaponComponentData', 'LaserDesignatorComponentData')


def view_of(runtime):
    sys.path.insert(0, str(Path(runtime) / 'scripts'))
    import build_profile  # noqa: E402 (HD2Runtime 0.40.1: the pinned build's datalibrary)
    from migration import build_view  # noqa: E402
    build_view.COMPONENTS = tuple(build_view.COMPONENTS) + EXTRA
    return build_view.from_datalibrary(build_profile.datalibrary())


def u32(raw, offset):
    return struct.unpack_from('<I', raw, offset)[0]


def u64(raw, offset):
    return struct.unpack_from('<Q', raw, offset)[0]


def field(key, label, component, resource, record, offset, storage, unit=None, lo=None, hi=None, note=None, kind='component'):
    f = {'kind': kind, 'component': component if kind == 'component' else '', 'resource': '0x%016X' % resource if kind == 'component' else '0x0',
         'record': record, 'offset': offset, 'storage': storage, 'min': lo, 'max': hi, 'unit': unit, 'shared': kind != 'component',
         'key': key, 'label': label}
    if note:
        f['note'] = note
    return f


def main(runtime, out):
    schema = json.loads((ROOT / 'src/HellForge.Core/stats-schema.json').read_text(encoding='utf-8'))
    targets = schema['targets']
    view = view_of(runtime)
    comps = view.resource_components()
    added = {}

    def record(component, resource):
        r = view.record(component, resource)
        return None if r is None else r['recordIndex']

    def add(category, name, f):
        t = targets[category][name]
        taken = {(g['kind'], g.get('component', ''), g['resource'], g['record'], g['offset']) for g in t['fields'].values()}
        if f['key'] in t['fields'] or (f['kind'], f['component'], f['resource'], f['record'], f['offset']) in taken:
            return
        if f['kind'] == 'component':
            f['shared'] = len(view.component(f['component']).owners_of(f['record'])) > 1
        added.setdefault(category, {}).setdefault(name, []).append(f)

    # every target's catalogued records must be this build's (the schema and the datalibrary agree)
    for category in ('weapons', 'throwables'):
        for name, t in targets[category].items():
            for f in t['fields'].values():
                if f['kind'] == 'component' and f['resource'] != '0x0':
                    assert record(f['component'], int(f['resource'], 16)) == f['record'], (name, f)

    # lock-on: the SensorEye of the weapon itself (Lock-on range) and of its missile (Missile seeker range), where no
    # page shows that record yet (a sentry's is on its stratagem's page)
    shown = {(f['resource'], f['record']) for c in targets.values() for t in c.values() for f in t['fields'].values()
             if f.get('component') == 'SensorEyeComponentData'}
    for category in ('weapons', 'throwables'):
        for name, t in targets[category].items():
            resources = sorted({int(f['resource'], 16) for f in t['fields'].values() if f['kind'] == 'component' and f['resource'] != '0x0'})
            for res in resources:
                eye = record('SensorEyeComponentData', res)
                if eye is None or ('0x%016X' % res, eye) in shown:
                    continue
                if 'SeekingMissileComponentData' in (comps.get(res) or []):
                    # the missile's own fields name it: missile.* or, for a weapon function's missile, function_missile.*
                    prefix = next((k.split('.')[0] for k, f in t['fields'].items() if f['resource'] == '0x%016X' % res and '.' in k), 'missile')
                    add(category, name, field(prefix + '.seeker_range', ('Function missile' if prefix == 'function_missile' else 'Missile') + ' seeker range', 'SensorEyeComponentData', res, eye, 0, 'f32', 'meters', 0, 1000,
                                              'How far its missile looks for a target in flight.'))
                else:
                    add(category, name, field('targeting.range', 'Seeker range' if category == 'throwables' else 'Lock-on range',
                                              'SensorEyeComponentData', res, eye, 0, 'f32', 'meters', 0, 1000,
                                              'How far it finds a target to lock on to.'))

    def rack_item(rack, slot):
        return u64(view.record('HellpodRackComponentData', rack)['bytes'], slot * 64)

    # the C4 pack's backpack: the charges it holds
    pack = rack_item(C4_RACK, 1)
    dep = record('DepositComponentData', pack)
    for key, label, offset, storage in (('deposit.capacity', 'Charges the pack holds', 0, 'u32'), ('deposit.start_amount', 'Charges at call-in (-1 = full)', 4, 'i32'),
                                        ('deposit.refill_amount', 'Charges from supply', 8, 'u32')):
        add('weapons', 'B/MD C4 Pack', field(key, label, 'DepositComponentData', pack, dep, offset, storage, 'count', -1 if storage == 'i32' else 0, 1048575))

    # the Grenadier Battlement's gun: the GL-21's handling and ammunition fields on the gun's own records
    gun = u64(view.record('MountComponentData', BATTLEMENT)['bytes'], 0)
    gl21 = targets['weapons']['GL-21 Grenade Launcher']['fields']
    for key in BATTLEMENT_KEYS:
        f = gl21[key]
        add('stratagems', 'E/GL-21 Grenadier Battlement', field(key, f['name'], f['component'], gun, record(f['component'], gun), f['offset'], f['storage'],
                                                                 f.get('unit'), f.get('min'), f.get('max')))

    # the Solo Silo's laser remote: its handling
    remote = rack_item(SILO_RACK, 1)
    wd = record('WeaponDataComponentData', remote)
    spear = targets['weapons'][SPEAR]['fields']
    for key in ('weapon.sway', 'weapon.ergonomics'):
        f = spear[key]
        add('weapons', 'MS-11 Solo Silo', field('remote.' + key.split('.')[1], 'Remote ' + f['name'][0].lower() + f['name'][1:], f['component'], remote, wd,
                                                f['offset'], f['storage'], f.get('unit'), f.get('min'), f.get('max')))

    # payloads: the Spear's round (the missile's hit row) and the silo missile's explosive
    assert u32(view.record('ProjectileWeaponComponentData', 0x25AA2FD4643CF4EE)['bytes'], 0) == SPEAR_ROUND
    add('weapons', SPEAR, field('terminal.primary.impact.explosion', 'Impact payload', '', 0, SPEAR_ROUND, 144, 'u32', kind='ProjectileSettings',
                                note="The explosion where the missile hits (its round's impact payload)."))
    missile = rack_item(SILO_RACK, 0)
    ex = record('ExplosiveComponentData', missile)
    add('weapons', 'MS-11 Solo Silo', field('fired.detonation_payload', 'Detonation payload', 'ExplosiveComponentData', missile, ex, 36, 'u32',
                                            note='The explosion the missile sets off at its target.'))
    add('weapons', 'MS-11 Solo Silo', field('fired.impact_payload', 'Impact payload', 'ExplosiveComponentData', missile, ex, 40, 'u32',
                                            note='The explosion where the missile hits something on its way.'))

    # shrapnel: each explosion's shrapnel projectile
    shrapnel = {}
    for _, kind, raw in view.settings_table('explosion').rows:
        if u32(raw, 80) and u32(raw, 84):
            shrapnel[str(kind)] = u32(raw, 84)

    result = {'format': 1, 'build': '25480438',
              'note': 'Fields players asked for on records the catalogues never reach (scripts/ExportEditorRequests.py).',
              'targets': {c: dict(sorted(v.items())) for c, v in sorted(added.items())},
              'shrapnel': dict(sorted(shrapnel.items(), key=lambda kv: int(kv[0])))}
    Path(out).write_text(json.dumps(result, indent=1, ensure_ascii=False) + '\n', encoding='utf-8')
    print('Exported', sum(len(v) for c in added.values() for v in c.values()), 'fields and', len(shrapnel), 'shrapnel rows')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(*sys.argv[1:])
