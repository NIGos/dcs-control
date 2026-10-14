"""Builds tools/hooks_manifest.json: every hard-coded DCS location in src/,
with its source lines, install-time checks and a recompile-robust signature
derived from the current DCS binaries.

    python tools/extract_manifest.py [--dcs-bin DIR] [--src DIR] [--out FILE]

DCS bin folder: --dcs-bin, else the DCS_BIN environment variable, else the
installed DCS found in the registry (Steam or standalone). The DLLs are only read.

Inputs:
  src_scan.py        finds the constants, tables, inline offsets and symbol
                     names in src/ (with file and line), and the CodeIs hash
                     ranges with the module variable they are checked against
  hooks_curated.py   what each location is (module, kind, RTTI, slot, checks)
Every scanned finding must be claimed by a curated entry or a parsed hash
range; leftovers are listed under "unclassified" and make the script exit 1,
so new hard-coded addresses cannot slip past the manifest.

Re-run after changing addresses in src/ or after re-deriving them for a new
DCS build; the manifest records the module timestamps it was derived from.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import dcs_sig  # noqa: E402
import hooks_curated  # noqa: E402
import src_scan  # noqa: E402

SHORT = {"NGModel.dll": "NG", "dx11backend.dll": "DX", "edCore.dll": "ED", "GraphicsCore.dll": "GC",
         "Visualizer.dll": "VIS", "animator.dll": "ANIM", "edterrainGraphics41.dll": "TERR",
         "metaShader.dll": "MS", "renderer.dll": "REND", "ModelDesc.dll": "MD", "Scene.dll": "SCENE",
         "SceneRenderer.dll": "SR", "GraphicsVista.dll": "GV"}


def hx(v):
    return None if v is None else f"0x{v:x}"


class Findings:
    def __init__(self, found):
        self.by_key = {}
        for f in found:
            self.by_key.setdefault((f["file"], f["symbol"]), []).append(f)
        self.used = set()

    def bind(self, binding, src_dir):
        """[(finding-like dict)] for one 'file:key' binding."""
        file, key = binding.split(":", 1)
        if key.startswith("re:"):
            rx = re.compile(key[3:])
            with open(os.path.join(src_dir, file), encoding="utf-8", errors="replace") as fh:
                for no, line in enumerate(fh, 1):
                    if rx.search(line):
                        return [{"file": file, "line": no, "symbol": key, "value": None}]
            raise SystemExit(f"binding {binding}: no line matches")
        hits = self.by_key.get((file, key))
        if not hits:
            raise SystemExit(f"binding {binding}: not found by the source scan")
        for h in hits:
            self.used.add(id(h))
        return hits


def build(src_dir, bin_dir):
    found = src_scan.scan(src_dir)
    ctx = src_scan.context(src_dir)
    fs = Findings(found)
    conflicts = []
    entries = []
    by_id = {}

    # ---- curated entries ----
    for c in hooks_curated.ENTRIES:
        e = {k: v for k, v in c.items() if k not in ("src", "import_")}
        if e["kind"] == "vtable_slot":
            e["module"] = by_id[e["vtable"]]["module"]
        if "import_" in c:
            e["import"] = list(c["import_"])
        refs, value = [], None
        for b in c.get("src", []):
            for h in fs.bind(b, src_dir):
                refs.append({"file": h["file"], "line": h["line"], "symbol": h["symbol"]})
                if h["value"] is not None:
                    if value is None:
                        value = h["value"]
                    elif h["value"] != value:
                        conflicts.append(f"{c['id']}: {h['file']}:{h['line']} has 0x{h['value']:x}, expected 0x{value:x}")
        for chk in e.get("checks", []):
            if "src" in chk:
                h = fs.bind(chk["src"], src_dir)[0]
                chk["ref"] = {"file": h["file"], "line": h["line"]}
                del chk["src"]
        e["refs"] = refs
        e["value"] = value
        e["value_from"] = "source" if value is not None else "binary"
        e["provenance"] = provenance(c, refs, ctx)
        entries.append(e)
        by_id[e["id"]] = e

    # ---- parsed CodeIs hash ranges ----
    ranges = {}
    for u in ctx["codeis"]:
        mods = ctx["modvar"].get(u["var"], set())
        module = next(iter(mods)) if len(mods) == 1 else None
        rows = []
        if "array" in u:
            k = 0
            while (u["file"], f"{u['ns']}::{u['array']}[{k}].0") in fs.by_key:
                rows.append([fs.bind(f"{u['file']}:{u['ns']}::{u['array']}[{k}].{i}", src_dir)[0] for i in range(3)])
                k += 1
        else:
            rows.append([fs.bind(f"{u['file']}:{u['ns']}::{s}", src_dir)[0] for s in (u["begin"], u["end"], u["hash"])])
        for b, en, h in rows:
            key = (module, b["value"], en["value"], h["value"])
            r = ranges.setdefault(key, {"module": module, "begin": b["value"], "end": en["value"], "hash": h["value"],
                                        "refs": [], "use": []})
            for f in (b, en, h):
                ref = {"file": f["file"], "line": f["line"], "symbol": f["symbol"]}
                if ref not in r["refs"]:
                    r["refs"].append(ref)
            r["use"].append({"file": u["file"], "line": u["line"]})
    for (module, begin, end, hsh), r in sorted(ranges.items(), key=lambda kv: (str(kv[0][0]), kv[0][1])):
        if module is None:
            conflicts.append(f"hash range 0x{begin:x}: module variable not resolvable")
            continue
        chk = {"type": "fnv", "at": 0, "len": end - begin, "hash": hx(hsh),
               "refs": r["refs"], "checked_at": r["use"]}
        owner = None
        for e in entries:
            if e.get("module") == module and e.get("value") == begin and e["kind"] in (
                    "function", "vtable_slot", "code_hash", "instruction"):
                owner = e
                break
        if owner is None:
            owner = {"id": f"{SHORT[module]}.code.{begin:x}", "module": module, "kind": "code_hash",
                     "value": begin, "value_from": "source", "provenance": "parsed", "refs": [], "checks": [],
                     "used_by": ", ".join(sorted({x["file"] for x in r["use"]}))}
            entries.append(owner)
            by_id[owner["id"]] = owner
        owner.setdefault("checks", []).append(chk)
        for ref in r["refs"]:
            if ref not in owner["refs"]:
                owner["refs"].append(ref)

    unclassified = [{"file": f["file"], "line": f["line"], "symbol": f["symbol"], "value": hx(f["value"]),
                     "text": f["text"]} for f in found
                    if id(f) not in fs.used and (f["file"], f["symbol"]) not in hooks_curated.IGNORE]

    # ---- derive from the binaries ----
    images = {}

    def img(module):
        if module not in images:
            p = os.path.join(bin_dir, module)
            images[module] = dcs_sig.Image(p) if os.path.isfile(p) else None
        return images[module]

    order = {"vtable": 0, "export": 1, "iat_import": 1, "function": 2, "data": 2, "instruction": 2, "external": 2,
             "vtable_slot": 3, "call_site": 4, "code_hash": 5}
    problems = []
    for e in sorted(entries, key=lambda e: order[e["kind"]]):
        if e["kind"] == "vtable_slot":
            e["module"] = by_id[e["vtable"]]["module"]
        if e["kind"] == "external":
            e["signature"] = {"type": "skipped", "why": "not a DCS module"}
            continue
        im = img(e["module"])
        if im is None:
            problems.append(f"{e['id']}: {e['module']} not found in {bin_dir}")
            continue
        try:
            derive(e, im, by_id, problems)
        except Exception as ex:  # keep going; report
            problems.append(f"{e['id']}: derivation failed: {ex!r}")

    # stable ordering for the file: module, then kind, then value
    entries.sort(key=lambda e: (e["module"], order[e["kind"]], e.get("value") or 0, e["id"]))
    for e in entries:
        for k in ("value", "vtable_rva", "target"):
            if isinstance(e.get(k), int):
                e[k] = hx(e[k])
        if e.get("anchor_delta") is not None:
            e["anchor_delta"] = hx(e["anchor_delta"])
    builds = {m: {"timestamp": hx(i.timestamp), "size_of_image": hx(i.size)} for m, i in sorted(images.items()) if i}
    counts = {}
    for e in entries:
        counts.setdefault(e["module"], {}).setdefault(e["kind"], 0)
        counts[e["module"]][e["kind"]] += 1
    no_unique = [e["id"] for e in entries if needs_pattern(e) and
                 (e.get("signature", {}).get("matches") != 1 or e.get("signature", {}).get("weak"))]
    manifest = {
        "about": "Hard-coded DCS locations in DcsQvCull src/. Generated by tools/extract_manifest.py from src_scan.py "
                 "findings and hooks_curated.py; do not edit by hand. Values are RVAs.",
        "dcs_build": builds,
        "counts": counts,
        "total": len(entries),
        "entries": entries,
        "no_unique_pattern": no_unique,
        "unclassified": unclassified,
        "conflicts": conflicts,
        "problems": problems,
    }
    return manifest


def needs_pattern(e):
    return e["kind"] in ("function", "instruction", "call_site", "code_hash", "vtable_slot", "data") and \
        not e.get("export") and not e.get("anchor")


def provenance(c, refs, ctx):
    """'parsed' when the source itself names the module (GetModuleHandleW in the
    binding's namespace, or a module variable such as `ng`) and, for vtables,
    the RTTI class; else 'curated'."""
    if c["kind"] in ("vtable_slot", "external"):
        return "curated"
    if c["kind"] in ("export", "iat_import"):
        return "parsed" if refs and refs[0]["symbol"].startswith("symbol:") else "curated"
    if not refs:
        return "curated"
    f0 = refs[0]
    ns = f0["symbol"].split("::")[0] if "::" in f0["symbol"] else None
    mods = set()
    rtti = set()
    for (file, nsname), v in ctx["ns"].items():
        if file == f0["file"] and (ns is None or nsname == ns):
            mods |= v["modules"]
            rtti |= v["rtti"]
    module_ok = mods == {c["module"]}
    if c["kind"] == "vtable":
        r = c.get("rtti")
        return "parsed" if module_ok and r and any(r.startswith(x) or x.startswith(r[:60]) for x in rtti) else "curated"
    return "parsed" if module_ok else "curated"


def derive(e, im, by_id, problems):
    k = e["kind"]
    sig = {}
    if k == "vtable":
        if e.get("export"):
            ev = im.exports.get(e["export"])
            if e.get("value") is None:
                e["value"] = ev
            elif ev != e["value"]:
                problems.append(f"{e['id']}: export at 0x{ev:x} != source 0x{e['value']:x}")
        name = im.rtti_of_vtable(e["value"])
        if not name:
            problems.append(f"{e['id']}: no RTTI at 0x{e['value']:x}")
            return
        want = e.get("rtti")
        if want and not (name == want or (e.get("rtti_prefix") and name.startswith(want))):
            problems.append(f"{e['id']}: RTTI is {name}, expected {want}")
        if e.get("rtti_part") and e["rtti_part"] not in name:
            problems.append(f"{e['id']}: RTTI {name} lacks {e['rtti_part']}")
        e["rtti_full"] = name
        if not want:
            e["rtti"] = name
            e["rtti_from_binary"] = True
        hits = [h for h in im.vtables_for_rtti(name) if h["offset"] == e.get("col_offset", 0)]
        if e.get("col_offset") is None:
            e["col_offset"] = im.u32(im.ptr_rva(e["value"] - 8) + 4)
        sig = {"type": "rtti", "name": name, "col_offset": e.get("col_offset", 0), "matches": len(hits)}
        if e.get("export"):
            sig["export"] = e["export"]
        for c in e.get("checks", []):
            if c["type"] == "vtable_len":
                n = 0
                while im.in_code(im.slot(e["value"], n) or 0):
                    n += 1
                if n != c["value"]:
                    problems.append(f"{e['id']}: vtable has {n} code entries, source expects {c['value']}")
    elif k == "vtable_slot":
        v = by_id[e["vtable"]]
        vt_rva = v["value"] if isinstance(v["value"], int) else int(v["value"], 16)
        target = im.slot(vt_rva, e["slot"])
        e["vtable_rva"] = vt_rva
        if e.get("value") is None:
            e["value"] = target
        elif target != e["value"]:
            problems.append(f"{e['id']}: slot {e['slot']} holds 0x{target:x}, source expects 0x{e['value']:x}")
        if e.get("export") and im.exports.get(e["export"]) != target:
            problems.append(f"{e['id']}: slot target differs from export {e['export'][:40]}")
        sig = code_signature(im, e["value"])
        sig.update({"via": "vtable", "vtable": e["vtable"], "slot": e["slot"]})
    elif k in ("function", "instruction", "code_hash", "call_site"):
        if e.get("export"):
            ev = im.exports.get(e["export"])
            if e.get("value") is None:
                e["value"] = ev
            elif ev != e["value"]:
                problems.append(f"{e['id']}: export at 0x{ev:x} != source 0x{e['value']:x}")
        if e.get("anchor"):
            parent = by_id[e["anchor"]]
            pv = parent["value"] if isinstance(parent["value"], int) else int(parent["value"], 16)
            e["anchor_delta"] = e["value"] - pv
            sig = {"type": "anchor", "anchor": e["anchor"], "delta": hx(e["anchor_delta"])}
        else:
            sig = code_signature(im, e["value"])
            if e.get("export"):
                sig["export"] = e["export"]
        if k == "call_site":
            cont = im.exports.get(e["container_export"])
            calls = calls_to(im, cont, by_id[e["call_target"]]["value"])
            sig["container_export"] = e["container_export"]
            sig["call_index"] = e["call_index"]
            if e["call_index"] >= len(calls) or calls[e["call_index"]] != e["value"]:
                problems.append(f"{e['id']}: is not call #{e['call_index']} to {e['call_target']} in its container")
    elif k == "data":
        if e.get("export"):
            ev = im.exports.get(e["export"])
            if e.get("value") is not None and ev != e["value"]:
                problems.append(f"{e['id']}: export at 0x{ev:x} != source 0x{e['value']:x}")
            e["value"] = e.get("value") or ev
            sig = {"type": "export", "export": e["export"]}
        else:
            sig = im.make_data_signature(e["value"])
            sig["type"] = "rip_xref"
    elif k == "iat_import":
        dll, name = e["import"]
        slot_rva = im.iat_slot(dll, name)
        if slot_rva is None:
            problems.append(f"{e['id']}: import {dll}!{name[:40]} not found")
        elif e.get("value") is None:
            e["value"] = slot_rva
        elif slot_rva != e["value"]:
            problems.append(f"{e['id']}: IAT slot is 0x{slot_rva:x}, source has 0x{e['value']:x}")
        sig = {"type": "import", "dll": dll, "name": name}
    elif k == "export":
        ev = im.exports.get(e["export"])
        if ev is None:
            problems.append(f"{e['id']}: export missing")
        e["value"] = ev
        sig = {"type": "export", "export": e["export"]}
        for s, fname in (e.get("slots") or {}).items():
            fv = im.exports.get(fname)
            if fv is None or im.slot(ev, int(s)) != fv:
                problems.append(f"{e['id']}: slot {s} is not export {fname[:40]}")
    # install-time checks on the current binary
    for c in e.get("checks", []):
        ok = run_check(c, im, e["value"], by_id)
        c["holds_in_derivation_build"] = ok
        if not ok:
            problems.append(f"{e['id']}: check {c['type']} at +{c.get('at', 0)} fails on the current binary")
    e["signature"] = sig


def code_signature(im, rva):
    """Import thunk identity when rva is `jmp [IAT]`, else a byte pattern."""
    imp = im.import_thunk(rva)
    if imp:
        return {"type": "import_thunk", "dll": imp[0], "name": imp[1],
                "matches": len(im.find_import_thunks(*imp))}
    sig = im.make_signature(rva)
    sig["type"] = "pattern"
    return sig


def calls_to(im, func, target):
    """Addresses of `call target` instructions in the .pdata function at func, in order."""
    fb = im.func_of(func)
    if not fb:
        return []
    out = []
    t = target if isinstance(target, int) else int(target, 16)
    for i in im.insns(fb[0], fb[1]):
        if i.mnemonic == "call" and im.rip_target(i) == t:
            out.append(i.address)
    return out


def run_check(c, im, value, by_id):
    if value is None:
        return False
    value = value if isinstance(value, int) else int(value, 16)
    t = c["type"]
    if t == "bytes":
        at = value + c.get("at", 0)
        for alt in [c["hex"]] + c.get("alt", []):
            if dcs_sig.find_pattern(im.mem, alt, at, at + len(alt.split()), 1) == [at]:
                return True
        return False
    if t == "fnv":
        return dcs_sig.fnv1a64(im.read(value + c.get("at", 0), c["len"])) == int(c["hash"], 16)
    if t == "call_target":
        insn = im.insn_at(value)
        tv = by_id[c["target"]]["value"]
        tv = tv if isinstance(tv, int) else int(tv, 16)
        return insn is not None and insn.mnemonic == "call" and im.rip_target(insn) == tv
    if t == "vtable_len":
        n = 0
        while im.in_code(im.slot(value, n) or 0):
            n += 1
        return n == c["value"]
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dcs-bin", help="DCS bin folder (default: $DCS_BIN, else the installed DCS from the registry)")
    ap.add_argument("--src", default=os.path.join(HERE, "..", "src"))
    ap.add_argument("--out", default=os.path.join(HERE, "hooks_manifest.json"))
    a = ap.parse_args()
    bin_dir = dcs_sig.dcs_bin_dir(a.dcs_bin)
    m = build(os.path.abspath(a.src), bin_dir)
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(m, f, indent=1)
        f.write("\n")
    print(f"{m['total']} locations -> {os.path.relpath(a.out)}")
    for mod, kinds in m["counts"].items():
        print(f"  {mod:26s} " + ", ".join(f"{k} {n}" for k, n in sorted(kinds.items())))
    prov = {}
    for e in m["entries"]:
        prov[e["provenance"]] = prov.get(e["provenance"], 0) + 1
    print("  provenance: " + ", ".join(f"{k} {v}" for k, v in sorted(prov.items())))
    if m["no_unique_pattern"]:
        print("no unique byte pattern (located another way or not at all):")
        for i in m["no_unique_pattern"]:
            print("  " + i)
    for title in ("unclassified", "conflicts", "problems"):
        if m[title]:
            print(f"{title}:")
            for x in m[title]:
                print("  " + (json.dumps(x) if isinstance(x, dict) else x))
    sys.exit(1 if m["unclassified"] or m["conflicts"] or m["problems"] else 0)


if __name__ == "__main__":
    main()
