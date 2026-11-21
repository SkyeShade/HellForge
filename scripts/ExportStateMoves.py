"""Export what HellForge moves out of the game state (native/mod-loader/state_moves.c): every instruction in game.dll that
reaches a moved array, with its reviewed bytes and where its displacement is, the allocation's compiled size, and the
constants that hold the arrays' capacities. A catalogue site names its field, so the loader can place it at any capacity:
each field's new head follows from the capacity, and a site keeps its distance from its field's head.

Usage: py scripts/ExportStateMoves.py <catalogue-sites.json> <game.dll.mapped.bin> native/mod-loader/state_move_sites.h

From the static pass in HD2Mods\\research\\unlimited-weapons\\catalogue-relocation (build 25480438). Each site is checked
against the image here, and the loader checks it again before it writes anything, so a game build these were not made for
moves nothing.
"""
import hashlib
import json
import sys

RESET_CALL = 0x136BA66
MEMSET = 0x208AAA0

# The catalogue block's fields and their offsets at the game's capacity, from the block's start: state_moves.c's
# MoveFieldAt(field, 4096), which its test holds to these.
ITEMS, PACKAGES = 4096, 256
FIELDS = (('count', 'MOVE_F_COUNT', 0), ('def', 'MOVE_F_DEFINITIONS', 4), ('hier', 'MOVE_F_HIERARCHY', 0xB8004),
          ('catstart', 'MOVE_F_STARTS', 0xD0004), ('cattotal', 'MOVE_F_TOTAL', 0xD0064), ('index', 'MOVE_F_INDEX', 0xD0068),
          ('status', 'MOVE_F_STATUS', 0xD4068), ('invstatus', 'MOVE_F_INVENTORY', 0xD406C))
BLOCK_END = 0xD4070
BLOCK_AT_P = 0x1CE0
FIELD_OF = {name: i for i, (name, _, _) in enumerate(FIELDS)}
OFF_BLOCK, SCALED = 0x10, 0x20 # a site's kind, beside its field: off the block's start, not P; in u32s, not bytes

# The capacities' constants: where, what they hold, and its value at the game's capacity.
CAPACITY = (('0x107ea50', 'MOVE_C_ITEMS', ITEMS), ('0x103927b', 'MOVE_C_ITEMS', ITEMS),
            ('0x12ca837', 'MOVE_C_ITEMS_PARSE', ITEMS * 0x70), ('0x12ca854', 'MOVE_C_ITEMS_PARSE', ITEMS * 0x70),
            ('0x136b8c7', 'MOVE_C_RESET', BLOCK_END - 4),
            ('0x1080093', 'MOVE_C_PACKAGES', PACKAGES), ('0x1038c54', 'MOVE_C_PACKAGES', PACKAGES),
            ('0x12c6bca', 'MOVE_C_PACKAGES_PARSE', PACKAGES * 0x30), ('0x12c6be3', 'MOVE_C_PACKAGES_PARSE', PACKAGES * 0x30))


def row(site, image, kind=0):
    rva, code = int(site['rva'], 16), bytes.fromhex(site['bytes'])
    at, size = site['imm_off'], site['imm_size']
    if image[rva:rva + len(code)] != code:
        raise SystemExit(f"{site['rva']} does not read as reviewed")
    if size != 4 or at + size > len(code) or len(code) > 15:
        raise SystemExit(f"{site['rva']} is not a constant the loader can write")
    value = int.from_bytes(code[at:at + 4], 'little', signed=True)
    pad = ', '.join(f'0x{b:02x}' for b in code)
    return f'    {{0x{rva:x}u, {value}, {len(code)}, {at}, {kind}, {{{pad}}}}},'


def table(name, rows):
    return [f'static const MoveSite {name}[{len(rows)}] = {{', *rows, '};', '']


def site_kind(site):
    """A catalogue site's field and form. The file is checked against itself and the layout: a site in the wrong field
    would land in another array at any capacity but the game's."""
    index = FIELD_OF[site['field']]
    head, end = FIELDS[index][2], FIELDS[index + 1][2] if index + 1 < len(FIELDS) else BLOCK_END
    scale, value, at = site['scale'], int(site['value'], 16), int(site['block_offset'], 16)
    code = bytes.fromhex(site['bytes'])
    if int.from_bytes(code[site['imm_off']:site['imm_off'] + 4], 'little') != value or value >= 1 << 31:
        raise SystemExit(f"{site['rva']}: its constant is not its value")
    if (site['base'], scale) not in (('P', 1), ('P', 4), ('C', 1)):
        raise SystemExit(f"{site['rva']}: not off P or the block, by 1 or 4")
    if value * scale - (BLOCK_AT_P if site['base'] == 'P' else 0) != at or not head <= at < end:
        raise SystemExit(f"{site['rva']}: block offset {at:#x} is not in {site['field']}")
    return index | (OFF_BLOCK if site['base'] == 'C' else 0) | (SCALED if scale == 4 else 0)


def capacity_rows(data, image):
    known = {c['rva']: c for c in data['constants'] + data['packages']['constants']}
    rows = []
    for rva, formula, was in CAPACITY:
        site = known[rva]
        if int.from_bytes(bytes.fromhex(site['bytes'])[site['imm_off']:site['imm_off'] + 4], 'little') != was:
            raise SystemExit(f'{rva} does not hold {was:#x}')
        rows.append(row(site, image, formula))
    return rows


def main(source, mapped, out):
    data = json.load(open(source, encoding='utf-8'))
    if data['build'] != 25480438:
        raise SystemExit(f"{source} is for build {data['build']}")
    image = open(mapped, 'rb').read()
    if hashlib.sha256(image).hexdigest() != data['sha256']:
        raise SystemExit(f'{mapped} is not the reviewed image')
    sizes = [c for c in data['constants'] if c['becomes'].startswith('0x7AF7628') and 'log text' not in c['why']]
    if len(sizes) != 4:
        raise SystemExit('the state allocation should have 4 size constants')
    call = image[RESET_CALL:RESET_CALL + 5]
    if call[0] != 0xE8 or RESET_CALL + 5 + int.from_bytes(call[1:], 'little', signed=True) != MEMSET:
        raise SystemExit('the package reset does not call memset')
    sites = data['catalogue_sites']
    if len(sites) != 1727 or len({s['rva'] for s in sites}) != len(sites):
        raise SystemExit('the catalogue should have 1,727 sites, each once')
    if any(s['moves_in_smoke_test'] != (s['base'] == 'P') for s in sites):
        raise SystemExit('a site off P did not move with the block, or one off the block did')
    lines = ['/* Generated by scripts/ExportStateMoves.py from research\\unlimited-weapons\\catalogue-relocation for game build',
             ' * 25480438; do not edit. {rva, displacement or immediate as reviewed, instruction length, its offset, kind, instruction}.',
             " * kind: a catalogue site's field and form (state_moves.c MoveSiteDelta), a capacity constant's formula. */",
             'typedef struct MoveSite { uint32_t rva; int32_t value; uint8_t length, at, kind; uint8_t code[15]; } MoveSite;',
             'enum { ' + ', '.join(name for _, name, _ in FIELDS) + ' };',
             'enum { MOVE_C_ITEMS, MOVE_C_ITEMS_PARSE, MOVE_C_RESET, MOVE_C_PACKAGES, MOVE_C_PACKAGES_PARSE };',
             f'#define MOVE_SITE_OFF_BLOCK 0x{OFF_BLOCK:x}u', f'#define MOVE_SITE_SCALED 0x{SCALED:x}u', '',
             f'#define MOVE_RESET_CALL 0x{RESET_CALL:x}u',
             f'#define MOVE_MEMSET 0x{MEMSET:x}u',
             'static const uint8_t kMoveResetCall[5] = {' + ', '.join(f'0x{b:02x}' for b in call) + '};', '']
    lines += table('kMoveStateSizes', [row(c, image) for c in sizes])
    lines += table('kMovePackageSites', [row(s, image) for s in data['packages']['sites']])
    lines += table('kMoveCatalogueSites', [row(s, image, site_kind(s)) for s in sites])
    lines += table('kMoveCapacitySites', capacity_rows(data, image))
    open(out, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
    print(f"{out}: {len(sizes)} sizes, {len(data['packages']['sites'])} package sites, {len(sites)} catalogue sites, {len(CAPACITY)} capacity constants")


if __name__ == '__main__':
    main(*sys.argv[1:4])
