#!/usr/bin/env python3
"""Prepare retail Korean Blue Dragon data for the US-code re:Blue runtime.

This tool does not contain, download, or redistribute game data.  It operates
only on a ``game`` directory that the user extracted from their own Korean
retail discs with re:Blue's installer.

Why this is needed
------------------
The Korean retail movies use a two-audio-stream Sofdec layout:

    C1 = Japanese
    C2 = Korean

The re:Blue static recompilation is built from the NTSC-U executable and its
movie path expects the three-stream system topology used by that executable.
For Korean data we keep every Korean-retail video/audio PES body byte-for-byte
and synthesize one compatibility stream:

    normal regional movie: C3 = clone of Korean-retail C1 (Japanese)
    BDopdemo special:       C3 = clone of Korean-retail C2 (Korean)

After removing C3, the transformed movie has the exact same C1/C2/E0 media
sequence and PES bodies as the Korean retail source.  Pack/SCR headers and the
Sofdec stream table are regenerated for the three-stream bitrate.

The operation is intentionally dry-run by default.  Pass ``--apply`` to make
atomic in-place replacements.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path
from typing import Iterable


SECTOR = 0x800
PACK_START = b"\x00\x00\x01\xBA"
END_START = b"\x00\x00\x01\xB9"
SYSTEM_START = b"\x00\x00\x01\xBB"
PADDING_START = b"\x00\x00\x01\xBE"
SOFDEC_LABEL = b"SofdecStream"

C1 = 0xC1
C2 = 0xC2
C3 = 0xC3
VIDEO = 0xE0


class PrepareError(RuntimeError):
    pass


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def sid(sec: bytes) -> int | None:
    if (
        len(sec) >= 16
        and sec[:4] == PACK_START
        and sec[12:15] == b"\x00\x00\x01"
    ):
        return sec[15]
    return None


def split_sectors(data: bytes) -> list[bytes]:
    if len(data) % SECTOR:
        raise PrepareError("SFD is not 0x800-sector aligned")
    return [data[i : i + SECTOR] for i in range(0, len(data), SECTOR)]


def label_offset(data: bytes) -> int:
    pos = data.find(SOFDEC_LABEL)
    if pos < 0:
        raise PrepareError("SofdecStream header not found")
    return pos


def stream_counts(data: bytes) -> tuple[int, int]:
    pos = label_offset(data)
    return data[pos + 0x90], data[pos + 0x91]


def audio_record(data: bytes, wanted: int) -> bytes:
    pos = label_offset(data)
    total = data[pos + 0x90]
    for i in range(total):
        off = pos + 0x160 + i * 0x40
        rec = data[off : off + 0x40]
        if len(rec) == 0x40 and rec[24] == wanted:
            return rec
    raise PrepareError(f"audio record {wanted:#x} not found")


def audio_record_offsets(data: bytes) -> dict[int, int]:
    pos = label_offset(data)
    total = data[pos + 0x90]
    out: dict[int, int] = {}
    for i in range(total):
        off = pos + 0x160 + i * 0x40
        rec = data[off : off + 0x40]
        if len(rec) == 0x40 and 0xC0 <= rec[24] <= 0xDF:
            out[rec[24]] = off
    return out


def collect_bodies(data: bytes, wanted: int) -> list[bytes]:
    return [sec[12:] for sec in split_sectors(data) if sid(sec) == wanted]


def patch_body_sid(body: bytes, new_sid: int) -> bytes:
    if len(body) < 4 or body[:3] != b"\x00\x00\x01":
        raise PrepareError("bad PES body while cloning audio stream")
    out = bytearray(body)
    out[3] = new_sid
    return bytes(out)


def mpeg_ts(value: int, hi: int) -> bytes:
    return bytes(
        (
            ((((value >> 30) & 7) << 1) | 1) | (hi & 0xF0),
            (value >> 22) & 0xFF,
            (((value >> 15) & 0x7F) << 1) | 1,
            (value >> 7) & 0xFF,
            ((value & 0x7F) << 1) | 1,
        )
    )


def round_positive_fraction(value: Fraction) -> int:
    return (2 * value.numerator + value.denominator) // (2 * value.denominator)


def pack_header(pack_index: int, exact_rate: Fraction, mux_rate: int) -> bytes:
    scr = round_positive_fraction(
        Fraction(90000 * (pack_index * SECTOR + 9), 1) / exact_rate
    )
    return PACK_START + mpeg_ts(scr, 0x20) + bytes(
        (
            ((mux_rate >> 15) & 0x7F) | 0x80,
            (mux_rate >> 7) & 0xFF,
            ((mux_rate & 0x7F) << 1) | 1,
        )
    )


def video_bps(data: bytes) -> int:
    for sec in split_sectors(data):
        if sid(sec) != VIDEO:
            continue
        plen = int.from_bytes(sec[16:18], "big")
        i = 18
        while i < SECTOR and sec[i] == 0xFF:
            i += 1
        if i < SECTOR and (sec[i] & 0xC0) == 0x40:
            i += 2
        if i < SECTOR:
            control = sec[i]
            if (control & 0xF0) == 0x20:
                i += 5
            elif (control & 0xF0) == 0x30:
                i += 10
            elif control == 0x0F:
                i += 1
        payload = sec[i : min(18 + plen, SECTOR)]
        marker = payload.find(b"\x00\x00\x01\xB3")
        if marker >= 0:
            q = payload[marker + 4 : marker + 11]
            if len(q) < 7:
                break
            bit_rate_value = (q[4] << 10) | (q[5] << 2) | ((q[6] & 0xC0) >> 6)
            return bit_rate_value * 400
    raise PrepareError("MPEG sequence header not found")


def exact_rate(data: bytes, bdop: bool) -> Fraction:
    video = Fraction(video_bps(data), 8) * Fraction(2048, 2018)
    aix = Fraction(1303680, 8) * Fraction(2048, 2016)
    if bdop:
        sfa = Fraction(396900, 8) * Fraction(2048, 2016)
        return video + sfa + aix + aix
    return video + aix + aix + aix


def mux_rate_for(rate: Fraction) -> int:
    return (rate.numerator + rate.denominator * 50 - 1) // (rate.denominator * 50)


def patch_system_rate(sec: bytearray, mux_rate: int) -> None:
    """Update a MPEG system-header rate if one follows the pack header."""
    if len(sec) != SECTOR or sec[12:16] != SYSTEM_START:
        return
    sec[18] = 0x80 | ((mux_rate >> 15) & 0x7F)
    sec[19] = (mux_rate >> 7) & 0xFF
    sec[20] = ((mux_rate & 0x7F) << 1) | 1


def make_three_stream_header(source: bytes, bdop: bool) -> tuple[list[bytes], Fraction, int]:
    sectors = split_sectors(source)
    if len(sectors) < 4:
        raise PrepareError("SFD is too small to contain a Sofdec header")

    header = bytearray(b"".join(sectors[:4]))
    label = label_offset(header)
    total_records = header[label + 0x90]
    audio_count = header[label + 0x91]
    if audio_count != 2:
        raise PrepareError(f"expected a 2-audio-stream Korean SFD, got {audio_count}")
    if total_records != 4:
        raise PrepareError(f"expected four Sofdec stream records, got {total_records}")

    c1 = audio_record(bytes(header), C1)
    c2 = audio_record(bytes(header), C2)
    c3 = bytearray(c2 if bdop else c1)
    c3[24] = C3

    # The Korean header has a free record slot inside the fixed four-sector
    # header block.  Expand the table from BF/E0/C1/C2 to BF/E0/C1/C2/C3.
    new_record_off = label + 0x160 + total_records * 0x40
    if new_record_off + 0x40 > len(header):
        raise PrepareError("Sofdec header has no room for a third audio record")
    header[new_record_off : new_record_off + 0x40] = c3
    header[label + 0x90] = total_records + 1
    header[label + 0x91] = audio_count + 1

    rate = exact_rate(source, bdop)
    mux_rate = mux_rate_for(rate)
    header[label + 0x94 : label + 0x98] = round_positive_fraction(rate).to_bytes(
        4, "little"
    )

    # Sector 0 carries the full system header.  Add a C3 stream descriptor,
    # grow the header by three bytes, and shrink the padding packet by three.
    first = bytearray(header[:SECTOR])
    if first[12:16] != SYSTEM_START:
        raise PrepareError("unexpected first-sector MPEG system header")
    old_len = int.from_bytes(first[16:18], "big")
    if old_len != 12:
        raise PrepareError(f"unexpected two-stream system header length {old_len}")
    if first[24:27] != bytes((C1, 0xC0, 0x04)):
        raise PrepareError("unexpected C1 system descriptor")
    if first[27:30] != bytes((C2, 0xC0, 0x04)):
        raise PrepareError("unexpected C2 system descriptor")

    first[16:18] = (15).to_bytes(2, "big")
    first[21] = (first[21] & 0xC3) | (3 << 2)  # audio_bound = 3
    first[30:33] = bytes((C3, 0xC0, 0x04))
    first[33:37] = PADDING_START
    # Padding payload begins with 0x0f and then 0xff bytes.
    first[37:39] = (SECTOR - 39).to_bytes(2, "big")
    first[39] = 0x0F
    first[40:] = b"\xFF" * (SECTOR - 40)
    patch_system_rate(first, mux_rate)
    header[:SECTOR] = first

    patched = split_sectors(bytes(header))
    for i, sec in enumerate(patched):
        out = bytearray(sec)
        patch_system_rate(out, mux_rate)
        patched[i] = bytes(out)
    return patched, rate, mux_rate


def rewrite_pack_headers(sectors: Iterable[bytes], rate: Fraction, mux_rate: int) -> bytes:
    out = bytearray()
    for i, sec in enumerate(sectors):
        if len(sec) != SECTOR:
            raise PrepareError("bad sector length while rebuilding SFD")
        if sec[:4] == END_START:
            out.extend(sec)
            continue
        if sec[:4] != PACK_START and sec[:12] != b"\x00" * 12:
            raise PrepareError("unexpected sector while rebuilding SFD")
        rebuilt = bytearray(pack_header(i, rate, mux_rate) + sec[12:])
        patch_system_rate(rebuilt, mux_rate)
        out.extend(rebuilt)
    return bytes(out)


def build_three_stream_korean_movie(source: bytes, *, bdop: bool) -> bytes:
    header, rate, mux_rate = make_three_stream_header(source, bdop)
    source_sectors = split_sectors(source)
    output: list[bytes] = list(header)
    end_sector: bytes | None = None

    c1_bodies = collect_bodies(source, C1)
    c2_bodies = collect_bodies(source, C2)

    if bdop:
        for sec in source_sectors[4:]:
            if sec[:4] == END_START:
                end_sector = sec
                continue
            output.append(sec)
            if sid(sec) == C2:
                output.append(b"\x00" * 12 + patch_body_sid(sec[12:], C3))
    else:
        occurrences = {C1: 0, C2: 0}
        emitted: set[int] = set()
        max_audio_packets = max(len(c1_bodies), len(c2_bodies))

        for sec in source_sectors[4:]:
            if sec[:4] == END_START:
                end_sector = sec
                continue
            stream_id = sid(sec)
            if stream_id not in (C1, C2):
                output.append(sec)
                continue

            index = occurrences[stream_id]
            occurrences[stream_id] += 1
            if index in emitted:
                continue
            emitted.add(index)

            if index < len(c1_bodies):
                output.append(b"\x00" * 12 + c1_bodies[index])
            if index < len(c2_bodies):
                output.append(b"\x00" * 12 + c2_bodies[index])
            if index < len(c1_bodies):
                output.append(b"\x00" * 12 + patch_body_sid(c1_bodies[index], C3))

        expected_indices = set(range(max_audio_packets))
        if emitted != expected_indices:
            missing = sorted(expected_indices - emitted)[:20]
            raise PrepareError(f"missing audio packet-group anchors: {missing}")
    if end_sector is None:
        end_sector = END_START + b"\xFF" * (SECTOR - 4)
    output.append(end_sector)

    rebuilt = rewrite_pack_headers(output, rate, mux_rate)
    verify_transformation(source, rebuilt, bdop=bdop)
    return rebuilt


def body_sequence(data: bytes, *, drop_c3: bool) -> list[bytes]:
    result: list[bytes] = []
    for sec in split_sectors(data)[4:]:
        if sec[:4] == END_START:
            continue
        if drop_c3 and sid(sec) == C3:
            continue
        result.append(sec[12:])
    return result


def verify_transformation(source: bytes, rebuilt: bytes, *, bdop: bool) -> None:
    _, src_audio = stream_counts(source)
    _, dst_audio = stream_counts(rebuilt)
    if src_audio != 2 or dst_audio != 3:
        raise PrepareError(
            f"stream-count verification failed: source={src_audio}, output={dst_audio}"
        )
    # Strong invariant used during the original Korean-voice validation:
    # hiding synthesized C3 must reproduce the Korean retail media bodies.
    if body_sequence(source, drop_c3=False) != body_sequence(rebuilt, drop_c3=True):
        raise PrepareError("Korean C1/C2/E0 body-sequence verification failed")
    c2_source = collect_bodies(source, C2)
    c2_output = collect_bodies(rebuilt, C2)
    if c2_source != c2_output:
        raise PrepareError("Korean C2 payload changed during conversion")
    c3_output = collect_bodies(rebuilt, C3)
    expected = collect_bodies(source, C2 if bdop else C1)
    expected = [patch_body_sid(body, C3) for body in expected]
    if c3_output != expected:
        raise PrepareError("synthesized C3 payload verification failed")


def parse_boot_codes(path: Path) -> dict[str, list[str]]:
    text = path.read_text(encoding="ascii", errors="replace")
    out: dict[str, list[str]] = {}
    for line in text.splitlines():
        match = re.match(r"^\[([^]]+)\]\s*(.*)$", line.strip())
        if not match:
            continue
        key = match.group(1).lower()
        values = [v.lower() for v in match.group(2).split()]
        out[key] = values
    return out


def patch_toml(path: Path, values: dict[str, str]) -> None:
    if path.exists():
        text = path.read_text(encoding="utf-8", errors="replace")
    else:
        text = "# Generated by prepare_korean_data.py\n"
    for key, value in values.items():
        line = f"{key} = {value}"
        pattern = re.compile(rf"(?m)^{re.escape(key)}\s*=.*$")
        if pattern.search(text):
            text = pattern.sub(line, text)
        else:
            if text and not text.endswith("\n"):
                text += "\n"
            text += line + "\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(text, encoding="utf-8", newline="\n")
    os.replace(tmp, path)


@dataclass
class MovieResult:
    path: str
    action: str
    before_size: int
    after_size: int
    before_sha256: str
    after_sha256: str


def relative_display(path: Path, root: Path) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return str(path)


def prepare_game(game_dir: Path, *, apply: bool, force: bool) -> dict:
    game_dir = game_dir.resolve()
    boot = game_dir / "bd_boot.ini"
    if not boot.is_file():
        raise PrepareError(f"bd_boot.ini not found under {game_dir}")

    codes = parse_boot_codes(boot)
    language_codes = codes.get("language", [])
    voice_codes = codes.get("voice", [])
    is_korean = "kr" in language_codes and "kr" in voice_codes
    if not is_korean and not force:
        raise PrepareError(
            "This does not look like Korean retail data: KR is not present in both "
            "[Language] and [Voice]. Use --force only if you know this data set is Korean."
        )
    try:
        kr_voice_index = voice_codes.index("kr") + 1
    except ValueError:
        kr_voice_index = 2

    sfds = sorted(game_dir.rglob("*.sfd"), key=lambda p: p.as_posix().lower())
    results: list[MovieResult] = []
    changed = 0
    already = 0
    untouched = 0

    for path in sfds:
        source = path.read_bytes()
        try:
            _, audio_count = stream_counts(source)
        except PrepareError:
            results.append(
                MovieResult(
                    relative_display(path, game_dir),
                    "not-sofdec",
                    len(source),
                    len(source),
                    sha256_bytes(source),
                    sha256_bytes(source),
                )
            )
            continue

        before_sha = sha256_bytes(source)
        if audio_count == 3:
            already += 1
            results.append(
                MovieResult(
                    relative_display(path, game_dir),
                    "already-3-track",
                    len(source),
                    len(source),
                    before_sha,
                    before_sha,
                )
            )
            continue
        if audio_count != 2:
            untouched += 1
            results.append(
                MovieResult(
                    relative_display(path, game_dir),
                    f"unchanged-{audio_count}-audio",
                    len(source),
                    len(source),
                    before_sha,
                    before_sha,
                )
            )
            continue

        bdop = path.name.lower() == "bdopdemo.sfd"
        rebuilt = build_three_stream_korean_movie(source, bdop=bdop)
        after_sha = sha256_bytes(rebuilt)
        changed += 1
        action = "would-convert" if not apply else "converted"

        if apply:
            tmp = path.with_suffix(path.suffix + ".reblue-kr.tmp")
            tmp.write_bytes(rebuilt)
            if sha256_file(tmp) != after_sha:
                tmp.unlink(missing_ok=True)
                raise PrepareError(f"post-write hash mismatch: {path}")
            os.replace(tmp, path)

        results.append(
            MovieResult(
                relative_display(path, game_dir),
                action,
                len(source),
                len(rebuilt),
                before_sha,
                after_sha,
            )
        )
        print(
            f"[{changed:02d}] {action:13} {relative_display(path, game_dir)} "
            f"{len(source):,} -> {len(rebuilt):,}"
        )

    if apply:
        # Android launcher reads <install-root>/reblue.toml.  Desktop re:Blue
        # normally uses profiles/default/reblue.toml.  Update both when useful.
        install_root = game_dir.parent
        config_values = {
            "user_language": "7",
            "bd_language": '"kr"',
            "bd_opt_voice_type": str(kr_voice_index),
            "bd_opt_subtitles": "1",
        }
        patch_toml(install_root / "reblue.toml", config_values)
        desktop_cfg = install_root / "profiles" / "default" / "reblue.toml"
        if desktop_cfg.exists() or (install_root / "profiles").is_dir():
            patch_toml(desktop_cfg, config_values)

    manifest = {
        "format": 1,
        "tool": "prepare_korean_data.py",
        "game_dir": str(game_dir),
        "apply": apply,
        "language_codes": language_codes,
        "voice_codes": voice_codes,
        "korean_voice_index": kr_voice_index,
        "sfd_total": len(sfds),
        "converted_or_would_convert": changed,
        "already_3_track": already,
        "unchanged": untouched,
        "movies": [r.__dict__ for r in results],
    }
    if apply:
        manifest_path = game_dir.parent / "reblue_korean_prepare_manifest.json"
        tmp = manifest_path.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
        os.replace(tmp, manifest_path)
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Prepare a re:Blue game directory extracted from the Korean retail "
            "Blue Dragon discs. Dry-run unless --apply is supplied."
        )
    )
    parser.add_argument("game_dir", type=Path, help="folder containing bd_boot.ini and movie/")
    parser.add_argument(
        "--apply",
        action="store_true",
        help="atomically replace the 2-track Korean regional SFDs in place",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="bypass KR [Language]/[Voice] sanity check",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="print the final summary as JSON",
    )
    args = parser.parse_args()

    try:
        manifest = prepare_game(args.game_dir, apply=args.apply, force=args.force)
    except (OSError, PrepareError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    mode = "APPLIED" if args.apply else "DRY RUN"
    print(
        f"\n{mode}: SFD={manifest['sfd_total']} "
        f"convert={manifest['converted_or_would_convert']} "
        f"already3={manifest['already_3_track']} "
        f"unchanged={manifest['unchanged']} "
        f"KR voice index={manifest['korean_voice_index']}"
    )
    if not args.apply:
        print("No game files were changed. Re-run with --apply after reviewing the result.")
    if args.json:
        print(json.dumps(manifest, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
