"""Export the entity-data profile HellForge native mods need from HD2Runtime's reviewed schema (schemas/current.lua).

The profile pins, for one game build (exe and game.dll SHA-256), the game's entity component region: its size, the
28-byte map header, the row count, and per component the section offset, framing header, component index, index row
count, record count, record offset and stride. HellForge uses it to find a component record by entity resource, exactly as
HD2Runtime's core/entity_catalog.lua does. Usage: ExportEntityProfile.py <HD2Runtime>/schemas/current.lua <output.json>
"""
import json
import re
import sys

source, output = sys.argv[1], sys.argv[2]
text = open(source, encoding='utf-8').read()
if not text.startswith('return '):
    raise SystemExit('unexpected schema file')
# The schema is a Lua table literal: ["key"]=value -> "key":value. Only the scalars and the components table (keyed
# entries only) are read; other parts hold Lua arrays.
body = re.sub(r'\["([^"\\]+)"\]=', r'"\1":', text[len('return '):].strip())


def scalar(key):
    found = re.search(r'"%s":("[^"]*"|-?[0-9]+)' % key, body)
    if not found:
        raise SystemExit('schema lacks ' + key)
    return json.loads(found.group(1))


def table(key):
    start = body.index('"%s":{' % key) + len(key) + 3
    depth = 0
    for at in range(start, len(body)):
        depth += {'{': 1, '}': -1}.get(body[at], 0)
        if depth == 0:
            return json.loads(body[start:at + 1])
    raise SystemExit('unbalanced ' + key)


schema = {key: scalar(key) for key in ('exe_sha', 'dll_sha', 'entity_region_size', 'map_header', 'map_rows')}
schema['components'] = table('components')
keep = ('ProjectileWeaponComponentData', 'DepositComponentData', 'HealthComponentData', 'WeaponReloadComponentData',
        'WeaponMagazineComponentData', 'ShieldComponentData', 'JumppackComponentData', 'RechargeComponentData',
        'MountComponentData', 'WeaponDataComponentData', 'TurretComponentData', 'SensorEyeComponentData', 'TagComponentData',
        'WeaponRoundsComponentData', 'WeaponChargeComponentData', 'WeaponHeatComponentData', 'WeaponWindUpComponentData',
        'ArcWeaponComponentData', 'BeamWeaponComponentData', 'SprayWeaponComponentData', 'MeleeWeaponComponentData',
        'ExplosiveComponentData', 'ThrowableComponentData', 'BombardmentComponentData', 'EagleComponentData',
        'BackpackComponentData', 'WeaponLinkedAmmoComponentData', 'ThrowerComponentData', 'LoadoutEntryComponentData',
        'ShieldControllerComponentData', 'VehicleMotionComponentData', 'HellpodPayloadComponentData',
        'OrbitalAbilityComponentData', 'WeaponCustomizationComponentData', 'RotationComponentData',
        'SeekingMissileComponentData', 'DisplacementComponentData', 'HellpodRackComponentData', 'GoreComponentData')
components = {name: schema['components'][name] for name in keep if name in schema['components']}
# Components the schema lacks, read from the same build's generated_entities.dl_bin (its LDLD frame: offset, framing
# header, the component index before it, index rows, records, stride) with HD2Runtime-dev6's scan.tables; the method
# reproduces DepositComponentData's schema entry exactly.
EXTRA = {
    'BehaviorComponentData': {'offset': 29820096, 'header': '3d809bba4c444c44010000003d809bbaa0ac00000100000000000000', 'index': 284,
                              'indices': 2008, 'records': 1005, 'record_offset': 32128, 'stride': 12, 'type': 3130753085},
    'TargetingComponentData': {'offset': 26108376, 'header': 'd268092c4c444c4401000000d268092ca06e00000100000000000000', 'index': 234,
                               'indices': 590, 'records': 295, 'record_offset': 9440, 'stride': 64, 'type': 738814162},
    'LoadoutPackageComponentData': {'offset': 681824, 'header': '9186857a4c444c44010000009186857a208700000100000000000000', 'index': 28, 'indices': 1080, 'records': 541,
                                    'record_offset': 17280, 'stride': 32, 'type': 2055571089},
    'UnitComponentData': {'offset': 912200, 'header': '4d5737444c444c44010000004d573744405604000100000000000000', 'index': 88,
                          'indices': 3382, 'records': 1692, 'record_offset': 54112, 'stride': 136, 'type': 1144477517},
    # From the retained mission-host snapshot (its entity allocation at 0x24AED420000, which reproduces the Behavior,
    # Targeting and Unit entries above exactly): an ability weapon's fire modes (the silo remote, the C4 detonator).
    'AbilityWeaponComponentData': {'offset': 27512448, 'header': '651c93ff4c444c4401000000651c93ff380400000100000000000000', 'index': 267,
                                   'indices': 18, 'records': 9, 'record_offset': 288, 'stride': 88, 'type': 4287831141},
    # The same way (each frame found by the component index before it, which reproduces all 46 entries above; counts and
    # strides as HD2Runtime's component catalogue has them): the structures' components the HD2R Editor's Enemies pages
    # edit (research/editor-phase2 components-needed).
    'AiSpawnerComponentData': {'offset': 7293320, 'header': '772615bc4c444c4401000000772615bc681d00000100000000000000', 'index': 182,
                               'indices': 66, 'records': 33, 'record_offset': 1056, 'stride': 196, 'type': 3155502711},
    'SquadTransportFormationComponentData': {'offset': 5909184, 'header': 'c7e78c494c444c4401000000c7e78c49201c00000100000000000000', 'index': 122,
                                             'indices': 180, 'records': 90, 'record_offset': 2880, 'stride': 48, 'type': 1233971143},
    'SquadTransportComponentData': {'offset': 2080624, 'header': 'd83cb9d24c444c4401000000d83cb9d2f00300000100000000000000', 'index': 97,
                                    'indices': 12, 'records': 6, 'record_offset': 192, 'stride': 136, 'type': 3535355096},
    'StratagemBlockerComponentData': {'offset': 681424, 'header': 'b78a5f8c4c444c4401000000b78a5f8c700100000100000000000000', 'index': 25,
                                      'indices': 14, 'records': 7, 'record_offset': 224, 'stride': 20, 'type': 2355071671},
    'BugholeComponentData': {'offset': 7710424, 'header': 'f3b10f174c444c4401000000f3b10f17900000000100000000000000', 'index': 216,
                             'indices': 4, 'records': 2, 'record_offset': 64, 'stride': 40, 'type': 386904563},
}
for name, c in EXTRA.items():
    components.setdefault(name, c)
# The Helldiver's avatar and inventory components (HD2Runtime domains/helldiver_fields.lua gives their layouts).
import os  # noqa: E402
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ExportStatsSchema import read_literal  # noqa: E402
helldiver = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(source))), 'domains', 'helldiver_fields.lua')
if os.path.exists(helldiver):
    from pathlib import Path  # noqa: E402
    h = read_literal(Path(helldiver))
    for record in ('avatar', 'inventory'):
        r = h.get(record) or {}
        if isinstance(r.get('layout'), dict) and r.get('component'):
            components.setdefault(r['component'], r['layout'])
# The melee components (HD2Runtime domains/melee_authoring.lua: a melee weapon's stamina cost).
melee = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(source))), 'domains', 'melee_authoring.lua')
if os.path.exists(melee):
    for name, layout in (read_literal(Path(melee)).get('components') or {}).items():
        components.setdefault(name, layout)
profile = {
    'source': 'HD2Runtime schemas/current.lua',
    'exeSha256': schema['exe_sha'], 'gameDllSha256': schema['dll_sha'],
    'entityRegionSize': schema['entity_region_size'], 'mapHeader': schema['map_header'], 'mapRows': schema['map_rows'],
    'components': {name: {'offset': c['offset'], 'header': c['header'], 'index': c['index'], 'indices': c['indices'],
                          'records': c['records'], 'recordOffset': c['record_offset'], 'stride': c['stride'],
                          'type': c['type']} for name, c in sorted(components.items())},
}
with open(output, 'w', encoding='utf-8', newline='\n') as stream:
    json.dump(profile, stream, indent=1)
    stream.write('\n')
print('%d components for exe %s' % (len(profile['components']), profile['exeSha256'][:12]))
