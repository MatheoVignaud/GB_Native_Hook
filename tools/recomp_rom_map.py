#!/usr/bin/env python3
import argparse
import binascii
import json
import math
import re
import struct
import zlib
from pathlib import Path


FUNC_JSON_RE = re.compile(r"^func_b([0-9]+)_([0-9A-Fa-f]{4})\.json$")


def is_ref_jp_scan(reason: object) -> bool:
    return isinstance(reason, str) and reason.strip().upper() == "REF_JP_SCAN"


def gb_rom_offset(bank: int, addr: int) -> int | None:
    addr &= 0xFFFF
    bank &= 0xFFFF
    if addr < 0x4000:
        return addr
    if addr < 0x8000:
        return bank * 0x4000 + (addr - 0x4000)
    return None


def parse_insn_bytes(byte_text: str) -> list[int]:
    out: list[int] = []
    for tok in str(byte_text).replace(",", " ").split():
        t = tok.strip()
        if len(t) != 2:
            continue
        try:
            out.append(int(t, 16) & 0xFF)
        except ValueError:
            continue
    return out


def parse_trace_ranges(path: Path) -> list[tuple[int, int]]:
    if not path.is_file():
        return []
    ranges: list[tuple[int, int]] = []
    for raw in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        try:
            if "-" in line:
                a, b = line.split("-", 1)
                start = int(a, 16)
                end = int(b, 16)
            else:
                start = int(line, 16)
                end = start
        except ValueError:
            continue
        if end < start:
            start, end = end, start
        ranges.append((start, end))
    return ranges


def collect_asset_offsets(read_trace_path: Path) -> set[int]:
    offsets: set[int] = set()
    for start, end in parse_trace_ranges(read_trace_path):
        offsets.update(range(start, end + 1))
    return offsets


def collect_code_offsets(input_dir: Path, include_ref_jp_scan: bool) -> tuple[set[int], int, int]:
    code_offsets: set[int] = set()
    dump_count = 0
    skipped_ref_jp = 0
    for p in sorted(input_dir.glob("func_b*_*.json")):
        m = FUNC_JSON_RE.match(p.name)
        if not m:
            continue
        try:
            data = json.loads(p.read_text(encoding="utf-8", errors="ignore"))
        except Exception:
            continue
        if (not include_ref_jp_scan) and is_ref_jp_scan(data.get("reason")):
            skipped_ref_jp += 1
            continue
        dump_count += 1
        for ins in data.get("instructions", []):
            if not isinstance(ins, dict):
                continue
            try:
                addr = int(ins.get("addr", 0)) & 0xFFFF
            except Exception:
                continue
            ibank_raw = ins.get("bank", None)
            try:
                ibank = int(ibank_raw) if ibank_raw is not None else int(data.get("entry", {}).get("bank", 0))
            except Exception:
                ibank = 0
            ibytes = parse_insn_bytes(ins.get("bytes", ""))
            if not ibytes:
                continue
            base = gb_rom_offset(ibank, addr)
            if base is None:
                continue
            for i in range(len(ibytes)):
                code_offsets.add(base + i)
    return code_offsets, dump_count, skipped_ref_jp


def guess_rom_path(input_dir: Path) -> Path | None:
    stem = input_dir.name
    names = [f"{stem}.gb", f"{stem}.gbc", f"{stem}.GB", f"{stem}.GBC"]
    for parent in [input_dir] + list(input_dir.parents):
        for n in names:
            cand = parent / n
            if cand.is_file():
                return cand.resolve()
    return None


def infer_rom_size(input_dir: Path, code_offsets: set[int], asset_offsets: set[int]) -> int:
    rom_path = guess_rom_path(input_dir)
    if rom_path and rom_path.is_file():
        return rom_path.stat().st_size
    max_seen = -1
    if code_offsets:
        max_seen = max(max_seen, max(code_offsets))
    if asset_offsets:
        max_seen = max(max_seen, max(asset_offsets))
    return max_seen + 1 if max_seen >= 0 else 0


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    crc = binascii.crc32(kind)
    crc = binascii.crc32(payload, crc) & 0xFFFFFFFF
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", crc)


def write_png_rgb(path: Path, width: int, height: int, rgb: bytes) -> None:
    if width <= 0 or height <= 0:
        raise ValueError("Invalid image size")
    row_bytes = width * 3
    if len(rgb) != row_bytes * height:
        raise ValueError("RGB buffer size mismatch")

    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter type 0
        start = y * row_bytes
        raw.extend(rgb[start:start + row_bytes])

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    idat = zlib.compress(bytes(raw), level=9)
    png = bytearray()
    png.extend(b"\x89PNG\r\n\x1a\n")
    png.extend(png_chunk(b"IHDR", ihdr))
    png.extend(png_chunk(b"IDAT", idat))
    png.extend(png_chunk(b"IEND", b""))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(png))


def build_image(rom_size: int, code_offsets: set[int], asset_offsets: set[int]) -> tuple[int, int, bytes]:
    side = max(1, math.ceil(math.sqrt(max(1, rom_size))))
    total_pixels = side * side

    # RGB colors
    GREEN = (0, 220, 0)        # recompiled executable bytes
    YELLOW = (240, 220, 0)     # assets / ROM reads
    GRAY = (110, 110, 110)     # untouched / unknown ROM bytes
    WHITE = (255, 255, 255)    # padding

    buf = bytearray(total_pixels * 3)

    for i in range(total_pixels):
        if i >= rom_size:
            color = WHITE
        elif i in code_offsets:
            color = GREEN
        elif i in asset_offsets:
            color = YELLOW
        else:
            color = GRAY
        off = i * 3
        buf[off + 0] = color[0]
        buf[off + 1] = color[1]
        buf[off + 2] = color[2]
    return side, side, bytes(buf)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate a square PNG map of ROM coverage from a game analysis directory."
    )
    parser.add_argument("input_dir", help="Game analysis directory (contains func_b*.json and rom_reads.txt)")
    parser.add_argument("--output", default="", help="Output PNG path (default: <input_dir>/rom_map.png)")
    parser.add_argument("--read-trace", default="", help="Optional read trace path (default: <input_dir>/rom_reads.txt)")
    parser.add_argument("--rom-size", default="", help="Override ROM size in bytes (decimal or 0xHEX)")
    parser.add_argument("--include-ref-jp-scan", action="store_true", help="Include REF_JP_SCAN dumps as recompiled code (unsafe/noisy)")
    args = parser.parse_args()

    input_dir = Path(args.input_dir).resolve()
    if not input_dir.is_dir():
        raise SystemExit(f"Input directory not found: {input_dir}")

    read_trace_path = Path(args.read_trace).resolve() if args.read_trace else (input_dir / "rom_reads.txt")

    code_offsets, dump_count, skipped_ref_jp = collect_code_offsets(input_dir, args.include_ref_jp_scan)
    asset_offsets = collect_asset_offsets(read_trace_path)

    if args.rom_size:
        try:
            rom_size = int(args.rom_size, 0)
        except ValueError:
            raise SystemExit(f"Invalid --rom-size: {args.rom_size}")
    else:
        rom_size = infer_rom_size(input_dir, code_offsets, asset_offsets)

    if rom_size <= 0:
        raise SystemExit("Unable to determine ROM size (no ROM file found and no offsets discovered).")

    # Clamp offsets to ROM size for classification and stats.
    code_offsets = {o for o in code_offsets if 0 <= o < rom_size}
    asset_offsets = {o for o in asset_offsets if 0 <= o < rom_size}

    both = code_offsets & asset_offsets
    asset_only = asset_offsets - code_offsets
    code_only = code_offsets - asset_offsets
    covered = code_offsets | asset_offsets

    width, height, rgb = build_image(rom_size, code_offsets, asset_offsets)

    output = Path(args.output).resolve() if args.output else (input_dir / "rom_map.png")
    write_png_rgb(output, width, height, rgb)

    def pct(n: int) -> str:
        return f"{(100.0 * n / rom_size):.2f}%"

    print(f"[rom_map] input_dir={input_dir}")
    print(f"[rom_map] output={output}")
    print(f"[rom_map] size={rom_size} bytes side={width} pixels padding={(width*height)-rom_size}")
    print(f"[rom_map] dumps_used={dump_count} skipped_ref_jp_scan={skipped_ref_jp}")
    print(
        f"[rom_map] code_bytes={len(code_offsets)} ({pct(len(code_offsets))}) "
        f"asset_bytes={len(asset_offsets)} ({pct(len(asset_offsets))}) overlap={len(both)} ({pct(len(both))})"
    )
    print(
        f"[rom_map] code_only={len(code_only)} ({pct(len(code_only))}) "
        f"asset_only={len(asset_only)} ({pct(len(asset_only))}) "
        f"uncovered={rom_size - len(covered)} ({pct(rom_size - len(covered))})"
    )
    print(f"[rom_map] covered_total={len(covered)} ({pct(len(covered))})")
    if not read_trace_path.is_file():
        print(f"[rom_map] note: read trace not found, assets layer empty ({read_trace_path})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
