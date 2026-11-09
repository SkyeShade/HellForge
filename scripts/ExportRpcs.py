"""Export the game's RPCs (its network messages) from the network_config resource to mappings/rpcs.json.
Metadata only: each RPC's name, name hash, whether it is session-bound, and its arguments' kinds, widths and ranges as the
game's network configuration declares them; no game bytes.

Usage: py scripts/ExportRpcs.py <network_config file> mappings/rpcs.json [--type-names <names.json>]

The network_config file is the resource's main data (type 0x3B1FA9E8F6BAC374, one per build), as GameDataReader reads
it from the game's data folder. --type-names: a JSON object {"<row>": "name"} of argument type names confirmed by their
hash (MurmurHash64A of the name, top 32 bits) and by their kind; an argument is named after its type when it has one,
else arg<n>.

The file's layout (build 25480438, version 35): a version, a header (u32, u8 type index bits, u8 RPC index bits, two
u32), the field types (count, then 24 bytes each: u8 kind, u8 bits, u8, u8 frequent, u32, u32 name hash, f32 threshold,
min, max), two per-object-type arrays (count + u32s, count + bytes), the default field data (count + bytes), the object
types (count, then: key, data size, defaults offset, two u32, count + field type rows, count + field name hashes, u32
frequent count) and the RPCs (count, then: u32 name hash, u8 send now, u32 priority, u8 session bound, char[64] name,
count + argument type rows). RPCs are sorted by their hash; that order is their index on the wire.
"""
import json
import struct
import sys
from pathlib import Path

KINDS = {0: "bool", 1: "int", 2: "float", 3: "vector3", 4: "quaternion", 5: "string", 6: "u32", 7: "u64", 8: "u64",
         9: "u64", 10: "array", 11: "direction", 12: "vector2"}


class Reader:
    def __init__(self, data: bytes):
        self.data, self.at = data, 0

    def u8(self) -> int:
        self.at += 1
        return self.data[self.at - 1]

    def u32(self) -> int:
        self.at += 4
        return struct.unpack_from("<I", self.data, self.at - 4)[0]

    def u32s(self, n: int) -> list[int]:
        self.at += 4 * n
        return list(struct.unpack_from(f"<{n}I", self.data, self.at - 4 * n))

    def skip(self, n: int) -> None:
        self.at += n


def as_f32(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def as_i32(bits: int) -> int:
    return struct.unpack("<i", struct.pack("<I", bits))[0]


def parse(data: bytes):
    r = Reader(data)
    version = r.u32()
    r.u32()
    r.u8()
    rpc_index_bits = r.u8()
    r.u32()
    r.u32()
    types = []
    for _ in range(r.u32()):
        kind, bits = r.u8(), r.u8()
        r.skip(2 + 4)
        name_hash = r.u32()
        r.skip(4)
        low, high = r.u32(), r.u32()
        types.append({"kind": kind, "bits": bits, "hash": name_hash, "low": low, "high": high})
    r.u32s(r.u32())
    r.skip(r.u32())
    r.skip(r.u32())
    for _ in range(r.u32()):
        r.skip(20)
        r.u32s(r.u32())
        r.u32s(r.u32())
        r.skip(4)
    rpcs = []
    for _ in range(r.u32()):
        name_hash = r.u32()
        r.skip(1 + 4)
        session_bound = r.u8()
        name = r.data[r.at:r.at + 64].split(b"\0")[0].decode("ascii")
        r.skip(64)
        rpcs.append({"hash": name_hash, "session_bound": session_bound, "name": name, "args": r.u32s(r.u32())})
    if r.at != len(data):
        raise SystemExit(f"{len(data) - r.at} bytes left after the RPCs: not the layout this script reads")
    return version, rpc_index_bits, types, rpcs


def argument(types, row: int, names: dict[int, str]) -> dict:
    t = types[row]
    kind = KINDS.get(t["kind"])
    if kind is None:
        raise SystemExit(f"type row {row}: unknown kind {t['kind']}")
    out = {"type": f"0x{t['hash']:08X}"}
    if row in names:
        out["name"] = names[row]
    out["kind"] = kind
    if kind == "int":
        low = as_i32(t["low"])
        out.update(bits=t["bits"], min=low, max=low + (1 << t["bits"]) - 1 if t["bits"] < 32 else as_i32(t["high"]))
    elif kind in ("float", "vector3", "vector2"):
        out.update(bits=t["bits"], min=round(as_f32(t["low"]), 6), max=round(as_f32(t["high"]), 6))
    elif kind in ("quaternion", "direction"):
        out["bits"] = t["bits"]
    elif kind == "array":
        out["max_count"] = t["high"]
        element = argument(types, t["low"], names)
        if element["kind"] == "array":
            raise SystemExit(f"type row {row}: an array of {element['kind']}")
        element.pop("name", None)
        out["element"] = element
    return out


def main(argv: list[str]) -> int:
    if len(argv) not in (3, 5) or (len(argv) == 5 and argv[3] != "--type-names"):
        print(__doc__)
        return 2
    version, rpc_index_bits, types, rpcs = parse(Path(argv[1]).read_bytes())
    names = {}
    if len(argv) == 5:
        names = {int(k): v for k, v in json.loads(Path(argv[4]).read_text(encoding="utf-8")).items()}
    if [m["hash"] for m in rpcs] != sorted(m["hash"] for m in rpcs):
        raise SystemExit("the RPCs are not sorted by their hash")
    out = {
        "format": 1,
        "build": "25480438",
        "note": "The game's RPCs (network messages) as its network configuration declares them (version "
                f"{version}; RPC index {rpc_index_bits} bits on the wire): the name, the name's hash (MurmurHash64A, top 32 "
                "bits), whether a session id goes with it (session-bound), and each argument's kind, width and range. "
                "An argument is named after its type where that name is confirmed, else arg<n>. Index: the RPC's place "
                "in the configuration, sorted by hash (mods add their own, so it is only this build's vanilla order). "
                "Exported by scripts/ExportRpcs.py.",
        "rpcs": {},
    }
    for index, m in enumerate(rpcs):
        args = [argument(types, row, names) for row in m["args"]]
        out["rpcs"][m["name"]] = {"hash": f"0x{m['hash']:08X}", "index": index, "session_bound": bool(m["session_bound"]),
                                  "args": args}
    text = json.dumps(out, indent=2) + "\n"
    Path(argv[2]).write_text(text, encoding="utf-8", newline="\n")
    print(f"{len(rpcs)} RPCs, {sum(len(m['args']) for m in rpcs)} arguments, {len(names)} type names")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
