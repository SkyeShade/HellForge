"""Rebuild the /Progression/items catalogue from the user's own retained HD2Runtime snapshot. Read-only.

The client stores each parsed catalogue entry (parser 0x10658a0, apply 0x1038fb0 -> 0x136ec00) at
[game.dll+0x347CEF8] + 0x1CE4 + i*0xB8, with a parallel hierarchy record at +0xB9CE4 + i*0x18. This
script inverts that store into the JSON the parser accepts, in the original order (the client links a
child to the preceding parent run). Word offsets from the definition start:
  +0x04 mixId, +0x08 itemId, +0x0C progressionCategory, +0x10 requiredLevel, +0x18 maxCap,
  +0x1C buyPrice[4] {mixId, amount, -}, +0x4C sellPrice[4], +0x7C requiredItems[4],
  +0x8C tag count, +0x90 tags[8], +0xB4 isConsumable (byte).
Hierarchy record: +0x10 distance back to the parent definition (0 = root).

Also writes progression-data.json from the same snapshot (all in [game.dll+0x347CEF8] = P unless noted):
  levelSpec   P+0 u32[300] required experience per level (status P+0x4B0; handler 0x12ca156)
  itemPackages [game.dll+0x346D568]+8: 0x108-byte {itemMixId32, count, {u64 mixId, u64 amount}[16]},
              count at +0x4200 (handler 0x12c94d6)
  seasonPass  P+0x17492D4: count, then 0x64-byte {index, id32, medalsTotalMixId, medalsConsumableMixId,
              medalsSpentMixId, premiumTokenMixIds[3], totalCost, page count, pageRequirements[...]}
              (handler 0x12c85f9, parser 0x1068510)
  seasonPassDetails  GET /SeasonPass/<id32>, only cached once a warbond is opened in that session (use
              --warbond-snapshot): P+0x1749F58 + index*0x17D4 (store 0x103dff0): +0 loaded, +4 id32,
              +0x18/+0x1C layoutSize x/y, +0x20 layoutType, +0x24 page count, pages 0x194 from +0x28
              {totalMedalsRequired, item count, items 0x24 {seasonPassItemId, itemMixId, medalCost,
              gridSize x/y, gridPosition x/y, listSortOrder, isRepeatable (byte +0x20)}}. isPremium is
              not stored by the client and is served as false; the rest comes from the season pass list.

Also writes profile templates: the starter kit is resolved by name through HD2Runtime's armory map
(entity paths from the same installation), never by invented IDs.
"""
import argparse
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parent.parent
DLL_SHA256 = "2E2C3B7C2500646DADD5F2B4C6E0504DBB7E7896139F64CDDC0D1813C718F51E"
CATALOGUE_GLOBAL = 0x347CEF8
STARTER_NAMES = ["AR-23 Liberator", "P-2 Peacemaker", "G-12 High Explosive"]
# Root items left out with their descendants. 0x6625d13f is an unused, non-selectable Eagle Strafe
# stratagem (stable id 929878807, HD2Runtime: "EAGLE. (NOT USED) ADDITIONAL STRAFING RUNS"), not owned
# by the account. With it in the catalogue the new-player ship_intro never completes (bisected over
# nine local boots, HellForge's startup research notes); the cause inside the intro is not yet understood.
EXCLUDED_ROOTS = {0x6625D13F: "unused Eagle Strafe stratagem; blocks the new-player ship_intro"}
BOOSTER_CATEGORY = 5


def read(snapshot, captured, address, size):
    out, at = bytearray(size), address
    while at < address + size:
        region = snapshot.region(at)
        if region is None or region["status"] != captured:
            raise ValueError(f"Snapshot does not hold {at:#x}")
        end = min(region["base"] + region["size"], address + size)
        snapshot.handle.seek(region["data_offset"] + at - region["base"])
        out[at - address:end - address] = snapshot.handle.read(end - at)
        at = end
    return bytes(out)


def export_progression_packages(s, read_memory):
    """Invert the ProgressionPackages store (apply 0x1039650; [game.dll+0x347CE78]+8). Costs are mapped back
    through the client's currency tables (runtime 0x32DE960, static 0x21CADE8); reward types 3/4 that the client
    derived from type 1 are restored. Text fields of assignments are not stored by the client and are served empty.
    Verified against a real response fragment: package 0x84697c06 rebuilds 25/25 levels identically."""
    read = lambda _s, a, n: read_memory(a, n)
    g = s.modules["game.dll"]["base"]
    currencies = [struct.unpack_from("<I", read(s, g + 0x32DE960 + i * 0x40, 4))[0] for i in range(3)]
    cost_table = list(struct.unpack("<14I", read(s, g + 0x21CADE8, 56)))
    obj = struct.unpack("<Q", read(s, g + 0x347CE78, 8))[0]
    arr, count = struct.unpack("<QI", read(s, obj + 8, 12))
    packages = []
    for k in range(count):
        p = read(s, arr + k * 0x70, 0x70)
        w = struct.unpack("<28I", p)
        levels_ptr = struct.unpack_from("<Q", p, 0x60)[0]
        levels = []
        for li in range(w[0x1A]):
            v = struct.unpack("<530I", read(s, levels_ptr + li * 0x848, 0x848))
            level = {"level": v[0], "requirementType": v[2], "requirement": v[1]}
            tasks = []
            for t in range(min(v[0xBE], 8)):
                b = 0x1E + t * 0x14
                values, types = [], []
                def add(vt, value):
                    if value: values.append(value); types.append(vt)
                add(1, v[b + 6]); add(2, v[b + 7]); add(3, v[b + 8] | v[b + 9] << 32); add(4, v[b + 0xE])
                add(5, v[b + 0xB]); add(6, v[b + 0xC]); add(7, v[b + 0xD]); add(8, v[b + 0xF]); add(9, v[b + 0x10]); add(10, v[b + 0x11])
                tasks.append({"type": v[b], "values": values, "valueTypes": types})
            if v[0x13] or tasks:
                level["assignmentSetting"] = {"type": v[0x13], "overrideTitle": "", "overrideBrief": "", "taskDescription": "",
                                              "tasks": tasks, "rewards": [], "flags": 0}
            rp, rn = struct.unpack_from("<Q", bytes(struct.pack("<530I", *v)), 0x20A * 4)[0], v[0x20C]
            rewards = []
            if rn:
                raw = read(s, rp, rn * 0xC)
                for j in range(rn):
                    rtype, rid, amount = struct.unpack_from("<IIi", raw, j * 0xC)
                    if rtype == 3 and rid == cost_table[0]: rtype = 1
                    elif rtype == 4 and rid == cost_table[2]: rtype = 1
                    rewards.append({"type": rtype, "id32": rid, "amount": amount})
            level["rewards"] = rewards
            cost = [{"mixId": currencies[i], "amount": v[3 + i]} for i in range(3) if v[3 + i]]
            cost += [{"mixId": cost_table[j - 1], "amount": v[6 + j]} for j in range(1, 12) if v[6 + j]]
            level["cost"] = cost
            up, un = struct.unpack_from("<Q", bytes(struct.pack("<530I", *v)), 0x20E * 4)[0], v[0x210]
            unlockable = []
            if un:
                raw = read(s, up, un * 0x30)
                for j in range(un):
                    e = struct.unpack_from("<12I", raw, j * 0x30)
                    costs = [{"mixId": e[3 + c * 2], "amount": e[4 + c * 2]} for c in range(min(e[11], 4))]
                    unlockable.append({"mixId": e[0], "amount": e[1], "sortOrder": e[2], "cost": costs})
            level["unlockableItems"] = unlockable
            levels.append(level)
        packages.append({"id32": w[0], "type": w[2], "requiredItems": list(w[3:3 + min(w[0x17], 20)]), "levels": levels})
    return packages


def prices(words):
    return [{"mixId": words[i], "amount": words[i + 1]} for i in range(0, 12, 3) if words[i]]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--snapshot", required=True, type=Path)
    parser.add_argument("--warbond-snapshot", type=Path, help="snapshot taken after opening every warbond")
    parser.add_argument("--store-snapshot", type=Path, help="snapshot taken after opening the Ship Management store")
    parser.add_argument("--hd2runtime", type=Path, default=ROOT.parent / "HD2Runtime")
    # HellForge ships no game catalogue (HellForge.Core GameCatalog): these two stay out of the repository.
    parser.add_argument("--catalogue-out", type=Path, default=ROOT / "artifacts/item-catalog-rebuild",
                        help="folder for item-catalog.json and progression-data.json (never committed)")
    args = parser.parse_args()
    sys.path.insert(0, str(args.hd2runtime / "scripts"))
    from snapshot_image import Snapshot, CAPTURED

    snapshot = Snapshot(args.snapshot)
    if snapshot.game_dll_sha256.upper() != DLL_SHA256:
        raise ValueError("Snapshot is from a different game build")
    game = snapshot.modules["game.dll"]["base"]
    catalogue = struct.unpack("<Q", read(snapshot, CAPTURED, game + CATALOGUE_GLOBAL, 8))[0]
    count = struct.unpack("<I", read(snapshot, CAPTURED, catalogue + 0x1CE0, 4))[0]
    if not 0 < count <= 0x1000:
        raise ValueError("Implausible catalogue size")
    definitions = read(snapshot, CAPTURED, catalogue + 0x1CE4, count * 0xB8)
    records = read(snapshot, CAPTURED, catalogue + 0xB9CE4, count * 0x18)

    items, owned = [], []
    for i in range(count):
        raw = definitions[i * 0xB8:(i + 1) * 0xB8]
        w = struct.unpack("<46I", raw)
        record = struct.unpack_from("<6I", records, i * 0x18)
        if w[0] != i or record[0] != i or record[1] != w[1]:
            raise ValueError(f"Definition {i} is not a consistent catalogue entry")
        item = {"itemId": w[2]}
        if record[4]:
            item["parentId"] = struct.unpack_from("<I", definitions, (i - record[4]) * 0xB8 + 4)[0]
        item.update(mixId=w[1], isConsumable=bool(raw[0xB4]), requiredLevel=w[4], progressionCategory=w[3],
                    tags=list(w[36:36 + min(w[35], 8)]), requiredItems=[v for v in w[31:35] if v],
                    buyPrice=prices(w[7:19]), sellPrice=prices(w[19:31]))
        if w[6] != 0x7FFFFFFF:
            item["maxCap"] = w[6]
        items.append(item)
        if w[5] == 2:
            owned.append({"mixId": w[1], "amount": max(w[44], 1)})

    by_mix = {item["mixId"]: item for item in items}

    def root(item):
        while "parentId" in item and item["parentId"] in by_mix:
            item = by_mix[item["parentId"]]
        return item["mixId"]
    excluded = [item for item in items if root(item) in EXCLUDED_ROOTS]
    items = [item for item in items if root(item) not in EXCLUDED_ROOTS]

    level_xp = struct.unpack("<300I", read(snapshot, CAPTURED, catalogue, 1200))
    if struct.unpack("<I", read(snapshot, CAPTURED, catalogue + 0x4B0, 4))[0] != 2:
        raise ValueError("Snapshot level spec was not loaded")
    level_spec = [{"level": i + 1, "requiredExperience": xp} for i, xp in enumerate(level_xp) if xp]

    packages_object = struct.unpack("<Q", read(snapshot, CAPTURED, game + 0x346D568, 8))[0]
    package_count = struct.unpack("<I", read(snapshot, CAPTURED, packages_object + 8 + 0x4200, 4))[0]
    item_packages = []
    for k in range(package_count):
        raw = read(snapshot, CAPTURED, packages_object + 8 + k * 0x108, 0x108)
        mix, n = struct.unpack_from("<II", raw, 0)
        entries = [struct.unpack_from("<QQ", raw, 8 + j * 16) for j in range(min(n, 16))]
        item_packages.append({"itemMixId32": mix, "items": [{"mixId": m & 0xFFFFFFFF, "amount": a} for m, a in entries]})

    season_count = struct.unpack("<I", read(snapshot, CAPTURED, catalogue + 0x17492D4, 4))[0]
    season_pass = []
    for k in range(season_count):
        w = struct.unpack("<25I", read(snapshot, CAPTURED, catalogue + 0x17492D4 + 4 + k * 0x64, 0x64))
        if w[0] != k:
            raise ValueError(f"Season pass {k} is not a consistent entry")
        season_pass.append({"id32": w[1], "medalsTotalMixId": w[2], "medalsConsumableMixId": w[3], "medalsSpentMixId": w[4],
                            "totalCost": w[8], "pageRequirements": list(w[10:10 + min(w[9], 15)]),
                            "premiumTokenMixIds": [t for t in w[5:8] if t]})

    progression_packages = export_progression_packages(snapshot, lambda a, n: read(snapshot, CAPTURED, a, n))
    details = {}
    if args.warbond_snapshot:
        warbonds = Snapshot(args.warbond_snapshot)
        if warbonds.game_dll_sha256.upper() != DLL_SHA256:
            raise ValueError("Warbond snapshot is from a different game build")
        wb_game = warbonds.modules["game.dll"]["base"]
        wb_p = struct.unpack("<Q", read(warbonds, CAPTURED, wb_game + CATALOGUE_GLOBAL, 8))[0]
        by_id = {entry["id32"]: entry for entry in season_pass}
        for k in range(len(season_pass)):
            raw = read(warbonds, CAPTURED, wb_p + 0x1749F58 + k * 0x17D4, 0x17D4)
            u = lambda o: struct.unpack_from("<I", raw, o)[0]
            s16 = lambda o: struct.unpack_from("<i", raw, o)[0]
            if not raw[0] or u(4) not in by_id:
                continue
            pages = []
            for p in range(min(u(0x24), 15)):
                base = 0x28 + p * 0x194
                page_items = []
                for j in range(min(u(base + 4), 11)):
                    e = base + 8 + j * 0x24
                    page_items.append({"seasonPassItemId": u(e), "itemMixId": u(e + 4), "medalCost": u(e + 8),
                                       "gridSize": {"x": s16(e + 0xC), "y": s16(e + 0x10)},
                                       "gridPosition": {"x": s16(e + 0x14), "y": s16(e + 0x18)},
                                       "listSortOrder": u(e + 0x1C), "isPremium": False, "isRepeatable": bool(raw[e + 0x20])})
                pages.append({"totalMedalsRequired": u(base), "seasonPassItems": page_items})
            summary = by_id[u(4)]
            details[str(u(4))] = {"id32": u(4), "medalsTotalMixId": summary["medalsTotalMixId"],
                                  "medalsConsumableMixId": summary["medalsConsumableMixId"],
                                  "medalsSpentMixId": summary["medalsSpentMixId"], "totalCost": summary["totalCost"],
                                  "layoutSize": {"x": s16(0x18), "y": s16(0x1C)}, "layoutType": u(0x20),
                                  "seasonPassPages": pages, "pageRequirements": summary["pageRequirements"],
                                  "premiumTokenMixIds": summary["premiumTokenMixIds"]}

    # GET /StoreFront (request 0x347d58a3, parser 0x1081260): stored at P+0x1BB9F8 by 0x103b0b0, storefront
    # stride 0x3C314 (count P+0x1BB9F8+0xF0C500): +0 id32, +4 layoutType, +8/+0xC layoutSize, dword 0xF0C4 section
    # count; sections from dword 4, stride 0x3C3 dwords: +0 id32, +0x3C1 item count, +0x3C2 bannerId32; items from
    # dword 1, stride 0xF: salesId (37-byte string), +10 mixId, +11/+12 position, +13/+14 size.
    store_front = []
    if args.store_snapshot:
        store = Snapshot(args.store_snapshot)
        if store.game_dll_sha256.upper() != DLL_SHA256:
            raise ValueError("Store snapshot is from a different game build")
        st_game = store.modules["game.dll"]["base"]
        st_p = struct.unpack("<Q", read(store, CAPTURED, st_game + CATALOGUE_GLOBAL, 8))[0]
        fronts = struct.unpack("<I", read(store, CAPTURED, st_p + 0x1BB9F8 + 0xF0C500, 4))[0]
        for k in range(min(fronts, 64)):
            raw = read(store, CAPTURED, st_p + 0x1BB9F8 + k * 0x3C314, 0x3C314)
            dw = lambda i: struct.unpack_from("<I", raw, i * 4)[0]
            sections = []
            for j in range(min(dw(0xF0C4), 64)):
                b = 4 + j * 0x3C3
                section_items = []
                for i in range(min(dw(b + 0x3C1), 64)):
                    e = b + 1 + i * 0xF
                    sales = raw[e * 4:e * 4 + 37].split(b"\0")[0].decode("ascii")
                    section_items.append({"salesId": sales, "mixId": dw(e + 10), "position": {"x": dw(e + 11), "y": dw(e + 12)},
                                          "size": {"x": dw(e + 13), "y": dw(e + 14)}})
                sections.append({"id32": dw(b), "bannerId32": dw(b + 0x3C2), "items": section_items})
            store_front.append({"id32": dw(0), "layoutType": dw(1), "layoutSize": {"x": dw(2), "y": dw(3)}, "sections": sections})

    # Package progress (/Progression packageProgress, applied by 0x1038bf0 to P+0x4D4 + index*0x18:
    # {id32, index, level, progress}) and onboarding flags ([game.dll+0x347CEB8]+0x284) from the newest
    # snapshot. Without the tutorial chain's progress the new-player ship_intro waits forever.
    package_progress, onboarding_flags = [], 0
    if args.store_snapshot:
        known_packages = {package["id32"] for package in progression_packages}
        for k in range(64):
            pid, _, level, progress = struct.unpack("<4I", read(store, CAPTURED, st_p + 0x4D4 + k * 0x18, 16))
            if pid in known_packages:
                package_progress.append({"id32": pid, "level": level, "progress": progress, "sortOrder": 0, "claimedLevels": []})
        onboarding = struct.unpack("<Q", read(store, CAPTURED, st_game + 0x347CEB8, 8))[0]
        onboarding_flags = struct.unpack("<I", read(store, CAPTURED, onboarding + 0x284, 4))[0]
    # Warbond purchases (/Progression purchasedSeasonPassItems, stored at P+0xD8A5C, count P+0xD8A58) are
    # seasonPassItemIds; boosters and other warbond items count as owned through them, not the inventory.
    warbond_items = sorted({item["seasonPassItemId"] for detail in details.values() for page in detail["seasonPassPages"]
                            for item in page["seasonPassItems"]})
    default_season_pass = season_pass[0]["id32"] if season_pass else 0
    tutorial = [p for p in package_progress if any(q["id32"] == p["id32"] and q["type"] == 3 for q in progression_packages)]

    names = json.loads((args.hd2runtime / "research/weapon-name-text-ids-F5FEE03DCFDB.json").read_text(encoding="utf-8"))
    by_name = {}
    for entry in names["armory"]:
        if entry.get("composedName"):
            by_name.setdefault(entry["composedName"], int(entry["itemId"], 16))
    known = {item["mixId"] for item in items}
    starter = []
    for name in STARTER_NAMES:
        mix = by_name.get(name)
        if mix is None or mix not in known:
            raise ValueError(f"Starter item {name!r} is not in this catalogue")
        starter.append({"mixId": mix, "amount": 1, "isDiscovered": True, "name": name})

    source = {"snapshot": args.snapshot.name, "gameDllSha256": DLL_SHA256, "count": count,
              "excluded": [{"mixId": item["mixId"], "reason": EXCLUDED_ROOTS.get(item["mixId"], "descendant of an excluded root")}
                           for item in excluded]}
    out = ROOT / "src/HellForge.LocalBackend"
    catalogue_out = args.catalogue_out
    catalogue_out.mkdir(parents=True, exist_ok=True)
    (catalogue_out / "item-catalog.json").write_text(json.dumps(
        {"scope": "Catalogue rebuilt from the user's own retained snapshot of the same build; served as /Progression/items.",
         "source": source, "items": items}, separators=(",", ":")), newline="\n")
    (catalogue_out / "progression-data.json").write_text(json.dumps(
        {"scope": "Progression data rebuilt from the user's own retained snapshot of the same build.", "source": {"snapshot": args.snapshot.name},
         "levelSpec": level_spec, "itemPackages": item_packages, "seasonPass": season_pass, "seasonPassDetails": details,
         "progressionPackages": progression_packages, "storeFront": store_front}, separators=(",", ":")), newline="\n")
    # The base (Helldivers Mobilize) boosters are consumable items the account holds once (amount 1); the
    # later warbond boosters are not consumable. unlock-all grants every booster: the account's, then any it does
    # not own (amount 1). The account the catalogue was rebuilt from lacked Integrated Extinguishers (FireExtinguish,
    # 3545987157), so no unlock-all profile had it (a 0.1.8 report).
    boosters = [{"mixId": o["mixId"], "amount": o["amount"], "isDiscovered": True} for o in owned
                if o["mixId"] in by_mix and by_mix[o["mixId"]]["isConsumable"] and by_mix[o["mixId"]]["progressionCategory"] == BOOSTER_CATEGORY]
    held = {b["mixId"] for b in boosters}
    boosters += [{"mixId": item["mixId"], "amount": 1, "isDiscovered": True} for item in items
                 if item["isConsumable"] and item["progressionCategory"] == BOOSTER_CATEGORY and item["mixId"] not in held]
    unlock_all = [{"mixId": item["mixId"], "amount": 1, "isDiscovered": True} for item in items if not item["isConsumable"]] + boosters
    (out / "profile-templates.json").write_text(json.dumps({
        "scope": "Inventory templates for new local profiles. unlock-all is a development profile that grants every non-consumable catalogue item, including content the account may not own, and every booster.",
        "templates": {
            "starter": {"description": "Fresh player after basic training: AR-23 Liberator, P-2 Peacemaker, G-12 High Explosive.", "items": starter,
                        "progression": {"onboardingFlags": onboarding_flags, "packageProgress": tutorial,
                                        "activeSeasonPass": default_season_pass, "purchasedSeasonPassItems": []}},
            "unlock-all": {"description": "Development: every non-consumable catalogue item, every booster and package progress.",
                           "progression": {"onboardingFlags": onboarding_flags, "packageProgress": package_progress,
                                           "activeSeasonPass": default_season_pass, "purchasedSeasonPassItems": warbond_items},
                           "items": unlock_all},
        }}, indent=1), newline="\n")
    print(json.dumps({"levels": len(level_spec), "itemPackages": len(item_packages), "seasonPass": len(season_pass), "seasonPassDetails": len(details), "progressionPackages": len(progression_packages), "storeFront": len(store_front),
                      "items": len(items), "excluded": len(excluded), "ownedInSnapshot": len(owned), "starter": [s["name"] for s in starter],
                      "unlockAll": len(unlock_all), "consumableBoosters": len(boosters)}))


if __name__ == "__main__":
    main()
