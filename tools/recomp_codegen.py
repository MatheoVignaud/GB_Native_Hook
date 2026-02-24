#!/usr/bin/env python3
import argparse
import glob
import json
import os
import re
from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional, Set, Tuple


@dataclass
class FunctionDesc:
    bank: int
    addr: int
    symbol: str
    keys: List[int]
    insn_bytes: Dict[int, List[int]]
    is_dynram: bool = False
    dynram_hash_by_key: Optional[Dict[int, int]] = None


@dataclass
class InlineHelper:
    name: str
    return_type: str
    params: List[Tuple[str, str]]
    body: List[str]


def make_key(bank: int, addr: int) -> int:
    return ((bank & 0xFFFF) << 16) | (addr & 0xFFFF)


def rom_offset(bank: int, addr: int) -> int:
    return ((bank & 0xFFFF) * 0x4000) + (addr & 0x3FFF)


def parse_insn_bytes(byte_text: str) -> List[int]:
    out: List[int] = []
    for tok in byte_text.replace(",", " ").split():
        t = tok.strip()
        if len(t) != 2:
            continue
        try:
            out.append(int(t, 16) & 0xFF)
        except ValueError:
            continue
    return out


def fnv1a_u32_from_bytes(chunks: Iterable[List[int]]) -> int:
    h = 2166136261
    for chunk in chunks:
        for b in chunk:
            h ^= (b & 0xFF)
            h = (h * 16777619) & 0xFFFFFFFF
    return h


def split_top_level_args(text: str) -> List[str]:
    args: List[str] = []
    depth = 0
    cur: List[str] = []
    for ch in text:
        if ch == "," and depth == 0:
            part = "".join(cur).strip()
            if part:
                args.append(part)
            cur = []
            continue
        if ch in "([{":
            depth += 1
        elif ch in ")]}" and depth > 0:
            depth -= 1
        cur.append(ch)
    tail = "".join(cur).strip()
    if tail:
        args.append(tail)
    return args


def parse_rom_read_trace(path: str) -> Set[int]:
    offsets: Set[int] = set()
    if not path:
        return offsets
    with open(path, "r", encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            token = line.lower().replace("0x", "")
            if "-" in token:
                a, b = token.split("-", 1)
                try:
                    start = int(a, 16)
                    end = int(b, 16)
                except ValueError:
                    continue
                if end < start:
                    start, end = end, start
                for off in range(start, end + 1):
                    offsets.add(off)
            else:
                try:
                    offsets.add(int(token, 16))
                except ValueError:
                    continue
    return offsets


def _is_heuristic_scan_reason(reason: object) -> bool:
    if not isinstance(reason, str):
        return False
    r = reason.strip().upper()
    # REF_CALL_SCAN is generally useful/valid in practice; REF_JP_SCAN is much
    # noisier and has produced bogus ownership (e.g. low-address NOP blobs).
    return r == "REF_JP_SCAN"


def parse_functions(input_dir: str, include_scan_dumps: bool = False) -> Tuple[List[FunctionDesc], Dict[int, int], Set[int]]:
    dump_name_re = re.compile(r"^func_b[0-9]+_[0-9A-Fa-f]{4}\.json$")
    paths = []
    for path in sorted(glob.glob(os.path.join(input_dir, "func_b*_*.json"))):
        name = os.path.basename(path)
        if not dump_name_re.match(name):
            continue
        paths.append(path)
    funcs: List[FunctionDesc] = []
    rom_bytes: Dict[int, int] = {}
    code_banks: Set[int] = set()

    for path in paths:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        if not include_scan_dumps and _is_heuristic_scan_reason(data.get("reason")):
            continue

        entry = data.get("entry", {})
        bank = int(entry.get("bank", 0))
        addr = int(entry.get("addr", 0))
        symbol = f"fn_b{bank:03d}_{addr:04X}"
        if bank >= 0:
            code_banks.add(bank)

        keyset: Set[int] = set()
        insn_by_key: Dict[int, List[int]] = {}
        for ins in data.get("instructions", []):
            iaddr = int(ins.get("addr", 0)) & 0xFFFF
            ibank = ins.get("bank")
            if ibank is None:
                ibank = 0 if iaddr < 0x4000 else bank
            ibank = int(ibank)
            ikey = make_key(ibank, iaddr)
            keyset.add(ikey)
            if iaddr < 0x8000:
                code_banks.add(ibank if iaddr >= 0x4000 else 0)

            base_off = rom_offset(ibank, iaddr)
            ibytes = parse_insn_bytes(str(ins.get("bytes", "")))
            if ibytes:
                insn_by_key.setdefault(ikey, ibytes)
            for idx, b in enumerate(ibytes):
                rom_bytes[base_off + idx] = b

        for block in data.get("blocks", []):
            bbank = block.get("bank")
            if bbank is not None:
                try:
                    code_banks.add(int(bbank))
                except (TypeError, ValueError):
                    pass
        for edge in data.get("edges", []):
            to_bank = edge.get("to_bank")
            if to_bank is not None:
                try:
                    code_banks.add(int(to_bank))
                except (TypeError, ValueError):
                    pass
        for call in data.get("calls", []):
            target_bank = call.get("target_bank")
            if target_bank is not None:
                try:
                    code_banks.add(int(target_bank))
                except (TypeError, ValueError):
                    pass

        entry_key = make_key(bank if addr >= 0x4000 else 0, addr)
        keyset.add(entry_key)
        keys = sorted(keyset)
        funcs.append(FunctionDesc(bank=bank, addr=addr, symbol=symbol, keys=keys, insn_bytes=insn_by_key))

    funcs.sort(key=lambda f: (f.bank, f.addr))
    return funcs, rom_bytes, code_banks


def parse_dynram_dumps(input_dir: str) -> List[FunctionDesc]:
    dyn_dir = os.path.join(input_dir, "dynram")
    if not os.path.isdir(dyn_dir):
        return []

    funcs: List[FunctionDesc] = []
    seen_variants: Set[Tuple[int, int]] = set()
    for path in sorted(glob.glob(os.path.join(dyn_dir, "dyn_*.asm"))):
        mname = re.search(
            r"dyn_([a-zA-Z0-9]+)_([0-9A-Fa-f]{4})(?:_([0-9A-Fa-f]{8}))?\.asm$",
            os.path.basename(path),
        )
        if not mname:
            continue
        region = mname.group(1).lower()
        entry_addr = int(mname.group(2), 16) & 0xFFFF
        version_tag = (mname.group(3) or "").upper()

        insn_by_key: Dict[int, List[int]] = {}
        keyset: Set[int] = set()
        ordered_lines: List[Tuple[int, List[int]]] = []
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            for raw in f:
                line = raw.rstrip("\r\n")
                m = re.match(r"^\s*([0-9A-Fa-f]{4}):\s+([0-9A-Fa-f ]+?)\s{2,}.*$", line)
                if not m:
                    continue
                addr = int(m.group(1), 16) & 0xFFFF
                bytes_text = m.group(2).strip()
                ibytes = parse_insn_bytes(bytes_text)
                if not ibytes:
                    continue
                key = make_key(0, addr)
                keyset.add(key)
                insn_by_key[key] = ibytes
                ordered_lines.append((key, ibytes))

        if not insn_by_key:
            continue

        entry_key = make_key(0, entry_addr)
        keyset.add(entry_key)
        key_hashes: Dict[int, int] = {}
        ordered_keys = [k for k, _ in ordered_lines]
        for i, key in enumerate(ordered_keys):
            suffix_chunks = [iby for _, iby in ordered_lines[i:]]
            key_hashes[key] = fnv1a_u32_from_bytes(suffix_chunks)
        if entry_key in insn_by_key and entry_key not in key_hashes:
            key_hashes[entry_key] = fnv1a_u32_from_bytes([insn_by_key[entry_key]])
        body_hash = key_hashes.get(entry_key, fnv1a_u32_from_bytes([insn_by_key[k] for k in sorted(insn_by_key)]))
        variant_id = (entry_key, body_hash)
        if variant_id in seen_variants:
            continue
        seen_variants.add(variant_id)
        if not version_tag:
            # Legacy non-versioned dumps: use the first opcode bytes as a stable-ish suffix.
            version_tag = f"{body_hash:08X}"
        symbol = f"fn_dyn_{region}_{entry_addr:04X}_{version_tag}"
        funcs.append(
            FunctionDesc(
                bank=0,
                addr=entry_addr,
                symbol=symbol,
                keys=sorted(keyset),
                insn_bytes=insn_by_key,
                is_dynram=True,
                dynram_hash_by_key=key_hashes,
            )
        )

    funcs.sort(key=lambda f: (f.bank, f.addr))
    return funcs


def merge_offsets_to_chunks(offsets: Iterable[int], rom_blob: bytes) -> List[Tuple[int, bytes]]:
    sorted_off = sorted({off for off in offsets if 0 <= off < len(rom_blob)})
    if not sorted_off:
        return []

    chunks: List[Tuple[int, bytes]] = []
    start = sorted_off[0]
    prev = start
    buf = bytearray([rom_blob[start]])

    for off in sorted_off[1:]:
        if off == prev + 1:
            buf.append(rom_blob[off])
            prev = off
            continue
        chunks.append((start, bytes(buf)))
        start = off
        prev = off
        buf = bytearray([rom_blob[off]])

    chunks.append((start, bytes(buf)))
    return chunks


def build_embedded_chunks(embed_rom_path: str,
                          discovered_rom_bytes: Dict[int, int],
                          code_banks: Set[int],
                          read_trace_offsets: Optional[Set[int]]) -> Tuple[int, List[Tuple[int, bytes]]]:
    with open(embed_rom_path, "rb") as f:
        rom_blob = f.read()
    if not rom_blob:
        raise SystemExit(f"Embedded ROM source is empty: {embed_rom_path}")

    wanted: Set[int] = set()
    # Always keep complete fixed bank 0. Missing startup/interrupt bytes here can
    # quickly trap execution in $0038 loops on partially discovered programs.
    wanted.update(range(min(0x4000, len(rom_blob))))
    rom_bank_count = (len(rom_blob) + 0x3FFF) // 0x4000
    include_full_code_banks = rom_bank_count > 2
    if include_full_code_banks:
        for bank in sorted(code_banks):
            if bank <= 0 or bank >= rom_bank_count:
                continue
            start = bank * 0x4000
            end = min(start + 0x4000, len(rom_blob))
            wanted.update(range(start, end))
    # Keep cartridge header bytes (metadata used by loader).
    wanted.update(range(min(0x150, len(rom_blob))))
    wanted.update(discovered_rom_bytes.keys())
    if read_trace_offsets:
        wanted.update(read_trace_offsets)

    # Heuristic for 32KB ROMs (2 banks):
    # if switchable bank coverage is too low, gameplay often renders white/blank
    # despite progressing. Promote bank1 to full in that case.
    if rom_bank_count == 2 and len(rom_blob) > 0x4000:
        b1_start = 0x4000
        b1_end = min(0x8000, len(rom_blob))
        b1_size = b1_end - b1_start
        if b1_size > 0:
            b1_seen = 0
            for off in wanted:
                if b1_start <= off < b1_end:
                    b1_seen += 1
            b1_cov = float(b1_seen) / float(b1_size)
            if b1_cov < 0.40:
                wanted.update(range(b1_start, b1_end))

    chunks = merge_offsets_to_chunks(wanted, rom_blob)
    return len(rom_blob), chunks


def build_owner_map(funcs: List[FunctionDesc]) -> List[Tuple[int, str]]:
    owner: Dict[int, str] = {}
    key_to_ibytes: Dict[int, List[int]] = {}
    for fn in funcs:
        for key in fn.keys:
            owner.setdefault(key, fn.symbol)
        for key, ibytes in fn.insn_bytes.items():
            if ibytes and key not in key_to_ibytes:
                key_to_ibytes[key] = ibytes

        # Entry key always belongs to its function.
        entry_bank = fn.bank if fn.addr >= 0x4000 else 0
        owner[make_key(entry_bank, fn.addr)] = fn.symbol

    # Cluster fusion is disabled by default because even conservative merges can
    # produce ownership/CFG regressions on some games (white screen / boot stall)
    # when dumps contain imperfect discovery. Keep the code path for future
    # opt-in tuning, but default to stable one-function ownership.
    ENABLE_CLUSTER_FUSION = False

    # Aggressive-but-bounded cluster fusion (opt-in): merge symbols connected by
    # static CFG edges. This increases local goto chaining and reduces dispatch
    # frequency without relying on C tail-call optimization.
    symbol_keys: Dict[str, Set[int]] = {}
    for key, sym in owner.items():
        symbol_keys.setdefault(sym, set()).add(key)
    symbols = sorted(symbol_keys.keys())
    parent: Dict[str, str] = {s: s for s in symbols}
    comp_size: Dict[str, int] = {s: len(symbol_keys[s]) for s in symbols}
    MAX_CLUSTER_KEYS = 128

    def find(x: str) -> str:
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    def union(a: str, b: str) -> None:
        ra = find(a)
        rb = find(b)
        if ra == rb:
            return
        if comp_size[ra] + comp_size[rb] > MAX_CLUSTER_KEYS:
            return
        # Deterministic representative for stable output.
        if ra > rb:
            ra, rb = rb, ra
        parent[rb] = ra
        comp_size[ra] += comp_size[rb]

    if ENABLE_CLUSTER_FUSION:
        for key, sym in list(owner.items()):
            ibytes = key_to_ibytes.get(key)
            if not ibytes:
                continue
            if not _is_cluster_mergeable_edge_insn(ibytes):
                continue
            for succ in _local_successor_keys_for_insn(key, ibytes):
                succ &= 0xFFFFFFFF
                succ_sym = owner.get(succ)
                if succ_sym is None or succ_sym == sym:
                    continue
                # Keep merges within the same ROM bank view to avoid giant cross-bank components.
                if ((key >> 16) & 0xFFFF) != ((succ >> 16) & 0xFFFF):
                    continue
                union(sym, succ_sym)

    fused_owner: Dict[int, str] = {}
    for key, sym in owner.items():
        fused_owner[key] = find(sym)

    return sorted(fused_owner.items(), key=lambda kv: kv[0])


def emit_u8_array_lines(symbol: str, data: bytes) -> List[str]:
    lines: List[str] = [f"static const uint8_t {symbol}[] = {{"]
    for i in range(0, len(data), 16):
        row = ", ".join(f"0x{b:02X}u" for b in data[i:i + 16])
        lines.append(f"    {row},")
    lines.append("};")
    return lines


def parse_opcode_bodies(source_path: str) -> Dict[int, List[str]]:
    with open(source_path, "r", encoding="utf-8") as f:
        lines = f.read().splitlines()

    bodies: Dict[int, List[str]] = {}
    i = 0
    while i < len(lines):
        m = re.match(r"^\s*void\s+cpu_op_0x([0-9A-Fa-f]{2,4})\s*\(CPUState \*cpu\)", lines[i])
        if not m:
            i += 1
            continue

        key = int(m.group(1), 16)
        i += 1
        while i < len(lines) and "{" not in lines[i]:
            i += 1
        if i >= len(lines):
            break

        brace_depth = lines[i].count("{") - lines[i].count("}")
        i += 1
        body: List[str] = []
        while i < len(lines) and brace_depth > 0:
            line = lines[i]
            brace_depth += line.count("{") - line.count("}")
            if brace_depth > 0:
                stripped = line.strip()
                if stripped:
                    body.append(stripped)
            i += 1
        bodies[key] = body

    return bodies


def parse_inline_helpers(source_path: str) -> Dict[str, InlineHelper]:
    with open(source_path, "r", encoding="utf-8") as f:
        lines = f.read().splitlines()

    helpers: Dict[str, InlineHelper] = {}
    i = 0
    while i < len(lines):
        m = re.match(r"^\s*static\s+inline\s+(.+?)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\((.*)\)\s*$", lines[i])
        if not m:
            i += 1
            continue

        return_type = m.group(1).strip()
        name = m.group(2).strip()
        params_text = m.group(3).strip()
        params: List[Tuple[str, str]] = []
        if params_text and params_text != "void":
            for p in split_top_level_args(params_text):
                pclean = p.strip()
                pm = re.match(r"^(.*?)([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\]]*\])?$", pclean)
                if pm:
                    ptype = pm.group(1).strip()
                    pname = pm.group(2).strip()
                    if ptype:
                        params.append((ptype, pname))

        i += 1
        while i < len(lines) and "{" not in lines[i]:
            i += 1
        if i >= len(lines):
            break

        brace_depth = lines[i].count("{") - lines[i].count("}")
        i += 1
        body: List[str] = []
        while i < len(lines) and brace_depth > 0:
            line = lines[i]
            brace_depth += line.count("{") - line.count("}")
            if brace_depth > 0:
                stripped = line.strip()
                if stripped:
                    body.append(stripped)
            i += 1

        helpers[name] = InlineHelper(
            name=name,
            return_type=return_type,
            params=params,
            body=body,
        )

    return helpers


def _imm16(imm: List[int]) -> int:
    lo = imm[0] if len(imm) > 0 else 0
    hi = imm[1] if len(imm) > 1 else 0
    return ((hi & 0xFF) << 8) | (lo & 0xFF)


def inline_helper_call(line: str,
                       inline_helpers: Dict[str, InlineHelper],
                       depth: int = 0,
                       max_depth: int = 8) -> List[str]:
    stmt = line.strip()
    if depth >= max_depth:
        return [stmt]

    m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\((.*)\);$", stmt)
    if not m:
        return [stmt]

    name = m.group(1)
    helper = inline_helpers.get(name)
    if not helper or helper.return_type != "void":
        return [stmt]

    args = split_top_level_args(m.group(2))
    if len(args) != len(helper.params):
        return [stmt]

    expanded: List[str] = ["{"]
    param_to_tmp: Dict[str, str] = {}
    for idx, ((ptype, pname), arg_expr) in enumerate(zip(helper.params, args)):
        tmp_name = f"__inl_{depth}_{idx}"
        param_to_tmp[pname] = tmp_name
        expanded.append(f"    {ptype} {tmp_name} = ({arg_expr});")

    for raw in helper.body:
        replaced = raw
        for pname, tmp_name in param_to_tmp.items():
            replaced = re.sub(rf"\b{re.escape(pname)}\b", tmp_name, replaced)

        nested = inline_helper_call(replaced, inline_helpers, depth + 1, max_depth)
        for nline in nested:
            expanded.append(f"    {nline}")
    expanded.append("}")
    return expanded


def translate_instruction_to_c(opcode_bodies: Dict[int, List[str]],
                               insn_bytes: List[int],
                               inline_helpers: Dict[str, InlineHelper]) -> List[str]:
    if not insn_bytes:
        return []

    opcode = insn_bytes[0] & 0xFF
    cb_prefixed = (opcode == 0xCB and len(insn_bytes) >= 2)
    if cb_prefixed:
        body_key = 0xCB00 | (insn_bytes[1] & 0xFF)
        imm: List[int] = insn_bytes[2:]
    else:
        body_key = opcode
        imm = insn_bytes[1:]

    body = opcode_bodies.get(body_key, [])
    if not body:
        return [f'fprintf(stderr, "[RECOMPILED][NATIVE] unsupported opcode body 0x{body_key:04X}\\n");', "g_running = false;"]

    imm16 = _imm16(imm)
    imm0 = imm[0] if len(imm) > 0 else 0
    imm1 = imm[1] if len(imm) > 1 else 0
    post_idx = 0
    translated: List[str] = []

    def append_stmt(stmt: str) -> None:
        expanded = inline_helper_call(stmt, inline_helpers)
        translated.extend(expanded)

    def repl_pc_post(_: re.Match[str]) -> str:
        nonlocal post_idx
        val = imm[post_idx] if post_idx < len(imm) else 0
        post_idx += 1
        return f"recomp_imm8(cpu, 0x{val & 0xFF:02X}u)"

    if cb_prefixed:
        # Step begin consumed the 0xCB prefix byte; consume the CB sub-opcode byte here.
        append_stmt(f"(void)recomp_imm8(cpu, 0x{insn_bytes[1] & 0xFF:02X}u);")

    for raw in body:
        line = raw

        m = re.match(r"^call_cc_n16\((.+), cpu\);$", line)
        if m:
            cond = m.group(1)
            append_stmt(f"recomp_call_cc_n16({cond}, cpu, 0x{imm16:04X}u);")
            continue
        if line == "call_n16(cpu);":
            append_stmt(f"recomp_call_n16(cpu, 0x{imm16:04X}u);")
            continue

        m = re.match(r"^jp_cc_n16\((.+), cpu\);$", line)
        if m:
            cond = m.group(1)
            append_stmt(f"recomp_jp_cc_n16({cond}, cpu, 0x{imm16:04X}u);")
            continue
        if line == "jp_n16(cpu);":
            append_stmt(f"recomp_jp_n16(cpu, 0x{imm16:04X}u);")
            continue

        m = re.match(r"^ld_r16_n16\(([^,]+), cpu\);$", line)
        if m:
            dest = m.group(1)
            append_stmt(f"recomp_ld_r16_n16({dest}, cpu, 0x{imm16:04X}u);")
            continue

        if line == "ld_n16_sp(cpu);":
            append_stmt(f"recomp_ld_n16_sp(cpu, 0x{imm16:04X}u);")
            continue
        if line == "ld_a_n16(cpu);":
            append_stmt(f"recomp_ld_a_n16(cpu, 0x{imm16:04X}u);")
            continue
        if line == "ld_hl_sp_e8(cpu);":
            append_stmt(f"recomp_ld_hl_sp_e8(cpu, (int8_t)0x{imm0 & 0xFF:02X}u);")
            continue
        if line == "ldh_a_a8(cpu);":
            append_stmt(f"recomp_ldh_a_a8(cpu, 0x{imm0 & 0xFF:02X}u);")
            continue
        if line == "add_sp_e8(cpu);":
            append_stmt(f"recomp_add_sp_e8(cpu, (int8_t)0x{imm0 & 0xFF:02X}u);")
            continue

        line = re.sub(r"memory_read\(cpu->memory, cpu->PC\+\+\)", repl_pc_post, line)
        line = line.replace("memory_read(cpu->memory, cpu->PC + 1)", f"((uint8_t)0x{imm1 & 0xFF:02X}u)")
        line = line.replace("memory_read(cpu->memory, cpu->PC)", f"((uint8_t)0x{imm0 & 0xFF:02X}u)")
        append_stmt(line)

    return translated


def build_codegen_state(funcs: List[FunctionDesc],
                        owner_map: List[Tuple[int, str]]) -> Tuple[Dict[int, List[int]],
                                                                   List[Tuple[int, int, str]],
                                                                   Dict[str, List[int]]]:
    insn_by_key: Dict[int, List[int]] = {}
    for fn in funcs:
        for key, ibytes in fn.insn_bytes.items():
            if key not in insn_by_key and ibytes:
                insn_by_key[key] = ibytes

    dynram_dispatch_entries: List[Tuple[int, int, str]] = []
    for fn in funcs:
        if not fn.is_dynram:
            continue
        for key in fn.keys:
            if (key & 0xFFFF) < 0x8000:
                continue
            if not fn.dynram_hash_by_key:
                continue
            h = fn.dynram_hash_by_key.get(key)
            if h is None:
                continue
            dynram_dispatch_entries.append((key, h, fn.symbol))

    owned_keys_by_symbol: Dict[str, List[int]] = {}
    for key, sym in owner_map:
        owned_keys_by_symbol.setdefault(sym, []).append(key)
    for fn in funcs:
        if fn.is_dynram:
            owned_keys_by_symbol[fn.symbol] = list(fn.keys)

    return insn_by_key, dynram_dispatch_entries, owned_keys_by_symbol


def _local_successor_keys_for_insn(key: int, ibytes: List[int]) -> List[int]:
    if not ibytes:
        return []

    pc = key & 0xFFFF
    bank = (key >> 16) & 0xFFFF
    op = ibytes[0] & 0xFF
    ilen = len(ibytes)
    next_pc = (pc + ilen) & 0xFFFF
    next_bank = 0 if next_pc < 0x4000 else bank
    fallthrough_key = ((next_bank & 0xFFFF) << 16) | next_pc

    def mk_abs(addr: int) -> int:
        a = addr & 0xFFFF
        b = 0 if a < 0x4000 else bank
        return ((b & 0xFFFF) << 16) | a

    out: List[int] = []

    def add(k: int) -> None:
        k &= 0xFFFFFFFF
        if k not in out:
            out.append(k)

    def n16() -> int:
        if len(ibytes) < 3:
            return 0
        return (ibytes[1] & 0xFF) | ((ibytes[2] & 0xFF) << 8)

    def e8_target() -> int:
        if len(ibytes) < 2:
            return next_pc
        rel = ibytes[1] & 0xFF
        if rel >= 0x80:
            rel -= 0x100
        return (pc + ilen + rel) & 0xFFFF

    # Unconditional jumps
    if op == 0xC3:  # JP n16
        add(mk_abs(n16()))
        return out
    if op == 0x18:  # JR e8
        add(mk_abs(e8_target()))
        return out
    if op == 0xE9:  # JP (HL)
        return out

    # Conditional JP/JR: taken + fallthrough
    if op in (0xC2, 0xCA, 0xD2, 0xDA):
        add(mk_abs(n16()))
        add(fallthrough_key)
        return out
    if op in (0x20, 0x28, 0x30, 0x38):
        add(mk_abs(e8_target()))
        add(fallthrough_key)
        return out

    # Calls / RST (after one instruction PC points to callee if taken)
    if op == 0xCD:
        add(mk_abs(n16()))
        return out
    if op in (0xC4, 0xCC, 0xD4, 0xDC):
        add(mk_abs(n16()))
        add(fallthrough_key)
        return out
    if op in (0xC7, 0xCF, 0xD7, 0xDF, 0xE7, 0xEF, 0xF7, 0xFF):
        add(mk_abs(op & 0x38))
        return out

    # Returns: only conditional false branch has a static successor
    if op in (0xC9, 0xD9):
        return out
    if op in (0xC0, 0xC8, 0xD0, 0xD8):
        add(fallthrough_key)
        return out

    # STOP/HALT: don't assume a stable next PC path for chaining
    if op in (0x10, 0x76):
        return out

    # Default: sequential fallthrough
    add(fallthrough_key)
    return out


def _is_cluster_mergeable_edge_insn(ibytes: List[int]) -> bool:
    """Merge across static CFG edges, but avoid call graph explosions."""
    if not ibytes:
        return False
    op = ibytes[0] & 0xFF
    # Allow straight-line code, unconditional JP/JR, conditional JP/JR, and
    # conditional RET false-fallthrough. These improve local CFG density.
    if op in (0x18, 0xC3):  # JR e8, JP n16
        return True
    if op in (0x20, 0x28, 0x30, 0x38):  # JR cc
        return True
    if op in (0xC2, 0xCA, 0xD2, 0xDA):  # JP cc
        return True
    if op in (0xC0, 0xC8, 0xD0, 0xD8):  # RET cc (false branch is static)
        return True
    if op in (
        0x10, 0x76,             # STOP, HALT
        0xE9,                   # JP(HL)
        0xCD,                   # CALL
        0xC4, 0xCC, 0xD4, 0xDC, # CALL cc
        0xC7, 0xCF, 0xD7, 0xDF, 0xE7, 0xEF, 0xF7, 0xFF,  # RST
        0xC9, 0xD9,             # RET, RETI
    ):
        return False
    return True


def _has_deterministic_single_successor(ibytes: List[int]) -> bool:
    """True if the instruction always continues to one statically-known next key."""
    if not ibytes:
        return False
    op = ibytes[0] & 0xFF
    # Dynamic/conditional control flow cannot be direct-chained safely.
    if op in (
        0x10, 0x76,             # STOP, HALT
        0xE9,                   # JP(HL)
        0x20, 0x28, 0x30, 0x38, # JR cc
        0xC2, 0xCA, 0xD2, 0xDA, # JP cc
        0xCD,                   # CALL n16
        0xC4, 0xCC, 0xD4, 0xDC, # CALL cc
        0xC7, 0xCF, 0xD7, 0xDF, 0xE7, 0xEF, 0xF7, 0xFF,  # RST
        0xC9, 0xD9,             # RET, RETI
        0xC0, 0xC8, 0xD0, 0xD8, # RET cc
    ):
        return False
    return True


def emit_function_definitions(out: List[str],
                              funcs_to_emit: List[FunctionDesc],
                              owned_keys_by_symbol: Dict[str, List[int]],
                              insn_by_key: Dict[int, List[int]],
                              opcode_bodies: Dict[int, List[str]],
                              inline_helpers: Dict[str, InlineHelper]) -> None:
    for fn in funcs_to_emit:
        owned_keys = list(owned_keys_by_symbol.get(fn.symbol, []))
        owned_key_set = set(owned_keys)
        single_key_fn = len(owned_keys) == 1
        owned_keys_sorted = sorted(owned_keys)
        # Stability-first default: always re-evaluate next key at runtime instead of
        # jumping directly to a static successor label.
        ENABLE_DIRECT_LOCAL_GOTO = False
        local_pred_count: Dict[int, int] = {k: 0 for k in owned_keys}
        for src_key in owned_keys:
            src_ibytes = insn_by_key.get(src_key, [])
            for sk in _local_successor_keys_for_insn(src_key, src_ibytes):
                if sk in owned_key_set:
                    local_pred_count[sk] = local_pred_count.get(sk, 0) + 1
        # Off by default: trace duplication made some games regress (runtime stalls).
        # Keep the machinery so it can be re-enabled later behind a flag.
        MAX_INLINE_TRACE_DEPTH = 0

        def key_label_name(key: int) -> str:
            return f"L_{key:08X}"

        def emit_transfer_after_key(cur_key: int,
                                    cur_ibytes: List[int],
                                    *,
                                    inline_depth: int,
                                    inline_seen: Set[int]) -> None:
            succ_keys = [k for k in _local_successor_keys_for_insn(cur_key, cur_ibytes) if k in owned_key_set]
            emitted_terminal_transfer = False
            force_runtime_key_check = False

            if ENABLE_DIRECT_LOCAL_GOTO and len(succ_keys) == 1 and _has_deterministic_single_successor(cur_ibytes):
                sk = succ_keys[0]
                sk_pc = sk & 0xFFFF
                # Switchable ROM window (0x4000-0x7FFF) is bank-dependent at runtime.
                # Even with a single static successor, a preceding MBC write can change
                # which bank should execute next, so direct goto/trace-inline is unsafe.
                if 0x4000 <= sk_pc < 0x8000:
                    force_runtime_key_check = True
                if not force_runtime_key_check:
                    can_inline = (
                        sk != cur_key and
                        sk not in inline_seen and
                        inline_depth < MAX_INLINE_TRACE_DEPTH and
                        local_pred_count.get(sk, 0) <= 1
                    )
                    if can_inline:
                        out.append("    if (!g_running)")
                        out.append("        return;")
                        out.append(f"    /* trace-inline {sk:08X} */")
                        emit_key_body(sk, emit_label=False, inline_depth=inline_depth + 1, inline_seen=(inline_seen | {sk}))
                        emitted_terminal_transfer = True
                    else:
                        out.append("    if (!g_running)")
                        out.append("        return;")
                        out.append(f"    goto {key_label_name(sk)};")
                        emitted_terminal_transfer = True
            # Safe local chaining path: even with a single static successor, we can
            # still avoid the full __dispatch scan by recomputing the runtime key
            # and checking only local candidate(s). This preserves correctness for
            # interrupts / bank changes while reducing dispatch overhead.
            if succ_keys and not emitted_terminal_transfer:
                out.append("    if (!g_running)")
                out.append("        return;")
                out.append("    {")
                out.append("        uint32_t __next_key = recomp_make_pc_key(&g_memory, g_cpu.PC);")
                for sk in succ_keys:
                    out.append(f"        if (__next_key == 0x{sk:08X}u) goto {key_label_name(sk)};")
                if single_key_fn:
                    out.append("        (void)__next_key;")
                out.append("    }")

            if not emitted_terminal_transfer:
                if not single_key_fn:
                    # __dispatch performs the g_running check already.
                    out.append("    goto __dispatch;")
                else:
                    # Returning unconditionally already covers the !g_running case.
                    out.append("    return;")

        def emit_key_body(key: int,
                          *,
                          emit_label: bool,
                          inline_depth: int,
                          inline_seen: Set[int]) -> None:
            ibytes = insn_by_key.get(key, [])
            if emit_label:
                out.append(f"{key_label_name(key)}:")
            if not ibytes:
                out.append("    return;")
                out.append("")
                return

            pc = key & 0xFFFF
            opcode = ibytes[0] & 0xFF
            translated_lines = translate_instruction_to_c(opcode_bodies, ibytes, inline_helpers)
            out.append("    {")
            out.append(f"        g_cpu.PC = 0x{pc:04X}u;")
            if pc >= 0x8000:
                out.append("        cpu_set_illegal_opcode_softfail(1);")
            out.append(f"        RECOMP_BEGIN_STEP(0x{opcode:02X}u);")
            for stmt in translated_lines:
                out.append(f"        {stmt}")
            out.append("        RECOMP_END_STEP();")
            if pc >= 0x8000:
                out.append("        cpu_set_illegal_opcode_softfail(0);")
            out.append("    }")
            emit_transfer_after_key(key, ibytes, inline_depth=inline_depth, inline_seen=inline_seen)
            out.append("")

        # Emit the common case (single owned block, no self-loop) as direct C,
        # without label/dispatch scaffolding. This makes the output look much
        # closer to a real recompilation unit.
        if single_key_fn and owned_keys:
            only_key = owned_keys[0]
            ibytes = insn_by_key.get(only_key, [])
            succ_keys = [k for k in _local_successor_keys_for_insn(only_key, ibytes) if k in owned_key_set]
            has_self_loop = any(k == only_key for k in succ_keys)
            if ibytes and not has_self_loop:
                pc = only_key & 0xFFFF
                opcode = ibytes[0] & 0xFF
                translated_lines = translate_instruction_to_c(opcode_bodies, ibytes, inline_helpers)
                out.append(f"void {fn.symbol}(void)")
                out.append("{")
                out.append("    {")
                out.append(f"        g_cpu.PC = 0x{pc:04X}u;")
                if pc >= 0x8000:
                    out.append("        cpu_set_illegal_opcode_softfail(1);")
                out.append(f"        RECOMP_BEGIN_STEP(0x{opcode:02X}u);")
                for stmt in translated_lines:
                    out.append(f"        {stmt}")
                out.append("        RECOMP_END_STEP();")
                if pc >= 0x8000:
                    out.append("        cpu_set_illegal_opcode_softfail(0);")
                out.append("    }")
                out.append("    if (!g_running)")
                out.append("        return;")
                out.append("    return;")
                out.append("}")
                out.append("")
                continue

        out.append(f"void {fn.symbol}(void)")
        out.append("{")
        if single_key_fn and owned_keys:
            out.append(f"    goto {key_label_name(owned_keys[0])};")
        else:
            out.append("    goto __dispatch;")
        out.append("")
        for key in owned_keys:
            emit_key_body(key, emit_label=True, inline_depth=0, inline_seen={key})
        if not single_key_fn:
            out.append("__dispatch:")
            out.append("    if (!g_running)")
            out.append("        return;")
            out.append("    {")
            out.append("        uint32_t __dispatch_key = recomp_make_pc_key(&g_memory, g_cpu.PC);")
            for key in owned_keys_sorted:
                out.append(f"        if (__dispatch_key == 0x{key:08X}u) goto {key_label_name(key)};")
            out.append("    }")
            out.append("    return;")
        out.append("}")
        out.append("")


def emit_exec_helper_block(out: List[str]) -> None:
    block = r"""
static inline uint8_t recomp_imm8(CPUState *cpu, uint8_t value)
{
    cpu->PC++;
    return value;
}

static inline void recomp_ld_r16_n16(uint16_t *dest, CPUState *cpu, uint16_t value)
{
    cpu->PC += 2;
    *dest = value;
    cpu->cycle_count += 3;
}

static inline void recomp_ld_n16_sp(CPUState *cpu, uint16_t addr)
{
    cpu->PC += 2;
    memory_write(cpu->memory, addr, (uint8_t)(cpu->SP & 0x00FFu));
    memory_write(cpu->memory, (uint16_t)(addr + 1u), (uint8_t)((cpu->SP >> 8) & 0x00FFu));
    cpu->cycle_count += 5;
}

static inline void recomp_ld_a_n16(CPUState *cpu, uint16_t addr)
{
    cpu->PC += 2;
    cpu->A = memory_read(cpu->memory, addr);
    cpu->cycle_count += 4;
}

static inline void recomp_ld_hl_sp_e8(CPUState *cpu, int8_t offset)
{
    cpu->PC += 1;
    uint16_t sp = cpu->SP;
    uint16_t result = (uint16_t)(sp + offset);
    cpu->HL = result;
    cpu->F = 0;
    if (((sp & 0x0Fu) + (((uint16_t)offset) & 0x0Fu)) > 0x0Fu)
        cpu->F |= FLAG_H;
    if (((sp & 0xFFu) + (((uint16_t)offset) & 0xFFu)) > 0xFFu)
        cpu->F |= FLAG_C;
    cpu->cycle_count += 3;
}

static inline void recomp_ldh_a_a8(CPUState *cpu, uint8_t imm)
{
    cpu->PC += 1;
    cpu->A = memory_read(cpu->memory, (uint16_t)(0xFF00u + imm));
    cpu->cycle_count += 3;
}

static inline void recomp_add_sp_e8(CPUState *cpu, int8_t value)
{
    cpu->PC += 1;
    uint16_t old_sp = cpu->SP;
    cpu->SP = (uint16_t)(cpu->SP + value);
    cpu->F = 0;
    if (((old_sp & 0x0Fu) + (((uint16_t)value) & 0x0Fu)) > 0x0Fu)
        cpu->F |= FLAG_H;
    if (((old_sp & 0xFFu) + (((uint16_t)value) & 0xFFu)) > 0xFFu)
        cpu->F |= FLAG_C;
    cpu->cycle_count += 4;
}

static inline void recomp_call_n16(CPUState *cpu, uint16_t addr)
{
    cpu->PC += 2;
    memory_write(cpu->memory, --cpu->SP, (uint8_t)((cpu->PC >> 8) & 0x00FFu));
    memory_write(cpu->memory, --cpu->SP, (uint8_t)(cpu->PC & 0x00FFu));
    cpu->PC = addr;
    cpu->cycle_count += 6;
}

static inline void recomp_call_cc_n16(bool condition, CPUState *cpu, uint16_t addr)
{
    if (condition)
        recomp_call_n16(cpu, addr);
    else
    {
        cpu->PC += 2;
        cpu->cycle_count += 3;
    }
}

static inline void recomp_jp_n16(CPUState *cpu, uint16_t addr)
{
    cpu->PC += 2;
    cpu->PC = addr;
    cpu->cycle_count += 4;
}

static inline void recomp_jp_cc_n16(bool condition, CPUState *cpu, uint16_t addr)
{
    if (condition)
    {
        recomp_jp_n16(cpu, addr);
        return;
    }
    cpu->PC += 2;
    cpu->cycle_count += 3;
}

#define RECOMP_BEGIN_STEP(OPCODE_LITERAL) \
    do                                      \
    {                                       \
        CPUState *cpu = &g_cpu;             \
        CPUDecodedStep step = {0};          \
        uint32_t cycles = cpu_decoded_step_begin(cpu, (uint8_t)(OPCODE_LITERAL), &step); \
        if (cycles == 0u)                   \
        {

#define RECOMP_END_STEP()                       \
            cycles = cpu_decoded_step_end(cpu, &step); \
        }                                       \
        recomp_after_step(cycles);              \
    } while (0)
"""
    out.extend(block.strip("\n").splitlines())
    out.append("")


def emit_fuzz_verify_block(out: List[str], function_count: int) -> None:
    block = r"""
typedef struct
{
    MemoryState mem;
    CPUState cpu;
    PPUState ppu;
    uint8_t *cart_ram_copy;
    size_t cart_ram_copy_size;
} RecompFuzzSnapshot;

static RecompFuzzSnapshot g_fuzz_template = {0};
static RecompFuzzSnapshot g_fuzz_base = {0};
static RecompFuzzSnapshot g_fuzz_native = {0};
static RecompFuzzSnapshot g_fuzz_interp = {0};

static void recomp_fuzz_snapshot_free(RecompFuzzSnapshot *snap)
{
    if (!snap)
        return;
    if (snap->cart_ram_copy)
    {
        free(snap->cart_ram_copy);
        snap->cart_ram_copy = NULL;
    }
    snap->cart_ram_copy_size = 0u;
}

static bool recomp_fuzz_snapshot_capture(RecompFuzzSnapshot *snap)
{
    if (!snap)
        return false;
    memcpy(&snap->mem, &g_memory, sizeof(g_memory));
    memcpy(&snap->cpu, &g_cpu, sizeof(g_cpu));
    memcpy(&snap->ppu, &g_ppu, sizeof(g_ppu));

    size_t ram_size = g_memory.cartridge.ram_size;
    uint8_t *ram_ptr = g_memory.cartridge.ram_data;
    if (ram_ptr && ram_size > 0u)
    {
        if (snap->cart_ram_copy_size != ram_size)
        {
            uint8_t *next = (uint8_t *)realloc(snap->cart_ram_copy, ram_size);
            if (!next)
                return false;
            snap->cart_ram_copy = next;
            snap->cart_ram_copy_size = ram_size;
        }
        memcpy(snap->cart_ram_copy, ram_ptr, ram_size);
    }
    else
    {
        recomp_fuzz_snapshot_free(snap);
    }
    return true;
}

static bool recomp_fuzz_snapshot_restore(const RecompFuzzSnapshot *snap)
{
    if (!snap)
        return false;

    uint8_t *live_rom = g_memory.cartridge.rom_data;
    uint8_t *live_ram = g_memory.cartridge.ram_data;
    size_t live_ram_size = g_memory.cartridge.ram_size;

    memcpy(&g_memory, &snap->mem, sizeof(g_memory));
    memcpy(&g_cpu, &snap->cpu, sizeof(g_cpu));
    memcpy(&g_ppu, &snap->ppu, sizeof(g_ppu));

    g_memory.cartridge.rom_data = live_rom;
    g_memory.cartridge.ram_data = live_ram;
    g_memory.cartridge.ram_size = live_ram_size;
    if (live_ram && snap->cart_ram_copy && live_ram_size == snap->cart_ram_copy_size && live_ram_size > 0u)
        memcpy(live_ram, snap->cart_ram_copy, live_ram_size);

    g_cpu.memory = &g_memory;
    g_memory.cpu = &g_cpu;
    g_ppu.mem = &g_memory.memory;
    return true;
}

static uint32_t recomp_fuzz_rng_next(uint32_t *state)
{
    uint32_t x = (state && *state) ? *state : 0xA341316Cu;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    if (state)
        *state = x ? x : 0x1u;
    return x;
}

static void recomp_fuzz_fill_bytes(uint8_t *dst, size_t count, uint32_t *rng)
{
    if (!dst || count == 0u)
        return;
    for (size_t i = 0; i < count; ++i)
        dst[i] = (uint8_t)(recomp_fuzz_rng_next(rng) & 0xFFu);
}

static void recomp_fuzz_set_rom_bank_for_key(MemoryState *mem, uint32_t key)
{
    if (!mem)
        return;
    CartridgeState *cart = &mem->cartridge;
    uint16_t pc = (uint16_t)(key & 0xFFFFu);
    uint16_t bank = (uint16_t)((key >> 16) & 0xFFFFu);
    if (pc < 0x4000u)
        return;
    if (pc >= 0x8000u)
        return;
    if (cart->rom_banks == 0u)
        return;
    bank = (uint16_t)(bank % (uint16_t)((cart->rom_banks > 0xFFFFu) ? 0x10000u : cart->rom_banks));
    switch (cart->mbc_type)
    {
    case MBC1:
    {
        uint8_t low5 = (uint8_t)(bank & 0x1Fu);
        if (low5 == 0u)
            low5 = 1u;
        cart->mbc1_low5 = low5;
        cart->mbc1_high2 = (uint8_t)((bank >> 5) & 0x03u);
        cart->mbc1_mode = 0u;
        break;
    }
    case MBC2:
    {
        uint8_t b = (uint8_t)(bank & 0x0Fu);
        if (b == 0u)
            b = 1u;
        cart->mbc2_rom_bank = b;
        break;
    }
    case MBC3:
    {
        uint8_t b = (uint8_t)(bank & 0x7Fu);
        if (b == 0u)
            b = 1u;
        cart->mbc3_rom_bank = b;
        break;
    }
    case MBC5:
        cart->mbc5_rom_bank = (uint16_t)(bank & 0x01FFu);
        break;
    case MBC_NONE:
    default:
        break;
    }
}

static void recomp_fuzz_randomize_for_key(uint32_t key, uint32_t *rng)
{
    recomp_fuzz_fill_bytes(g_memory.memory.vram, sizeof(g_memory.memory.vram), rng);
    if (g_memory.cartridge.ram_data && g_memory.cartridge.ram_size > 0u)
        recomp_fuzz_fill_bytes(g_memory.cartridge.ram_data, g_memory.cartridge.ram_size, rng);
    else
        recomp_fuzz_fill_bytes(g_memory.memory.eram, sizeof(g_memory.memory.eram), rng);
    recomp_fuzz_fill_bytes(g_memory.memory.wram, sizeof(g_memory.memory.wram), rng);
    memcpy(g_memory.memory.echo, g_memory.memory.wram, sizeof(g_memory.memory.echo));
    recomp_fuzz_fill_bytes(g_memory.memory.oam, sizeof(g_memory.memory.oam), rng);
    recomp_fuzz_fill_bytes(g_memory.memory.io, sizeof(g_memory.memory.io), rng);
    recomp_fuzz_fill_bytes(g_memory.memory.hram, sizeof(g_memory.memory.hram), rng);
    g_memory.memory.IE = (uint8_t)(recomp_fuzz_rng_next(rng) & 0x1Fu);
    g_memory.bios_enabled = false;
    g_memory.memory.BOOT = 1u;

    g_memory.joypad_buttons = (uint8_t)(recomp_fuzz_rng_next(rng) & 0x0Fu);
    g_memory.joypad_dpad = (uint8_t)(recomp_fuzz_rng_next(rng) & 0x0Fu);
    g_memory.joypad_select = (uint8_t)(0x10u * (recomp_fuzz_rng_next(rng) & 0x03u));

    g_cpu.AF = (uint16_t)recomp_fuzz_rng_next(rng);
    g_cpu.BC = (uint16_t)recomp_fuzz_rng_next(rng);
    g_cpu.DE = (uint16_t)recomp_fuzz_rng_next(rng);
    g_cpu.HL = (uint16_t)recomp_fuzz_rng_next(rng);
    g_cpu.SP = (uint16_t)recomp_fuzz_rng_next(rng);
    g_cpu.PC = (uint16_t)(key & 0xFFFFu);
    g_cpu.F &= 0xF0u;
    g_cpu.IME = (recomp_fuzz_rng_next(rng) & 1u) != 0u;
    g_cpu.EI_pending = (recomp_fuzz_rng_next(rng) & 1u) != 0u;
    g_cpu.IME_enable_pending = (recomp_fuzz_rng_next(rng) & 1u) != 0u;
    g_cpu.halt = false;
    g_cpu.stop = false;
    g_cpu.halt_bug = (recomp_fuzz_rng_next(rng) & 1u) != 0u;
    g_cpu.cycle_count = 0u;

    g_ppu.frame_ready = false;
    g_ppu.mem = &g_memory.memory;

    g_running = true;
    g_instr_count = 0u;
    g_frame_count = 0u;

    recomp_fuzz_set_rom_bank_for_key(&g_memory, key);
}

static bool recomp_fuzz_exec_one_native(uint32_t key, bool *out_end_running, RecompFn *out_fn_used)
{
    uint64_t saved_max_instr = g_max_instr;
    uint64_t saved_max_frames = g_max_frames;
    bool saved_running = g_running;

    uint64_t start_instr = g_instr_count;
    g_max_instr = start_instr + 1u;
    g_max_frames = 0u;
    g_running = true;

    RecompFn fn = recomp_lookup(key);
    if (out_fn_used)
        *out_fn_used = fn;
    bool ok = false;
    if (fn)
    {
        fn();
        ok = (g_instr_count == (start_instr + 1u));
    }
    else
    {
        ok = recomp_try_dynamic_missing_step(key);
    }
    bool end_running = g_running;

    g_max_instr = saved_max_instr;
    g_max_frames = saved_max_frames;
    g_running = saved_running;
    if (out_end_running)
        *out_end_running = end_running;
    return ok;
}

static bool recomp_fuzz_exec_one_interp(void)
{
    cpu_set_illegal_opcode_softfail(1);
    uint32_t cycles = cpu_execute_instruction(&g_cpu);
    cpu_set_illegal_opcode_softfail(0);
    ppu_step(&g_ppu, cycles);
    g_instr_count++;
    if (g_ppu.frame_ready)
    {
        g_frame_count++;
        g_ppu.frame_ready = false;
    }
    return true;
}

static bool recomp_dispatch_quarantine_test(size_t idx)
{
    if (idx >= k_dispatch_count)
        return false;
    return (g_dispatch_quarantine_bits[idx >> 3u] & (uint8_t)(1u << (idx & 7u))) != 0u;
}

static bool recomp_dispatch_quarantine_set(size_t idx)
{
    if (idx >= k_dispatch_count)
        return false;
    uint8_t *slot = &g_dispatch_quarantine_bits[idx >> 3u];
    uint8_t mask = (uint8_t)(1u << (idx & 7u));
    bool was_set = ((*slot & mask) != 0u);
    *slot |= mask;
    return !was_set;
}

static size_t recomp_quarantine_fn(RecompFn fn, uint32_t trigger_key)
{
    if (!fn)
        return 0u;
    size_t newly = 0u;
    for (size_t i = 0; i < k_dispatch_count; ++i)
    {
        if (k_dispatch_map[i].fn != fn)
            continue;
        if (recomp_dispatch_quarantine_set(i))
        {
            newly++;
            if (g_quarantine_keys_file)
            {
                fprintf(g_quarantine_keys_file, "%08X\n", k_dispatch_map[i].key);
            }
        }
    }
    if (g_quarantine_keys_file && newly > 0u)
        fflush(g_quarantine_keys_file);
    if (newly > 0u)
    {
        g_shadow_quarantine_fn_count++;
        fprintf(stderr,
                "\n[SHADOW][QUAR] trigger=%08X quarantined_keys=%zu total_quarantined=%llu\n",
                trigger_key,
                newly,
                (unsigned long long)g_shadow_quarantine_fn_count);
    }
    return newly;
}

static bool recomp_fuzz_cpu_equal(const CPUState *a, const CPUState *b)
{
    if (!a || !b)
        return false;
    return a->AF == b->AF &&
           a->BC == b->BC &&
           a->DE == b->DE &&
           a->HL == b->HL &&
           a->SP == b->SP &&
           a->PC == b->PC &&
           a->IME == b->IME &&
           a->EI_pending == b->EI_pending &&
           a->IME_enable_pending == b->IME_enable_pending &&
           a->halt == b->halt &&
           a->stop == b->stop &&
           a->cycle_count == b->cycle_count &&
           a->halt_bug == b->halt_bug &&
           a->div_counter == b->div_counter &&
           a->timer_prev_signal == b->timer_prev_signal &&
           a->timer_reload_active == b->timer_reload_active &&
           a->timer_reload_delay == b->timer_reload_delay;
}

static bool recomp_fuzz_ppu_equal(const PPUState *a, const PPUState *b)
{
    if (!a || !b)
        return false;
    if (a->dots != b->dots ||
        a->frame_ready != b->frame_ready ||
        a->last_mode != b->last_mode ||
        a->lcd_enabled != b->lcd_enabled ||
        a->lyc_match != b->lyc_match ||
        a->sprite_count != b->sprite_count)
        return false;
    if (a->sprite_count > 0u)
    {
        size_t used = (size_t)a->sprite_count;
        if (used > 10u)
            used = 10u;
        if (memcmp(a->scanline_sprites, b->scanline_sprites, used * sizeof(a->scanline_sprites[0])) != 0)
            return false;
    }
    return true;
}

static bool recomp_fuzz_cart_regs_equal(const CartridgeState *a, const CartridgeState *b)
{
    if (!a || !b)
        return false;
    if (a->ram_enabled != b->ram_enabled ||
        a->mbc1_low5 != b->mbc1_low5 ||
        a->mbc1_high2 != b->mbc1_high2 ||
        a->mbc1_mode != b->mbc1_mode ||
        a->mbc2_rom_bank != b->mbc2_rom_bank ||
        a->mbc3_rom_bank != b->mbc3_rom_bank ||
        a->mbc3_ram_rtc_select != b->mbc3_ram_rtc_select ||
        a->mbc3_latch_state != b->mbc3_latch_state ||
        a->mbc3_rtc_latched_valid != b->mbc3_rtc_latched_valid ||
        a->mbc5_rom_bank != b->mbc5_rom_bank ||
        a->mbc5_ram_bank != b->mbc5_ram_bank)
        return false;
    if (memcmp(a->mbc3_rtc_regs, b->mbc3_rtc_regs, sizeof(a->mbc3_rtc_regs)) != 0)
        return false;
    if (memcmp(a->mbc3_rtc_latched, b->mbc3_rtc_latched, sizeof(a->mbc3_rtc_latched)) != 0)
        return false;
    return true;
}

static bool recomp_fuzz_snap_equal(const RecompFuzzSnapshot *a, const RecompFuzzSnapshot *b)
{
    if (!a || !b)
        return false;
    if (!recomp_fuzz_cpu_equal(&a->cpu, &b->cpu))
        return false;
    if (!recomp_fuzz_ppu_equal(&a->ppu, &b->ppu))
        return false;
    if (!recomp_fuzz_cart_regs_equal(&a->mem.cartridge, &b->mem.cartridge))
        return false;
    if (a->mem.bios_enabled != b->mem.bios_enabled ||
        a->mem.joypad_buttons != b->mem.joypad_buttons ||
        a->mem.joypad_dpad != b->mem.joypad_dpad ||
        a->mem.joypad_select != b->mem.joypad_select)
        return false;
    if (memcmp(a->mem.memory.data, b->mem.memory.data, sizeof(a->mem.memory.data)) != 0)
        return false;
    if (a->cart_ram_copy_size != b->cart_ram_copy_size)
        return false;
    if (a->cart_ram_copy_size > 0u)
    {
        if (!a->cart_ram_copy || !b->cart_ram_copy)
            return false;
        if (memcmp(a->cart_ram_copy, b->cart_ram_copy, a->cart_ram_copy_size) != 0)
            return false;
    }
    return true;
}

static uint16_t recomp_fuzz_first_mem_diff(const RecompFuzzSnapshot *a, const RecompFuzzSnapshot *b)
{
    for (uint32_t i = 0; i < 0x10000u; ++i)
    {
        if (a->mem.memory.data[i] != b->mem.memory.data[i])
            return (uint16_t)i;
    }
    return 0xFFFFu;
}

static void recomp_fuzz_report_mismatch(uint32_t key, uint32_t iter)
{
    uint16_t pc_before = (uint16_t)(key & 0xFFFFu);
    uint8_t opcode = memory_read(&g_memory, pc_before);
    uint16_t diff = recomp_fuzz_first_mem_diff(&g_fuzz_native, &g_fuzz_interp);
    bool has_reg_diff =
        (g_fuzz_native.cpu.PC != g_fuzz_interp.cpu.PC) ||
        (g_fuzz_native.cpu.AF != g_fuzz_interp.cpu.AF) ||
        (g_fuzz_native.cpu.BC != g_fuzz_interp.cpu.BC) ||
        (g_fuzz_native.cpu.DE != g_fuzz_interp.cpu.DE) ||
        (g_fuzz_native.cpu.HL != g_fuzz_interp.cpu.HL) ||
        (g_fuzz_native.cpu.SP != g_fuzz_interp.cpu.SP);
    fprintf(stderr,
            "\n[DIFF][MISMATCH] key=%08X iter=%u op=%02X pc0=%04X "
            "native_pc=%04X interp_pc=%04X native_af=%04X interp_af=%04X "
            "native_bc=%04X interp_bc=%04X native_de=%04X interp_de=%04X "
            "native_hl=%04X interp_hl=%04X native_sp=%04X interp_sp=%04X "
            "mem_diff=%s\n",
            key, iter, opcode, pc_before,
            g_fuzz_native.cpu.PC, g_fuzz_interp.cpu.PC,
            g_fuzz_native.cpu.AF, g_fuzz_interp.cpu.AF,
            g_fuzz_native.cpu.BC, g_fuzz_interp.cpu.BC,
            g_fuzz_native.cpu.DE, g_fuzz_interp.cpu.DE,
            g_fuzz_native.cpu.HL, g_fuzz_interp.cpu.HL,
            g_fuzz_native.cpu.SP, g_fuzz_interp.cpu.SP,
            (diff == 0xFFFFu) ? "none" : "set");
    if (diff != 0xFFFFu)
    {
        fprintf(stderr, "[DIFF][MISMATCH] mem[%04X] native=%02X interp=%02X\n",
                diff,
                g_fuzz_native.mem.memory.data[diff],
                g_fuzz_interp.mem.memory.data[diff]);
    }
    fprintf(stderr,
            "[DIFF][CPU-INT] cycles %llu/%llu div_counter %u/%u "
            "timer_prev %u/%u timer_reload %u/%u delay %u/%u halt_bug %u/%u\n",
            (unsigned long long)g_fuzz_native.cpu.cycle_count,
            (unsigned long long)g_fuzz_interp.cpu.cycle_count,
            (unsigned)g_fuzz_native.cpu.div_counter,
            (unsigned)g_fuzz_interp.cpu.div_counter,
            g_fuzz_native.cpu.timer_prev_signal ? 1u : 0u,
            g_fuzz_interp.cpu.timer_prev_signal ? 1u : 0u,
            g_fuzz_native.cpu.timer_reload_active ? 1u : 0u,
            g_fuzz_interp.cpu.timer_reload_active ? 1u : 0u,
            (unsigned)g_fuzz_native.cpu.timer_reload_delay,
            (unsigned)g_fuzz_interp.cpu.timer_reload_delay,
            g_fuzz_native.cpu.halt_bug ? 1u : 0u,
            g_fuzz_interp.cpu.halt_bug ? 1u : 0u);
    fprintf(stderr,
            "[DIFF][CPU-FLAGS] ime %u/%u ei_pending %u/%u ime_pending %u/%u halt %u/%u stop %u/%u\n",
            g_fuzz_native.cpu.IME ? 1u : 0u,
            g_fuzz_interp.cpu.IME ? 1u : 0u,
            g_fuzz_native.cpu.EI_pending ? 1u : 0u,
            g_fuzz_interp.cpu.EI_pending ? 1u : 0u,
            g_fuzz_native.cpu.IME_enable_pending ? 1u : 0u,
            g_fuzz_interp.cpu.IME_enable_pending ? 1u : 0u,
            g_fuzz_native.cpu.halt ? 1u : 0u,
            g_fuzz_interp.cpu.halt ? 1u : 0u,
            g_fuzz_native.cpu.stop ? 1u : 0u,
            g_fuzz_interp.cpu.stop ? 1u : 0u);
    fprintf(stderr,
            "[DIFF][PPU] dots %u/%u frame_ready %u/%u last_mode %u/%u lcd_en %u/%u lyc %u/%u sprites %u/%u | LY %02X/%02X STAT %02X/%02X LCDC %02X/%02X IF %02X/%02X IE %02X/%02X DIV %02X/%02X TIMA %02X/%02X TAC %02X/%02X\n",
            (unsigned)g_fuzz_native.ppu.dots,
            (unsigned)g_fuzz_interp.ppu.dots,
            g_fuzz_native.ppu.frame_ready ? 1u : 0u,
            g_fuzz_interp.ppu.frame_ready ? 1u : 0u,
            (unsigned)g_fuzz_native.ppu.last_mode,
            (unsigned)g_fuzz_interp.ppu.last_mode,
            g_fuzz_native.ppu.lcd_enabled ? 1u : 0u,
            g_fuzz_interp.ppu.lcd_enabled ? 1u : 0u,
            g_fuzz_native.ppu.lyc_match ? 1u : 0u,
            g_fuzz_interp.ppu.lyc_match ? 1u : 0u,
            (unsigned)g_fuzz_native.ppu.sprite_count,
            (unsigned)g_fuzz_interp.ppu.sprite_count,
            g_fuzz_native.mem.memory.LY, g_fuzz_interp.mem.memory.LY,
            g_fuzz_native.mem.memory.STAT, g_fuzz_interp.mem.memory.STAT,
            g_fuzz_native.mem.memory.LCDC, g_fuzz_interp.mem.memory.LCDC,
            g_fuzz_native.mem.memory.IF, g_fuzz_interp.mem.memory.IF,
            g_fuzz_native.mem.memory.IE, g_fuzz_interp.mem.memory.IE,
            g_fuzz_native.mem.memory.DIV, g_fuzz_interp.mem.memory.DIV,
            g_fuzz_native.mem.memory.TIMA, g_fuzz_interp.mem.memory.TIMA,
            g_fuzz_native.mem.memory.TAC, g_fuzz_interp.mem.memory.TAC);
    if (!has_reg_diff && diff == 0xFFFFu)
    {
        fprintf(stderr, "[DIFF][NOTE] visible regs/memory match; mismatch is in timing/internal CPU/PPU/MBC state.\n");
    }
}

static int recomp_fuzz_verify(uint32_t iters_per_key, uint32_t seed, uint32_t max_keys)
{
    if (iters_per_key == 0u)
        iters_per_key = 64u;
    if (seed == 0u)
        seed = (uint32_t)time(NULL);

    fprintf(stderr,
            "[FUZZ] differential verifier start seed=%u functions=%u dispatch_keys=%u iters_per_key=%u\n",
            seed, (unsigned)__FUNC_COUNT__, (unsigned)k_dispatch_count, (unsigned)iters_per_key);

    if (!recomp_fuzz_snapshot_capture(&g_fuzz_template))
    {
        fprintf(stderr, "[FUZZ] failed to capture template snapshot\n");
        return 2;
    }

    uint64_t tests = 0u;
    uint64_t mismatches = 0u;
    uint64_t skipped = 0u;
    uint32_t rng = seed;
    uint64_t last_tick = (uint64_t)time(NULL);
    time_t t0 = time(NULL);

    size_t total_keys = k_dispatch_count;
    if (max_keys > 0u && (size_t)max_keys < total_keys)
        total_keys = (size_t)max_keys;

    bool saved_allow_dynram = g_allow_dynram_dispatch;
    bool saved_allow_rom_fallback = g_allow_rom_fallback;
    bool saved_display = g_display.enabled;
    g_allow_dynram_dispatch = false;
    g_allow_rom_fallback = false;
    g_display.enabled = false;

    for (size_t ki = 0; ki < total_keys; ++ki)
    {
        uint32_t key = k_dispatch_map[ki].key;
        uint16_t pc = (uint16_t)(key & 0xFFFFu);
        if (pc >= 0x8000u)
            continue;

        for (uint32_t iter = 0; iter < iters_per_key; ++iter)
        {
            if (!recomp_fuzz_snapshot_restore(&g_fuzz_template))
            {
                fprintf(stderr, "\n[FUZZ] failed to restore template\n");
                g_allow_dynram_dispatch = saved_allow_dynram;
                g_allow_rom_fallback = saved_allow_rom_fallback;
                g_display.enabled = saved_display;
                return 2;
            }
            recomp_fuzz_randomize_for_key(key, &rng);
            if (!recomp_fuzz_snapshot_capture(&g_fuzz_base))
            {
                fprintf(stderr, "\n[FUZZ] failed to capture base snapshot\n");
                g_allow_dynram_dispatch = saved_allow_dynram;
                g_allow_rom_fallback = saved_allow_rom_fallback;
                g_display.enabled = saved_display;
                return 2;
            }

            if (!recomp_fuzz_snapshot_restore(&g_fuzz_base) || !recomp_fuzz_exec_one_native(key, NULL, NULL) || !recomp_fuzz_snapshot_capture(&g_fuzz_native))
            {
                skipped++;
                continue;
            }

            if (!recomp_fuzz_snapshot_restore(&g_fuzz_base) || !recomp_fuzz_exec_one_interp() || !recomp_fuzz_snapshot_capture(&g_fuzz_interp))
            {
                skipped++;
                continue;
            }

            tests++;
            if (!recomp_fuzz_snap_equal(&g_fuzz_native, &g_fuzz_interp))
            {
                mismatches++;
                recomp_fuzz_report_mismatch(key, iter);
                if (mismatches >= 64u)
                {
                    fprintf(stderr, "[FUZZ] stopping after %llu mismatches\n", (unsigned long long)mismatches);
                    ki = total_keys;
                    break;
                }
            }

            uint64_t now_tick = (uint64_t)time(NULL);
            if (now_tick != last_tick)
            {
                last_tick = now_tick;
                double pct = (total_keys == 0u) ? 100.0 : (100.0 * (double)(ki + 1u) / (double)total_keys);
                uint64_t elapsed = (uint64_t)(time(NULL) - t0);
                fprintf(stderr,
                        "\r[FUZZ] t=%llus keys=%zu/%zu (%.1f%%) tests=%llu skipped=%llu mismatches=%llu      ",
                        (unsigned long long)elapsed,
                        ki + 1u, total_keys, pct,
                        (unsigned long long)tests,
                        (unsigned long long)skipped,
                        (unsigned long long)mismatches);
                fflush(stderr);
            }
        }
    }

    g_allow_dynram_dispatch = saved_allow_dynram;
    g_allow_rom_fallback = saved_allow_rom_fallback;
    g_display.enabled = saved_display;

    fprintf(stderr,
            "\n[FUZZ] done tests=%llu skipped=%llu mismatches=%llu seed=%u\n",
            (unsigned long long)tests,
            (unsigned long long)skipped,
            (unsigned long long)mismatches,
            seed);
    return (mismatches == 0u) ? 0 : 3;
}

static void recomp_shadow_status_tick(uint32_t key)
{
    static time_t s_last = 0;
    time_t now = time(NULL);
    if (now == s_last)
        return;
    s_last = now;
    fprintf(stderr,
            "\r[SHADOW] checked=%llu mismatches=%llu skipped_ext=%llu pc=%04X key=%08X          ",
            (unsigned long long)g_shadow_diff_checked,
            (unsigned long long)g_shadow_diff_mismatches,
            (unsigned long long)g_shadow_diff_skipped_external,
            g_cpu.PC,
            key);
    fflush(stderr);
}

static bool recomp_shadow_lockstep_once(void)
{
    uint32_t key = recomp_make_pc_key(&g_memory, g_cpu.PC);
    if (!recomp_fuzz_snapshot_capture(&g_fuzz_base))
    {
        fprintf(stderr, "\n[SHADOW] snapshot capture failed (base)\n");
        g_running = false;
        return false;
    }

    uint64_t base_instr = g_instr_count;
    uint64_t base_frames = g_frame_count;
    bool base_running = g_running;

    bool native_running = g_running;
    RecompFn native_fn = NULL;
    bool native_ok = recomp_fuzz_exec_one_native(key, &native_running, &native_fn);
    uint64_t native_instr = g_instr_count;
    uint64_t native_frames = g_frame_count;

    if (!recomp_fuzz_snapshot_capture(&g_fuzz_native))
    {
        fprintf(stderr, "\n[SHADOW] snapshot capture failed (native)\n");
        g_running = false;
        return false;
    }

    if (!recomp_fuzz_snapshot_restore(&g_fuzz_base))
    {
        fprintf(stderr, "\n[SHADOW] snapshot restore failed (base)\n");
        g_running = false;
        return false;
    }
    g_instr_count = base_instr;
    g_frame_count = base_frames;
    g_running = base_running;

    bool interp_ok = recomp_fuzz_exec_one_interp();
    bool interp_running = g_running;
    uint64_t interp_instr = g_instr_count;
    uint64_t interp_frames = g_frame_count;
    (void)interp_instr;
    (void)interp_frames;

    if (!recomp_fuzz_snapshot_capture(&g_fuzz_interp))
    {
        fprintf(stderr, "\n[SHADOW] snapshot capture failed (interp)\n");
        g_running = false;
        return false;
    }

    bool external_nondet_step = false;
    if (g_display.enabled)
    {
        uint64_t next_instr = base_instr + 1u;
        bool poll_tick = ((next_instr & 0x1FFFULL) == 0ULL);
        bool frame_present_tick = (native_frames != base_frames);
        external_nondet_step = poll_tick || frame_present_tick;
    }

    if (external_nondet_step)
    {
        g_shadow_diff_skipped_external++;
        if (!recomp_fuzz_snapshot_restore(&g_fuzz_native))
        {
            fprintf(stderr, "\n[SHADOW] snapshot restore failed (native/ext)\n");
            g_running = false;
            return false;
        }
        g_instr_count = native_instr;
        g_frame_count = native_frames;
        g_running = native_ok ? base_running : native_running;
        recomp_shadow_status_tick(key);
        return native_ok;
    }

    g_shadow_diff_checked++;
    (void)interp_running;
    bool same = (native_ok == interp_ok) &&
                recomp_fuzz_snap_equal(&g_fuzz_native, &g_fuzz_interp);
    if (!same)
    {
        g_shadow_diff_mismatches++;
        if (g_shadow_diff_mismatches <= g_shadow_diff_max_reports)
            recomp_fuzz_report_mismatch(key, (uint32_t)g_shadow_diff_checked);
        if (g_shadow_quarantine_enabled && native_fn)
            (void)recomp_quarantine_fn(native_fn, key);
        if (g_shadow_diff_stop_on_mismatch)
            native_running = false;
    }

    if (!recomp_fuzz_snapshot_restore(&g_fuzz_native))
    {
        fprintf(stderr, "\n[SHADOW] snapshot restore failed (native)\n");
        g_running = false;
        return false;
    }
    g_instr_count = native_instr;
    g_frame_count = native_frames;
    g_running = native_ok ? base_running : native_running;

    recomp_shadow_status_tick(key);
    return native_ok;
}
"""
    out.extend(block.strip("\n").replace("__FUNC_COUNT__", str(function_count)).splitlines())
    out.append("")


def render_shard_c(shard_funcs: List[FunctionDesc],
                   shard_index: int,
                   shard_count: int,
                   owned_keys_by_symbol: Dict[str, List[int]],
                   insn_by_key: Dict[int, List[int]],
                   opcode_bodies: Dict[int, List[str]],
                   inline_helpers: Dict[str, InlineHelper]) -> str:
    shard_emit_funcs = [fn for fn in shard_funcs if fn.is_dynram or owned_keys_by_symbol.get(fn.symbol)]
    out: List[str] = []
    out.append("/* Auto-generated shard by tools/recomp_codegen.py */")
    out.append("#include \"CPU.h\"")
    out.append("#include \"Memory.h\"")
    out.append("")
    out.append("#include <stdbool.h>")
    out.append("#include <stdint.h>")
    out.append("")
    out.append(f"/* shard {shard_index + 1}/{shard_count}, functions={len(shard_emit_funcs)} */")
    out.append("")
    out.append("extern MemoryState g_memory;")
    out.append("extern CPUState g_cpu;")
    out.append("extern bool g_running;")
    out.append("uint32_t recomp_make_pc_key(const MemoryState *mem, uint16_t pc);")
    out.append("void recomp_after_step(uint32_t cycles);")
    out.append("")
    emit_exec_helper_block(out)
    emit_function_definitions(out, shard_emit_funcs, owned_keys_by_symbol, insn_by_key, opcode_bodies, inline_helpers)
    return "\n".join(out)


def split_function_shards(funcs: List[FunctionDesc], funcs_per_shard: int) -> List[List[FunctionDesc]]:
    if funcs_per_shard <= 0:
        funcs_per_shard = 256
    shards: List[List[FunctionDesc]] = []
    cur: List[FunctionDesc] = []
    for fn in funcs:
        cur.append(fn)
        if len(cur) >= funcs_per_shard:
            shards.append(cur)
            cur = []
    if cur:
        shards.append(cur)
    return shards or [list(funcs)]


def render_c(funcs: List[FunctionDesc],
             owner_map: List[Tuple[int, str]],
             opcode_bodies: Dict[int, List[str]],
             inline_helpers: Dict[str, InlineHelper],
             default_rom: str,
             embedded_chunks: Optional[List[Tuple[int, bytes]]] = None,
             embedded_rom_size: int = 0,
             emit_function_bodies: bool = True,
             codegen_state: Optional[Tuple[Dict[int, List[int]],
                                           List[Tuple[int, int, str]],
                                           Dict[str, List[int]]]] = None) -> str:
    out: List[str] = []
    out.append("/* Auto-generated by tools/recomp_codegen.py */")
    out.append("#ifndef _WIN32")
    out.append("#define _POSIX_C_SOURCE 200809L")
    out.append("#endif")
    out.append("#include \"CPU.h\"")
    out.append("#include \"Memory.h\"")
    out.append("#include \"PPU.h\"")
    out.append("#include \"RecompProbe.h\"")
    out.append("#include <SDL3/SDL.h>")
    out.append("")
    out.append("#include <stdbool.h>")
    out.append("#include <stdint.h>")
    out.append("#include <stdio.h>")
    out.append("#include <stdlib.h>")
    out.append("#include <string.h>")
    out.append("#include <time.h>")
    out.append("")
    out.append("typedef struct")
    out.append("{")
    out.append("    bool enabled;")
    out.append("    bool initialized;")
    out.append("    int scale;")
    out.append("    SDL_Window *window;")
    out.append("    SDL_Renderer *renderer;")
    out.append("    SDL_Texture *texture;")
    out.append("} DisplayContext;")
    out.append("")
    out.append("MemoryState g_memory;")
    out.append("CPUState g_cpu;")
    out.append("static PPUState g_ppu;")
    out.append("static DisplayContext g_display = {0};")
    out.append("bool g_running = true;")
    out.append("static uint64_t g_instr_count = 0;")
    out.append("static uint64_t g_frame_count = 0;")
    out.append("static uint64_t g_max_instr = 0ULL;")
    out.append("static uint64_t g_max_frames = 0ULL;")
    out.append("static bool g_allow_rom_fallback = true;")
    out.append("static bool g_allow_dynram_dispatch = true;")
    out.append("static uint8_t g_missing_dyn_log[8192] = {0};")
    out.append("static FILE *g_missing_keys_file = NULL;")
    out.append("static bool g_shadow_diff_enabled = false;")
    out.append("static bool g_shadow_diff_stop_on_mismatch = false;")
    out.append("static bool g_shadow_quarantine_enabled = false;")
    out.append("static bool g_trust_only_enabled = false;")
    out.append("static uint32_t g_shadow_diff_every = 1u;")
    out.append("static uint64_t g_shadow_diff_checked = 0u;")
    out.append("static uint64_t g_shadow_diff_mismatches = 0u;")
    out.append("static uint64_t g_shadow_diff_skipped_external = 0u;")
    out.append("static uint64_t g_shadow_diff_max_reports = 200u;")
    out.append("static uint64_t g_shadow_quarantine_fn_count = 0u;")
    out.append("static uint64_t g_trust_only_trusted_fn_count = 0u;")
    out.append("static uint64_t g_trust_only_quarantined_key_count = 0u;")
    out.append("static FILE *g_quarantine_keys_file = NULL;")
    out.append("static bool g_last_lookup_quarantined = false;")
    out.append("static bool g_runtime_discover_enabled = false;")
    out.append("static bool g_runtime_read_trace_enabled = false;")
    out.append("static time_t g_runtime_read_trace_last_flush = 0;")
    out.append("")
    out.append("typedef void (*RecompFn)(void);")
    out.append("")
    if embedded_chunks:
        out.append("#define RECOMP_HAS_EMBEDDED_ROM 1")
    else:
        out.append("#define RECOMP_HAS_EMBEDDED_ROM 0")
    out.append("")
    out.append("static bool read_env_enabled(const char *name)")
    out.append("{")
    out.append("    const char *v = getenv(name);")
    out.append("    if (!v || v[0] == '\\0' || v[0] == '0')")
    out.append("        return false;")
    out.append("    return true;")
    out.append("}")
    out.append("")
    out.append("static void recomp_path_to_stem(const char *path, char *out, size_t out_size)")
    out.append("{")
    out.append("    if (!out || out_size == 0u)")
    out.append("        return;")
    out.append("    out[0] = '\\0';")
    out.append("    if (!path || path[0] == '\\0')")
    out.append("    {")
    out.append("        snprintf(out, out_size, \"rom\");")
    out.append("        return;")
    out.append("    }")
    out.append("    const char *base = path;")
    out.append("    for (const char *p = path; *p; ++p)")
    out.append("    {")
    out.append("        if (*p == '/' || *p == '\\\\')")
    out.append("            base = p + 1;")
    out.append("    }")
    out.append("    snprintf(out, out_size, \"%s\", base);")
    out.append("    char *dot = strrchr(out, '.');")
    out.append("    if (dot)")
    out.append("        *dot = '\\0';")
    out.append("    if (out[0] == '\\0')")
    out.append("        snprintf(out, out_size, \"rom\");")
    out.append("    for (char *p = out; *p; ++p)")
    out.append("    {")
    out.append("        unsigned char c = (unsigned char)*p;")
    out.append("        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-'))")
    out.append("            *p = '_';")
    out.append("    }")
    out.append("}")
    out.append("")
    out.append("static void recomp_default_artifact_path(const char *rom_path, const char *name, char *out, size_t out_size)")
    out.append("{")
    out.append("    char stem[260];")
    out.append("    recomp_path_to_stem(rom_path, stem, sizeof(stem));")
    out.append("    snprintf(out, out_size, \"%s/%s\", stem, name ? name : \"artifact.bin\");")
    out.append("}")
    out.append("")
    out.append("static uint32_t read_env_u32(const char *name, uint32_t fallback)")
    out.append("{")
    out.append("    const char *v = getenv(name);")
    out.append("    if (!v || v[0] == '\\0')")
    out.append("        return fallback;")
    out.append("    char *end = NULL;")
    out.append("    unsigned long parsed = strtoul(v, &end, 10);")
    out.append("    if (end == v || parsed > 0xFFFFFFFFul)")
    out.append("        return fallback;")
    out.append("    return (uint32_t)parsed;")
    out.append("}")
    out.append("")
    out.append("static uint64_t read_env_u64(const char *name, uint64_t fallback)")
    out.append("{")
    out.append("    const char *v = getenv(name);")
    out.append("    if (!v || v[0] == '\\0')")
    out.append("        return fallback;")
    out.append("    char *end = NULL;")
    out.append("    unsigned long long parsed = strtoull(v, &end, 10);")
    out.append("    if (end == v)")
    out.append("        return fallback;")
    out.append("    return (uint64_t)parsed;")
    out.append("}")
    out.append("")
    out.append("static bool recomp_set_env_str(const char *name, const char *value)")
    out.append("{")
    out.append("    if (!name || name[0] == '\\0' || !value)")
    out.append("        return false;")
    out.append("#if defined(_WIN32)")
    out.append("    return _putenv_s(name, value) == 0;")
    out.append("#else")
    out.append("    return setenv(name, value, 1) == 0;")
    out.append("#endif")
    out.append("}")
    out.append("")
    if embedded_chunks:
        out.append("typedef struct")
        out.append("{")
        out.append("    uint32_t offset;")
        out.append("    uint32_t size;")
        out.append("    const uint8_t *data;")
        out.append("} EmbeddedRomChunk;")
        out.append("")
        for idx, (_, chunk_data) in enumerate(embedded_chunks):
            out.extend(emit_u8_array_lines(f"k_embedded_rom_chunk_{idx}", chunk_data))
            out.append("")
        out.append("static const EmbeddedRomChunk k_embedded_rom_chunks[] = {")
        for idx, (start, chunk_data) in enumerate(embedded_chunks):
            out.append(f"    {{0x{start:08X}u, {len(chunk_data)}u, k_embedded_rom_chunk_{idx}}},")
        out.append("};")
        out.append("")
        out.append("static int load_embedded_rom(MemoryState *mem)")
        out.append("{")
        out.append(f"    const size_t rom_size = {embedded_rom_size}u;")
        out.append("    uint8_t *blob = (uint8_t *)malloc(rom_size);")
        out.append("    if (!blob)")
        out.append("        return -1;")
        out.append("    memset(blob, 0xFF, rom_size);")
        out.append("    for (size_t i = 0; i < (sizeof(k_embedded_rom_chunks) / sizeof(k_embedded_rom_chunks[0])); ++i)")
        out.append("    {")
        out.append("        const EmbeddedRomChunk *chunk = &k_embedded_rom_chunks[i];")
        out.append("        if ((size_t)chunk->offset + (size_t)chunk->size > rom_size)")
        out.append("            continue;")
        out.append("        memcpy(blob + chunk->offset, chunk->data, chunk->size);")
        out.append("    }")
        out.append("    int rc = load_rom_from_buffer(blob, rom_size, mem);")
        out.append("    free(blob);")
        out.append("    return rc;")
        out.append("}")
        out.append("")
    out.append("static void display_shutdown(DisplayContext *display)")
    out.append("{")
    out.append("    if (!display)")
    out.append("        return;")
    out.append("    if (display->texture)")
    out.append("    {")
    out.append("        SDL_DestroyTexture(display->texture);")
    out.append("        display->texture = NULL;")
    out.append("    }")
    out.append("    if (display->renderer)")
    out.append("    {")
    out.append("        SDL_DestroyRenderer(display->renderer);")
    out.append("        display->renderer = NULL;")
    out.append("    }")
    out.append("    if (display->window)")
    out.append("    {")
    out.append("        SDL_DestroyWindow(display->window);")
    out.append("        display->window = NULL;")
    out.append("    }")
    out.append("    if (display->initialized)")
    out.append("    {")
    out.append("        SDL_Quit();")
    out.append("        display->initialized = false;")
    out.append("    }")
    out.append("    display->enabled = false;")
    out.append("}")
    out.append("")
    out.append("static bool display_init(DisplayContext *display, int scale)")
    out.append("{")
    out.append("    if (!display || !display->enabled)")
    out.append("        return true;")
    out.append("    if (scale < 1)")
    out.append("        scale = 3;")
    out.append("    display->scale = scale;")
    out.append("    if (SDL_Init(SDL_INIT_VIDEO) < 0)")
    out.append("    {")
    out.append("        fprintf(stderr, \"SDL_Init failed: %s\\n\", SDL_GetError());")
    out.append("        display->enabled = false;")
    out.append("        return false;")
    out.append("    }")
    out.append("    display->initialized = true;")
    out.append("    display->window = SDL_CreateWindow(\"GB Recompiled\",")
    out.append("                                       GB_SCREEN_WIDTH * display->scale,")
    out.append("                                       GB_SCREEN_HEIGHT * display->scale,")
    out.append("                                       0);")
    out.append("    if (!display->window)")
    out.append("    {")
    out.append("        fprintf(stderr, \"SDL_CreateWindow failed: %s\\n\", SDL_GetError());")
    out.append("        display_shutdown(display);")
    out.append("        return false;")
    out.append("    }")
    out.append("    display->renderer = SDL_CreateRenderer(display->window, NULL);")
    out.append("    if (!display->renderer)")
    out.append("    {")
    out.append("        fprintf(stderr, \"SDL_CreateRenderer failed: %s\\n\", SDL_GetError());")
    out.append("        display_shutdown(display);")
    out.append("        return false;")
    out.append("    }")
    out.append("    SDL_SetRenderVSync(display->renderer, true);")
    out.append("    SDL_SetRenderDrawColor(display->renderer, 0.0f, 0.0f, 0.0f, 1.0f);")
    out.append("    display->texture = SDL_CreateTexture(display->renderer,")
    out.append("                                         SDL_PIXELFORMAT_ARGB8888,")
    out.append("                                         SDL_TEXTUREACCESS_STREAMING,")
    out.append("                                         GB_SCREEN_WIDTH,")
    out.append("                                         GB_SCREEN_HEIGHT);")
    out.append("    if (!display->texture)")
    out.append("    {")
    out.append("        fprintf(stderr, \"SDL_CreateTexture failed: %s\\n\", SDL_GetError());")
    out.append("        display_shutdown(display);")
    out.append("        return false;")
    out.append("    }")
    out.append("    SDL_SetTextureScaleMode(display->texture, SDL_SCALEMODE_NEAREST);")
    out.append("    return true;")
    out.append("}")
    out.append("")
    out.append("static void handle_key_event(MemoryState *memory, SDL_Keycode key, bool pressed)")
    out.append("{")
    out.append("    switch (key)")
    out.append("    {")
    out.append("    case SDLK_RIGHT:")
    out.append("        memory_set_button_state(memory, JOYPAD_RIGHT, pressed);")
    out.append("        break;")
    out.append("    case SDLK_LEFT:")
    out.append("        memory_set_button_state(memory, JOYPAD_LEFT, pressed);")
    out.append("        break;")
    out.append("    case SDLK_UP:")
    out.append("        memory_set_button_state(memory, JOYPAD_UP, pressed);")
    out.append("        break;")
    out.append("    case SDLK_DOWN:")
    out.append("        memory_set_button_state(memory, JOYPAD_DOWN, pressed);")
    out.append("        break;")
    out.append("    case SDLK_A:")
    out.append("        memory_set_button_state(memory, JOYPAD_A, pressed);")
    out.append("        break;")
    out.append("    case SDLK_Z:")
    out.append("        memory_set_button_state(memory, JOYPAD_B, pressed);")
    out.append("        break;")
    out.append("    case SDLK_Q:")
    out.append("        memory_set_button_state(memory, JOYPAD_START, pressed);")
    out.append("        break;")
    out.append("    case SDLK_S:")
    out.append("        memory_set_button_state(memory, JOYPAD_SELECT, pressed);")
    out.append("        break;")
    out.append("    default:")
    out.append("        break;")
    out.append("    }")
    out.append("}")
    out.append("")
    out.append("static bool display_poll_events(DisplayContext *display, MemoryState *memory)")
    out.append("{")
    out.append("    if (!display || !display->enabled)")
    out.append("        return true;")
    out.append("    SDL_Event event;")
    out.append("    while (SDL_PollEvent(&event))")
    out.append("    {")
    out.append("        switch (event.type)")
    out.append("        {")
    out.append("        case SDL_EVENT_QUIT:")
    out.append("            return false;")
    out.append("        case SDL_EVENT_KEY_DOWN:")
    out.append("            if (event.key.repeat)")
    out.append("                break;")
    out.append("            if (event.key.key == SDLK_ESCAPE)")
    out.append("                return false;")
    out.append("            handle_key_event(memory, event.key.key, true);")
    out.append("            break;")
    out.append("        case SDL_EVENT_KEY_UP:")
    out.append("            handle_key_event(memory, event.key.key, false);")
    out.append("            break;")
    out.append("        default:")
    out.append("            break;")
    out.append("        }")
    out.append("    }")
    out.append("    return true;")
    out.append("}")
    out.append("")
    out.append("static void display_present(DisplayContext *display, const PPUState *ppu)")
    out.append("{")
    out.append("    if (!display || !display->enabled || !display->texture || !display->renderer)")
    out.append("        return;")
    out.append("    if (SDL_UpdateTexture(display->texture, NULL, ppu->fb, GB_SCREEN_WIDTH * (int)sizeof(uint32_t)) < 0)")
    out.append("    {")
    out.append("        fprintf(stderr, \"SDL_UpdateTexture failed: %s\\n\", SDL_GetError());")
    out.append("        return;")
    out.append("    }")
    out.append("    SDL_RenderClear(display->renderer);")
    out.append("    SDL_FRect dst = {")
    out.append("        0.0f,")
    out.append("        0.0f,")
    out.append("        (float)(GB_SCREEN_WIDTH * display->scale),")
    out.append("        (float)(GB_SCREEN_HEIGHT * display->scale)};")
    out.append("    SDL_RenderTexture(display->renderer, display->texture, NULL, &dst);")
    out.append("    SDL_RenderPresent(display->renderer);")
    out.append("}")
    out.append("")
    out.append("static size_t recomp_effective_bank(const MemoryState *mem, uint16_t address)")
    out.append("{")
    out.append("    if (!mem || address >= 0x8000u)")
    out.append("        return 0u;")
    out.append("    if (address < 0x4000u)")
    out.append("        return 0u;")
    out.append("    const CartridgeState *cart = &mem->cartridge;")
    out.append("    size_t bank = 1u;")
    out.append("    switch (cart->mbc_type)")
    out.append("    {")
    out.append("    case MBC1:")
    out.append("    {")
    out.append("        uint8_t low = (uint8_t)(cart->mbc1_low5 & 0x1Fu);")
    out.append("        if (low == 0)")
    out.append("            low = 1;")
    out.append("        bank = (size_t)(low | ((cart->mbc1_high2 & 0x03u) << 5u));")
    out.append("        break;")
    out.append("    }")
    out.append("    case MBC2:")
    out.append("        bank = (size_t)(cart->mbc2_rom_bank & 0x0Fu);")
    out.append("        if (bank == 0)")
    out.append("            bank = 1;")
    out.append("        break;")
    out.append("    case MBC3:")
    out.append("        bank = (size_t)(cart->mbc3_rom_bank & 0x7Fu);")
    out.append("        if (bank == 0)")
    out.append("            bank = 1;")
    out.append("        break;")
    out.append("    case MBC5:")
    out.append("        bank = (size_t)(cart->mbc5_rom_bank & 0x01FFu);")
    out.append("        break;")
    out.append("    case MBC_NONE:")
    out.append("    default:")
    out.append("        bank = 1u;")
    out.append("        break;")
    out.append("    }")
    out.append("    if (cart->rom_banks > 0)")
    out.append("        bank %= cart->rom_banks;")
    out.append("    return bank;")
    out.append("}")
    out.append("")
    out.append("uint32_t recomp_make_pc_key(const MemoryState *mem, uint16_t pc)")
    out.append("{")
    out.append("    size_t bank = recomp_effective_bank(mem, pc);")
    out.append("    return (uint32_t)(((uint32_t)(bank & 0xFFFFu) << 16) | pc);")
    out.append("}")
    out.append("")
    out.append("void recomp_after_step(uint32_t cycles)")
    out.append("{")
    out.append("    ppu_step(&g_ppu, cycles);")
    out.append("    g_instr_count++;")
    out.append("    if (g_display.enabled && (g_instr_count & 0x1FFFULL) == 0ULL)")
    out.append("    {")
    out.append("        if (!display_poll_events(&g_display, &g_memory))")
    out.append("            g_running = false;")
    out.append("    }")
    out.append("    if (g_ppu.frame_ready)")
    out.append("    {")
    out.append("        g_frame_count++;")
    out.append("        if (g_display.enabled)")
    out.append("        {")
    out.append("            if (!display_poll_events(&g_display, &g_memory))")
    out.append("                g_running = false;")
    out.append("            display_present(&g_display, &g_ppu);")
    out.append("        }")
    out.append("        g_ppu.frame_ready = false;")
    out.append("    }")
    out.append("    if ((g_max_instr > 0ULL && g_instr_count >= g_max_instr) ||")
    out.append("        (g_max_frames > 0ULL && g_frame_count >= g_max_frames))")
    out.append("        g_running = false;")
    out.append("    if (g_runtime_read_trace_enabled)")
    out.append("    {")
    out.append("        time_t now = time(NULL);")
    out.append("        if (now != g_runtime_read_trace_last_flush)")
    out.append("        {")
    out.append("            g_runtime_read_trace_last_flush = now;")
    out.append("            memory_flush_rom_read_trace();")
    out.append("        }")
    out.append("    }")
    out.append("}")
    out.append("")
    out.append("static bool recomp_is_rom_exec_pc(uint16_t pc)")
    out.append("{")
    out.append("    return pc < 0x8000u;")
    out.append("}")
    out.append("")
    out.append("static bool recomp_mark_missing_logged(uint16_t pc)")
    out.append("{")
    out.append("    uint16_t idx = (uint16_t)(pc >> 3);")
    out.append("    uint8_t mask = (uint8_t)(1u << (pc & 7u));")
    out.append("    if ((g_missing_dyn_log[idx] & mask) != 0)")
    out.append("        return false;")
    out.append("    g_missing_dyn_log[idx] |= mask;")
    out.append("    return true;")
    out.append("}")
    out.append("")
    out.append("static void recomp_missing_log_key(const char *kind, uint32_t key, uint16_t pc)")
    out.append("{")
    out.append("    if (!g_missing_keys_file)")
    out.append("        return;")
    out.append("    fprintf(g_missing_keys_file, \"%s %08X %04X\\n\", kind ? kind : \"UNK\", key, pc);")
    out.append("    fflush(g_missing_keys_file);")
    out.append("}")
    out.append("")
    out.append("static bool recomp_try_dynamic_missing_step(uint32_t key)")
    out.append("{")
    out.append("    bool is_rom = recomp_is_rom_exec_pc(g_cpu.PC);")
    out.append("    if (is_rom && !g_allow_rom_fallback)")
    out.append("        return false;")
    out.append("    recomp_missing_log_key(is_rom ? \"ROM\" : \"RAM\", key, g_cpu.PC);")
    out.append("    if (!is_rom)")
    out.append("        recomp_probe_dyn_dump_code(&g_cpu, g_cpu.PC, \"DYN_FALLBACK\");")
    out.append("    if (recomp_mark_missing_logged(g_cpu.PC))")
    out.append("    {")
    out.append("        fprintf(stderr, \"[RECOMPILED][DYN-%s] interpreter fallback key=%08X pc=%04X op=%02X\\n\",")
    out.append("                is_rom ? \"ROM\" : \"RAM\",")
    out.append("                key,")
    out.append("                g_cpu.PC,")
    out.append("                memory_read(&g_memory, g_cpu.PC));")
    out.append("    }")
    out.append("    cpu_set_illegal_opcode_softfail(1);")
    out.append("    uint32_t cycles = cpu_execute_instruction(&g_cpu);")
    out.append("    cpu_set_illegal_opcode_softfail(0);")
    out.append("    recomp_after_step(cycles);")
    out.append("    return true;")
    out.append("}")
    out.append("")
    out.append("static bool recomp_try_quarantined_interp_step(void)")
    out.append("{")
    out.append("    cpu_set_illegal_opcode_softfail(1);")
    out.append("    uint32_t cycles = cpu_execute_instruction(&g_cpu);")
    out.append("    cpu_set_illegal_opcode_softfail(0);")
    out.append("    recomp_after_step(cycles);")
    out.append("    return true;")
    out.append("}")
    out.append("")
    out.append("static void recomp_apply_post_boot_dmg_state(CPUState *cpu, MemoryState *mem)")
    out.append("{")
    out.append("    if (!cpu || !mem)")
    out.append("        return;")
    out.append("    cpu->A = 0x01;")
    out.append("    cpu->F = 0xB0;")
    out.append("    cpu->B = 0x00;")
    out.append("    cpu->C = 0x13;")
    out.append("    cpu->D = 0x00;")
    out.append("    cpu->E = 0xD8;")
    out.append("    cpu->H = 0x01;")
    out.append("    cpu->L = 0x4D;")
    out.append("    cpu->SP = 0xFFFE;")
    out.append("    cpu->PC = 0x0100;")
    out.append("    mem->memory.TIMA = 0x00;")
    out.append("    mem->memory.TMA = 0x00;")
    out.append("    mem->memory.TAC = 0x00;")
    out.append("    mem->memory.IF = 0xE1;")
    out.append("    mem->memory.IE = 0x00;")
    out.append("    mem->memory.NR10 = 0x80;")
    out.append("    mem->memory.NR11 = 0xBF;")
    out.append("    mem->memory.NR12 = 0xF3;")
    out.append("    mem->memory.NR14 = 0xBF;")
    out.append("    mem->memory.NR21 = 0x3F;")
    out.append("    mem->memory.NR22 = 0x00;")
    out.append("    mem->memory.NR24 = 0xBF;")
    out.append("    mem->memory.NR30 = 0x7F;")
    out.append("    mem->memory.NR31 = 0xFF;")
    out.append("    mem->memory.NR32 = 0x9F;")
    out.append("    mem->memory.NR33 = 0xBF;")
    out.append("    mem->memory.NR41 = 0xFF;")
    out.append("    mem->memory.NR42 = 0x00;")
    out.append("    mem->memory.NR43 = 0x00;")
    out.append("    mem->memory.NR44 = 0xBF;")
    out.append("    mem->memory.NR50 = 0x77;")
    out.append("    mem->memory.NR51 = 0xF3;")
    out.append("    mem->memory.NR52 = 0xF1;")
    out.append("    mem->memory.BOOT = 0x01;")
    out.append("    mem->bios_enabled = false;")
    out.append("}")
    out.append("")
    out.append("static inline uint8_t recomp_imm8(CPUState *cpu, uint8_t value)")
    out.append("{")
    out.append("    cpu->PC++;")
    out.append("    return value;")
    out.append("}")
    out.append("")
    out.append("static inline void recomp_ld_r16_n16(uint16_t *dest, CPUState *cpu, uint16_t value)")
    out.append("{")
    out.append("    cpu->PC += 2;")
    out.append("    *dest = value;")
    out.append("    cpu->cycle_count += 3;")
    out.append("}")
    out.append("")
    out.append("static inline void recomp_ld_n16_sp(CPUState *cpu, uint16_t addr)")
    out.append("{")
    out.append("    cpu->PC += 2;")
    out.append("    memory_write(cpu->memory, addr, (uint8_t)(cpu->SP & 0x00FFu));")
    out.append("    memory_write(cpu->memory, (uint16_t)(addr + 1u), (uint8_t)((cpu->SP >> 8) & 0x00FFu));")
    out.append("    cpu->cycle_count += 5;")
    out.append("}")
    out.append("")
    out.append("static inline void recomp_ld_a_n16(CPUState *cpu, uint16_t addr)")
    out.append("{")
    out.append("    cpu->PC += 2;")
    out.append("    cpu->A = memory_read(cpu->memory, addr);")
    out.append("    cpu->cycle_count += 4;")
    out.append("}")
    out.append("")
    out.append("static inline void recomp_ld_hl_sp_e8(CPUState *cpu, int8_t offset)")
    out.append("{")
    out.append("    cpu->PC += 1;")
    out.append("    uint16_t sp = cpu->SP;")
    out.append("    uint16_t result = (uint16_t)(sp + offset);")
    out.append("    cpu->HL = result;")
    out.append("    cpu->F = 0;")
    out.append("    if (((sp & 0x0Fu) + (((uint16_t)offset) & 0x0Fu)) > 0x0Fu)")
    out.append("        cpu->F |= FLAG_H;")
    out.append("    if (((sp & 0xFFu) + (((uint16_t)offset) & 0xFFu)) > 0xFFu)")
    out.append("        cpu->F |= FLAG_C;")
    out.append("    cpu->cycle_count += 3;")
    out.append("}")
    out.append("")
    out.append("static inline void recomp_ldh_a_a8(CPUState *cpu, uint8_t imm)")
    out.append("{")
    out.append("    cpu->PC += 1;")
    out.append("    cpu->A = memory_read(cpu->memory, (uint16_t)(0xFF00u + imm));")
    out.append("    cpu->cycle_count += 3;")
    out.append("}")
    out.append("")
    out.append("static inline void recomp_add_sp_e8(CPUState *cpu, int8_t value)")
    out.append("{")
    out.append("    cpu->PC += 1;")
    out.append("    uint16_t old_sp = cpu->SP;")
    out.append("    cpu->SP = (uint16_t)(cpu->SP + value);")
    out.append("    cpu->F = 0;")
    out.append("    if (((old_sp & 0x0Fu) + (((uint16_t)value) & 0x0Fu)) > 0x0Fu)")
    out.append("        cpu->F |= FLAG_H;")
    out.append("    if (((old_sp & 0xFFu) + (((uint16_t)value) & 0xFFu)) > 0xFFu)")
    out.append("        cpu->F |= FLAG_C;")
    out.append("    cpu->cycle_count += 4;")
    out.append("}")
    out.append("")
    out.append("static inline void recomp_call_n16(CPUState *cpu, uint16_t addr)")
    out.append("{")
    out.append("    cpu->PC += 2;")
    out.append("    memory_write(cpu->memory, --cpu->SP, (uint8_t)((cpu->PC >> 8) & 0x00FFu));")
    out.append("    memory_write(cpu->memory, --cpu->SP, (uint8_t)(cpu->PC & 0x00FFu));")
    out.append("    cpu->PC = addr;")
    out.append("    cpu->cycle_count += 6;")
    out.append("}")
    out.append("")
    out.append("static inline void recomp_call_cc_n16(bool condition, CPUState *cpu, uint16_t addr)")
    out.append("{")
    out.append("    if (condition)")
    out.append("        recomp_call_n16(cpu, addr);")
    out.append("    else")
    out.append("    {")
    out.append("        cpu->PC += 2;")
    out.append("        cpu->cycle_count += 3;")
    out.append("    }")
    out.append("}")
    out.append("")
    out.append("static inline void recomp_jp_n16(CPUState *cpu, uint16_t addr)")
    out.append("{")
    out.append("    cpu->PC += 2;")
    out.append("    cpu->PC = addr;")
    out.append("    cpu->cycle_count += 4;")
    out.append("}")
    out.append("")
    out.append("static inline void recomp_jp_cc_n16(bool condition, CPUState *cpu, uint16_t addr)")
    out.append("{")
    out.append("    if (condition)")
    out.append("    {")
    out.append("        recomp_jp_n16(cpu, addr);")
    out.append("        return;")
    out.append("    }")
    out.append("    cpu->PC += 2;")
    out.append("    cpu->cycle_count += 3;")
    out.append("}")
    out.append("")
    out.append("#define RECOMP_BEGIN_STEP(OPCODE_LITERAL) \\")
    out.append("    do                                      \\")
    out.append("    {                                       \\")
    out.append("        CPUState *cpu = &g_cpu;             \\")
    out.append("        CPUDecodedStep step = {0};          \\")
    out.append("        uint32_t cycles = cpu_decoded_step_begin(cpu, (uint8_t)(OPCODE_LITERAL), &step); \\")
    out.append("        if (cycles == 0u)                   \\")
    out.append("        {")
    out.append("")
    out.append("#define RECOMP_END_STEP()                       \\")
    out.append("            cycles = cpu_decoded_step_end(cpu, &step); \\")
    out.append("        }                                       \\")
    out.append("        recomp_after_step(cycles);              \\")
    out.append("    } while (0)")
    out.append("")

    if codegen_state is None:
        insn_by_key, dynram_dispatch_entries, owned_keys_by_symbol = build_codegen_state(funcs, owner_map)
    else:
        insn_by_key, dynram_dispatch_entries, owned_keys_by_symbol = codegen_state

    active_symbols: Set[str] = {sym for _, sym in owner_map}
    for fn in funcs:
        if fn.is_dynram:
            active_symbols.add(fn.symbol)
    for fn in funcs:
        if fn.symbol in active_symbols:
            out.append(f"void {fn.symbol}(void);")
    out.append("")

    out.append("typedef struct")
    out.append("{")
    out.append("    uint32_t key;")
    out.append("    RecompFn fn;")
    out.append("} DispatchEntry;")
    out.append("")
    out.append("typedef struct")
    out.append("{")
    out.append("    uint32_t key;")
    out.append("    uint32_t hash;")
    out.append("    RecompFn fn;")
    out.append("} DynRamDispatchEntry;")
    out.append("")

    out.append("static const DispatchEntry k_dispatch_map[] = {")
    for key, sym in owner_map:
        out.append(f"    {{0x{key:08X}u, {sym}}},")
    out.append("};")
    out.append("")
    out.append("static const size_t k_dispatch_count = sizeof(k_dispatch_map) / sizeof(k_dispatch_map[0]);")
    out.append("static uint8_t g_dispatch_quarantine_bits[(sizeof(k_dispatch_map) / sizeof(k_dispatch_map[0]) + 7u) / 8u] = {0};")
    out.append("")
    if dynram_dispatch_entries:
        out.append("static const DynRamDispatchEntry k_dynram_dispatch_map[] = {")
        for key, h, sym in sorted(dynram_dispatch_entries, key=lambda item: (item[0], item[1], item[2])):
            out.append(f"    {{0x{key:08X}u, 0x{h:08X}u, {sym}}},")
        out.append("};")
        out.append("static const size_t k_dynram_dispatch_count = sizeof(k_dynram_dispatch_map) / sizeof(k_dynram_dispatch_map[0]);")
    else:
        out.append("static const DynRamDispatchEntry k_dynram_dispatch_map[1] = {{0u, 0u, NULL}};")
        out.append("static const size_t k_dynram_dispatch_count = 0u;")
    out.append("")
    out.append("static bool recomp_dispatch_quarantine_test(size_t idx);")
    out.append("static bool recomp_dispatch_quarantine_set(size_t idx);")
    out.append("")
    out.append("static bool recomp_parse_key_line(const char *line, uint32_t *out_key)")
    out.append("{")
    out.append("    if (!line || !out_key)")
    out.append("        return false;")
    out.append("    const char *p = line;")
    out.append("    while (*p == ' ' || *p == '\\t')")
    out.append("        ++p;")
    out.append("    if (*p == '\\0' || *p == '\\r' || *p == '\\n' || *p == '#')")
    out.append("        return false;")
    out.append("    unsigned bank = 0u, addr = 0u, key = 0u;")
    out.append("    if (sscanf(p, \"%x:%x\", &bank, &addr) == 2 || sscanf(p, \"%x %x\", &bank, &addr) == 2)")
    out.append("    {")
    out.append("        if (addr >= 0x8000u)")
    out.append("            return false;")
    out.append("        *out_key = (((uint32_t)bank & 0xFFFFu) << 16) | ((uint32_t)addr & 0xFFFFu);")
    out.append("        return true;")
    out.append("    }")
    out.append("    if (sscanf(p, \"%x\", &key) == 1)")
    out.append("    {")
    out.append("        if ((key & 0xFFFFu) >= 0x8000u)")
    out.append("            return false;")
    out.append("        *out_key = (uint32_t)key;")
    out.append("        return true;")
    out.append("    }")
    out.append("    return false;")
    out.append("}")
    out.append("")
    out.append("static bool recomp_fn_in_list(RecompFn fn, const RecompFn *list, size_t count)")
    out.append("{")
    out.append("    if (!fn || !list)")
    out.append("        return false;")
    out.append("    for (size_t i = 0; i < count; ++i)")
    out.append("    {")
    out.append("        if (list[i] == fn)")
    out.append("            return true;")
    out.append("    }")
    out.append("    return false;")
    out.append("}")
    out.append("")
    out.append("static bool recomp_apply_trusted_functions_file(const char *path)")
    out.append("{")
    out.append("    if (!path || path[0] == '\\0')")
    out.append("        return false;")
    out.append("    FILE *f = fopen(path, \"r\");")
    out.append("    if (!f)")
    out.append("        return false;")
    out.append("    RecompFn *trusted = NULL;")
    out.append("    size_t trusted_count = 0u;")
    out.append("    size_t trusted_cap = 0u;")
    out.append("    char line[256];")
    out.append("    while (fgets(line, sizeof(line), f))")
    out.append("    {")
    out.append("        uint32_t key = 0u;")
    out.append("        if (!recomp_parse_key_line(line, &key))")
    out.append("            continue;")
    out.append("        RecompFn fn = NULL;")
    out.append("        for (size_t i = 0; i < k_dispatch_count; ++i)")
    out.append("        {")
    out.append("            if (k_dispatch_map[i].key == key)")
    out.append("            {")
    out.append("                fn = k_dispatch_map[i].fn;")
    out.append("                break;")
    out.append("            }")
    out.append("        }")
    out.append("        if (!fn || recomp_fn_in_list(fn, trusted, trusted_count))")
    out.append("            continue;")
    out.append("        if (trusted_count == trusted_cap)")
    out.append("        {")
    out.append("            size_t next_cap = (trusted_cap == 0u) ? 64u : (trusted_cap * 2u);")
    out.append("            RecompFn *next = (RecompFn *)realloc(trusted, next_cap * sizeof(RecompFn));")
    out.append("            if (!next)")
    out.append("                break;")
    out.append("            trusted = next;")
    out.append("            trusted_cap = next_cap;")
    out.append("        }")
    out.append("        trusted[trusted_count++] = fn;")
    out.append("    }")
    out.append("    fclose(f);")
    out.append("    g_trust_only_trusted_fn_count = (uint64_t)trusted_count;")
    out.append("    g_trust_only_quarantined_key_count = 0u;")
    out.append("    for (size_t i = 0; i < k_dispatch_count; ++i)")
    out.append("    {")
    out.append("        if (recomp_fn_in_list(k_dispatch_map[i].fn, trusted, trusted_count))")
    out.append("            continue;")
    out.append("        if (recomp_dispatch_quarantine_set(i))")
    out.append("            g_trust_only_quarantined_key_count++;")
    out.append("    }")
    out.append("    free(trusted);")
    out.append("    return true;")
    out.append("}")
    out.append("")
    out.append("static RecompFn recomp_lookup_dynram_variant(uint32_t key)")
    out.append("{")
    out.append("    uint16_t pc = (uint16_t)(key & 0xFFFFu);")
    out.append("    if (pc < 0x8000u || k_dynram_dispatch_count == 0u)")
    out.append("        return NULL;")
    out.append("    uint32_t hash = recomp_probe_dyn_body_hash(&g_cpu, pc);")
    out.append("    size_t lo = 0u;")
    out.append("    size_t hi = k_dynram_dispatch_count;")
    out.append("    while (lo < hi)")
    out.append("    {")
    out.append("        size_t mid = lo + ((hi - lo) / 2u);")
    out.append("        uint32_t mkey = k_dynram_dispatch_map[mid].key;")
    out.append("        if (mkey < key)")
    out.append("            lo = mid + 1u;")
    out.append("        else")
    out.append("            hi = mid;")
    out.append("    }")
    out.append("    for (size_t i = lo; i < k_dynram_dispatch_count && k_dynram_dispatch_map[i].key == key; ++i)")
    out.append("    {")
    out.append("        const DynRamDispatchEntry *sig = &k_dynram_dispatch_map[i];")
    out.append("        if (sig->hash == hash)")
    out.append("            return sig->fn;")
    out.append("    }")
    out.append("    return NULL;")
    out.append("}")
    out.append("")
    out.append("static RecompFn recomp_lookup(uint32_t key)")
    out.append("{")
    out.append("    g_last_lookup_quarantined = false;")
    out.append("    uint16_t pc = (uint16_t)(key & 0xFFFFu);")
    out.append("    if (pc >= 0x8000u)")
    out.append("    {")
    out.append("        if (!g_allow_dynram_dispatch)")
    out.append("            return NULL;")
    out.append("        return recomp_lookup_dynram_variant(key);")
    out.append("    }")
    out.append("    size_t lo = 0;")
    out.append("    size_t hi = k_dispatch_count;")
    out.append("    while (lo < hi)")
    out.append("    {")
    out.append("        size_t mid = lo + ((hi - lo) / 2u);")
    out.append("        uint32_t mkey = k_dispatch_map[mid].key;")
    out.append("        if (mkey < key)")
    out.append("            lo = mid + 1u;")
    out.append("        else")
    out.append("            hi = mid;")
    out.append("    }")
    out.append("    if (lo < k_dispatch_count && k_dispatch_map[lo].key == key)")
    out.append("    {")
    out.append("        if (recomp_dispatch_quarantine_test(lo))")
    out.append("        {")
    out.append("            g_last_lookup_quarantined = true;")
    out.append("            return NULL;")
    out.append("        }")
    out.append("        return k_dispatch_map[lo].fn;")
    out.append("    }")
    out.append("    return NULL;")
    out.append("}")
    out.append("")
    emit_fuzz_verify_block(out, len(funcs))

    if emit_function_bodies:
        emit_function_definitions(out, funcs, owned_keys_by_symbol, insn_by_key, opcode_bodies, inline_helpers)

    out.append("int main(int argc, char **argv)")
    out.append("{")
    out.append("    const char *default_rom_path = \"" + default_rom.replace("\\", "\\\\") + "\";")
    out.append("    const char *rom_path = default_rom_path;")
    out.append("    bool rom_arg_set = false;")
    out.append("    bool display_enabled = !read_env_enabled(\"GB_RECOMP_NO_DISPLAY\");")
    out.append("    bool fuzz_verify = false;")
    out.append("    uint32_t fuzz_iters_per_key = 64u;")
    out.append("    uint32_t fuzz_seed = 0u;")
    out.append("    uint32_t fuzz_max_keys = 0u;")
    out.append("    bool shadow_diff = false;")
    out.append("    bool shadow_quarantine = false;")
    out.append("    bool trust_only = false;")
    out.append("    bool runtime_discover = false;")
    out.append("    bool runtime_read_trace = false;")
    out.append("    bool runtime_input_profile = false;")
    out.append("    bool runtime_input_snapshots = false;")
    out.append("    const char *runtime_read_trace_cli_path = NULL;")
    out.append("    const char *trusted_funcs_cli_path = NULL;")
    out.append("    uint32_t display_scale = read_env_u32(\"GB_RECOMP_SCALE\", 3u);")
    out.append("    if (display_scale == 0)")
    out.append("        display_scale = 3u;")
    out.append("")
    out.append("    for (int i = 1; i < argc; ++i)")
    out.append("    {")
    out.append("        if (strcmp(argv[i], \"--no-display\") == 0)")
    out.append("            display_enabled = false;")
    out.append("        else if (strcmp(argv[i], \"--display\") == 0)")
    out.append("            display_enabled = true;")
    out.append("        else if (strcmp(argv[i], \"--fuzz-verify\") == 0)")
    out.append("            fuzz_verify = true;")
    out.append("        else if (strcmp(argv[i], \"--fuzz-iters\") == 0 && (i + 1) < argc)")
    out.append("            fuzz_iters_per_key = (uint32_t)strtoul(argv[++i], NULL, 10);")
    out.append("        else if (strcmp(argv[i], \"--fuzz-seed\") == 0 && (i + 1) < argc)")
    out.append("            fuzz_seed = (uint32_t)strtoul(argv[++i], NULL, 10);")
    out.append("        else if (strcmp(argv[i], \"--fuzz-max-keys\") == 0 && (i + 1) < argc)")
    out.append("            fuzz_max_keys = (uint32_t)strtoul(argv[++i], NULL, 10);")
    out.append("        else if (strcmp(argv[i], \"--shadow-diff\") == 0)")
    out.append("            shadow_diff = true;")
    out.append("        else if (strcmp(argv[i], \"--shadow-diff-every\") == 0 && (i + 1) < argc)")
    out.append("            g_shadow_diff_every = (uint32_t)strtoul(argv[++i], NULL, 10);")
    out.append("        else if (strcmp(argv[i], \"--shadow-diff-max-reports\") == 0 && (i + 1) < argc)")
    out.append("            g_shadow_diff_max_reports = (uint64_t)strtoull(argv[++i], NULL, 10);")
    out.append("        else if (strcmp(argv[i], \"--shadow-diff-stop\") == 0)")
    out.append("            g_shadow_diff_stop_on_mismatch = true;")
    out.append("        else if (strcmp(argv[i], \"--shadow-quarantine\") == 0)")
    out.append("            shadow_quarantine = true;")
    out.append("        else if (strcmp(argv[i], \"--trust-only\") == 0)")
    out.append("            trust_only = true;")
    out.append("        else if (strcmp(argv[i], \"--trusted-funcs\") == 0 && (i + 1) < argc)")
    out.append("            trusted_funcs_cli_path = argv[++i];")
    out.append("        else if (strcmp(argv[i], \"--runtime-discover\") == 0)")
    out.append("            runtime_discover = true;")
    out.append("        else if (strcmp(argv[i], \"--runtime-read-trace\") == 0)")
    out.append("            runtime_read_trace = true;")
    out.append("        else if (strcmp(argv[i], \"--runtime-input-profile\") == 0)")
    out.append("            runtime_input_profile = true;")
    out.append("        else if (strcmp(argv[i], \"--runtime-input-snapshots\") == 0)")
    out.append("            runtime_input_snapshots = true;")
    out.append("        else if (strcmp(argv[i], \"--runtime-read-trace-path\") == 0 && (i + 1) < argc)")
    out.append("        {")
    out.append("            runtime_read_trace = true;")
    out.append("            runtime_read_trace_cli_path = argv[++i];")
    out.append("        }")
    out.append("        else if (strcmp(argv[i], \"--runtime-learn\") == 0)")
    out.append("        {")
    out.append("            runtime_discover = true;")
    out.append("            runtime_read_trace = true;")
    out.append("            runtime_input_profile = true;")
    out.append("            runtime_input_snapshots = true;")
    out.append("        }")
    out.append("        else if (strcmp(argv[i], \"--no-dynram\") == 0)")
    out.append("            g_allow_dynram_dispatch = false;")
    out.append("        else if (strcmp(argv[i], \"--dynram\") == 0)")
    out.append("            g_allow_dynram_dispatch = true;")
    out.append("        else if (argv[i][0] != '-' && argv[i][0] != '\\0')")
    out.append("        {")
    out.append("            rom_path = argv[i];")
    out.append("            rom_arg_set = true;")
    out.append("        }")
    out.append("    }")
    out.append("")
    out.append("    g_max_instr = read_env_u64(\"GB_RECOMP_MAX_INSTR\", g_max_instr);")
    out.append("    g_max_frames = read_env_u64(\"GB_RECOMP_MAX_FRAMES\", g_max_frames);")
    out.append("    g_allow_rom_fallback = !read_env_enabled(\"GB_RECOMP_STRICT_MISSING\");")
    out.append("    if (read_env_enabled(\"GB_RECOMP_NO_DYNRAM\"))")
    out.append("        g_allow_dynram_dispatch = false;")
    out.append("    if (read_env_enabled(\"GB_RECOMP_SHADOW_DIFF\"))")
    out.append("        shadow_diff = true;")
    out.append("    if (read_env_enabled(\"GB_RECOMP_SHADOW_DIFF_STOP\"))")
    out.append("        g_shadow_diff_stop_on_mismatch = true;")
    out.append("    if (read_env_enabled(\"GB_RECOMP_SHADOW_QUARANTINE\"))")
    out.append("        shadow_quarantine = true;")
    out.append("    if (read_env_enabled(\"GB_RECOMP_TRUST_ONLY\"))")
    out.append("        trust_only = true;")
    out.append("    if (read_env_enabled(\"GB_RECOMP_RUNTIME_DISCOVER\"))")
    out.append("        runtime_discover = true;")
    out.append("    if (read_env_enabled(\"GB_RECOMP_RUNTIME_READ_TRACE\"))")
    out.append("        runtime_read_trace = true;")
    out.append("    if (read_env_enabled(\"GB_RECOMP_INPUT_PROFILE\"))")
    out.append("        runtime_input_profile = true;")
    out.append("    if (read_env_enabled(\"GB_RECOMP_INPUT_SNAPSHOTS\") || read_env_enabled(\"GB_RECOMP_INPUT_SAMPLES\"))")
    out.append("        runtime_input_snapshots = true;")
    out.append("    if (runtime_input_profile || runtime_input_snapshots)")
    out.append("        runtime_discover = true;")
    out.append("    g_shadow_diff_every = read_env_u32(\"GB_RECOMP_SHADOW_DIFF_EVERY\", g_shadow_diff_every);")
    out.append("    {")
    out.append("        uint64_t tmp = read_env_u64(\"GB_RECOMP_SHADOW_DIFF_MAX_REPORTS\", g_shadow_diff_max_reports);")
    out.append("        g_shadow_diff_max_reports = tmp;")
    out.append("    }")
    out.append("    if (g_shadow_diff_every == 0u)")
    out.append("        g_shadow_diff_every = 1u;")
    out.append("    fuzz_iters_per_key = read_env_u32(\"GB_RECOMP_FUZZ_ITERS\", fuzz_iters_per_key);")
    out.append("    if (fuzz_seed == 0u)")
    out.append("        fuzz_seed = read_env_u32(\"GB_RECOMP_FUZZ_SEED\", fuzz_seed);")
    out.append("    fuzz_max_keys = read_env_u32(\"GB_RECOMP_FUZZ_MAX_KEYS\", fuzz_max_keys);")
    out.append("    const char *missing_keys_path = getenv(\"GB_RECOMP_MISSING_KEYS_PATH\");")
    out.append("    const char *quarantine_keys_path = getenv(\"GB_RECOMP_QUARANTINE_KEYS_PATH\");")
    out.append("    const char *trusted_funcs_env_path = getenv(\"GB_RECOMP_TRUSTED_FUNCS_FILE\");")
    out.append("    const char *runtime_read_trace_env_path = getenv(\"GB_RECOMP_RUNTIME_READ_TRACE_PATH\");")
    out.append("    char missing_keys_default_path[512] = {0};")
    out.append("    char quarantine_keys_default_path[512] = {0};")
    out.append("    char trusted_funcs_default_path[512] = {0};")
    out.append("    char runtime_read_trace_default_path[512] = {0};")
    out.append("")
    out.append("    if (runtime_read_trace)")
    out.append("    {")
    out.append("        const char *trace_path = runtime_read_trace_cli_path;")
    out.append("        if (!trace_path || trace_path[0] == '\\0')")
    out.append("            trace_path = runtime_read_trace_env_path;")
    out.append("        if (!trace_path || trace_path[0] == '\\0')")
    out.append("        {")
    out.append("            recomp_default_artifact_path(rom_path, \"rom_reads.txt\", runtime_read_trace_default_path, sizeof(runtime_read_trace_default_path));")
    out.append("            trace_path = runtime_read_trace_default_path;")
    out.append("        }")
    out.append("        if (trace_path && trace_path[0] != '\\0')")
    out.append("            (void)recomp_set_env_str(\"GB_ROM_READ_TRACE_PATH\", trace_path);")
    out.append("    }")
    out.append("    if (runtime_discover)")
    out.append("        (void)recomp_set_env_str(\"GB_RECOMP_PROBE\", \"1\");")
    out.append("    if (runtime_input_profile)")
    out.append("        (void)recomp_set_env_str(\"GB_RECOMP_INPUT_PROFILE\", \"1\");")
    out.append("    if (runtime_input_snapshots)")
    out.append("        (void)recomp_set_env_str(\"GB_RECOMP_INPUT_SNAPSHOTS\", \"1\");")
    out.append("")
    out.append("    memory_init(&g_memory);")
    out.append("    memory_set_logging(false);")
    out.append("    recomp_probe_set_logging(false);")
    out.append("")
    out.append("    bool use_bios = read_env_enabled(\"GB_RECOMP_USE_BIOS\");")
    out.append("    if (use_bios && load_bios(\"dmg_rom.bin\", g_memory.bios) == 0)")
    out.append("        g_memory.bios_enabled = true;")
    out.append("    else")
    out.append("        g_memory.bios_enabled = false;")
    out.append("    if (g_memory.bios_enabled)")
    out.append("        fprintf(stderr, \"[RECOMPILED] BIOS enabled in strict mode: startup may fail unless BIOS was also recompiled.\\n\");")
    out.append("")
    out.append("    int rom_load_rc = -1;")
    out.append("#if RECOMP_HAS_EMBEDDED_ROM")
    out.append("    if (!rom_arg_set)")
    out.append("    {")
    out.append("        rom_load_rc = load_embedded_rom(&g_memory);")
    out.append("    }")
    out.append("    else")
    out.append("    {")
    out.append("        rom_load_rc = load_rom(rom_path, &g_memory);")
    out.append("        if (rom_load_rc != 0)")
    out.append("        {")
    out.append("            fprintf(stderr, \"[RECOMPILED] external ROM failed (%s), fallback embedded assets.\\n\", rom_path);")
    out.append("            rom_load_rc = load_embedded_rom(&g_memory);")
    out.append("        }")
    out.append("    }")
    out.append("#else")
    out.append("    rom_load_rc = load_rom(rom_path, &g_memory);")
    out.append("    if (rom_load_rc != 0 && rom_path != default_rom_path)")
    out.append("        rom_load_rc = load_rom(default_rom_path, &g_memory);")
    out.append("#endif")
    out.append("    if (rom_load_rc != 0)")
    out.append("    {")
    out.append("        fprintf(stderr, \"Failed to load ROM or embedded assets.\\n\");")
    out.append("        memory_shutdown(&g_memory);")
    out.append("        return 1;")
    out.append("    }")
    out.append("    memory_set_save_path_hint(&g_memory, rom_path);")
    out.append("    if (runtime_discover)")
    out.append("        recomp_probe_init(rom_path, &g_memory);")
    out.append("    recomp_probe_dyn_dump_init(rom_path, &g_memory);")
    out.append("    g_runtime_discover_enabled = runtime_discover;")
    out.append("    g_runtime_read_trace_enabled = runtime_read_trace;")
    out.append("    g_runtime_read_trace_last_flush = 0;")
    out.append("    if (!missing_keys_path || missing_keys_path[0] == '\\0')")
    out.append("    {")
    out.append("        recomp_default_artifact_path(rom_path, \"recompiled_missing_keys.txt\", missing_keys_default_path, sizeof(missing_keys_default_path));")
    out.append("        missing_keys_path = missing_keys_default_path;")
    out.append("    }")
    out.append("    if (missing_keys_path && missing_keys_path[0] != '\\0')")
    out.append("    {")
    out.append("        g_missing_keys_file = fopen(missing_keys_path, \"a\");")
    out.append("    }")
    out.append("    if (!quarantine_keys_path || quarantine_keys_path[0] == '\\0')")
    out.append("    {")
    out.append("        recomp_default_artifact_path(rom_path, \"recompiled_quarantine_keys.txt\", quarantine_keys_default_path, sizeof(quarantine_keys_default_path));")
    out.append("        quarantine_keys_path = quarantine_keys_default_path;")
    out.append("    }")
    out.append("    if (quarantine_keys_path && quarantine_keys_path[0] != '\\0')")
    out.append("        g_quarantine_keys_file = fopen(quarantine_keys_path, \"a\");")
    out.append("    const char *trusted_funcs_path = trusted_funcs_cli_path;")
    out.append("    if (!trusted_funcs_path || trusted_funcs_path[0] == '\\0')")
    out.append("        trusted_funcs_path = trusted_funcs_env_path;")
    out.append("    if (trust_only && (!trusted_funcs_path || trusted_funcs_path[0] == '\\0'))")
    out.append("    {")
    out.append("        recomp_default_artifact_path(rom_path, \"recompiled_trusted_funcs.txt\", trusted_funcs_default_path, sizeof(trusted_funcs_default_path));")
    out.append("        trusted_funcs_path = trusted_funcs_default_path;")
    out.append("    }")
    out.append("    g_trust_only_enabled = trust_only;")
    out.append("    if (g_trust_only_enabled)")
    out.append("    {")
    out.append("        if (!recomp_apply_trusted_functions_file(trusted_funcs_path))")
    out.append("        {")
    out.append("            fprintf(stderr, \"[TRUST] failed to load trusted funcs file: %s\\n\", trusted_funcs_path ? trusted_funcs_path : \"(null)\");")
    out.append("            if (g_missing_keys_file) { fclose(g_missing_keys_file); g_missing_keys_file = NULL; }")
    out.append("            if (g_quarantine_keys_file) { fclose(g_quarantine_keys_file); g_quarantine_keys_file = NULL; }")
    out.append("            if (g_runtime_discover_enabled)")
    out.append("                recomp_probe_shutdown();")
    out.append("            memory_shutdown(&g_memory);")
    out.append("            return 2;")
    out.append("        }")
    out.append("        fprintf(stderr, \"[TRUST] enabled file=%s trusted_fns=%llu quarantined_keys=%llu\\n\",")
    out.append("                trusted_funcs_path ? trusted_funcs_path : \"\",")
    out.append("                (unsigned long long)g_trust_only_trusted_fn_count,")
    out.append("                (unsigned long long)g_trust_only_quarantined_key_count);")
    out.append("    }")
    out.append("")
    out.append("    memset(&g_cpu, 0, sizeof(g_cpu));")
    out.append("    memset(&g_ppu, 0, sizeof(g_ppu));")
    out.append("    g_cpu.memory = &g_memory;")
    out.append("    g_memory.cpu = &g_cpu;")
    out.append("    g_ppu.mem = &g_memory.memory;")
    out.append("    cpu_reset(&g_cpu);")
    out.append("    ppu_reset(&g_ppu, g_memory.bios_enabled);")
    out.append("    if (!g_memory.bios_enabled)")
    out.append("        recomp_apply_post_boot_dmg_state(&g_cpu, &g_memory);")
    out.append("")
    out.append("    if (fuzz_verify)")
    out.append("    {")
    out.append("        int fuzz_rc = recomp_fuzz_verify(fuzz_iters_per_key, fuzz_seed, fuzz_max_keys);")
    out.append("        if (g_missing_keys_file)")
    out.append("        {")
    out.append("            fclose(g_missing_keys_file);")
    out.append("            g_missing_keys_file = NULL;")
    out.append("        }")
    out.append("        if (g_quarantine_keys_file)")
    out.append("        {")
    out.append("            fclose(g_quarantine_keys_file);")
    out.append("            g_quarantine_keys_file = NULL;")
    out.append("        }")
    out.append("        if (g_runtime_read_trace_enabled)")
    out.append("            memory_flush_rom_read_trace();")
    out.append("        if (g_runtime_discover_enabled)")
    out.append("            recomp_probe_shutdown();")
    out.append("        memory_shutdown(&g_memory);")
    out.append("        return fuzz_rc;")
    out.append("    }")
    out.append("")
    out.append("    g_shadow_diff_enabled = shadow_diff;")
    out.append("    g_shadow_quarantine_enabled = shadow_quarantine;")
    out.append("    if (g_shadow_diff_enabled)")
    out.append("    {")
    out.append("        fprintf(stderr, \"[SHADOW] enabled every=%u stop=%d max_reports=%llu dynram=%s quarantine=%s\\n\",")
    out.append("                g_shadow_diff_every,")
    out.append("                g_shadow_diff_stop_on_mismatch ? 1 : 0,")
    out.append("                (unsigned long long)g_shadow_diff_max_reports,")
    out.append("                g_allow_dynram_dispatch ? \"on\" : \"off\",")
    out.append("                g_shadow_quarantine_enabled ? \"on\" : \"off\");")
    out.append("    }")
    out.append("    if (g_runtime_discover_enabled || g_runtime_read_trace_enabled || runtime_input_profile || runtime_input_snapshots)")
    out.append("    {")
    out.append("        fprintf(stderr, \"[LEARN] runtime_discover=%s runtime_read_trace=%s runtime_input_profile=%s runtime_input_snapshots=%s\\n\",")
    out.append("                g_runtime_discover_enabled ? \"on\" : \"off\",")
    out.append("                g_runtime_read_trace_enabled ? \"on\" : \"off\",")
    out.append("                runtime_input_profile ? \"on\" : \"off\",")
    out.append("                runtime_input_snapshots ? \"on\" : \"off\");")
    out.append("    }")
    out.append("")
    out.append("    g_display.enabled = display_enabled;")
    out.append("    if (g_display.enabled)")
    out.append("    {")
    out.append("        if (!display_init(&g_display, (int)display_scale))")
    out.append("        {")
    out.append("            fprintf(stderr, \"[RECOMPILED] display unavailable, fallback headless.\\n\");")
    out.append("            g_display.enabled = false;")
    out.append("        }")
    out.append("    }")
    out.append("")
    out.append("    while (g_running)")
    out.append("    {")
    out.append("        if (g_shadow_diff_enabled && (g_shadow_diff_every <= 1u || ((g_instr_count % (uint64_t)g_shadow_diff_every) == 0u)))")
    out.append("        {")
    out.append("            if (!recomp_shadow_lockstep_once())")
    out.append("            {")
    out.append("                if (g_running)")
    out.append("                    g_running = false;")
    out.append("            }")
    out.append("            continue;")
    out.append("        }")
    out.append("        uint32_t key = recomp_make_pc_key(&g_memory, g_cpu.PC);")
    out.append("        RecompFn fn = recomp_lookup(key);")
    out.append("        if (fn)")
    out.append("            fn();")
    out.append("        else")
    out.append("        {")
    out.append("            if (g_last_lookup_quarantined)")
    out.append("            {")
    out.append("                if (!recomp_try_quarantined_interp_step())")
    out.append("                    g_running = false;")
    out.append("                continue;")
    out.append("            }")
    out.append("            if (!recomp_try_dynamic_missing_step(key))")
    out.append("            {")
    out.append("                fprintf(stderr, \"[RECOMPILED][NATIVE] missing compiled block key=%08X pc=%04X\\n\", key, g_cpu.PC);")
    out.append("                g_running = false;")
    out.append("            }")
    out.append("        }")
    out.append("    }")
    out.append("")
    out.append("    printf(\"[RECOMPILED] done instr=%llu frames=%llu pc=%04X\\n\",")
    out.append("           (unsigned long long)g_instr_count,")
    out.append("           (unsigned long long)g_frame_count,")
    out.append("           g_cpu.PC);")
    out.append("    if (g_shadow_diff_enabled)")
    out.append("        fprintf(stderr, \"\\n[SHADOW] summary checked=%llu mismatches=%llu\\n\",")
    out.append("                (unsigned long long)g_shadow_diff_checked,")
    out.append("                (unsigned long long)g_shadow_diff_mismatches);")
    out.append("    if (g_shadow_diff_enabled)")
    out.append("        fprintf(stderr, \"[SHADOW] skipped_external=%llu\\n\",")
    out.append("                (unsigned long long)g_shadow_diff_skipped_external);")
    out.append("    if (g_shadow_diff_enabled)")
    out.append("        fprintf(stderr, \"[SHADOW] quarantined_fns=%llu\\n\",")
    out.append("                (unsigned long long)g_shadow_quarantine_fn_count);")
    out.append("")
    out.append("    display_shutdown(&g_display);")
    out.append("    if (g_missing_keys_file)")
    out.append("    {")
    out.append("        fclose(g_missing_keys_file);")
    out.append("        g_missing_keys_file = NULL;")
    out.append("    }")
    out.append("    if (g_quarantine_keys_file)")
    out.append("    {")
    out.append("        fclose(g_quarantine_keys_file);")
    out.append("        g_quarantine_keys_file = NULL;")
    out.append("    }")
    out.append("    if (g_runtime_read_trace_enabled)")
    out.append("        memory_flush_rom_read_trace();")
    out.append("    if (g_runtime_discover_enabled)")
    out.append("        recomp_probe_shutdown();")
    out.append("    memory_shutdown(&g_memory);")
    out.append("    return 0;")
    out.append("}")
    out.append("")

    return "\n".join(out)


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate a playable recompiled runtime from function JSON dumps.")
    parser.add_argument("--input-dir", required=True, help="Directory containing func_bXXX_YYYY.json files")
    parser.add_argument("--output", default="generated/recompiled_main.c", help="Output C file")
    parser.add_argument("--default-rom", default="", help="Default ROM path used when no CLI ROM arg is provided")
    parser.add_argument("--embed-rom", default="", help="Optional ROM file used to embed sparse assets")
    parser.add_argument("--embed-full-rom", action="store_true", help="Embed the full ROM image (testing mode, disables sparse asset selection)")
    parser.add_argument("--read-trace", default="", help="Optional GB_ROM_READ_TRACE_PATH output file")
    parser.add_argument("--shard-funcs", default="256", help="Approx number of functions per generated shard C file")
    parser.add_argument("--include-scan-dumps", action="store_true", help="Include heuristic REF_*_SCAN dumps (unsafe, can cause bogus code ownership)")
    args = parser.parse_args()

    input_dir = os.path.normpath(args.input_dir)
    if not os.path.isdir(input_dir):
        raise SystemExit(f"Input directory not found: {input_dir}")

    funcs, discovered_rom_bytes, code_banks = parse_functions(input_dir, include_scan_dumps=bool(args.include_scan_dumps))
    if not funcs:
        raise SystemExit(f"No function JSON found in: {input_dir}")
    dynram_funcs = parse_dynram_dumps(input_dir)
    if dynram_funcs:
        funcs.extend(dynram_funcs)
        funcs.sort(key=lambda f: (f.bank, f.addr))

    owner_map = build_owner_map([f for f in funcs if not f.is_dynram])
    default_rom = args.default_rom
    if not default_rom:
        default_rom = os.path.basename(input_dir) + ".gb"

    embedded_chunks: Optional[List[Tuple[int, bytes]]] = None
    embedded_rom_size = 0
    if args.embed_rom:
        embed_rom_path = os.path.normpath(args.embed_rom)
        if not os.path.isfile(embed_rom_path):
            raise SystemExit(f"embed-rom not found: {embed_rom_path}")
        if args.embed_full_rom:
            with open(embed_rom_path, "rb") as f:
                blob = f.read()
            embedded_rom_size = len(blob)
            embedded_chunks = [(0, blob)] if blob else []
        else:
            read_trace_offsets = parse_rom_read_trace(os.path.normpath(args.read_trace)) if args.read_trace else set()
            embedded_rom_size, embedded_chunks = build_embedded_chunks(embed_rom_path, discovered_rom_bytes, code_banks, read_trace_offsets)

    repo_root = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))
    opcode_bodies = parse_opcode_bodies(os.path.join(repo_root, "src", "CPU_OP_Codes.c"))
    inline_helpers = parse_inline_helpers(os.path.join(repo_root, "include", "CPU_Instructions.h"))
    codegen_state = build_codegen_state(funcs, owner_map)
    c_src = render_c(
        funcs,
        owner_map,
        opcode_bodies,
        inline_helpers,
        default_rom,
        embedded_chunks,
        embedded_rom_size,
        emit_function_bodies=False,
        codegen_state=codegen_state,
    )

    out_path = os.path.normpath(args.output)
    out_dir = os.path.dirname(out_path) or "."
    os.makedirs(out_dir, exist_ok=True)
    try:
        funcs_per_shard = max(1, int(args.shard_funcs))
    except ValueError:
        funcs_per_shard = 256
    shard_groups = split_function_shards(funcs, funcs_per_shard)

    for stale in glob.glob(os.path.join(out_dir, "recompiled_shard_*.c")):
        try:
            os.remove(stale)
        except OSError:
            pass

    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(c_src)
    insn_by_key, _, owned_keys_by_symbol = codegen_state
    for shard_index, shard_funcs in enumerate(shard_groups):
        shard_src = render_shard_c(
            shard_funcs,
            shard_index,
            len(shard_groups),
            owned_keys_by_symbol,
            insn_by_key,
            opcode_bodies,
            inline_helpers,
        )
        shard_path = os.path.join(out_dir, f"recompiled_shard_{shard_index:03d}.c")
        with open(shard_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(shard_src)

    if embedded_chunks:
        embedded_bytes = sum(len(chunk) for _, chunk in embedded_chunks)
        pct = (100.0 * embedded_bytes / embedded_rom_size) if embedded_rom_size else 0.0
        print(
            f"[recomp_codegen] functions={len(funcs)} dispatch_keys={len(owner_map)} "
            f"dynram_funcs={len(dynram_funcs)} "
            f"shards={len(shard_groups)} shard_funcs={funcs_per_shard} "
            f"embed_chunks={len(embedded_chunks)} embed_bytes={embedded_bytes}/{embedded_rom_size} ({pct:.2f}%) "
            f"output={out_path}"
        )
    else:
        print(
            f"[recomp_codegen] functions={len(funcs)} dispatch_keys={len(owner_map)} "
            f"dynram_funcs={len(dynram_funcs)} shards={len(shard_groups)} shard_funcs={funcs_per_shard} "
            f"output={out_path}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
