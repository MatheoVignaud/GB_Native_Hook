#!/usr/bin/env python3
import argparse
import os
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path


def ensure_path(base: Path, raw: str) -> Path:
    p = Path(raw)
    if p.is_absolute():
        return p
    return (base / p).resolve()


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


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run a recompiled executable with automatic diagnostic logs (stdout + MBC + ROM reads)."
    )
    parser.add_argument("--exe", required=True, help="Path to *_recompiled executable")
    parser.add_argument("--rom", default="", help="Optional ROM path argument passed to executable")
    parser.add_argument("--headless", action="store_true", help="Set GB_RECOMP_NO_DISPLAY=1")
    parser.add_argument("--max-instr", default="", help="Optional GB_RECOMP_MAX_INSTR value")
    parser.add_argument("--max-frames", default="", help="Optional GB_RECOMP_MAX_FRAMES value")
    parser.add_argument("--out-dir", default="diag", help="Base diagnostics directory")
    parser.add_argument(
        "--merge-into",
        default="",
        help="Optional ROM read trace destination to merge this run's rom_reads.txt into.",
    )
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    exe_path = ensure_path(repo_root, args.exe)
    if not exe_path.is_file():
        raise SystemExit(f"Executable not found: {exe_path}")

    rom_path = ""
    if args.rom:
        resolved_rom = ensure_path(repo_root, args.rom)
        rom_path = str(resolved_rom)

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    run_name = f"{exe_path.stem}_{stamp}"
    diag_root = ensure_path(repo_root, args.out_dir)
    run_dir = (diag_root / run_name).resolve()
    run_dir.mkdir(parents=True, exist_ok=True)

    stdout_log = run_dir / "stdout.log"
    mbc_log = run_dir / "mbc_trace.log"
    rom_reads_log = run_dir / "rom_reads.txt"

    env = os.environ.copy()
    env["GB_TRACE_MBC"] = "1"
    env["GB_TRACE_MBC_PATH"] = str(mbc_log)
    env["GB_ROM_READ_TRACE_PATH"] = str(rom_reads_log)
    if args.headless:
        env["GB_RECOMP_NO_DISPLAY"] = "1"
    if args.max_instr:
        env["GB_RECOMP_MAX_INSTR"] = args.max_instr
    if args.max_frames:
        env["GB_RECOMP_MAX_FRAMES"] = args.max_frames

    cmd = [str(exe_path)]
    if rom_path:
        cmd.append(rom_path)

    print(f"[recomp_diag] exe={exe_path}")
    if rom_path:
        print(f"[recomp_diag] rom={rom_path}")
    print(f"[recomp_diag] out={run_dir}")
    print(f"[recomp_diag] cmd={' '.join(cmd)}")

    proc = None
    retcode = 1
    try:
        fallback_mbc = exe_path.parent / "mbc_trace.log"
        if fallback_mbc.is_file():
            try:
                fallback_mbc.unlink()
            except OSError:
                pass

        with open(stdout_log, "w", encoding="utf-8", newline="\n") as out:
            proc = subprocess.Popen(
                cmd,
                cwd=str(exe_path.parent),
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
            )
            assert proc.stdout is not None
            for line in proc.stdout:
                sys.stdout.write(line)
                out.write(line)
            retcode = proc.wait()
    except KeyboardInterrupt:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=2)
        retcode = 130

    if not mbc_log.is_file():
        fallback_mbc = exe_path.parent / "mbc_trace.log"
        if fallback_mbc.is_file():
            try:
                shutil.copy2(fallback_mbc, mbc_log)
            except OSError:
                pass

    print(f"[recomp_diag] exit_code={retcode}")
    print(f"[recomp_diag] stdout={stdout_log}")
    print(f"[recomp_diag] mbc={mbc_log} exists={mbc_log.is_file()}")
    print(f"[recomp_diag] rom_reads={rom_reads_log} exists={rom_reads_log.is_file()}")

    if args.merge_into and rom_reads_log.is_file():
        merge_path = ensure_path(repo_root, args.merge_into)
        merged = merge_trace_ranges(parse_trace_ranges(merge_path) + parse_trace_ranges(rom_reads_log))
        merged_unique = write_trace_ranges(merge_path, merged, [merge_path.as_posix(), rom_reads_log.as_posix()])
        print(
            f"[recomp_diag] merged_rom_reads={merge_path} "
            f"ranges={len(merged)} unique_reads={merged_unique}"
        )

    if retcode != 0 and retcode != 130:
        return retcode
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
