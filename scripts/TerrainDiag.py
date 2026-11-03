#!/usr/bin/env python3
"""Read, compare and render the loader's terrain diagnostics (HELLFORGE_TERRAIN_DIAG, difficulty_world_diag.c).

  TerrainDiag.py summary terrain-diag-3.json
      the level's settings, plan, slowest passes, every measured image and the flags

  TerrainDiag.py compare BASELINE.json TEST.json [--json] [--map layer-heightfield=tile-heights]
      a vanilla level's report against an extra large one: each image the two share (by label; --map pairs differently
      named ones, such as a terrain tile against vanilla's heights), metric by metric, with what looks wrong (less
      detail, less relief, flatter, longer features, unpainted, clamp bars, sparse), and the passes' times

  TerrainDiag.py render LOGS_DIR [--level N] [--out DIR] [--max 2048]
      PNGs of the images level 2 dumped (terrain-<level>-<nn>-<label>.bin + .txt): heights as a hillshade with a slope
      map beside it, the material mix by slot and weight, the ground ids by id and density, anything else in false colour

summary and compare need only Python; render needs numpy and Pillow.
"""

import argparse
import glob
import json
import math
import os
import sys

HEIGHT_METRICS = [
    # (path, name, bad direction: "low" = smaller in the test is the problem, "high" = larger is, ratio that counts)
    (("heights", "sd"), "height sd (m)", "low", 0.5),
    (("heights", "p99_p01"), "height range p1-p99 (m)", "low", 0.5),
    (("slope", "mean_deg"), "mean slope (deg)", "low", 0.6),
    (("slope", "flat_lt1"), "flat share (<1 deg)", "high", 1.5),
    (("slope", "steep_ge35"), "steep share (>=35 deg)", "low", 0.5),
    (("curvature_sd_per_m",), "curvature sd (1/m)", "low", 0.5),
    (("detail_sd_m", "8"), "detail @8 m (m)", "low", 0.5),
    (("detail_sd_m", "32"), "detail @32 m (m)", "low", 0.5),
    (("detail_sd_m", "128"), "detail @128 m (m)", "low", 0.5),
    (("detail_sd_m", "512"), "detail @512 m (m)", "low", 0.5),
    (("detail_sd_m", "2048"), "detail @2048 m (m)", "low", 0.5),
    (("relief_m", "32"), "relief in 32 m blocks (m)", "low", 0.5),
    (("relief_m", "128"), "relief in 128 m blocks (m)", "low", 0.5),
    (("relief_m", "512"), "relief in 512 m blocks (m)", "low", 0.5),
    (("relief_p90_m", "128"), "relief p90, 128 m (m)", "low", 0.5),
    (("correlation_m", "x_512"), "feature length x (m)", "high", 1.5),
    (("correlation_m", "y_512"), "feature length y (m)", "high", 1.5),
    (("correlation_m", "x_2048"), "feature length x, 2 km trend (m)", "high", 1.5),
    (("correlation_m", "y_2048"), "feature length y, 2 km trend (m)", "high", 1.5),
    (("metres_per_sample",), "metres per sample", "high", 1.01),
]
PACKED_METRICS = [
    (("painted_share",), "painted share", "low", 0.5),
    (("mode_share",), "most common value's share", "high", 1.5),
    (("clamp_bar_share",), "clamp bars (straight runs of one value)", "high", 2.0),
    (("weight_a_mean",), "weight a mean", "low", 0.5),
    (("weight_b_mean",), "weight b mean", "low", 0.5),
    (("density_mean",), "ground density", "low", 0.6),
]
PLAN_METRICS = [
    (("vertices_per_km2",), "Voronoi vertices per km2", "low", 0.5),
    (("spacing_median_m",), "lattice spacing median (m)", "high", 1.5),
    (("height_sd",), "plan height sd", "low", 0.5),
    (("height_zero_share",), "plan heights at 0", "high", 1.5),
]


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def get(obj, path):
    for key in path:
        if not isinstance(obj, dict) or key not in obj:
            return None
        obj = obj[key]
    if isinstance(obj, str) and obj.startswith(">"):  # a correlation length beyond the loader's search: at least this
        try:
            return float(obj[1:])
        except ValueError:
            return None
    return obj if isinstance(obj, (int, float)) and not isinstance(obj, bool) else None


def fmt(v):
    if v is None:
        return "-"
    if isinstance(v, int) or abs(v) >= 100:
        return f"{v:.0f}"
    if abs(v) >= 1:
        return f"{v:.2f}"
    return f"{v:.4f}"


def image_key(image):
    """Labels carry a capture set or op suffix; match images by what they are."""
    return image.get("label", "?")


# ---- summary ----------------------------------------------------------------------------------------------------------

def summary(args):
    r = load(args.report)
    s = r.get("settings") or {}
    print(f"level {r.get('level')}  world {fmt(r.get('world_m'))} m ({fmt(r.get('world_scale'))}x)  disk {fmt(r.get('disk_m'))} m  "
          f"scatter radius {fmt(r.get('scatter_radius_m'))} m  {r.get('records')} passes  {r.get('frames')} frames")
    if r.get("completed") is False:
        print("INCOMPLETE: generation never reached its last pass (a FixUnreachableAreas restart or a quit); the figures are partial")
    if s:
        print(f"settings: fade {fmt(s.get('fade_m'))} m, disk {fmt(s.get('disk_m'))} m, vista fade {fmt(s.get('vista_fade_m'))} m, "
              f"tiles {s.get('merge_tiles')} x {fmt(s.get('tile_samples'))} samples, {fmt(s.get('metres_per_sample'))} m per sample, "
              f"difficulty {s.get('difficulty')}, map type {s.get('map_type_raw')}, zone repeat {s.get('zone_repeat')}")
    p = r.get("plan")
    if p:
        print(f"plan: {p.get('vertices')} vertices ({fmt(p.get('vertices_per_km2'))}/km2), spacing median {fmt(p.get('spacing_median_m'))} m, "
              f"heights {fmt(p.get('height_min'))}..{fmt(p.get('height_max'))} sd {fmt(p.get('height_sd'))} ({fmt(100 * (p.get('height_zero_share') or 0))}% at 0), "
              f"{p.get('subregion_cells')} subregion cells, {p.get('locations')} locations")
        print(f"      height groups {p.get('groups')}  types {p.get('types')}")
    ops = sorted(r.get("ops", []), key=lambda o: -o.get("ms", 0))
    print("slowest passes:")
    for o in ops[:12]:
        print(f"  {o['record']:4d} {o['op']} {o['pass']:<40} {o['ms']:9.1f} ms  {o['steps']:5d} steps  span {o['span_ms']} ms"
              + (f"  +{o['locations']} locations" if o.get("locations") else ""))
    unfinished = [o for o in r.get("ops", []) if not o.get("done")]
    if unfinished:
        print(f"passes that never finished: {', '.join(o['op'] + ' ' + o['pass'] for o in unfinished[:10])}")
    print("images:")
    for im in r.get("images", []):
        if im.get("stale"):
            print(f"  {im['label']:<26} after {im['after_op']} {im['after_pass']}: STALE, the previous level's image (not measured)")
            continue
        if "same_as" in im:
            print(f"  {im['label']:<26} after {im['after_op']} {im['after_pass']}: the same image as {im['same_as']}")
            continue
        line = f"  {im['label']:<26} {im['kind']:<12} {im['columns']}x{im['rows']} after {im['after_op']} {im['after_pass']}"
        if "heights" in im and im["heights"]:
            line += (f": sd {fmt(get(im, ('heights', 'sd')))} m, slope {fmt(get(im, ('slope', 'mean_deg')))} deg, flat {fmt(get(im, ('slope', 'flat_lt1')))}, "
                     f"detail@32 {fmt(get(im, ('detail_sd_m', '32')))} m, relief@128 {fmt(get(im, ('relief_m', '128')))} m, "
                     f"length x/y {fmt(get(im, ('correlation_m', 'x_512')))}/{fmt(get(im, ('correlation_m', 'y_512')))} m")
        elif "painted_share" in im:
            line += (f": painted {fmt(im['painted_share'])}, most common {im.get('mode_value')} {fmt(im.get('mode_share'))}, "
                     f"clamp bars {fmt(im.get('clamp_bar_share'))}, slots {im.get('slot_cover')}")
        elif "density_mean" in im:
            line += f": density {fmt(im['density_mean'])}, ids {im.get('id_cover')}"
        elif "distinct" in im:
            line += f": {im['distinct']} distinct values"
        print(line)
    flags = r.get("flags", [])
    print("flags:" if flags else "flags: none")
    for f in flags:
        print(f"  {f['code']}: {f['text']}")


# ---- compare ----------------------------------------------------------------------------------------------------------

def judge(base, test, bad, ratio):
    if base is None or test is None:
        return None, ""
    if base == 0:
        return None, "" if test == 0 else "new"
    q = test / base
    if bad == "low" and q < ratio:
        return q, "LOW"
    if bad == "high" and q > ratio:
        return q, "HIGH"
    return q, ""


def compare(args):
    a, b = load(args.baseline), load(args.test)
    findings, rows = [], []

    def section(title, metrics, base, test):
        rows.append((title, None, None, None, ""))
        for path, name, bad, ratio in metrics:
            vb, vt = get(base, path), get(test, path)
            if vb is None and vt is None:
                continue
            q, verdict = judge(vb, vt, bad, ratio)
            rows.append((name, vb, vt, q, verdict))
            if verdict in ("LOW", "HIGH"):
                findings.append(f"{title}: {name} {fmt(vb)} -> {fmt(vt)} ({fmt(q)}x)")

    section("plan", PLAN_METRICS, a.get("plan") or {}, b.get("plan") or {})
    for name, r in (("baseline", a), ("test", b)):
        if r.get("completed") is False:
            findings.append(f"the {name} report is INCOMPLETE (generation never reached its last pass): its figures are partial")
    ia = {image_key(i): i for i in a.get("images", []) if "same_as" not in i and not i.get("stale")}
    ib = {image_key(i): i for i in b.get("images", []) if "same_as" not in i and not i.get("stale")}
    pairs = [(k, k) for k in ia if k in ib]
    for m in args.map or []:  # e.g. layer-heightfield=tile-heights: a baseline image against a differently named test image
        x, _, y = m.partition("=")
        if x in ia and y in ib:
            pairs.append((x, y))
        else:
            findings.append(f"--map {m}: {'no ' + x + ' in the baseline' if x not in ia else 'no ' + y + ' in the test'}")
    for la, lb in pairs:
        label = la if la == lb else f"{la} vs {lb}"
        metrics = HEIGHT_METRICS if "heights" in ia[la] else PACKED_METRICS
        section(f"image {label} ({ia[la]['kind']})", metrics, ia[la], ib[lb])
        label = lb
        if "heights" in ib[label]:
            x, y = get(ib[label], ("correlation_m", "x_512")), get(ib[label], ("correlation_m", "y_512"))
            if x and y and (x / y > 1.33 or y / x > 1.33):
                findings.append(f"image {label}: features {fmt(x)} m along x, {fmt(y)} m along y (uneven stretching)")
    only = sorted(set(ia) ^ set(ib))
    ta = {o["op"]: o for o in a.get("ops", [])}
    tb = {o["op"]: o for o in b.get("ops", [])}
    timing = sorted(((tb[k]["ms"] - ta[k]["ms"], k) for k in tb if k in ta), reverse=True)[:10]
    if args.json:
        json.dump({"rows": [dict(zip(("metric", "baseline", "test", "ratio", "verdict"), r)) for r in rows], "findings": findings,
                   "only_in_one": only, "slower_passes": [{"op": k, "pass": tb[k]["pass"], "extra_ms": d} for d, k in timing]}, sys.stdout, indent=2)
        print()
        return
    print(f"baseline level {a.get('level')} ({fmt(a.get('world_m'))} m)  vs  test level {b.get('level')} ({fmt(b.get('world_m'))} m)")
    for name, vb, vt, q, verdict in rows:
        if vb is None and vt is None and q is None:
            print(f"\n{name}")
            continue
        print(f"  {name:<34} {fmt(vb):>12} {fmt(vt):>12} {fmt(q):>9}x {verdict}")
    if only:
        print(f"\nimages in only one report: {', '.join(only)}")
    print("\npasses that took longest more in the test:")
    for d, k in timing:
        print(f"  {k} {tb[k]['pass']:<40} {d:+10.1f} ms")
    print("\nflags (test): " + ("; ".join(f["code"] for f in b.get("flags", [])) or "none"))
    print("\nfindings:" if findings else "\nfindings: none")
    for f in findings:
        print(f"  - {f}")


# ---- render -----------------------------------------------------------------------------------------------------------

def read_dump(bin_path):
    import numpy as np
    meta = {}
    with open(bin_path[:-4] + ".txt", encoding="utf-8") as f:
        for line in f:
            k, _, v = line.strip().partition(" ")
            meta[k] = v
    w, h, bpp = int(meta["columns"]), int(meta["rows"]), int(meta["bytes_per_sample"])
    dtype = {2: np.uint16, 4: np.uint32}[bpp]
    data = np.fromfile(bin_path, dtype=dtype, count=w * h).reshape(h, w)
    kind = meta["kind"]
    if kind == "heights_u16":
        data = data.astype(np.float32) / 65535.0 * 1000.0 - 500.0
    elif kind == "heights_f32":
        data = data.view(np.float32)
    if meta.get("rows_flipped") == "1":
        data = data[::-1]
    return meta, kind, data


def shrink(a, limit):
    import numpy as np
    f = max(1, math.ceil(max(a.shape) / limit))
    if f == 1:
        return a, 1
    h, w = a.shape[0] // f * f, a.shape[1] // f * f
    if a.dtype.kind == "f":
        return a[:h, :w].reshape(h // f, f, w // f, f).mean(axis=(1, 3)), f
    return a[:h:f, :w:f], f


def palette(n):
    import colorsys
    return [tuple(int(255 * c) for c in colorsys.hsv_to_rgb((i * 0.618) % 1, 0.65, 0.95)) for i in range(n)]


def render_heights(meta, data, limit):
    import numpy as np
    from PIL import Image
    m = float(meta.get("metres_per_sample", 1))
    z, f = shrink(np.nan_to_num(data.astype(np.float32)), limit)
    cell = m * f
    gy, gx = np.gradient(z, cell)
    slope = np.degrees(np.arctan(np.hypot(gx, gy)))
    az, alt = math.radians(315), math.radians(45)
    aspect = np.arctan2(-gx, gy)
    shade = np.sin(alt) * np.cos(np.arctan(np.hypot(gx, gy))) + np.cos(alt) * np.sin(np.arctan(np.hypot(gx, gy))) * np.cos(az - aspect)
    lo, hi = np.percentile(z, 1), np.percentile(z, 99)
    t = np.clip((z - lo) / max(hi - lo, 1e-6), 0, 1)
    tint = np.stack([0.35 + 0.5 * t, 0.45 + 0.35 * t, 0.30 + 0.2 * t], axis=-1)
    left = np.clip(tint * (0.35 + 0.65 * np.clip(shade, 0, 1))[..., None], 0, 1)
    s = np.clip(slope / 45.0, 0, 1)
    right = np.stack([s, np.clip(1.6 * s * (1 - s) + 0.1, 0, 1), 1 - s], axis=-1)
    right[slope < 1.0] = (0.08, 0.08, 0.08)  # flat: dark
    out = np.concatenate([left, right], axis=1)
    return Image.fromarray((out * 255).astype(np.uint8)), f"hillshade | slope (dark < 1 deg, blue..red 0..45 deg), {cell:.2f} m per pixel"


def render_packed(meta, data, ground, limit):
    import numpy as np
    from PIL import Image
    v, f = shrink(data, limit)
    pal = np.array(palette(32), dtype=np.float32) / 255
    if ground:
        ids = (v >> 17) & 31
        level = (v & 0x7F).astype(np.float32) / 127
        rgb = pal[ids] * (0.25 + 0.75 * level)[..., None]
        note = "ground: colour = id 0's table slot, brightness = density"
    else:
        a, b = (v & 0xFF).astype(np.float32) / 255, ((v >> 8) & 0xFF).astype(np.float32) / 255
        rest = np.clip(1 - a - b, 0, 1)
        rgb = pal[(v >> 16) & 31] * a[..., None] + pal[(v >> 21) & 31] * b[..., None] + pal[(v >> 26) & 31] * rest[..., None] * 0.35
        note = "materials: slots coloured by weight (the unweighted third slot dimmed); black-ish = unpainted"
    return Image.fromarray((np.clip(rgb, 0, 1) * 255).astype(np.uint8)), note


def render_raw(meta, data, limit):
    import numpy as np
    from PIL import Image
    v, f = shrink(data, limit)
    values, inverse = np.unique(v, return_inverse=True)
    if len(values) <= 256:
        pal = np.array(palette(len(values)), dtype=np.uint8)
        return Image.fromarray(pal[inverse.reshape(v.shape)]), f"{len(values)} distinct values, false colour"
    x = v.astype(np.float64)
    lo, hi = np.percentile(x, 1), np.percentile(x, 99)
    g = np.clip((x - lo) / max(hi - lo, 1e-9), 0, 1)
    return Image.fromarray((g * 255).astype(np.uint8)), f"{len(values)} distinct values, grey p1..p99"


def render(args):
    out = args.out or os.path.join(args.logs, "terrain-png")
    os.makedirs(out, exist_ok=True)
    pattern = f"terrain-{args.level}-*.bin" if args.level is not None else "terrain-*-*.bin"
    paths = sorted(glob.glob(os.path.join(args.logs, pattern)))
    if not paths:
        sys.exit(f"no terrain-*.bin dumps in {args.logs} (they need HELLFORGE_TERRAIN_DIAG=2)")
    for path in paths:
        meta, kind, data = read_dump(path)
        if kind.startswith("heights"):
            image, note = render_heights(meta, data, args.max)
        elif kind in ("materials", "ground"):
            image, note = render_packed(meta, data, kind == "ground", args.max)
        else:
            image, note = render_raw(meta, data, args.max)
        target = os.path.join(out, os.path.basename(path)[:-4] + ".png")
        image.save(target)
        print(f"{target}: {meta.get('label')} {meta.get('columns')}x{meta.get('rows')} ({note})")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("summary")
    p.add_argument("report")
    p.set_defaults(run=summary)
    p = sub.add_parser("compare")
    p.add_argument("baseline")
    p.add_argument("test")
    p.add_argument("--json", action="store_true")
    p.add_argument("--map", action="append", metavar="BASE=TEST", help="also compare a baseline image with a differently named test image")
    p.set_defaults(run=compare)
    p = sub.add_parser("render")
    p.add_argument("logs")
    p.add_argument("--level", type=int)
    p.add_argument("--out")
    p.add_argument("--max", type=int, default=2048)
    p.set_defaults(run=render)
    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
