#!/usr/bin/env python3
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


ANSI_RE = re.compile(r"\x1B\[[0-?]*[ -/]*[@-~]")


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text)


def run_checked(cmd: list[str], cwd: Path, env: dict[str, str] | None = None) -> str:
    print("[recomp_build] >", " ".join(str(c) for c in cmd))
    completed = subprocess.run(
        cmd,
        cwd=str(cwd),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=True,
    )
    output = completed.stdout or ""
    if output:
        print(output, end="" if output.endswith("\n") else "\n")
    return output


def parse_targetfile(show_output: str) -> str:
    clean = strip_ansi(show_output)
    for line in clean.splitlines():
        if "targetfile" in line:
            parts = line.split(":", 1)
            if len(parts) == 2:
                value = parts[1].strip()
                if value:
                    return value
    raise RuntimeError("Unable to locate targetfile in `xmake show` output.")


def sanitize_name(name: str) -> str:
    lowered = name.lower()
    return "".join(ch if ch.isalnum() else "_" for ch in lowered).strip("_") or "recompiled"


def ensure_path(base_dir: Path, maybe_relative: str) -> Path:
    p = Path(maybe_relative)
    if p.is_absolute():
        return p
    return (base_dir / p).resolve()


def parse_trace_ranges(path: Path) -> list[tuple[int, int]]:
    if not path.is_file():
        return []
    ranges: list[tuple[int, int]] = []
    for raw in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "-" in line:
            a, b = line.split("-", 1)
            try:
                start = int(a, 16)
                end = int(b, 16)
            except ValueError:
                continue
        else:
            try:
                start = int(line, 16)
            except ValueError:
                continue
            end = start
        if end < start:
            start, end = end, start
        ranges.append((start, end))
    return ranges


def merge_trace_ranges(ranges: list[tuple[int, int]]) -> list[tuple[int, int]]:
    if not ranges:
        return []
    ordered = sorted(ranges, key=lambda item: (item[0], item[1]))
    merged: list[list[int]] = [[ordered[0][0], ordered[0][1]]]
    for start, end in ordered[1:]:
        tail = merged[-1]
        if start <= (tail[1] + 1):
            if end > tail[1]:
                tail[1] = end
        else:
            merged.append([start, end])
    return [(start, end) for start, end in merged]


def write_trace_ranges(path: Path, ranges: list[tuple[int, int]], sources: list[str]) -> int:
    path.parent.mkdir(parents=True, exist_ok=True)
    unique = sum((end - start + 1) for start, end in ranges)
    with path.open("w", encoding="utf-8", newline="\n") as f:
        f.write("# GB ROM read trace\n")
        if sources:
            f.write(f"# merged_sources={','.join(sources)}\n")
        f.write(f"# unique_reads={unique}\n")
        for start, end in ranges:
            if start == end:
                f.write(f"{start:06X}\n")
            else:
                f.write(f"{start:06X}-{end:06X}\n")
    return unique


def merge_trace_files(dest: Path, sources: list[Path], include_dest: bool = True) -> tuple[int, int]:
    ranges: list[tuple[int, int]] = []
    labels: list[str] = []
    if include_dest and dest.is_file():
        ranges.extend(parse_trace_ranges(dest))
        labels.append(dest.as_posix())

    for src in sources:
        if not src.is_file():
            continue
        ranges.extend(parse_trace_ranges(src))
        labels.append(src.as_posix())

    merged = merge_trace_ranges(ranges)
    if not merged:
        return 0, 0
    unique = write_trace_ranges(dest, merged, labels)
    return len(merged), unique


def parse_missing_keys_file(path: Path) -> set[tuple[str, int, int]]:
    out: set[tuple[str, int, int]] = set()
    if not path.is_file():
        return out
    for raw in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        kind = parts[0].upper()
        try:
            key = int(parts[1], 16)
        except ValueError:
            continue
        addr = key & 0xFFFF
        bank = (key >> 16) & 0xFFFF
        out.add((kind, bank, addr))
    return out


def parse_seed_keys_file(path: Path) -> set[tuple[int, int]]:
    out: set[tuple[int, int]] = set()
    if not path.is_file():
        return out
    for raw in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        try:
            if ":" in line:
                btxt, atxt = line.split(":", 1)
                out.add((int(btxt, 16), int(atxt, 16)))
            else:
                key = int(line, 16)
                out.add(((key >> 16) & 0xFFFF, key & 0xFFFF))
        except ValueError:
            continue
    return out


def write_seed_keys_file(path: Path, items: set[tuple[int, int]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as f:
        f.write("# bank:addr seeds generated from recompiled missing ROM fallbacks\n")
        for bank, addr in sorted(items):
            if addr >= 0x8000:
                continue
            f.write(f"{bank:04X}:{addr:04X}\n")


FUNC_JSON_RE = re.compile(r"^func_b([0-9]+)_([0-9A-Fa-f]{4})\.json$")
FUNC_INPUTS_JSON_RE = re.compile(r"^func_b([0-9]+)_([0-9A-Fa-f]{4})\.inputs\.json$")


def parse_dump_entry_keys(input_dir: Path) -> set[tuple[int, int]]:
    out: set[tuple[int, int]] = set()
    if not input_dir.is_dir():
        return out
    for p in input_dir.glob("func_b*_*.json"):
        m = FUNC_JSON_RE.match(p.name)
        if not m:
            continue
        try:
            bank = int(m.group(1), 10)
            addr = int(m.group(2), 16)
        except ValueError:
            continue
        if addr >= 0x8000:
            continue
        out.add((bank & 0xFFFF, addr & 0xFFFF))
    return out


def merge_dump_entries_into_seeds(input_dir: Path) -> tuple[int, int]:
    seed_keys_file = input_dir / "recompiled_seed_keys.txt"
    seeds = parse_seed_keys_file(seed_keys_file)
    before = len(seeds)
    seeds.update(parse_dump_entry_keys(input_dir))
    write_seed_keys_file(seed_keys_file, seeds)
    return len(seeds) - before, len(seeds)


def merge_player_missing_rom_keys_into_seeds(input_dir: Path) -> tuple[int, int]:
    missing_keys_file = input_dir / "recompiled_missing_keys.txt"
    seed_keys_file = input_dir / "recompiled_seed_keys.txt"
    missing = parse_missing_keys_file(missing_keys_file)
    if not missing:
        return 0, len(parse_seed_keys_file(seed_keys_file))

    seeds = parse_seed_keys_file(seed_keys_file)
    before = len(seeds)
    for kind, bank, addr in missing:
        if kind != "ROM":
            continue
        if addr >= 0x8000:
            continue
        seeds.add((bank, addr))
    write_seed_keys_file(seed_keys_file, seeds)
    return len(seeds) - before, len(seeds)


def parse_input_profile_rom_offsets(input_dir: Path) -> set[int]:
    offsets: set[int] = set()
    if not input_dir.is_dir():
        return offsets
    for p in input_dir.glob("func_b*_*.inputs.json"):
        if not FUNC_INPUTS_JSON_RE.match(p.name):
            continue
        try:
            data = json.loads(p.read_text(encoding="utf-8", errors="ignore"))
        except Exception:
            continue
        reads = data.get("reads")
        if not isinstance(reads, list):
            continue
        for item in reads:
            if not isinstance(item, dict):
                continue
            rom_offsets = item.get("rom_offsets")
            if not isinstance(rom_offsets, list):
                continue
            for raw in rom_offsets:
                try:
                    if isinstance(raw, str):
                        off = int(raw, 0)
                    else:
                        off = int(raw)
                except (TypeError, ValueError):
                    continue
                if off >= 0:
                    offsets.add(off)
    return offsets


def merge_input_profile_rom_reads_into_trace(input_dir: Path, trace_path: Path) -> tuple[int, int]:
    profile_offsets = parse_input_profile_rom_offsets(input_dir)
    if not profile_offsets:
        if trace_path.is_file():
            merged = merge_trace_ranges(parse_trace_ranges(trace_path))
            unique = sum((b - a + 1) for a, b in merged)
            return 0, unique
        return 0, 0

    existing_ranges = parse_trace_ranges(trace_path) if trace_path.is_file() else []
    existing_offsets: set[int] = set()
    for start, end in existing_ranges:
        for off in range(start, end + 1):
            existing_offsets.add(off)
    before = len(existing_offsets)
    existing_offsets.update(profile_offsets)
    if len(existing_offsets) != before or not trace_path.is_file():
        merged = merge_offsets_to_ranges(existing_offsets)
        unique = write_trace_ranges(trace_path, merged, [trace_path.as_posix(), "inputs-profile"])
        return len(existing_offsets) - before, unique
    return 0, len(existing_offsets)


def merge_offsets_to_ranges(offsets: set[int]) -> list[tuple[int, int]]:
    if not offsets:
        return []
    ordered = sorted(offsets)
    out: list[tuple[int, int]] = []
    start = ordered[0]
    prev = ordered[0]
    for off in ordered[1:]:
        if off == prev + 1:
            prev = off
            continue
        out.append((start, prev))
        start = prev = off
    out.append((start, prev))
    return out


def copy_exe_with_lock_fallback(src: Path, dst: Path) -> Path:
    dst.parent.mkdir(parents=True, exist_ok=True)
    try:
        shutil.copy2(src, dst)
        return dst
    except PermissionError:
        alt = dst.with_name(dst.stem + "_new" + dst.suffix)
        shutil.copy2(src, alt)
        print(
            f"[recomp_build] warning: destination locked, copied to {alt} "
            f"(close {dst.name} and replace it manually)"
        )
        return alt


def has_dump_json(input_dir: Path) -> bool:
    for p in input_dir.glob("func_b*_*.json"):
        if FUNC_JSON_RE.match(p.name):
            return True
    return False


def count_dump_json(input_dir: Path) -> int:
    return sum(1 for p in input_dir.glob("func_b*_*.json") if FUNC_JSON_RE.match(p.name))


def maybe_prefer_rom_stem_dir(repo_root: Path, current_input_dir: Path, rom_path: str) -> Path:
    stem_dir = (repo_root / Path(rom_path).stem).resolve()
    if not has_dump_json(stem_dir):
        return current_input_dir

    if stem_dir == current_input_dir:
        return current_input_dir

    current_count = count_dump_json(current_input_dir)
    stem_count = count_dump_json(stem_dir)
    if stem_count > current_count:
        print(
            f"[recomp_build] input-dir switched to ROM stem dump: {stem_dir} "
            f"(functions={stem_count}, previous={current_count})"
        )
        return stem_dir
    return current_input_dir


def resolve_input_dir(base_dir: Path, raw_input: str) -> Path:
    direct = ensure_path(base_dir, raw_input)
    if direct.is_dir():
        if has_dump_json(direct):
            return direct
        raise SystemExit(f"Input directory found but contains no func_b*_*.json: {direct}")

    wanted_name = Path(raw_input).name.lower()
    candidates: list[Path] = []
    build_dir = base_dir / "build"
    if build_dir.is_dir():
        for p in build_dir.rglob("*"):
            if not p.is_dir():
                continue
            if p.name.lower() != wanted_name:
                continue
            if has_dump_json(p):
                candidates.append(p.resolve())

    if len(candidates) == 1:
        print(f"[recomp_build] input-dir auto-resolved: {candidates[0]}")
        return candidates[0]
    if len(candidates) > 1:
        joined = "\n".join(f"  - {p}" for p in candidates)
        raise SystemExit(
            f"Multiple dump directories match '{raw_input}'. Use --input-dir with an exact path:\n{joined}"
        )

    raise SystemExit(f"Input directory not found: {direct}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate and build a recompiled executable from JSON dumps.")
    parser.add_argument("--input-dir", default="", help="Directory containing func_bXXX_YYYY.json files (optional with --rom)")
    parser.add_argument("--rom", default="", help="Default ROM path embedded in generated runner")
    parser.add_argument("--mode", default="release", choices=["debug", "release"], help="xmake build mode")
    parser.add_argument("--target", default="GB_Recompiled", help="xmake target name to build")
    parser.add_argument("--generated-c", default="generated/recompiled_main.c", help="Generated C output path")
    parser.add_argument("--shard-funcs", default="256", help="Approx number of functions per generated shard C file")
    parser.add_argument("--output-exe", default="", help="Optional output executable path")
    parser.add_argument("--read-trace", default="", help="Optional ROM read trace file (ranges from GB_ROM_READ_TRACE_PATH)")
    parser.add_argument(
        "--merge-read-trace",
        action="append",
        default=[],
        help="Additional ROM read trace file(s) to merge into the selected read trace before codegen. Can be repeated.",
    )
    parser.add_argument("--auto-static", action="store_true", help="Run GB_Native_Hook --static-recomp first and use the generated dump directory")
    parser.add_argument("--no-auto-static", action="store_true", help="Disable implicit static dump refresh when embedding ROM assets")
    parser.add_argument("--auto-trace", action="store_true", help="Run GB_Native_Hook auto-recomp first to generate/refresh ROM read trace")
    parser.add_argument("--trace-total-steps", default="2000000", help="Total auto-recomp steps used during auto-trace")
    parser.add_argument("--trace-path-steps", default="20000", help="Per-path budget used during auto-trace")
    parser.add_argument("--trace-max-clones", default="24", help="Max clone paths used during auto-trace")
    parser.add_argument("--no-embed-assets", action="store_true", help="Disable ROM asset embedding and keep external ROM loading")
    parser.add_argument("--embed-full-rom", action="store_true", help="Embed the full ROM image instead of sparse assets (testing/debug mode)")
    parser.add_argument("--refine-missing", action="store_true", help="Iteratively run recompiled exe and regenerate from missing ROM fallback PCs")
    parser.add_argument("--refine-rounds", default="3", help="Max refinement rounds after first build")
    parser.add_argument("--refine-run-instr", default="300000", help="Headless instruction budget per refinement round")
    parser.add_argument(
        "--mingw",
        default="",
        help="Optional MinGW SDK directory passed to xmake (Windows only), e.g. C:/MinGW",
    )
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    input_dir: Path | None = None
    input_dir_hint: Path | None = None
    if args.input_dir:
        try:
            input_dir = resolve_input_dir(repo_root, args.input_dir)
        except SystemExit as exc:
            hinted = ensure_path(repo_root, args.input_dir)
            # Allow a first-time "from scratch" build: the static pass will create
            # the dump directory (usually named after the ROM stem).
            if hinted.exists():
                raise
            input_dir_hint = hinted
            print(
                f"[recomp_build] input-dir will be created by static pass: {hinted} "
                f"({exc})"
            )

    if args.rom:
        default_rom = str(ensure_path(repo_root, args.rom))
    else:
        if input_dir is None:
            raise SystemExit("Missing required --rom (or provide --input-dir with a matching <name>.gb next to the repo root).")
        guessed = (repo_root / f"{input_dir.name}.gb").resolve()
        default_rom = str(guessed) if guessed.is_file() else f"{input_dir.name}.gb"
    auto_static_enabled = args.auto_static or (not args.no_auto_static and not args.no_embed_assets) or (input_dir is None)
    if input_dir is not None:
        input_dir = maybe_prefer_rom_stem_dir(repo_root, input_dir, default_rom)
    generated_c = ensure_path(repo_root, args.generated_c)
    seed_dir_hint = (input_dir if input_dir is not None else (repo_root / Path(default_rom).stem).resolve())

    generated_c.parent.mkdir(parents=True, exist_ok=True)
    configure_cmd = ["xmake", "f", "-m", args.mode]
    if os.name == "nt":
        configure_cmd.extend(["-p", "mingw", "-a", "x86_64"])
        mingw_dir = args.mingw or os.environ.get("MINGW_HOME", "") or os.environ.get("MINGW", "")
        if mingw_dir:
            configure_cmd.append(f"--mingw={mingw_dir}")
    run_checked(configure_cmd, repo_root)

    host_target: Path | None = None

    def ensure_host_target() -> Path:
        nonlocal host_target
        if host_target and host_target.is_file():
            return host_target
        run_checked(["xmake", "build", "GB_Native_Hook"], repo_root)
        host_show = run_checked(["xmake", "show", "-t", "GB_Native_Hook"], repo_root)
        host_target_raw = parse_targetfile(host_show)
        host_target = ensure_path(repo_root, host_target_raw)
        if not host_target.is_file():
            raise SystemExit(f"GB_Native_Hook executable not found: {host_target}")
        return host_target

    rom_file = Path(default_rom)
    if (auto_static_enabled or not args.no_embed_assets) and not rom_file.is_file():
        raise SystemExit(
            "ROM file is missing.\n"
            f"Resolved ROM path: {rom_file}\n"
            "Use --rom <path-to-rom.gb>."
        )

    if seed_dir_hint and seed_dir_hint.is_dir():
        added_from_dumps, total_seed_count = merge_dump_entries_into_seeds(seed_dir_hint)
        if added_from_dumps > 0:
            print(
                f"[recomp_build] seeded static pass from existing dumps -> {seed_dir_hint / 'recompiled_seed_keys.txt'} "
                f"(new={added_from_dumps} total={total_seed_count})"
            )
        new_seed_count, total_seed_count = merge_player_missing_rom_keys_into_seeds(seed_dir_hint)
        if new_seed_count > 0:
            print(
                f"[recomp_build] imported player DYN-ROM coverage -> {seed_dir_hint / 'recompiled_seed_keys.txt'} "
                f"(new={new_seed_count} total={total_seed_count})"
            )

    if auto_static_enabled:
        host_exe = ensure_host_target()
        static_env = os.environ.copy()
        static_env["GB_STATIC_RECOMP"] = "1"
        static_env["GB_AUTO_RECOMP_DISPLAY"] = "0"
        static_env["GB_AUTO_RECOMP_VERBOSE"] = "0"
        static_seed_file = (seed_dir_hint / "recompiled_seed_keys.txt") if seed_dir_hint else None
        if static_seed_file and static_seed_file.is_file():
            static_env["GB_RECOMP_STATIC_EXTRA_ENTRIES_FILE"] = str(static_seed_file)
        run_checked([str(host_exe), default_rom, "--static-recomp", "--no-display"], repo_root, env=static_env)

        static_dir = (repo_root / Path(default_rom).stem).resolve()
        if has_dump_json(static_dir):
            if input_dir is None:
                input_dir = static_dir
                print(f"[recomp_build] using static dump dir: {input_dir} (functions={count_dump_json(input_dir)})")
            else:
                old_count = count_dump_json(input_dir)
                new_count = count_dump_json(static_dir)
                if new_count >= old_count:
                    input_dir = static_dir
                    print(f"[recomp_build] using static dump dir: {input_dir} (functions={new_count})")
        else:
            if args.auto_static or input_dir is None:
                raise SystemExit(f"Auto-static did not generate dump json in: {static_dir}")
            print(f"[recomp_build] warning: implicit auto-static produced no dumps in: {static_dir}")

    if input_dir is None:
        raise SystemExit("No input dump directory available after static pass. Check ROM path and static recomp output.")

    read_trace_path = ""
    if args.read_trace:
        read_trace_path = str(ensure_path(repo_root, args.read_trace))
    else:
        candidate_trace = input_dir / "rom_reads.txt"
        if candidate_trace.is_file():
            read_trace_path = str(candidate_trace)

    merge_sources: list[Path] = []
    for raw in args.merge_read_trace:
        if not raw:
            continue
        merge_sources.append(ensure_path(repo_root, raw))
    if merge_sources:
        missing = [str(p) for p in merge_sources if not p.is_file()]
        if missing:
            raise SystemExit("Merge read trace file(s) not found:\n" + "\n".join(missing))
        if not read_trace_path:
            read_trace_path = str((input_dir / "rom_reads.txt").resolve())
        merged_ranges, merged_unique = merge_trace_files(Path(read_trace_path), merge_sources, include_dest=True)
        print(
            f"[recomp_build] merged read traces -> {read_trace_path} "
            f"(ranges={merged_ranges} unique_reads={merged_unique})"
        )

    auto_trace_enabled = args.auto_trace
    if not args.no_embed_assets and not auto_trace_enabled and not args.read_trace:
        auto_trace_enabled = True
        print("[recomp_build] auto-trace enabled (refresh default trace)")

    if not args.no_embed_assets and auto_trace_enabled:
        if not read_trace_path:
            read_trace_path = str(input_dir / "rom_reads.txt")
        read_trace_file = Path(read_trace_path)
        read_trace_file.parent.mkdir(parents=True, exist_ok=True)
        pre_auto_ranges = parse_trace_ranges(read_trace_file) if read_trace_file.is_file() else []

        host_exe = ensure_host_target()
        trace_env = os.environ.copy()
        trace_env["GB_ROM_READ_TRACE_PATH"] = str(read_trace_file)
        trace_env["GB_AUTO_RECOMP"] = "1"
        trace_env["GB_AUTO_RECOMP_TOTAL_STEPS"] = str(args.trace_total_steps)
        trace_env["GB_AUTO_RECOMP_PATH_STEPS"] = str(args.trace_path_steps)
        trace_env["GB_AUTO_RECOMP_MAX_CLONES"] = str(args.trace_max_clones)
        trace_env["GB_AUTO_RECOMP_DISPLAY"] = "0"
        trace_env["GB_AUTO_RECOMP_VERBOSE"] = "0"
        trace_env["GB_RECOMP_PROBE"] = "1"
        # Auto-recomp exploration may jump into RAM/VRAM/data on some games (especially CGB).
        # Do not abort the whole build on invalid opcodes during this non-authoritative trace pass.
        trace_env["GB_ILLEGAL_OPCODE_NOP"] = "1"
        trace_env["GB_AUTO_RECOMP_SNAPSHOT_DIR"] = str((input_dir / "snapshots").resolve())
        run_checked([str(host_exe), default_rom, "--auto-recomp", "--no-display"], repo_root, env=trace_env)
        if pre_auto_ranges:
            post_auto_ranges = parse_trace_ranges(read_trace_file)
            merged = merge_trace_ranges(pre_auto_ranges + post_auto_ranges)
            if merged != post_auto_ranges:
                merged_unique = write_trace_ranges(
                    read_trace_file,
                    merged,
                    [read_trace_file.as_posix(), "auto-trace"],
                )
                print(
                    f"[recomp_build] preserved pre-existing read trace coverage "
                    f"(ranges={len(merged)} unique_reads={merged_unique})"
                )

    if not args.no_embed_assets:
        if not read_trace_path:
            read_trace_path = str((input_dir / "rom_reads.txt").resolve())
        added_from_inputs, trace_unique = merge_input_profile_rom_reads_into_trace(input_dir, Path(read_trace_path))
        if added_from_inputs > 0:
            print(
                f"[recomp_build] imported input-profile ROM reads -> {read_trace_path} "
                f"(new={added_from_inputs} unique_reads={trace_unique})"
            )

    codegen_cmd = [
        sys.executable,
        "tools/recomp_codegen.py",
        "--input-dir",
        str(input_dir),
        "--output",
        str(generated_c),
        "--default-rom",
        default_rom,
        "--shard-funcs",
        str(args.shard_funcs),
    ]

    if not args.no_embed_assets:
        codegen_cmd.extend(["--embed-rom", str(rom_file)])
        if args.embed_full_rom:
            codegen_cmd.append("--embed-full-rom")
        if read_trace_path:
            if not Path(read_trace_path).is_file():
                raise SystemExit(f"Read trace file not found: {read_trace_path}")
            codegen_cmd.extend(["--read-trace", read_trace_path])

    run_checked(codegen_cmd, repo_root)
    run_checked(["xmake", "build" , "-j4", args.target], repo_root)
    show_out = run_checked(["xmake", "show", "-t", args.target], repo_root)

    targetfile_raw = parse_targetfile(show_out)
    targetfile = ensure_path(repo_root, targetfile_raw)
    if not targetfile.is_file():
        raise SystemExit(f"Built executable not found: {targetfile}")

    if args.output_exe:
        output_exe = ensure_path(repo_root, args.output_exe)
    else:
        suffix = ".exe" if os.name == "nt" else ""
        exe_name = f"{sanitize_name(input_dir.name)}_recompiled{suffix}"
        output_exe = input_dir / exe_name

    output_exe.parent.mkdir(parents=True, exist_ok=True)
    copy_exe_with_lock_fallback(targetfile, output_exe)
    print(f"[recomp_build] built={targetfile}")
    print(f"[recomp_build] copied={output_exe}")

    refine_enabled = bool(args.refine_missing)
    try:
        refine_rounds = max(0, int(args.refine_rounds))
    except ValueError:
        refine_rounds = 3
    try:
        refine_run_instr = max(1, int(args.refine_run_instr))
    except ValueError:
        refine_run_instr = 3000000

    if not refine_enabled or refine_rounds <= 0:
        return 0

    missing_keys_file = input_dir / "recompiled_missing_keys.txt"
    seed_keys_file = input_dir / "recompiled_seed_keys.txt"
    known_seed_keys = parse_seed_keys_file(seed_keys_file)

    print(
        f"[recomp_build] refine-missing enabled rounds={refine_rounds} "
        f"run_instr={refine_run_instr} seeds={len(known_seed_keys)}"
    )

    for round_idx in range(1, refine_rounds + 1):
        if missing_keys_file.exists():
            missing_keys_file.unlink()

        run_env = os.environ.copy()
        run_env["GB_RECOMP_NO_DISPLAY"] = "1"
        run_env["GB_RECOMP_MAX_INSTR"] = str(refine_run_instr)
        run_env["GB_RECOMP_MISSING_KEYS_PATH"] = str(missing_keys_file)
        run_cmd = [str(output_exe)]
        if args.no_embed_assets:
            run_cmd.append(default_rom)

        print(f"[recomp_build] refine round {round_idx}/{refine_rounds}: run recompiled")
        completed = subprocess.run(
            run_cmd,
            cwd=str(repo_root),
            env=run_env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        run_output = completed.stdout or ""
        if run_output:
            print(run_output, end="" if run_output.endswith("\n") else "\n")

        missing = parse_missing_keys_file(missing_keys_file)
        new_rom_keys = {
            (bank, addr)
            for (kind, bank, addr) in missing
            if kind == "ROM" and addr < 0x8000 and (bank, addr) not in known_seed_keys
        }

        dyn_ram_hits = sum(1 for (kind, _, addr) in missing if kind == "RAM" and addr >= 0x8000)
        if not new_rom_keys:
            print(
                f"[recomp_build] refine round {round_idx}: no new ROM missing keys "
                f"(dyn_ram_hits={dyn_ram_hits})"
            )
            break

        known_seed_keys.update(new_rom_keys)
        write_seed_keys_file(seed_keys_file, known_seed_keys)
        print(
            f"[recomp_build] refine round {round_idx}: new_rom_keys={len(new_rom_keys)} "
            f"total_seeds={len(known_seed_keys)} seed_file={seed_keys_file}"
        )

        host_exe = ensure_host_target()
        static_env = os.environ.copy()
        static_env["GB_STATIC_RECOMP"] = "1"
        static_env["GB_AUTO_RECOMP_DISPLAY"] = "0"
        static_env["GB_AUTO_RECOMP_VERBOSE"] = "0"
        static_env["GB_RECOMP_STATIC_EXTRA_ENTRIES_FILE"] = str(seed_keys_file)
        run_checked([str(host_exe), default_rom, "--static-recomp", "--no-display"], repo_root, env=static_env)

        static_dir = (repo_root / Path(default_rom).stem).resolve()
        if has_dump_json(static_dir):
            if static_dir != input_dir:
                old_count = count_dump_json(input_dir)
                new_count = count_dump_json(static_dir)
                if new_count >= old_count:
                    input_dir = static_dir
                    print(f"[recomp_build] refine round {round_idx}: using static dump dir {input_dir} (functions={new_count})")

        codegen_cmd = [
            sys.executable,
            "tools/recomp_codegen.py",
            "--input-dir",
            str(input_dir),
            "--output",
            str(generated_c),
            "--default-rom",
            default_rom,
            "--shard-funcs",
            str(args.shard_funcs),
        ]
        if not args.no_embed_assets:
            codegen_cmd.extend(["--embed-rom", str(rom_file)])
            if args.embed_full_rom:
                codegen_cmd.append("--embed-full-rom")
            if read_trace_path:
                if not Path(read_trace_path).is_file():
                    raise SystemExit(f"Read trace file not found: {read_trace_path}")
                codegen_cmd.extend(["--read-trace", read_trace_path])

        run_checked(codegen_cmd, repo_root)
        run_checked(["xmake", "build", "-j4" , args.target], repo_root)
        show_out = run_checked(["xmake", "show", "-t", args.target], repo_root)
        targetfile_raw = parse_targetfile(show_out)
        targetfile = ensure_path(repo_root, targetfile_raw)
        if not targetfile.is_file():
            raise SystemExit(f"Built executable not found: {targetfile}")
        copy_exe_with_lock_fallback(targetfile, output_exe)
        print(f"[recomp_build] refine round {round_idx}: rebuilt and copied {output_exe}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
