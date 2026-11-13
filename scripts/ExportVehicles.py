"""Export the vehicles' pages (HD2R Editor's vehicle pages) from HD2Runtime's entity and vehicle weapon tables.
Metadata only: names, sections, field ids, record ids, offsets, ranges and defaults as labels; no game bytes.

Usage: py scripts/ExportVehicles.py <HD2Runtime> mappings/vehicles.json

Per vehicle: its fields HD2R Editor shows that the stats schema lacks, keyed for the stats file (entity and rotation
fields by their field id; a damage zone's as "<zone>.<field>": "zone_3.armor"), each in HD2R Editor's section (Health
& Armor, or the zone's name: its native name when the game has one, "Zone N" otherwise); the mounts that are shields
(the EXO-55's left arm); and its mount slots (MountComponentData slot k at +24k: the mounted entity's resource) with the
weapons HD2Runtime allows there.
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from ExportReferences import read_table  # noqa: E402

# HD2R Editor's mount labels (catalog.lua MOUNT_LABELS); the others are their humanized mount node
MOUNT_LABELS = {
    'TD-110 Maelstrom': {'attach_tank_gun': 'Main cannon', 'slot_0': 'Main cannon', 'slot_2': 'Driver gun',
                         'slot_3': 'Missile pods (slot 3)', 'slot_4': 'Missile pods (slot 4)'},
    'TD-220 Bastion MK XVI': {'attach_tank_gun': 'Cannon', 'attach_tank_gun_mg': 'Coaxial MG', 'slot_0': 'Cannon', 'slot_1': 'Coaxial MG'},
}


# HD2R Editor's labels for vehicle fields (its string tables; HD2Runtime's entity fields carry none)
LABELS = {'entity.health': 'Health', 'entity.armor': 'Armor', 'zone.armor': 'Armor', 'zone.health': 'Health',
          'zone.affects_main_health': 'Affects main health', 'rotation.turn_speed': 'Turn speed', 'rotation.acceleration': 'Acceleration',
          'rotation.deceleration': 'Deceleration', 'vehicle.steering_response_speed': 'Steering response speed'}


# Native names of the zones HD2Runtime leaves unnamed, by zone index. A zone's name is a thin hash (top 32 bits of
# MurmurHash64A) at zone +96 in HealthComponentData; its collision actors (zone +456) and, on some zones, an effect
# joint (zone +32) carry names of their own. Each name below matches the zone's hash and a second hash of the same zone
# that agrees with it, so no chance hits. Research: HD2Mods\research\bastion-zones. Variants of one vehicle share their
# zones hash for hash (the tanks' health records 104 and 427 are byte-equal).
# Tank: actors "c_<name>", effect joints "fx_<name>". Track panels and side panels count front to back.
TANK_ZONES = dict(enumerate([
    'hull_front', 'right_front_block', 'left_front_block', 'right_top_block', 'left_top_block', 'back_door_block',
    'right_box_0', 'right_box_1', 'left_box_0', 'left_box_1', 'right_box_2', 'left_box_2',
    'right_side_panel_0', 'right_side_panel_1', 'left_side_panel_0', 'left_side_panel_1',
    'right_side_track_panel_0', 'right_side_track_panel_1', 'right_side_track_panel_2', 'right_side_track_panel_3',
    'left_side_track_panel_0', 'left_side_track_panel_1', 'left_side_track_panel_2', 'left_side_track_panel_3',
    'r_back_panel', 'l_back_panel', 'hull_extras', 'right_track', 'left_track', 'radar', 'antenna']))
# FRV: actors "damage_<short name>" (damage_fl_headlights for front_left_headlights; "break" is the game's spelling),
# the same short name in VisibilityMask and Gore, and on the Supply FRV a damageables joint "c_<short name>". Zones 0-3
# are named like their VisibilityMask entries (the tyres' destroyed meshes).
FRV_ZONES = {0: 'fl_wheel_tire_destroyed', 1: 'fr_wheel_tire_destroyed', 2: 'bl_wheel_tire_destroyed', 3: 'br_wheel_tire_destroyed',
             6: 'right_front_door', 7: 'left_front_door', 8: 'right_rear_door', 9: 'left_rear_door',
             11: 'top_right_hatch', 12: 'top_left_hatch', 14: 'front_left_headlights', 15: 'front_left_foglights',
             16: 'front_right_headlights', 17: 'front_right_foglights', 18: 'front_left_position_light',
             19: 'front_right_position_light', 20: 'right_position_light', 21: 'left_position_light',
             22: 'back_left_break_light', 23: 'back_right_break_light', 24: 'back_fog_light',
             25: 'back_left_wheel_hub', 26: 'front_left_wheel_hub', 27: 'back_right_wheel_hub', 28: 'front_right_wheel_hub'}
# Exosuits: the cockpit's front halves and the two front lights, on the zone hash alone, each from a search of under
# 400k guesses beside its named neighbours (cockpit_rear; the lights a mirrored pair).
EXO_ZONES = {0: 'cockpit_front_left', 1: 'cockpit_front_right', 6: 'light_right_front', 7: 'light_left_front'}
# GATER Oil Rig: its ten wheels, actors "c_<side>_damage_wheel_<front|middle|rear>_<nn>". Zones 0 and 1 hash as
# l_/r_whole_wheel_front_01, the others' own names are not found yet: all ten are named after their actors.
RIG_ZONES = {i: f'{"lr"[i % 2]}_wheel_{pos}' for i, pos in enumerate(
    ['front_01', 'front_01', 'front_02', 'front_02', 'middle_01', 'middle_01', 'rear_01', 'rear_01', 'rear_02', 'rear_02'])}
ZONE_NAMES = {'TD-220 Bastion MK XVI': TANK_ZONES, 'TD-110 Maelstrom': TANK_ZONES, 'GATER Oil Rig': RIG_ZONES,
              'FRV (Super Earth variant)': FRV_ZONES, 'M-102 Gunner FRV': FRV_ZONES, 'M-104 Incinerator FRV': FRV_ZONES,
              'M-103 Supply FRV': FRV_ZONES | {29: 'front_left_dirtlight', 30: 'front_right_dirtlight', 31: 'front_left_rooflight',
                                               32: 'front_right_rooflight', 33: 'front_left_bullbarlight',
                                               34: 'front_right_bullbarlight'},
              **{n: EXO_ZONES for n in ('EXO-45 Patriot Exosuit', 'EXO-49 Emancipator Exosuit', 'EXO-51 Lumberer Exosuit',
                                       'EXO-55 Breakthrough Exosuit')}}


def label_of(f):
    sid = f['semanticFieldId']
    return f.get('displayName') or LABELS.get(sid) or humanize(sid.split('.')[-1])


def humanize(text):
    words = str(text).replace('/', ' ').replace('.', ' ').replace('_', ' ').split()
    out = ' '.join(words)
    return out[:1].upper() + out[1:] if out else str(text)


def mount_label(vehicle, mount, slot):
    labels = MOUNT_LABELS.get(vehicle, {})
    return labels.get(mount or '') or (labels.get(f'slot_{slot}') if slot is not None else None) or humanize(mount or slot or '')


def zone_label(native):
    """A native zone name as a section title: "r_back_panel" -> "Right back panel", "fl_wheel_tire_destroyed" -> "Front left
    wheel tire" (and the FRV's "break" light a brake light)."""
    for short, side in (('fl_', 'front_left_'), ('fr_', 'front_right_'), ('bl_', 'back_left_'), ('br_', 'back_right_'),
                        ('l_', 'left_'), ('r_', 'right_')):
        if native.startswith(short):
            native = side + native[len(short):]
            break
    return humanize(native.removesuffix('_destroyed').replace('break_light', 'brake_light'))


def zone_namer(zones, native={}):
    names, count = {}, {}
    for key, z in (zones or {}).items():
        if not isinstance(z, dict):
            continue
        index = z.get('index')
        if isinstance(z.get('name'), str) and z['name']:
            label = humanize(z['name'])
        elif index in native:
            label = zone_label(native[index])
        else:
            continue
        names[z.get('zoneId') or key] = label
        count[label] = count.get(label, 0) + 1

    def name(zone):
        label = names.get(zone)
        if label and count[label] > 1:
            label += f' ({humanize(zone)})'
        return label or humanize(zone)
    return name


def field_entry(f, label, section, resource, key):
    b = f['backing']
    return {'key': key, 'label': label, 'section': section, 'component': b['component'], 'resource': b.get('resource') or resource,
            'record': b['recordIndex'], 'offset': b['offset'], 'storage': b['storage'], 'min': f.get('min'), 'max': f.get('max'),
            'unit': f.get('unit'), 'base': f.get('currentDefault'), 'hd2r': f.get('instanceKey'),
            'note': next((n['text'] for n in f.get('notices') or [] if n.get('kind') == 'lifecycle'), None)}


def main(runtime, out):
    domains = Path(runtime) / 'domains'
    entities = read_table(domains / 'entity_authoring.lua')
    weapons = read_table(domains / 'vehicle_weapon_authoring.lua')['weapons']
    vehicles = {}
    for name, v in sorted(entities['vehicles'].items()):
        zone_name = zone_namer(v.get('zones'), ZONE_NAMES.get(name, {}))
        fields, mounts = [], []
        for f in v['fields']:
            t = f.get('target') or {}
            if not f.get('editable'):
                continue
            if t.get('path') == 'mount' and f.get('type') == 'mounted_weapon_reference':
                slot = t.get('mount')
                info = (v.get('mounts') or {}).get(slot) or {}
                b = f['backing']
                mounts.append({'slot': info.get('slot'), 'key': slot, 'label': mount_label(name, info.get('role') or slot, info.get('slot')),
                               'component': b['component'], 'record': b['recordIndex'], 'offset': b['offset'],
                               'resource': b.get('resource') or v['resource'], 'default': f['currentDefault'],
                               'allowed': f.get('allowedValues') or [], 'hd2r': f.get('instanceKey')})
            elif isinstance(f.get('currentDefault'), (int, float)) and not isinstance(f.get('currentDefault'), bool) and t.get('path') in ('entity', 'damage_zone'):
                leaf = f['semanticFieldId']
                if t['path'] == 'entity':
                    fields.append(field_entry(f, label_of(f), 'Health & Armor', v['resource'], leaf))
                else:
                    fields.append(field_entry(f, label_of(f), zone_name(t['zone']), v['resource'], f'{t["zone"]}.{leaf.split(".", 1)[1]}'))
        # a mount that is a shield (the EXO-55's left arm): its page part is titled "Shield", its fields are the mount's own
        shields = sorted(wkey for wkey, w in weapons.items() if w.get('vehicle') == name and w.get('mountedShield'))
        keys = [f['key'] for f in fields]
        if len(set(keys)) != len(keys):
            raise ValueError(f'{name}: a key twice')
        vehicles[name] = {'resource': v['resource'], 'fields': fields, 'mounts': mounts, 'shields': shields}
    mounted = {mid: {'name': humanize(m.get('displayName') or mid), 'resource': m.get('resource'), 'family': m.get('attackFamily')}
               for mid, m in sorted(entities.get('mountedWeapons', {}).items())}
    data = {'format': 1, 'build': '25480438',
            'note': 'Vehicle pages as HD2R Editor lays them out (HD2Runtime 0.40.1 domains/entity_authoring, vehicle_weapon_authoring; '
                    'scripts/ExportVehicles.py).',
            'vehicles': vehicles, 'mountedWeapons': mounted}
    Path(out).write_text(json.dumps(data, indent=1, ensure_ascii=False) + '\n', encoding='utf-8', newline='\n')
    print(f'{len(vehicles)} vehicles, {sum(len(v["fields"]) for v in vehicles.values())} fields, '
          f'{sum(len(v["mounts"]) for v in vehicles.values())} mount slots, {len(mounted)} mounted weapons')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(*sys.argv[1:])
