"""Export the weapon class templates for standalone weapons (native/mod-loader/weapon_classes.h): each class's component
list and every field whose value differs between its members, named, with its offset, size and kind. Metadata only:
no game values. The loader takes each value at load time (standalone.c).

Usage: python3 scripts/ExportWeaponClass.py <plain-dir> <filediver-datalibrary> native/mod-loader/weapon_classes.h

<plain-dir> holds build 25480438's generated_entities.dl_bin and dl_library.dl_typelib, decrypted (`.plain`), the same
build the loader's component shapes are for. Field offsets and sizes are the typelib's (its names are stripped); names
come from filediver's Go structs, zipped in member order where the sizes match, else `unk<i>_<offset>`.

A class is every arsenal entity with the class's component list and loadout type. A field is identity (a model,
package, sound, effect, text, node or attachment the gun declares: 0 unless declared) or handling (the class's most
common value unless declared).
"""
import collections
import glob
import re
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
# The BR-14 Adjudicator's entity: only its component list is read, to name the primary class's list.
PRIMARY_LIST_OF = 0x5FECAB819F96A3E8
PRIMARY_LOADOUT_TYPE = 1
STORAGE = {20: 'struct'}
IDENTITY = re.compile(r'Audio|VO|Event|Effect|Path|Package|AudioResource|Icon|LocName|Description|Prefix|Fluff|'
                      r'MuzzleFlash|Node|DefaultCustomizations|CustomizationSlots|MaterialOverride|UnknownThinHash2|'
                      r'^Ability$|ReloadAnimEvents|unk0_0$|unk3_c$|Crosshair|Shake')
HANDLING_EXCEPTIONS = {'FireNodes', 'HolsterNodeRotationOffset', 'HolsterNodePositionOffset'}


def dl_hash(text):
    r = 5381
    for ch in text:
        r = (r * 33 + ord(ch)) & 0xFFFFFFFF
    return (r - 5381) & 0xFFFFFFFF


def component_shapes():
    src = (REPO / 'native' / 'mod-loader' / 'component_sizes.h').read_text()
    return {int(a): (int(b), int(c), int(e)) for a, b, c, e in re.findall(r'\{(\d+), (\d+)u, (\d+)u, (\d+)u\}', src)}


def read_entities(path, shapes):
    d = path.read_bytes()
    blocks, first, i = {}, None, 0
    while (j := d.find(b'LDLD', i)) >= 0:
        size = struct.unpack_from('<I', d, j + 12)[0]
        if first is None:
            first = (j + 24, size)
        else:
            blocks[struct.unpack_from('<I', d, j - 8)[0]] = (j + 24, size, struct.unpack_from('<I', d, j + 8)[0])
        i = j + 24 + size
    m, ms = first
    entities = {}
    for k in range(0, ms, 32):
        res, off, cnt, _ = struct.unpack_from('<QQQI', d, m + k)
        if res == 0:
            continue
        if off + 2 * cnt > ms or cnt > 400:
            break
        entities[res] = list(struct.unpack_from('<%dH' % cnt, d, m + off))
    tables = {}

    def record(c, entity):
        if c not in tables:
            if c not in blocks or c not in shapes:
                tables[c] = None
            else:
                p, _, _ = blocks[c]
                slots, _, size = shapes[c]
                index = {}
                for s in range(slots):
                    r, ix = struct.unpack_from('<QI', d, p + 16 * s)
                    if r:
                        index[r] = ix
                tables[c] = (index, p + 16 * slots, size)
        t = tables[c]
        if not t or entity not in t[0]:
            return None
        index, base, size = t
        return d[base + size * index[entity]:base + size * (index[entity] + 1)]
    return entities, blocks, record


def read_typelib(path):
    t = path.read_bytes()
    _, _, tc, ec, mc, evc, eac, dvs, _ = struct.unpack_from('<4sI7I', t, 0)
    p = 36
    hashes = struct.unpack_from('<%dI' % tc, t, p)
    p += 4 * tc + 4 * ec
    descs = [struct.unpack_from('<II2I2IIII', t, p + 36 * i) for i in range(tc)]
    p += 36 * tc + 32 * ec
    members = [struct.unpack_from('<IIIBBHI2I2I2IIII', t, p + 72 * i) for i in range(mc)]
    types = {}
    for h, td in zip(hashes, descs):
        mem = [dict(atom=m[3], storage=m[4], alen=m[5], type=m[6], size=m[8], off=m[12]) for m in members[td[7]:td[7] + td[6]]]
        types[h] = dict(size=td[2], members=mem)
    return types


BASIC = {'uint8': 1, 'int8': 1, 'bool': 1, 'byte': 1, 'uint16': 2, 'int16': 2, 'uint32': 4, 'int32': 4, 'float32': 4,
         'uint64': 8, 'int64': 8, 'float64': 8, 'stingray.Hash': 8, 'stingray.ThinHash': 4, 'mgl32.Vec2': 8,
         'mgl32.Vec3': 12, 'mgl32.Vec4': 16, 'mgl32.Quat': 16, 'mgl32.Mat4': 64, 'mgl32.Mat3': 36, 'DLHash': 4}


def read_go(datalibrary):
    aliases, structs = {}, {}
    for f in glob.glob(str(Path(datalibrary).parent / '**' / '*.go'), recursive=True):
        src = Path(f).read_text(errors='replace')
        for m in re.finditer(r'^type (\w+) (u?int\d+|float\d+|bool)\s*$', src, re.M):
            aliases[m.group(1)] = m.group(2)
        for m in re.finditer(r'^type (\w+) struct \{\n(.*?)^\}', src, re.S | re.M):
            fields = []
            for line in m.group(2).split('\n'):
                p = line.split('//')[0].split()
                if len(p) >= 2:
                    fields.append((p[0], p[1]))
            structs.setdefault(m.group(1), fields)
    return aliases, structs


class Names:
    def __init__(self, types, aliases, structs):
        self.types, self.aliases, self.structs = types, aliases, structs
        self.by_hash = {dl_hash(n): n for n in structs}

    def bare(self, t):
        return t.split('.')[-1] if t.split('.')[0] in ('enum', 'datalib') else t

    def size(self, t):
        if m := re.match(r'\[(\d+)\](.*)', t):
            s = self.size(m.group(2))
            return None if s is None else int(m.group(1)) * s
        if t in BASIC:
            return BASIC[t]
        b = self.bare(t)
        if b in self.aliases:
            return BASIC[self.aliases[b]]
        if b in self.structs:
            sizes = [self.size(ft) for _, ft in self.structs[b]]
            return None if None in sizes else sum(sizes)
        return None

    def leaves(self, type_hash, go_name, base=0, path=''):
        go = [(n, t) for n, t in self.structs.get(self.bare(go_name) if go_name else '', []) if n != '_']
        out, gi = [], 0
        for i, m in enumerate(self.types[type_hash]['members']):
            name = go_type = None
            for k in range(gi, min(gi + 3, len(go))):
                s = self.size(go[k][1])
                if s == m['size'] or (m['storage'] == 20 and m['atom'] == 0 and s is not None and abs(s - m['size']) <= 8):
                    name, go_type = go[k]
                    gi = k + 1
                    break
            full = (path + '.' if path else '') + (name or 'unk%d_%x' % (i, m['off']))
            if m['storage'] == 20 and m['atom'] == 0 and m['type'] in self.types:
                out += self.leaves(m['type'], go_type, base + m['off'], full)
            else:
                out.append((full, base + m['off'], m['size']))
        return out


def kind(name):
    top = name.split('.')[0]
    if top in HANDLING_EXCEPTIONS:
        return 'CLASS_HANDLING'
    return 'CLASS_IDENTITY' if IDENTITY.search(name.split('.')[-1]) or IDENTITY.search(top) else 'CLASS_HANDLING'


def main(plain, datalibrary, out):
    plain = Path(plain)
    shapes = component_shapes()
    entities, blocks, record = read_entities(plain / 'generated_entities.dl_bin.plain', shapes)
    types = read_typelib(plain / 'dl_library.dl_typelib.plain')
    names = Names(types, *read_go(datalibrary))
    components = entities[PRIMARY_LIST_OF]
    members = [e for e, c in entities.items() if c == components and record(29, e) is not None
               and struct.unpack_from('<I', record(29, e), 4)[0] == PRIMARY_LOADOUT_TYPE]
    rows = []
    for c in components:
        if c not in blocks or c not in shapes:
            continue
        data_type = types[blocks[c][2]]
        record_type = data_type['members'][1]['type']
        if types[record_type]['size'] != shapes[c][2]:
            sys.exit('component %d: typelib record size %d, table %d: not the same build' % (c, types[record_type]['size'], shapes[c][2]))
        for name, off, size in names.leaves(record_type, names.by_hash.get(record_type)):
            values = collections.Counter(record(c, e)[off:off + size] for e in members)
            if len(values) > 1:
                rows.append((c, off, size, kind(name), name, len(values)))
    lines = ['/* Generated by scripts/ExportWeaponClass.py from build 25480438\'s generated_entities.dl_bin and typelib; do not',
             ' * edit. Metadata only: each class\'s component list, and every field that differs between its members (its',
             ' * component, offset, size, kind and name). standalone.c takes the values at load time. */',
             'enum { CLASS_IDENTITY = 1, CLASS_HANDLING = 2 };',
             'typedef struct ClassField { uint16_t component, offset, size, kind; const char* name; } ClassField;',
             '#define CLASS_PRIMARY_LOADOUT_TYPE %du' % PRIMARY_LOADOUT_TYPE,
             '#define CLASS_PRIMARY_MEMBERS %du /* the members when this was generated */' % len(members),
             'static const uint16_t kPrimaryComponents[%d] = {%s};' % (len(components), ', '.join(map(str, components))),
             'static const ClassField kPrimaryFields[] = {']
    for c, off, size, k, name, _ in rows:
        lines.append('    {%d, 0x%x, %d, %s, "%s"},' % (c, off, size, k, name))
    lines.append('};')
    Path(out).write_text('\n'.join(lines) + '\n')
    print('%d members, %d components, %d varying fields' % (len(members), len(components), len(rows)))


if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
