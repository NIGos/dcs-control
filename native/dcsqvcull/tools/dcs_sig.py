"""Shared helpers for DcsQvCull's patch tooling: PE image model, RTTI vtable
lookup, wildcard byte patterns, signature derivation and search.

Read-only: DLLs are opened, read into memory and closed. Nothing is written
next to them.

Used by extract_manifest.py (derives signatures) and dcs_patch_check.py
(re-locates them in a new DCS build).
"""
from __future__ import annotations

import bisect
import os
import re
import struct

import capstone
import pefile

def _registry_value(root: str, key: str, name: str) -> str | None:
    """A string value from the Windows registry, or None (also off Windows)."""
    try:
        import winreg
    except ImportError:
        return None
    hive = {"HKLM": winreg.HKEY_LOCAL_MACHINE, "HKCU": winreg.HKEY_CURRENT_USER}[root]
    for view in (winreg.KEY_WOW64_64KEY, winreg.KEY_WOW64_32KEY):
        try:
            with winreg.OpenKey(hive, key, 0, winreg.KEY_READ | view) as k:
                value, _ = winreg.QueryValueEx(k, name)
                if isinstance(value, str) and value:
                    return value
        except OSError:
            continue
    return None


def _steam_dcs_dirs() -> list[str]:
    """DCSWorld folders in every Steam library listed in libraryfolders.vdf."""
    steam = (_registry_value("HKCU", r"Software\Valve\Steam", "SteamPath")
             or _registry_value("HKLM", r"SOFTWARE\Valve\Steam", "InstallPath"))
    if not steam:
        return []
    libraries = [steam]
    vdf = os.path.join(steam, "steamapps", "libraryfolders.vdf")
    try:
        with open(vdf, encoding="utf-8", errors="replace") as f:
            libraries += [m.replace("\\\\", "\\") for m in re.findall(r'"path"\s+"([^"]+)"', f.read())]
    except OSError:
        pass
    return [os.path.join(lib, "steamapps", "common", "DCSWorld") for lib in libraries]


def _standalone_dcs_dirs() -> list[str]:
    """Eagle Dynamics' own installer (release and the former open beta)."""
    out = []
    for key in (r"Software\Eagle Dynamics\DCS World", r"Software\Eagle Dynamics\DCS World OpenBeta"):
        path = _registry_value("HKCU", key, "Path")
        if path:
            out.append(path)
    return out


def find_dcs_bin() -> str | None:
    """The bin folder of an installed DCS World (Steam or standalone), or None."""
    for root in _steam_dcs_dirs() + _standalone_dcs_dirs():
        candidate = os.path.join(root, "bin")
        if os.path.isfile(os.path.join(candidate, "DCS.exe")):
            return candidate
    return None


def dcs_bin_dir(arg: str | None) -> str:
    """DCS bin folder: the argument, else DCS_BIN, else the installed DCS found in the registry."""
    path = arg or os.environ.get("DCS_BIN") or find_dcs_bin()
    if not path:
        raise SystemExit("DCS bin folder not found: pass --dcs-bin or set DCS_BIN")
    if not os.path.isdir(path):
        raise SystemExit(f"DCS bin folder not found: {path} (pass --dcs-bin or set DCS_BIN)")
    return path


def fnv1a64(data: bytes) -> int:
    """Same hash as shadowtex::Fnv / CodeIs in src/shadow_tex.h."""
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


# ---------------------------------------------------------------------------
# Patterns: "48 8B 05 ?? ?? ?? ?? C3"
# ---------------------------------------------------------------------------

def pattern_to_str(data: bytes, mask: list[bool]) -> str:
    return " ".join(f"{b:02X}" if m else "??" for b, m in zip(data, mask))


_SCOPE = re.compile(r"\?[0-9A-P]{1,4}@|\?[0-9]\?\?")


def normalize_rtti(name: str) -> str:
    """RTTI name with MSVC scope/lambda discriminators blanked (see
    Image.vtables_for_rtti_normalized)."""
    return _SCOPE.sub("?#@", name)


def _trim(pat: str) -> str:
    toks = pat.split()
    while toks and toks[-1] == "??":
        toks.pop()
    return " ".join(toks)


def parse_pattern(pat: str) -> tuple[bytes, list[bool]]:
    data, mask = bytearray(), []
    for tok in pat.split():
        if tok in ("?", "??"):
            data.append(0)
            mask.append(False)
        else:
            data.append(int(tok, 16))
            mask.append(True)
    return bytes(data), mask


def _pattern_regex(pat: str) -> re.Pattern:
    parts = []
    for tok in pat.split():
        parts.append(b"." if tok in ("?", "??") else re.escape(bytes([int(tok, 16)])))
    return re.compile(b"(?=(" + b"".join(parts) + b"))", re.DOTALL)


def find_pattern(buf: bytes | bytearray, pat: str, start: int = 0, end: int | None = None,
                 limit: int = 16) -> list[int]:
    """Offsets of every (overlapping) match of pat in buf[start:end], at most `limit`."""
    rx = _pattern_regex(pat)
    out = []
    for m in rx.finditer(buf, start, len(buf) if end is None else end):
        out.append(m.start())
        if len(out) >= limit:
            break
    return out


# ---------------------------------------------------------------------------
# PE image
# ---------------------------------------------------------------------------

class Image:
    """A DLL mapped at its section RVAs (no relocation applied)."""

    def __init__(self, path: str):
        self.path = path
        self.name = os.path.basename(path)
        pe = pefile.PE(path, fast_load=True)
        pe.parse_data_directories(directories=[
            pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"],
            pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
            pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"],
            pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"],
        ])
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.size = pe.OPTIONAL_HEADER.SizeOfImage
        self.timestamp = pe.FILE_HEADER.TimeDateStamp
        mem = bytearray(self.size)
        raw = pe.__data__
        hdr = pe.OPTIONAL_HEADER.SizeOfHeaders
        mem[:hdr] = raw[:hdr]
        self.sections = []
        for s in pe.sections:
            name = s.Name.rstrip(b"\0").decode("ascii", "replace")
            va, vs = s.VirtualAddress, max(s.Misc_VirtualSize, s.SizeOfRawData)
            n = min(s.SizeOfRawData, s.Misc_VirtualSize or s.SizeOfRawData)
            mem[va:va + n] = raw[s.PointerToRawData:s.PointerToRawData + n]
            self.sections.append((name, va, s.Misc_VirtualSize or s.SizeOfRawData, s.Characteristics))
        self.mem = bytes(mem)
        self.exports = {}
        if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
            for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
                if e.name:
                    self.exports[e.name.decode("ascii", "replace")] = e.address
        self.imports = {}  # (dll lower, name) -> IAT slot RVA
        if hasattr(pe, "DIRECTORY_ENTRY_IMPORT"):
            for d in pe.DIRECTORY_ENTRY_IMPORT:
                dll = d.dll.decode("ascii", "replace").lower()
                for imp in d.imports:
                    if imp.name:
                        self.imports[(dll, imp.name.decode("ascii", "replace"))] = imp.address - self.base
        self.relocs = set()
        if hasattr(pe, "DIRECTORY_ENTRY_BASERELOC"):
            for blk in pe.DIRECTORY_ENTRY_BASERELOC:
                for e in blk.entries:
                    if e.type == 10:  # DIR64
                        self.relocs.update(range(e.rva, e.rva + 8))
        self.funcs = []
        if hasattr(pe, "DIRECTORY_ENTRY_EXCEPTION"):
            for rf in pe.DIRECTORY_ENTRY_EXCEPTION:
                self.funcs.append((rf.struct.BeginAddress, rf.struct.EndAddress))
        self.funcs.sort()
        self._func_starts = [b for b, _ in self.funcs]
        pe.close()
        self.code = [(va, va + vs) for (n, va, vs, ch) in self.sections if ch & 0x20000000]
        self._md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self._md.detail = True

    # -- raw reads --
    def u32(self, rva): return struct.unpack_from("<I", self.mem, rva)[0]
    def i32(self, rva): return struct.unpack_from("<i", self.mem, rva)[0]
    def u64(self, rva): return struct.unpack_from("<Q", self.mem, rva)[0]
    def read(self, rva, n): return self.mem[rva:rva + n]

    def cstr(self, rva, maxlen=512):
        end = self.mem.find(b"\0", rva, rva + maxlen)
        return self.mem[rva:end if end >= 0 else rva + maxlen].decode("latin-1")

    def section_of(self, rva):
        for (n, va, vs, ch) in self.sections:
            if va <= rva < va + vs:
                return n
        return None

    def section(self, name):
        for (n, va, vs, ch) in self.sections:
            if n == name:
                return va, va + vs
        return None

    def in_code(self, rva):
        return any(a <= rva < b for a, b in self.code)

    def ptr_rva(self, rva):
        """Pointer stored at rva, as an RVA (None when outside the image)."""
        v = self.u64(rva) - self.base
        return v if 0 <= v < self.size else None

    def func_of(self, rva):
        """(begin, end) of the .pdata function containing rva, or None."""
        i = bisect.bisect_right(self._func_starts, rva) - 1
        if i >= 0 and self.funcs[i][0] <= rva < self.funcs[i][1]:
            return self.funcs[i]
        return None

    # -- disassembly --
    def insns(self, rva, end):
        return list(self._md.disasm(self.mem[rva:end], rva))

    def insn_at(self, rva):
        for i in self._md.disasm(self.mem[rva:rva + 16], rva, count=1):
            return i
        return None

    def insn_mask(self, insn) -> list[bool]:
        """True = byte kept in a signature; relocatable parts are wildcards."""
        n = insn.size
        mask = [True] * n
        try:
            if insn.disp_size and insn.disp_offset and any(
                    op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP
                    for op in insn.operands):
                for k in range(insn.disp_offset, insn.disp_offset + insn.disp_size):
                    mask[k] = False
            # [base + index*scale + rva]: MSVC switch tables address their
            # table as __ImageBase + RVA; the RVA moves with the table.
            if insn.disp_size == 4 and insn.disp_offset and any(
                    op.type == capstone.x86.X86_OP_MEM and op.mem.base != capstone.x86.X86_REG_RIP and
                    op.mem.index != 0 and 0x1000 <= (op.mem.disp & 0xFFFFFFFF) < self.size
                    for op in insn.operands):
                for k in range(insn.disp_offset, insn.disp_offset + 4):
                    mask[k] = False
            if insn.imm_size == 4 and capstone.CS_GRP_BRANCH_RELATIVE in insn.groups:
                for k in range(insn.imm_offset, insn.imm_offset + 4):
                    mask[k] = False
        except capstone.CsError:
            pass
        for k in range(n):
            if insn.address + k in self.relocs:
                mask[k] = False
        return mask

    def rip_target(self, insn):
        for op in insn.operands:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                return insn.address + insn.size + op.mem.disp
        if capstone.CS_GRP_BRANCH_RELATIVE in insn.groups:
            for op in insn.operands:
                if op.type == capstone.x86.X86_OP_IMM:
                    return op.imm
        return None

    # -- search --
    def code_matches(self, pat, limit=16):
        out = []
        for a, b in self.code:
            out += find_pattern(self.mem, pat, a, b, limit)
        return out

    # -- RTTI --
    def type_descriptors(self, name: str, prefix: bool = False) -> list[int]:
        """RVAs of RTTI type descriptors whose name is `name` (or starts with it)."""
        needle = name.encode("latin-1") + (b"" if prefix else b"\0")
        out, pos = [], 0
        while True:
            pos = self.mem.find(needle, pos)
            if pos < 0:
                break
            td = pos - 16
            if td > 0 and self.ptr_rva(td) is not None:  # pVFTable of type_info
                out.append(td)
            pos += 1
        return out

    def cols_for_td(self, td: int) -> list[int]:
        """Complete object locators referencing type descriptor td (.rdata scan)."""
        out = []
        packed = struct.pack("<I", td)
        for name in (".rdata",):
            sec = self.section(name)
            if not sec:
                continue
            pos = sec[0]
            while True:
                pos = self.mem.find(packed, pos, sec[1])
                if pos < 0:
                    break
                col = pos - 12
                if col % 4 == 0 and self.u32(col) == 1 and self.u32(col + 20) == col:
                    out.append(col)
                pos += 1
        return out

    def vtables_for_col(self, col: int) -> list[int]:
        packed = struct.pack("<Q", self.base + col)
        out, pos = [], 0
        sec = self.section(".rdata") or (0, self.size)
        pos = sec[0]
        while True:
            pos = self.mem.find(packed, pos, sec[1])
            if pos < 0:
                break
            if pos % 8 == 0:
                out.append(pos + 8)
            pos += 1
        return out

    def vtables_for_rtti(self, name: str, prefix: bool = False) -> list[dict]:
        """Every vtable whose COL names `name`: [{vtable, col, offset, name}]."""
        out = []
        for td in self.type_descriptors(name, prefix):
            full = self.cstr(td + 16, 4096)
            for col in self.cols_for_td(td):
                for vt in self.vtables_for_col(col):
                    out.append({"vtable": vt, "col": col, "offset": self.u32(col + 4), "name": full})
        return out

    def vtables_for_rtti_normalized(self, name: str) -> list[dict]:
        """Vtables whose RTTI name equals `name` once lambda/local scope numbers
        (`@?L@`, `@?DA@`, `?1??`) are ignored: they renumber when unrelated
        source lines change, the rest of the name does not."""
        want = normalize_rtti(name)
        out, pos = [], 0
        while True:
            pos = self.mem.find(b".?A", pos)
            if pos < 0:
                break
            td = pos - 16
            if td > 0 and self.ptr_rva(td) is not None:
                full = self.cstr(pos, 4096)
                if normalize_rtti(full) == want:
                    for col in self.cols_for_td(td):
                        for vt in self.vtables_for_col(col):
                            out.append({"vtable": vt, "col": col, "offset": self.u32(col + 4), "name": full})
            pos += 3
        return out

    def rtti_of_vtable(self, vt: int) -> str | None:
        """Same lookup as allocslab::RttiIs: vtbl[-1] -> COL -> TD -> name."""
        col = self.ptr_rva(vt - 8)
        if col is None or col + 24 > self.size:
            return None
        td = self.u32(col + 12)
        if not 0 < td < self.size:
            return None
        return self.cstr(td + 16, 4096)

    def slot(self, vt: int, i: int) -> int | None:
        return self.ptr_rva(vt + 8 * i)

    # -- signatures --
    def extent(self, rva: int) -> tuple[int, int]:
        """Function containing rva: its .pdata range, or for leaf code without
        unwind data, rva up to the first ret/jmp followed by int3 padding."""
        fn = self.func_of(rva)
        if fn:
            return fn
        pos = rva
        for insn in self._md.disasm(self.mem[rva:rva + 1024], rva):
            pos = insn.address + insn.size
            if insn.mnemonic in ("ret", "jmp") and (self.mem[pos] == 0xCC or self.func_of(pos)):
                break
        return (rva, pos)

    def make_signature(self, rva: int, min_len: int = 16, max_len: int = 256,
                       max_back: int = 24, hard_max: int = 1024) -> dict:
        """A unique wildcard pattern locating `rva` in the code sections.

        Grows forward from rva one instruction at a time to the end of its
        function (at least max_len bytes, at most hard_max); if that never
        becomes unique, starts 1..max_back instructions earlier in the same
        function. Relative branch targets, RIP-relative operands, relocated
        bytes and switch-table RVAs are wildcards.

        Returns {pattern, offset, matches, crosses_function_end, weak}: offset
        is rva minus the pattern start; matches == 1 means unique in the
        module; weak means the bytes inside the function carry too little
        content (tiny getters, `ret 0`) to trust the match after a patch."""
        fn = self.extent(rva)
        starts = [rva]
        if fn[0] < rva:
            bounds = [i.address for i in self.insns(fn[0], rva)]
            starts += list(reversed(bounds))[:max_back]

        def result(pat, anchor, pos, n):
            inside = " ".join(pat.split()[:max(0, fn[1] - anchor)])
            return {"pattern": pat, "offset": rva - anchor, "matches": n,
                    "crosses_function_end": pos > fn[1], "weak": self.weak(inside)}

        best = None
        for anchor in starts:
            limit = min(max(max_len, fn[1] - anchor), hard_max)
            data, mask = bytearray(), []
            pos = anchor
            for insn in self._md.disasm(self.mem[anchor:anchor + limit + 16], anchor):
                if insn.address != pos:
                    break
                data += insn.bytes
                mask += self.insn_mask(insn)
                pos += insn.size
                if pos <= rva or len(data) < min_len:
                    continue  # must cover the target
                pat = _trim(pattern_to_str(bytes(data), mask))
                if len(self.code_matches(pat, limit=2)) == 1:
                    r = result(pat, anchor, pos, 1)
                    if not r["weak"] or pos >= fn[1]:
                        return r  # else: unique but thin, grow while inside the function
                if len(data) >= limit:
                    break
            if best is None and data:
                pat = _trim(pattern_to_str(bytes(data), mask))
                best = result(pat, anchor, pos, len(self.code_matches(pat, 64)))
        return best or {"pattern": "", "offset": 0, "matches": 0}

    def resolve_signature(self, sig: dict) -> list[int]:
        """Locations (pattern match + offset) of a signature in the code sections."""
        if not sig or not sig.get("pattern"):
            return []
        return [m + sig.get("offset", 0) for m in self.code_matches(sig["pattern"], limit=8)]

    # -- data cross references --
    def xrefs_to(self, target: int, limit: int = 64) -> list[int]:
        """Instructions in the code sections with a RIP-relative operand at target.

        Candidates come from a vectorised scan for disp32 values that point at
        target; each is confirmed by disassembling its .pdata function from the
        start, so a decode can never begin inside another instruction."""
        import numpy as np
        cands = []
        for a, b in self.code:
            buf = np.frombuffer(self.mem[a:b], dtype=np.uint8)
            if len(buf) < 8:
                continue
            v = (buf[:-3].astype(np.int64) | (buf[1:-2].astype(np.int64) << 8) |
                 (buf[2:-1].astype(np.int64) << 16) | (buf[3:].astype(np.int64) << 24))
            v = np.where(v >= 1 << 31, v - (1 << 32), v)
            pos = np.arange(len(v), dtype=np.int64) + a
            for extra in (0, 1, 2, 4):
                cands += [int(pos[i]) for i in np.nonzero(pos + 4 + extra + v == target)[0]]
        out, seen_funcs = set(), set()
        for disp_at in sorted(set(cands)):
            fn = self.func_of(disp_at)
            if fn:
                if fn in seen_funcs:
                    continue
                seen_funcs.add(fn)
                for insn in self.insns(fn[0], fn[1]):
                    if self.rip_target(insn) == target and capstone.CS_GRP_BRANCH_RELATIVE not in insn.groups:
                        out.add(insn.address)
            else:  # leaf code without unwind data: longest plausible decode
                for k in range(8, 0, -1):
                    insn = self.insn_at(disp_at - k)
                    if insn and insn.disp_offset == k and self.rip_target(insn) == target:
                        out.add(insn.address)
                        break
            if len(out) >= limit:
                break
        return sorted(out)

    def import_thunk(self, rva: int):
        """(dll, name) when rva is `jmp [rip+IAT slot]`, else None."""
        insn = self.insn_at(rva)
        if not insn or insn.mnemonic != "jmp" or bytes(insn.bytes[:2]) != bytes([0xFF, 0x25]):
            return None
        t = self.rip_target(insn)
        for k, v in self.imports.items():
            if v == t:
                return k
        return None

    def find_import_thunks(self, dll: str, name: str) -> list[int]:
        slot = self.iat_slot(dll, name)
        if slot is None:
            return []
        out = []
        for a, b in self.code:
            pos = a
            while True:
                pos = self.mem.find(bytes([0xFF, 0x25]), pos, b)
                if pos < 0:
                    break
                if pos + 6 + self.i32(pos + 2) == slot:
                    out.append(pos)
                pos += 1
        return out

    @staticmethod
    def weak(pat: str) -> bool:
        """True when a pattern has too little real content to mean anything:
        fewer than 10 fixed non-padding bytes, or fewer than 4 distinct ones."""
        fixed = [t for t in pat.split() if t not in ("??", "CC")]
        return len(fixed) < 10 or len(set(fixed)) < 4

    def make_data_signature(self, target: int, prefer_funcs: list[int] | None = None) -> dict:
        """Signature for a data RVA: a unique code pattern around an instruction
        that references it RIP-relatively. Resolve with resolve_data_signature."""
        xs = self.xrefs_to(target)
        if not xs:
            return {"pattern": "", "offset": 0, "matches": 0, "xrefs": 0}
        if prefer_funcs:
            pref = [x for x in xs if any(f <= x < f + 0x800 for f in prefer_funcs)]
            xs = pref + [x for x in xs if x not in pref]
        best = None
        for x in xs[:12]:
            s = self.make_signature(x)
            if s["matches"] == 1 and (best is None or len(s["pattern"]) < len(best["pattern"])):
                best = s
                best["xref"] = x
        if best is None:
            return {"pattern": "", "offset": 0, "matches": 0, "xrefs": len(xs)}
        best["xrefs"] = len(xs)
        best["kind"] = "rip_xref"
        return best

    def resolve_data_signature(self, sig: dict) -> list[int]:
        out = []
        for loc in self.resolve_signature(sig):
            insn = self.insn_at(loc)
            t = self.rip_target(insn) if insn else None
            if t is not None:
                out.append(t)
        return out

    # -- imports / exports --
    def iat_slot(self, dll: str, name: str) -> int | None:
        return self.imports.get((dll.lower(), name))
