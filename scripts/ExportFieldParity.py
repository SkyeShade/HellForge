"""Export the fields HellForge's editor adds where HD2Runtime's catalogues stop short (field parity): the same fields as a
like item, on the records and rows this item owns. Metadata only: field ids, labels, record ids, offsets, ranges and
row ids; no game values.

Usage: py scripts/ExportFieldParity.py <HD2Runtime-dev6> mappings/parity.json

Reads src/HellForge.Core/stats-schema.json (the exported catalogues), mappings/vehicles.json, and the pinned build's
generated_entities.dl_bin and settings files through HD2Runtime-dev6's scanner (scripts/scan: which component records an
entity owns, a projectile row's damage type and explosions, a spray weapon's damage type). Adds, where a target lacks
them:
  handling      a mounted or underbarrel weapon's sway and recoil (WeaponData), as the AR-23 Liberator shows them
                (and the underbarrel's ergonomics)
  charge        a weapon owning a WeaponCharge record: the PLAS-39 Accelerator Rifle's charge fields (the Purifier,
                the Loyalist)
  arc rate      an arc weapon's rate of fire (ArcWeapon +4), as the ARC-12 Blitzer shows it (the K-9's gun)
  underbarrel   an underbarrel's projectile and damage rows: a weapon firing the same projectile lends its rows (the
                One-Two's grenade is the GP-31's), else the Liberator's projectile and damage fields on its own rows;
                a spray underbarrel its damage type (SprayWeapon +200), as the FLAM-66 Torcher shows it
  explosions    the explosion damage share of every target with a health record: the default zone's (+388) and, for
                each zone its page shows, the zone's own share (+0x144) and whether explosions use it (+0x143)
                (game.dll 0x129D180: a zone with the flag uses its own share, else the default zone's; an unset share,
                FLT_MAX, falls back to the default, and an unset default to 1, full damage)
  weapon rows   rows the like weapons have, on every weapon whose record has the byte (weapon_rows):
                reload       the reload duration override (WeaponReload +56) of a weapon with a reload ability
                feeds        rounds per reload of a weapon loading round by round (WeaponRounds +92)
                suppressed   the Suppressed flag (WeaponData +112) of the support weapons
                fire rate    the rate of fire (ProjectileWeapon +8, and its slots +4 and +12 where set)
                reticle      the third-person reticle (WeaponData +400) where the weapon has one
"""
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HANDLING = ['weapon.sway', 'weapon.recoil_drift_horizontal', 'weapon.recoil_drift_vertical', 'weapon.recoil_climb_horizontal',
            'weapon.recoil_climb_vertical']
PROJECTILE_KEYS = ['projectile.velocity', 'projectile.mass', 'projectile.drag', 'projectile.gravity', 'projectile.pellet_count',
                   'projectile.penetration_slowdown']
DAMAGE_KEYS = ['damage.standard_damage', 'damage.durable_damage', 'damage.ap_direct', 'damage.ap_slight', 'damage.ap_large',
               'damage.ap_extreme', 'damage.demolition', 'damage.stagger', 'damage.push_force', 'damage.status_1_strength',
               'damage.status_2_strength', 'damage.status_3_strength', 'damage.status_4_strength']
ZONE_BASE, ZONE_STRIDE, DEFAULT_ZONE = 520, 552, 64
EXPLOSION_FLAG, EXPLOSION_SHARE = 0x143, 0x144
DEFAULT_NOTE = ('Share of explosion damage taken where a zone has no explosion share of its own (1 = full damage; '
                '3.4e38 = unset, full damage)')
FLAG_NOTE = 'Yes: explosions hitting this zone take its own explosion share; No: the default share (Health & Armor)'
SHARE_NOTE = 'Used while "Own explosion share" is Yes (3.4e38 = unset: the default share)'


def scanner(dev6):
    sys.path.insert(0, str(Path(dev6) / 'scripts'))
    from scan.tables import pinned  # noqa: E402 (HD2Runtime-dev6: the pinned build's generated_entities.dl_bin)
    from scan.settings import SettingsView  # noqa: E402
    tables = pinned()
    return tables, SettingsView(tables)


def entry(key, f, **over):
    """A parity field from a schema field (the like item's), re-pointed by over."""
    e = {'key': key, 'label': f['name'], 'kind': f['kind'], 'component': f.get('component', ''), 'resource': f['resource'],
         'record': f['record'], 'offset': f['offset'], 'storage': f['storage'], 'min': f.get('min'), 'max': f.get('max'),
         'unit': f.get('unit'), 'shared': f['kind'] != 'component'}
    if f.get('options') is not None:
        e['options'] = f['options']
    e.update(over)
    return e


RELOAD_NOTE = 'Seconds the reload takes; 0 keeps the length of its reload animation (game.dll 0x774B60 scales the animation to it)'
FEED_NOTE = 'Rounds each reload step loads into the feed and takes from the spare rounds (game.dll 0x7768F0); 1 loads them one by one'
SUPPRESSED_NOTE = "The flag the primaries and secondaries show as Suppressed, on this weapon's own handling record"
NEW_RETICLE = {11: 'Laser cannon (11): LAS-98 Laser Cannon'}
HANDHELD = ('primary', 'secondary', 'support')


def weapon_rows(weapons, owner_of, tables, add):
    """Rows the like weapons show, on every weapon whose own record has the byte (the module docstring's weapon rows)."""
    def raw(component, record):
        return tables.component(component).raw(record)
    mg43 = weapons['MG-43 Machine Gun']['fields']
    liberator = weapons['AR-23 Liberator']['fields']
    amr = weapons['APW-1 Anti-Materiel Rifle']['fields']
    kit = json.loads((ROOT / 'mappings/attachments.json').read_text(encoding='utf-8'))
    # weapons whose attachments set the reload duration (their own record's is overwritten when they are assembled)
    timed = {name for name, slots in kit['weapons'].items() for slot in slots.values() if isinstance(slot, dict)
             for a in slot.get('attachments', []) if any(f['id'] == 'attachment.reload_duration' for f in kit['attachments'].get(a, {}).get('fields', []))}
    fired = ('ProjectileWeaponComponentData', 'BeamWeaponComponentData', 'SprayWeaponComponentData', 'ArcWeaponComponentData')
    reticle = dict(map(tuple, amr['weapon.third_person_reticle']['options']))
    reticle.update(NEW_RETICLE)
    for name, t in weapons.items():
        fields = t['fields']
        resource, record = owner_of(name, 'WeaponReloadComponentData')
        if record is not None and name not in timed and struct.unpack_from('<I', raw('WeaponReloadComponentData', record), 4)[0] != 0:
            add('weapons', name, entry('reload.duration', mg43['reload.duration'], resource=resource, record=record, note=RELOAD_NOTE))
        resource, record = owner_of(name, 'WeaponRoundsComponentData')
        if record is not None:
            add('weapons', name, {'key': 'rounds.rounds_per_reload', 'label': 'Rounds per reload', 'kind': 'component',
                                  'component': 'WeaponRoundsComponentData', 'resource': resource, 'record': record, 'offset': 92,
                                  'storage': 'u32', 'min': 1, 'max': 255, 'unit': 'rounds', 'shared': False, 'note': FEED_NOTE})
        resource, record = owner_of(name, 'WeaponDataComponentData')
        if record is not None and owner_of(name, 'AbilityWeaponComponentData')[1] is None:
            data = raw('WeaponDataComponentData', record)
            if t.get('slot') == 'support' and any(owner_of(name, c)[1] is not None for c in fired):
                add('weapons', name, entry('weapon.suppressed', liberator['weapon.suppressed'], resource=resource, record=record,
                                           note=SUPPRESSED_NOTE))
            style = struct.unpack_from('<I', data, 400)[0]
            if style != 0 and t.get('slot') in HANDHELD and 'weapon.third_person_reticle' not in t.get('blocked', {}):
                if style not in reticle:
                    raise SystemExit(f'{name}: reticle style {style} has no label')
                add('weapons', name, entry('weapon.third_person_reticle', amr['weapon.third_person_reticle'], resource=resource, record=record,
                                           options=[[k, v] for k, v in sorted(reticle.items())]))
        resource, record = owner_of(name, 'ProjectileWeaponComponentData')
        if record is not None and t.get('slot') in HANDHELD:
            slots = struct.unpack_from('<3f', raw('ProjectileWeaponComponentData', record), 4)
            if slots[1] > 0 and 'weapon.fire_rate' not in fields:
                for key, value in (('fire_rate.modes.x', slots[0]), ('weapon.fire_rate', slots[1]), ('fire_rate.modes.z', slots[2])):
                    if value > 0:
                        add('weapons', name, entry(key, amr[key], resource=resource, record=record))


def main(dev6, out):
    schema = json.loads((ROOT / 'src/HellForge.Core/stats-schema.json').read_text(encoding='utf-8'))
    vehicles = json.loads((ROOT / 'mappings/vehicles.json').read_text(encoding='utf-8'))['vehicles']
    tables, settings = scanner(dev6)
    targets = schema['targets']
    weapons = targets['weapons']
    added = {}
    owners = {}

    def owned(component, record):
        if (component, record) not in owners:
            owners[(component, record)] = len(tables.component(component).owners(record))
        return owners[(component, record)]

    def add(category, name, e):
        t = targets[category].get(name)
        if t is not None:
            if e['key'] in t['fields']:
                return
            taken = {(f['kind'], f.get('component', ''), f['resource'], f['record'], f['offset']) for f in t['fields'].values()}
            if (e['kind'], e['component'], e['resource'], e['record'], e['offset']) in taken:
                return
        if e['kind'] == 'component':
            e['shared'] = owned(e['component'], e['record']) > 1
        added.setdefault(category, {}).setdefault(name, []).append(e)

    def records(name):
        """A weapon target's entities (its component fields' resources) and the component records each owns."""
        out = {}
        for r in sorted({f['resource'] for f in weapons[name]['fields'].values() if f['kind'] == 'component'}):
            out[r] = tables.entity(int(r, 16))
        return out

    def owner_of(name, component):
        for r, comps in records(name).items():
            if component in comps:
                return r, comps[component]
        return None, None

    def row(kind, native):
        raw = settings.row(kind, native)
        if raw is None:
            raise SystemExit(f'no {kind} row {native}')
        return raw

    liberator = weapons['AR-23 Liberator']['fields']
    accelerator = weapons['PLAS-39 Accelerator Rifle']['fields']
    blitzer = weapons['ARC-12 Blitzer']['fields']
    torcher = weapons['FLAM-66 Torcher']['fields']

    for name, t in weapons.items():
        slot = t.get('slot')
        # handling: sway and recoil of mounted and underbarrel weapons (not a shield arm, an AbilityWeapon)
        if slot in ('mounted', 'underbarrel'):
            resource, record = owner_of(name, 'WeaponDataComponentData')
            if record is not None and owner_of(name, 'AbilityWeaponComponentData')[1] is None:
                for key in HANDLING + (['weapon.ergonomics'] if slot == 'underbarrel' else []):
                    add('weapons', name, entry(key, liberator[key], resource=resource, record=record))
        # charge: a weapon owning a WeaponCharge record shows its charge fields
        resource, record = owner_of(name, 'WeaponChargeComponentData')
        if record is not None and not any(k.startswith('charge.') for k in t['fields']):
            for key, f in sorted(accelerator.items(), key=lambda kv: kv[1]['order']):
                if f.get('component') == 'WeaponChargeComponentData':
                    add('weapons', name, entry(key, f, resource=resource, record=record))
        # arc rate of fire (ArcWeapon +4, rounds per minute; the Arc Thrower's -1 is charge-driven, not a rate)
        resource, record = owner_of(name, 'ArcWeaponComponentData')
        if record is not None:
            rate = struct.unpack_from('<f', tables.component('ArcWeaponComponentData').raw(record), 4)[0]
            if rate > 0:
                add('weapons', name, entry('weapon.fire_rate', blitzer['weapon.fire_rate'], resource=resource, record=record))
        # underbarrel: the projectile and damage rows it fires
        if slot == 'underbarrel':
            resource, record = owner_of(name, 'ProjectileWeaponComponentData')
            if record is not None:
                projectile = struct.unpack_from('<I', tables.component('ProjectileWeaponComponentData').raw(record), 0)[0]
                raw = row('projectile', projectile)
                damage = struct.unpack_from('<I', raw, 60)[0]
                explosions = {struct.unpack_from('<I', raw, 144)[0], struct.unpack_from('<I', raw, 156)[0]} - {0}
                donor = next((w for w, wt in weapons.items() if w != name and wt.get('slot') != 'underbarrel' and
                              any(f['kind'] == 'ProjectileSettings' and f['record'] == projectile and k.startswith('projectile.')
                                  and len(k.split('.')) == 2 for k, f in wt['fields'].items())), None)
                if donor:
                    chain = {('ProjectileSettings', projectile), ('DamageInfo', damage)}
                    chain |= {('ExplosionSettings', x) for x in explosions}
                    # the explosions' damage rows: the donor's rows under its explosion keys
                    df = weapons[donor]['fields']
                    for k, f in df.items():
                        if f['kind'] == 'ExplosionSettings' and f['record'] in explosions:
                            prefix = k.rsplit('.', 1)[0] + '.damage.'
                            chain |= {('DamageInfo', g['record']) for kk, g in df.items() if kk.startswith(prefix) and g['kind'] == 'DamageInfo'}
                    for k, f in sorted(df.items(), key=lambda kv: kv[1]['order']):
                        if (f['kind'], f['record']) in chain and k.split('.')[0] in ('projectile', 'damage', 'explosion'):
                            add('weapons', name, entry(k, f))
                else:
                    if explosions:
                        raise SystemExit(f'{name}: projectile {projectile} has explosions {explosions} and no weapon to lend its rows')
                    for key in PROJECTILE_KEYS:
                        add('weapons', name, entry(key, liberator[key], record=projectile))
                    for key in DAMAGE_KEYS:
                        add('weapons', name, entry(key, liberator[key], record=damage))
            resource, record = owner_of(name, 'SprayWeaponComponentData')
            if record is not None:
                damage = struct.unpack_from('<I', tables.component('SprayWeaponComponentData').raw(record), 200)[0]
                for key in DAMAGE_KEYS:
                    add('weapons', name, entry(key, torcher[key], record=damage))

    # overcharge explosion (WeaponCharge +200, an ExplosionType; set off when "explode at overcharge" is on): on every
    # weapon that shows that flag (HD2R Editor's Epoch and Railgun row; the Accelerator's, the Purifier's, the Loyalist's)
    overcharge = {}  # each charge weapon's own overcharge explosion, by name (HD2R Editor presets name the weapon)
    for name, t in weapons.items():
        flagged = [f for k, f in t['fields'].items() if k == 'charge.explode_at_overcharge'] or \
            [e for e in added.get('weapons', {}).get(name, []) if e['key'] == 'charge.explode_at_overcharge']
        if flagged:
            f = flagged[0]
            add('weapons', name, {'key': 'charge.overcharge_explosion', 'label': 'Overcharge explosion', 'kind': 'component',
                                  'component': 'WeaponChargeComponentData', 'resource': f['resource'], 'record': f['record'], 'offset': 200,
                                  'storage': 'u32', 'min': None, 'max': None, 'unit': None, 'shared': False, 'ref': 'explosion',
                                  'note': 'Set off when the overcharge time is reached, while "Explode when the overcharge time is reached" is Yes'})
            overcharge[name] = struct.unpack_from('<I', tables.component('WeaponChargeComponentData').raw(f['record']), 200)[0]

    # HD2R Editor's one-off fields the catalogues lack (its rows on the same bytes)
    def health_of(name):
        return sorted({(f['resource'], f['record']) for f in targets['backpacks'][name]['fields'].values() if f.get('component') == 'HealthComponentData'},
                      key=lambda rr: rr[1])
    armor = {'kind': 'component', 'component': 'HealthComponentData', 'offset': 280, 'storage': 'u32', 'min': 0, 'max': 10, 'unit': 'armor_class',
             'shared': False}
    for name in ('AX/AR-23 Guard Dog', 'AX/ARC-3 K-9', 'AX/FLAM-75 Hot Dog', 'AX/LAS-5 Rover', 'AX/TX-13 Dog Breath'):
        (resource, record), = health_of(name)
        add('backpacks', name, dict(armor, key='entity.armor', label='Drone armor', resource=resource, record=record))
    (resource, record), = health_of('SH-20 Ballistic Shield Backpack')
    add('backpacks', 'SH-20 Ballistic Shield Backpack', dict(armor, key='entity.armor', label='Armor (default zone)', resource=resource, record=record,
        note='The default zone\'s: hits on the shield plate use its zone\'s armor (Damage zones)'))
    sh51 = {f['record']: f['resource'] for f in targets['backpacks']['SH-51 Directional Shield']['fields'].values() if f.get('component') == 'HealthComponentData'}
    add('backpacks', 'SH-51 Directional Shield', dict(armor, key='entity.armor', label='Armor (backpack)', resource=sh51[296], record=296))
    add('backpacks', 'SH-51 Directional Shield', dict(armor, key='entity.armor_deployed', label='Armor (deployed shield)', resource=sh51[80], record=80))
    shield = targets['backpacks']['SH-51 Directional Shield']['fields']['shield.recharge_rate']
    add('backpacks', 'SH-51 Directional Shield', {'key': 'shield.radius', 'label': 'Shield radius', 'kind': 'component', 'component': 'ShieldComponentData',
        'resource': shield['resource'], 'record': shield['record'], 'offset': 0, 'storage': 'f32', 'min': 0.1, 'max': 100, 'unit': 'meters', 'shared': False})
    deposit = tables.component('DepositComponentData')
    owner = deposit.owners(9)
    if len(owner) == 1:
        supply = targets['backpacks']['B-1 Supply Pack']['fields']
        for key, label in (('deposit.capacity', 'Capacity'), ('deposit.start_amount', 'Amount at call-in'), ('deposit.refill_amount', 'Restored by resupply')):
            f = supply[key]
            add('backpacks', 'B-100 Portable Hellbomb', entry(key, f, label=label, resource='0x%016X' % owner[0], record=9, unit=None))
    resource, record = owner_of('AR/GL-21 One-Two', 'WeaponDataComponentData')
    modes = struct.unpack_from('<3I', tables.component('WeaponDataComponentData').raw(record), 144)
    names = dict(liberator['fire_mode.modes.1']['options'])
    add('weapons', 'AR/GL-21 One-Two', {'key': 'weapon.default_fire_mode', 'label': 'Default fire mode', 'kind': 'component',
        'component': 'WeaponDataComponentData', 'resource': resource, 'record': record, 'offset': 144, 'storage': 'u32', 'min': None, 'max': None,
        'unit': None, 'shared': False, 'options': [[m, names.get(m, str(m))] for m in sorted(set(modes)) if m in (1, 2)],  # Automatic, Single
        'note': "Its own fire modes only, reordered (HD2R Editor: the weapon's other fire-control data is not understood)"})
    for name in ('A/G-16 Gatling Sentry / weapon', 'A/MG-43 Machine Gun Sentry / weapon'):
        sentry = {'A/G-16 Gatling Sentry / weapon': '0xEF85D6CF58E31D70', 'A/MG-43 Machine Gun Sentry / weapon': '0x37CDE43876BA26BB'}[name]
        record = tables.entity(int(sentry, 16)).get('ProjectileWeaponComponentData')
        if record is not None:
            add('weapons', name, {'key': 'attack.primary.projectile', 'label': 'Projectile', 'kind': 'component',
                'component': 'ProjectileWeaponComponentData', 'resource': sentry, 'record': record, 'offset': 0, 'storage': 'u32',
                'min': None, 'max': None, 'unit': None, 'shared': False, 'ref': 'projectile'})

    weapon_rows(weapons, owner_of, tables, add)

    # explosions: every target with a health record
    def explosion_fields(category, name, resource, record, zones, section=None, zone_sections=None):
        raw = tables.component('HealthComponentData').raw(record)
        common = {'kind': 'component', 'component': 'HealthComponentData', 'resource': resource, 'record': record,
                  'unit': 'multiplier', 'shared': False}
        e = dict(common, key='entity.explosive_damage_percentage', label='Explosion damage taken', offset=DEFAULT_ZONE + EXPLOSION_SHARE,
                 storage='f32', min=0, max=10, note=DEFAULT_NOTE)
        if section:
            e['section'] = section
        add(category, name, e)
        for z, key in zones:
            base = ZONE_BASE + z * ZONE_STRIDE
            if struct.unpack_from('<I', raw, base + 96)[0] == 0:
                continue  # an unpopulated zone slot
            extra = {'section': zone_sections[z]} if zone_sections else {}
            add(category, name, dict(common, key=key + '.affected_by_explosions', label='Own explosion share', offset=base + EXPLOSION_FLAG,
                                     storage='u8', min=0, max=1, unit=None, options=[[0, 'No'], [1, 'Yes']], note=FLAG_NOTE, **extra))
            add(category, name, dict(common, key=key + '.explosive_damage_percentage', label='Explosion damage taken',
                                     offset=base + EXPLOSION_SHARE, storage='f32', min=0, max=10, note=SHARE_NOTE, **extra))

    for category, ts in targets.items():
        if category in ('helldiver', 'vehicles', 'projectiles', 'damage'):
            continue  # the Helldiver's zone shares are never read (no avatar zone has the flag); vehicles below
        for name, t in ts.items():
            health = [f for f in t['fields'].values() if f.get('component') == 'HealthComponentData']
            if not health:
                continue
            zoned = [f for f in health if f['offset'] >= ZONE_BASE]
            pick = (zoned or health)[0]
            zones = sorted({(f['offset'] - ZONE_BASE) // ZONE_STRIDE for f in zoned if f['resource'] == pick['resource']})
            explosion_fields(category, name, pick['resource'], pick['record'], [(z, 'zone') for z in zones] if len(zones) == 1 else
                             [(z, f'zone_{z}') for z in zones])
    for name, v in vehicles.items():
        health = next(f for f in v['fields'] if f['key'] == 'entity.health')
        sections = {}
        for f in v['fields']:
            if f['key'].startswith('zone_'):
                sections[int(f['key'][5:f['key'].index('.')])] = f['section']
        explosion_fields('vehicles', name, health['resource'], health['record'], [(z, f'zone_{z}') for z in sorted(sections)],
                         section='Health & Armor', zone_sections=sections)

    count = sum(len(fs) for c in added.values() for fs in c.values())
    data = {'format': 1, 'build': '25480438',
            'note': 'Fields the editor adds where HD2Runtime 0.40.1 catalogues stop short (scripts/ExportFieldParity.py).',
            'targets': {c: {n: added[c][n] for n in sorted(added[c])} for c in sorted(added)},
            'overchargeExplosions': dict(sorted(overcharge.items()))}
    Path(out).write_text(json.dumps(data, indent=1, ensure_ascii=False) + '\n', encoding='utf-8', newline='\n')
    print(f'{count} fields on {sum(len(c) for c in added.values())} targets')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(*sys.argv[1:])
