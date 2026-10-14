"""Source scanner for hard-coded DCS locations in DcsQvCull's src/.

Finds, with file and line:
  - hex constants:        constexpr/const uint32_t|uint64_t kName = 0x..., kOther = 0x...
  - constant arrays:      const uint32_t kSites[4] = {0x..., ...}
  - table rows:           {0x8b9e0, 0x8b9f3, 0x45...ull}, {L"X.dll", 0x215698, 2, "..."}
  - inline module offsets: base + 0x..., dx + 0x..., ng + 0x..., innerOk(kX, 0x...),
                          reloc::Rva(hooksig::Id, 0x...)
  - export / import names: "?name@..." and "ED_get_time" string literals

Each finding is {file, line, ns, symbol, value, text}. The symbol is
"ns::kName", "ns::kName[i]", "ns::array[row].field", or "literal:0x..." for
inline offsets; together with the file it is the key hooks_curated.py binds
manifest entries to.
"""
from __future__ import annotations

import os
import re

HEX = r"0x[0-9a-fA-F]+"
RE_NS = re.compile(r"^\s*namespace\s+(\w+)\s*\{")
RE_NS_END = re.compile(r"^\}\s*//\s*namespace\s+(\w+)")
RE_DECL = re.compile(r"^\s*(?:static\s+)?(?:constexpr|const)\s+(uint32_t|uint64_t)\s+(.*?);")
RE_ASSIGN = re.compile(r"(\w+)\s*=\s*(" + HEX + r")(?:ull|u)?\b")
RE_ARRAY_DECL = re.compile(r"^\s*(?:static\s+)?(?:constexpr|const)\s+[\w:]+\s+(\w+)\s*\[\w*\]\s*=\s*\{(.*)$")
RE_ARRAY_INLINE = re.compile(r"^\s*(?:static\s+)?(?:constexpr|const)\s+uint32_t\s+(\w+)\s*\[\w*\]\s*=\s*\{([^}]*)\}")
RE_PLAIN_ARRAY = re.compile(r"^\s*(\w+)\s+(\w+)\s*\[\]\s*=\s*\{")  # Probe g_probes[] = {
RE_ROW = re.compile(r"\{([^{}]*" + HEX + r"[^{}]*)\}")
RE_INLINE = re.compile(r"\b(?:base|ng|dx|gc|md|g_ng|g_dx)\s*\+\s*(" + HEX + r")\b")
RE_INNEROK = re.compile(r"innerOk\(\s*\w+\s*,\s*(" + HEX + r")\)")
RE_RELOC = re.compile(r"reloc::Rva\(\s*hooksig::\w+\s*,\s*(" + HEX + r")\s*\)")
RE_SYMSTR = re.compile(r"\"(\?[^\"]+|ED_get_time)\"")
RE_COMMENT = re.compile(r"//.*$")

# Sizes, magics and other hex constants that are not DCS locations.
NOT_LOCATION_NAMES = re.compile(r"Magic|Tag$|Size$|Offset$|Stride$|Bytes$|Max|Count|kPage")

# Generated from the manifest (tools/gen_hook_sigs.py): its RVAs are copies.
GENERATED = {"hook_sigs.h"}


def _strip_comment(line: str) -> str:
    # good enough for this code base: no "//" inside string literals that matter
    return RE_COMMENT.sub("", line)


def scan_file(path: str, rel: str) -> list[dict]:
    out = []
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.read().split("\n")
    ns_stack: list[str] = []
    array = None  # (name, row index) while inside a multi-line array initializer
    depth = 0
    for no, raw in enumerate(lines, 1):
        line = _strip_comment(raw)
        m = RE_NS.match(line)
        if m:
            ns_stack.append(m.group(1))
        ns = ns_stack[-1] if ns_stack else ""

        def add(symbol, value, kind):
            out.append({"file": rel, "line": no, "ns": ns, "symbol": symbol, "value": value,
                        "match": kind, "text": raw.strip()[:200]})

        # array declarations
        mi = RE_ARRAY_INLINE.match(line)
        if mi:
            for i, v in enumerate(re.findall(HEX, mi.group(2))):
                add(f"{ns}::{mi.group(1)}[{i}]", int(v, 16), "array")
        else:
            ma = RE_ARRAY_DECL.match(line) or RE_PLAIN_ARRAY.match(line)
            if ma and "{" in line and not line.rstrip().endswith("};"):
                name = ma.group(1) if RE_ARRAY_DECL.match(line) else ma.group(2)
                array = [name, 0]
                depth = 0
            elif ma and line.rstrip().endswith("};"):
                pass
            md = RE_DECL.match(line)
            if md and not array:
                for name, v in RE_ASSIGN.findall(md.group(2)):
                    if NOT_LOCATION_NAMES.search(name) and not name.startswith("kSites"):
                        continue
                    add(f"{ns}::{name}", int(v, 16), "const")
        if array:
            rows = RE_ROW.findall(line)
            for row in rows:
                for fi, v in enumerate(re.findall(HEX, row)):
                    add(f"{ns}::{array[0]}[{array[1]}].{fi}", int(v, 16), "row")
                array[1] += 1
            if re.search(r"^\s*\};|\};\s*$", line):
                array = None
        for v in RE_INLINE.findall(line) + RE_INNEROK.findall(line) + RE_RELOC.findall(line):
            add(f"literal:{v.lower()}", int(v, 16), "inline")
        for s in RE_SYMSTR.findall(raw):
            add(f"symbol:{s}", None, "symbol")
        if RE_NS_END.match(raw) and ns_stack:
            ns_stack.pop()
    # multi-line string literals (mangled names split over lines): join them
    return out


def joined_strings(path: str) -> list[tuple[int, str]]:
    """Adjacent string literals concatenated (C++ style), with the first line."""
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    out = []
    for m in re.finditer(r"(\"(?:[^\"\\\n]|\\.)*\"\s*)+", text):
        parts = re.findall(r"\"((?:[^\"\\\n]|\\.)*)\"", m.group(0))
        s = "".join(parts)
        if (s.startswith("?") and "@" in s) or s == "ED_get_time":
            out.append((text.count("\n", 0, m.start()) + 1, s))
    return out


RE_MODVAR = re.compile(r"(\w+)\s*=\s*(?:reinterpret_cast<uint8_t\*>\()?GetModuleHandleW\(L\"([\w.]+)\"\)")
RE_GETMOD = re.compile(r"GetModuleHandleW\(L\"([\w.]+)\"\)")
RE_RTTI = re.compile(r"\"(\.\?A[^\"]+)\"")
RE_CODEIS = re.compile(r"CodeIs\(\s*(?:hooksig::\w+\s*,\s*)?(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\)")
RE_FORCODE = re.compile(r"for\s*\(\s*const\s+[\w:]+&\s*(\w+)\s*:\s*(\w+)\s*\)")


def context(src_dir: str) -> dict:
    """Per (file, namespace): modules opened with GetModuleHandleW, RTTI names
    used, module variables (`auto* ng = ...GetModuleHandleW(L"NGModel.dll")`),
    and CodeIs(var, begin, end, hash) uses (arrays through range-for loops)."""
    ctx = {"ns": {}, "modvar": {}, "codeis": []}
    for name in sorted(os.listdir(src_dir)):
        if not name.endswith((".h", ".cpp", ".c")):
            continue
        with open(os.path.join(src_dir, name), encoding="utf-8", errors="replace") as f:
            lines = f.read().split("\n")
        ns_stack, loops = [], {}
        for no, raw in enumerate(lines, 1):
            m = RE_NS.match(raw)
            if m:
                ns_stack.append(m.group(1))
            ns = ns_stack[-1] if ns_stack else ""
            c = ctx["ns"].setdefault((name, ns), {"modules": set(), "rtti": set()})
            c["modules"].update(RE_GETMOD.findall(raw))
            c["rtti"].update(RE_RTTI.findall(raw))
            for var, mod in RE_MODVAR.findall(raw):
                ctx["modvar"].setdefault(var, set()).add(mod)
            for it, arr in RE_FORCODE.findall(raw):
                loops[it] = arr
            for var, b, e, h in RE_CODEIS.findall(raw):
                ctx["codeis"].append({"file": name, "line": no, "ns": ns, "var": var, "begin": b, "end": e, "hash": h})
            for it, arr in loops.items():
                if re.search(r"CodeIs\(\s*(\w+)\s*,\s*" + it + r"\.begin", raw):
                    var = re.search(r"CodeIs\(\s*(\w+)", raw).group(1)
                    ctx["codeis"].append({"file": name, "line": no, "ns": ns, "var": var, "array": arr})
            if RE_NS_END.match(raw) and ns_stack:
                ns_stack.pop()
    return ctx


def scan(src_dir: str) -> list[dict]:
    found = []
    for name in sorted(os.listdir(src_dir)):
        if not name.endswith((".h", ".cpp", ".c")) or name in GENERATED:
            continue
        path = os.path.join(src_dir, name)
        items = [f for f in scan_file(path, name) if f["match"] != "symbol"]
        found += items
        for line, s in joined_strings(path):
            found.append({"file": name, "line": line, "ns": "", "symbol": f"symbol:{s}", "value": None,
                          "match": "symbol", "text": s[:200]})
    return found


if __name__ == "__main__":
    import sys
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "src")
    for f in scan(src):
        v = hex(f["value"]) if f["value"] is not None else ""
        print(f"{f['file']}:{f['line']}\t{f['symbol']}\t{v}")
