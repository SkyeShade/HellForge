"""Export the shader hosts (docs/modding/shaders.md) into mappings/shader-hosts.json: the game's root materials a mod's
pixel shader can go into, and what the data spec's "shader" resource builds from each. Metadata only: resource ids,
offsets, sizes, digests, and the names and registers of each stage's signatures and bindings; no shader bytes.

Usage: py scripts/ExportShaderHosts.py <folder> mappings/shader-hosts.json

<folder> holds each root's GPU part as gpu_<root in lowercase hex>.bin, read from the supported build's game data. The
hosts below are research (HD2Mods research/gravity-anomaly/rendering, two-copies.md); after a game update check them
there, change the offsets here and run this again. Stages of one host must be the same shader.
"""
import json
import struct
import sys
from pathlib import Path

BOX = {'copy': '0x1B798F4F99A0B8C8', 'material': '0x79F6761BE6F61772', 'at': '0x69C'}  # decal_box: a 1 m cube, one material slot
HOSTS = {
    'decal': {'root': 0xFDE0D8CB17821DC0, 'stages': [0x11C38], 'mesh': False,
              'draws': 'in the decals pass, on what is already drawn inside the unit box'},
    'transparent': {'root': 0x717B4E0431528B44, 'stages': [0x335F0, 0x46E00, 0x673E8, 0x7ABF8], 'mesh': True,
                    'child': {'copy': '0xBF6C75E150316E2A', 'parentAt': '0x18'},
                    'draws': 'a mesh in hdr_transparent, added to the scene, both walls'},
    'refract': {'root': 0xB45631ACD16D5183, 'stages': [0x19E38], 'mesh': True,
                'draws': 'a mesh in hdr_transparent_pre, replacing the scene behind it, near wall only'},
}
RESOURCE = {0: 'b', 1: 't', 2: 't', 3: 's', 5: 't', 7: 't'}  # RDEF's bind types: cbuffer, tbuffer, texture, sampler, structured and raw buffers


def text(data, at):
    return data[at:data.index(0, at)].decode()


def chunks(gpu, offset):
    if gpu[offset:offset + 4] != b'DXBC':
        raise SystemExit(f'no DXBC container at 0x{offset:X}')
    size, count = struct.unpack_from('<II', gpu, offset + 24)
    found = {}
    for i in range(count):
        at = offset + struct.unpack_from('<I', gpu, offset + 32 + 4 * i)[0]
        length = struct.unpack_from('<I', gpu, at + 4)[0]
        found[gpu[at:at + 4].decode()] = gpu[at + 8:at + 8 + length]
    return size, found


def signature(data, prefix):
    lines = []
    for i in range(struct.unpack_from('<I', data, 0)[0]):
        name, index, _, _, register, mask = struct.unpack_from('<6I', data, 8 + 24 * i)
        components = ''.join(c for bit, c in enumerate('xyzw') if mask >> bit & 1)
        lines.append(f'{prefix}{register} {text(data, name)}{index} {components}')
    return lines


def bindings(data):
    cbuffer_count, cbuffers, count, binds = struct.unpack_from('<4I', data, 0)
    sizes = {}
    for i in range(cbuffer_count):
        name, _, _, size = struct.unpack_from('<4I', data, cbuffers + 24 * i)
        sizes[text(data, name)] = size
    lines = []
    for i in range(count):
        name, kind, _, _, _, point = struct.unpack_from('<6I', data, binds + 32 * i)
        name = text(data, name)
        lines.append(f'{RESOURCE[kind]}{point} {name}' + (f' {sizes[name]}' if name in sizes else ''))
    return lines


def host(folder, spec):
    gpu = (Path(folder) / f'gpu_{spec["root"]:016x}.bin').read_bytes()
    stages, facts = [], set()
    for offset in spec['stages']:
        size, found = chunks(gpu, offset)
        stages.append({'offset': f'0x{offset:X}', 'size': size, 'digest': gpu[offset + 4:offset + 20].hex().upper()})
        facts.add(json.dumps([signature(found['ISGN'], 'v'), signature(found['OSGN'], 'o'), bindings(found['RDEF'])]))
    if len(facts) != 1:
        raise SystemExit(f'the stages of root 0x{spec["root"]:016X} are not one shader')
    inputs, outputs, binds = json.loads(facts.pop())
    entry = {'root': f'0x{spec["root"]:016X}', 'draws': spec['draws'], 'stages': stages}
    if 'child' in spec:
        entry['child'] = spec['child']
    entry.update(unit=BOX, mesh=spec['mesh'], inputs=inputs, outputs=outputs, bindings=binds)
    return entry


def main(folder, out):
    build = json.loads((Path(__file__).parent.parent / 'src/HellForge.Core/supported-build.json').read_text('utf-8'))['buildId']
    mapping = {
        'format': 1, 'build': build,
        'note': "Shader hosts (scripts/ExportShaderHosts.py): the root materials a data spec's \"shader\" resource copies, per game build. "
                "digest pins each stage; inputs, outputs and bindings say what it has, and the builder compares a mod's shader with the "
                "stage in the game file, not with these lists. docs/modding/shaders.md.",
        'hosts': {name: host(folder, spec) for name, spec in HOSTS.items()},
    }
    Path(out).write_text(json.dumps(mapping, indent=1) + '\n', encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main(*sys.argv[1:])
