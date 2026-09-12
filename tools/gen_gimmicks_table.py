#!/usr/bin/env python3
"""Bake the gimmick table from the game's own data.

Emits res/embed/gimmicks_table.bin, which cmake/embed.cmake bakes in as
g_gimmicks_table. Run it by hand when the source data changes and commit the
result: it needs ShadowForge to decompile the scene scripts, which is not
something a normal build should have to do.

    python tools/gen_gimmicks_table.py <game-root>

<game-root> is the game data directory reblue reads. Either layout works: a
packaged install, where the inputs are pack/script.ipk and pack/!necessity.ipk,
or a loose tree with script/ and !necessity/ already extracted.

ShadowForge is found on PATH unless --sforge names it. Pass --bdsl instead to
reuse an existing `sforge rpj batch-decompile` output and skip ShadowForge
entirely.

Three sources feed the table:

  script/{town,indoor,dungeon}/<stem>_it<NN>.csv   the search points, each row
      carrying a two-bit flag id and a world position.
  !necessity/bd_gamedat/flaglist_{box,barrier}.csv the chest and barrier flags,
      which are 32-bit script globals rather than two-bit flags.
  script/*/sc*.rpj, decompiled to BDSL                which map places each
      chest and barrier and where it stands, taken from the entry that sets the
      flag beside a give_* call. Nothing shipped records that.
"""
import argparse
import collections
import csv
import glob
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

MAGIC = 0x31474442  # 'BDG1'
VERSION = 2

# Gmk::ReactGim's kind, in the order of the engine's own name table.
POINT_TYPES = ["NONE", "MESS", "ITEM", "GOLD", "HEAL", "DAMG", "STAT", "MEDL",
               "LOCK", "KUSA", "PARA"]
TYPE_INDEX = {n: i for i, n in enumerate(POINT_TYPES)}

NO_FLAG = 0xFFFF  # a point with no Flag ID respawns and cannot be tracked

AREA_CATEGORY = {"town": 1, "indoor": 2, "dungeon": 3, "world": 4, "cube": 5}

# flaglist_barrier's color column, keyed by the first two Shift-JIS bytes of
# the name the way bdFlagListLoad switches on them.
BARRIER_COLORS = {0x90C2: 0, 0x90D4: 1, 0x97CE: 2, 0x9492: 3, 0x8D95: 4}

# bdFieldAreaLoadStep builds the it-file name from the stage's combinedNum, so
# only these three shapes are ever opened. bi19_it01c.csv matches none of them,
# being a typo'd duplicate of bi19_itc01.csv.
IT_FILE_PATTERNS = [
    ("town", re.compile(r"(bg\d\d)_it(\d\d)$"), "{0}_{1}"),
    ("dungeon", re.compile(r"(dg\d\d)_it(\d\d)$"), "{0}_{1}"),
    ("indoor", re.compile(r"(bi\d\d)_it([a-z]\d\d)$"), "{0}{1}"),
]

PACKS = ["script.ipk", "!necessity.ipk"]


def stage_name(cat, num):
    """Mirror of bdStageNameBuild, matching engine/script.cpp."""
    hi, lo = num // 100, num % 100
    if cat == 1:
        return f"bg{hi:02d}_{lo:02d}"
    if cat == 3:
        return f"dg{hi:02d}_{lo:02d}"
    if cat == 2:
        return f"bi{num // 10000:02d}{chr(ord('a') + (num % 10000) // 100)}{lo:02d}"
    if cat == 5:
        return f"wc{hi:02d}_{lo:02d}"
    if cat == 4:
        c = ord('a') + hi
        if c >= ord('l'):
            c += 1
        return f"wd{chr(c)}{lo + 1:02d}"
    return ""


def read_shift_jis_csv(path):
    raw = open(path, "rb").read().decode("cp932", errors="replace")
    return list(csv.reader(raw.splitlines()))


def find_one(root, name):
    hits = glob.glob(os.path.join(root, "**", name), recursive=True)
    if not hits:
        sys.exit(f"{name} not found under {root}")
    return hits[0]


def script_dirs_in(root):
    """The town, indoor and dungeon script folders, or {} if any is missing."""
    out = {}
    for folder, _, _ in IT_FILE_PATTERNS:
        hits = [p for p in glob.glob(os.path.join(root, "**", "script", folder),
                                     recursive=True) if os.path.isdir(p)]
        if not hits:
            return {}
        out[folder] = hits[0]
    return out


def resolve_sforge(explicit):
    path = explicit or shutil.which("sforge")
    if not path or not os.path.exists(path):
        sys.exit("ShadowForge not found. Put sforge on PATH, pass --sforge, or "
                 "pass --bdsl to reuse an existing decompile.")
    return path


def run(*args):
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"{args[0]} failed: {result.stderr.strip() or result.stdout.strip()}")
    return result.stdout


def stage_inputs(game_root, sforge, work):
    """The directory holding the game data, unpacking it first if it is packed."""
    if script_dirs_in(game_root):
        return game_root

    pack_dir = os.path.join(game_root, "pack")
    archives = [os.path.join(pack_dir, p) for p in PACKS]
    missing = [a for a in archives if not os.path.exists(a)]
    if missing:
        sys.exit(f"{game_root} has neither a loose script/ tree nor "
                 f"{', '.join(os.path.basename(m) for m in missing)} under pack/")

    unpacked = os.path.join(work, "game")
    for archive in archives:
        print(f"  extracting {os.path.basename(archive)}")
        run(sforge, "ipk", "extract", archive, "-o", unpacked)
    return unpacked


def load_points(script_dirs):
    """map stem -> [(flagId, typeIndex, x, y, z)], live rows only."""
    out = collections.defaultdict(list)
    unreachable = []
    for folder, pattern, shape in IT_FILE_PATTERNS:
        for path in sorted(glob.glob(os.path.join(script_dirs[folder],
                                                  "*_it*.csv"))):
            base = os.path.basename(path)[:-4]
            if base.endswith("_itinfo"):
                continue
            m = pattern.match(base)
            if not m:
                unreachable.append(base)
                continue
            key = shape.format(m.group(1), m.group(2))

            rows = read_shift_jis_csv(path)
            head = next((i for i, r in enumerate(rows)
                         if r and r[0].strip() == "pointID"), None)
            if head is None:
                continue
            col = {n.strip(): j for j, n in enumerate(rows[head])}
            # Spelled Disable(#) in one file, unnamed in dg05_it05 and
            # dg05_it06. It always follows Comment.
            dele = next((col[c] for c in ("Delete(#)", "Disable(#)") if c in col),
                        None)
            if dele is None and col.get("Comment", len(rows[head])) + 1 < len(rows[head]):
                dele = col["Comment"] + 1

            for row in rows[head + 1:]:
                if not row or not row[0].strip():
                    continue

                def field(name, row=row, col=col):
                    j = col.get(name)
                    return row[j].strip() if j is not None and j < len(row) else ""

                kind = field("Type")
                if kind not in TYPE_INDEX:
                    continue  # two rows carry a mangled Type and place nothing
                if dele is not None and dele < len(row) and row[dele].strip() == "#":
                    continue

                try:
                    flag = int(field("Flag ID"))
                except ValueError:
                    flag = -1
                if flag > NO_FLAG:
                    print(f"  {base}: flag {flag} exceeds the u16 field",
                          file=sys.stderr)
                    continue

                def coord(name, field=field):
                    try:
                        return float(field(name))
                    except ValueError:
                        return 0.0

                out[key].append((NO_FLAG if flag < 0 else flag,
                                 TYPE_INDEX[kind], coord("posX"),
                                 coord("posY"), coord("posZ")))
    if unreachable:
        print(f"  ignored {len(unreachable)} it-file(s) the engine cannot name: "
              + ", ".join(sorted(unreachable)), file=sys.stderr)
    return out


def load_flaglist(path, want_color):
    """[(flagId, colorIndex)] for the live rows of a flaglist CSV."""
    out = []
    for row in read_shift_jis_csv(path):
        if len(row) < 2 or row[0].strip() != "":
            continue
        if not row[1].strip().lstrip("-").isdigit():
            continue
        color = 0
        if want_color and len(row) > 2:
            name = row[2].strip().encode("cp932", errors="replace")
            if len(name) >= 2:
                color = BARRIER_COLORS.get((name[0] << 8) | name[1], 0)
        out.append((int(row[1]), color))
    return out


def scan_scripts(bdsl_dir):
    """flagId -> {map stem: (strength, pos, openValue)} over every scene."""
    entry = re.compile(r'^(spawn|link|box|zone|enemy|warp|entity) "(.*)" id=(\d+)',
                       re.M)
    setvar = re.compile(r"set_variable\(dest_var=0x([0-9A-Fa-f]+),"
                        r"(?: operator=0x0, source_var=0x0,"
                        r" value=0x([0-9A-Fa-f]+))?")
    give = re.compile(r"\b(give_item|give_gold|give_medal|give_item_special)\(")
    where = re.compile(r"^\s*position = \(([-\d.E+]+), ([-\d.E+]+), ([-\d.E+]+)\)",
                       re.M)

    scored = collections.defaultdict(dict)
    for path in sorted(glob.glob(os.path.join(bdsl_dir, "**", "*.bdsl"),
                                 recursive=True)):
        text = open(path, encoding="utf-8", errors="replace").read()
        area = re.search(r"^\s*area = (\w+)", text, re.M)
        if not area:
            continue
        cat = AREA_CATEGORY.get(area.group(1), 0)
        num = re.search(r"^\s*stage_id = (\d+)", text, re.M)
        # The world script has no stage_id and covers the whole overworld.
        key = stage_name(cat, int(num.group(1))) if num else "wd_world"
        if not key:
            continue

        marks = [(m.start(), m.group(1)) for m in entry.finditer(text)]
        marks.append((len(text), None))
        for i in range(len(marks) - 1):
            body = text[marks[i][0]:marks[i + 1][0]]
            spot = where.search(body)
            if not spot:
                continue
            pos = tuple(float(c) for c in spot.groups())
            strength = 1 if give.search(body) else 0
            # A guarded chest shares its flag with the guard, which sets it
            # to 1 on defeat.
            opened = {}
            if strength:
                for var, value in setvar.findall(body):
                    if not value:
                        continue
                    flag = int(var, 16) - 256
                    if flag >= 0 and int(value, 16):
                        opened[flag] = max(opened.get(flag, 0), int(value, 16))
            for var, _ in setvar.findall(body):
                # Script variable ids below 256 are per-script locals.
                flag = int(var, 16) - 256
                if flag < 0:
                    continue
                prev = scored[flag].get(key)
                if prev is None or strength >= prev[0]:
                    scored[flag][key] = (strength, pos, opened.get(flag, 0))

    return scored


def attribute(scored, flags):
    """(flagId -> {map stem: (x, y, z)}, flagId -> open value).

    A chest is a link entry whose block gives an item and then sets the flag.
    Blocks that only set it are chapter resets, so one that also gives outranks
    one that does not. What ties for the win is a map with several story-state
    variants (Talta is bg03_01, bg04_01 and bg32_01), where counting the chest
    under each variant is what we want.
    """
    out = {}
    opened = {}
    for flag in flags:
        cands = scored.get(flag)
        if not cands:
            continue
        best = max(v[0] for v in cands.values())
        won = {k: v for k, v in cands.items() if v[0] == best}
        if len(won) > 1:
            # The overworld script also carries a debug dump of chest flags.
            real = {k: v for k, v in won.items() if k != "wd_world"}
            if real:
                won = real
        out[flag] = {k: v[1] for k, v in won.items()}
        opened[flag] = max(max(v[2] for v in won.values()), 1)
    return out, opened


def pack(points, chests, barriers, chest_maps, barrier_maps, chest_open):
    strings = bytearray()
    string_off = {}

    def intern(s):
        if s not in string_off:
            string_off[s] = len(strings)
            strings.extend(s.encode("ascii") + b"\0")
        return string_off[s]

    chest_flags = sorted(f for f, _ in chests)
    chest_slot = {f: i for i, f in enumerate(chest_flags)}
    barrier_flags = sorted(f for f, _ in barriers)
    barrier_slot = {f: i for i, f in enumerate(barrier_flags)}
    barrier_color = dict(barriers)

    def by_map(attributed, slot_of):
        out = collections.defaultdict(list)
        for flag, maps in attributed.items():
            for name, pos in maps.items():
                out[name].append((slot_of[flag],) + pos)
        return out

    by_map_chest = by_map(chest_maps, chest_slot)
    by_map_barrier = by_map(barrier_maps, barrier_slot)

    names = sorted(set(points) | set(by_map_chest) | set(by_map_barrier))
    maps, point_rows, chest_placed, barrier_placed = [], [], [], []
    for name in names:
        pts = points.get(name, [])
        ci = sorted(by_map_chest.get(name, []))
        bi = sorted(by_map_barrier.get(name, []))
        maps.append((intern(name), len(point_rows), len(pts),
                     len(chest_placed), len(ci), len(barrier_placed), len(bi)))
        point_rows.extend(pts)
        chest_placed.extend(ci)
        barrier_placed.extend(bi)

    def align4(buf):
        buf.extend(b"\0" * (-len(buf) % 4))

    body = bytearray()
    for rec in maps:
        body.extend(struct.pack("<7I", *rec))
    for flag, kind, x, y, z in point_rows:
        body.extend(struct.pack("<HBxfff", flag, kind, x, y, z))
    for f in chest_flags:
        body.extend(struct.pack("<H", f))
    align4(body)
    for f in chest_flags:
        body.append(min(chest_open.get(f, 1), 255))
    align4(body)
    for f in barrier_flags:
        body.extend(struct.pack("<H", f))
    align4(body)
    for f in barrier_flags:
        body.append(barrier_color[f])
    align4(body)
    for rec in chest_placed + barrier_placed:
        body.extend(struct.pack("<HHfff", rec[0], 0, rec[1], rec[2], rec[3]))
    body.extend(strings)
    align4(body)

    header = struct.pack("<9I", MAGIC, VERSION, len(maps), len(point_rows),
                         len(chest_flags), len(barrier_flags),
                         len(chest_placed), len(barrier_placed), len(strings))
    return header + bytes(body), maps, point_rows


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game_root",
                    help="game data directory, packaged or loosely extracted")
    ap.add_argument("--sforge", help="path to sforge (default: found on PATH)")
    ap.add_argument("--bdsl",
                    help="existing rpj batch-decompile output, skips ShadowForge")
    ap.add_argument("--out", default=os.path.join(here, "res",
                                                  "embed", "gimmicks_table.bin"))
    ap.add_argument("--work", help="keep intermediate files here instead of a "
                                   "temporary directory")
    args = ap.parse_args()

    work = args.work or tempfile.mkdtemp(prefix="gimmicks_")
    os.makedirs(work, exist_ok=True)
    try:
        sforge = None if args.bdsl else resolve_sforge(args.sforge)
        root = stage_inputs(args.game_root, sforge, work)
        script_dirs = script_dirs_in(root)
        if not script_dirs:
            sys.exit(f"no script/town, script/indoor and script/dungeon under {root}")

        bdsl = args.bdsl
        if not bdsl:
            bdsl = os.path.join(work, "bdsl")
            print("  decompiling scene scripts")
            run(sforge, "rpj", "batch-decompile",
                os.path.dirname(next(iter(script_dirs.values()))), "-o", bdsl)

        points = load_points(script_dirs)
        chests = load_flaglist(find_one(root, "flaglist_box.csv"), False)
        barriers = load_flaglist(find_one(root, "flaglist_barrier.csv"), True)

        scored = scan_scripts(bdsl)
        chest_maps, chest_open = attribute(scored, [f for f, _ in chests])
        barrier_maps, _ = attribute(scored, [f for f, _ in barriers])

        blob, maps, rows = pack(points, chests, barriers, chest_maps,
                                barrier_maps, chest_open)
        os.makedirs(os.path.dirname(args.out), exist_ok=True)
        with open(args.out, "wb") as f:
            f.write(blob)
    finally:
        if not args.work:
            shutil.rmtree(work, ignore_errors=True)

    trackable = sum(1 for r in rows if r[0] != NO_FLAG)
    print(f"{args.out}: {len(blob)} bytes")
    print(f"  maps      {len(maps)}")
    print(f"  points    {len(rows)}  trackable {trackable}  "
          f"respawning {len(rows) - trackable}")
    guarded = sum(1 for v in chest_open.values() if v > 1)
    print(f"  chests    {len(chests)}  placed {len(chest_maps)}  "
          f"unplaced {len(chests) - len(chest_maps)}  "
          f"opened-at-2-or-more {guarded}")
    print(f"  barriers  {len(barriers)}  placed {len(barrier_maps)}")


if __name__ == "__main__":
    main()
