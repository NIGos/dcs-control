"""Checks DcsQvCull's hard-coded DCS locations against a DCS build.

    python tools/dcs_patch_check.py [--dcs-bin DIR] [--manifest FILE]
                                    [--json OUT] [--module NAME=PATH ...] [--qvfr PATH] [--all]

DCS bin folder: --dcs-bin, else the DCS_BIN environment variable, else the
installed DCS found in the registry (Steam or standalone). The DLLs are only read.

For every location in tools/hooks_manifest.json it
  1. checks whether the recorded value still holds (RTTI at the vtable RVA,
     the slot's target, the byte pattern at the function RVA, the import or
     export by name);
  2. otherwise searches by signature (RTTI type name + COL offset, unique
     wildcard pattern, export/import name, data cross reference, call index in
     an exported container) and reports the new value when exactly one match
     is found, else "ambiguous" or "lost";
  3. runs the install-time checks the hook itself makes (expected bytes, FNV
     code hashes, call targets, vtable length) at the found location.

Statuses: unchanged, moved (new value found, checks pass), changed (location
found but an install-time check fails: the hook would refuse to install),
ambiguous, lost, skipped (not a DCS module / module missing).

Prints a report and writes a JSON patch suggestion (old -> new per location,
with the source lines to edit) to --json (default: dcs_patch_suggestion.json
in the current directory). Exit code: 0 when everything is unchanged, 1 when
something moved or changed, 2 when something is ambiguous or lost.

--module NAME=PATH replaces one DLL (testing, or a single updated file).
"""
from __future__ import annotations

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import dcs_sig  # noqa: E402

ORDER = {"vtable": 0, "export": 1, "iat_import": 1, "function": 2, "data": 2, "instruction": 2, "external": 2,
         "vtable_slot": 3, "call_site": 4, "code_hash": 5}


def ival(v):
    return None if v is None else (v if isinstance(v, int) else int(v, 16))


def hx(v):
    return None if v is None else f"0x{v:x}"


class Checker:
    def __init__(self, manifest, bin_dir, overrides, qvfr=None):
        self.m = manifest
        self.bin_dir = bin_dir
        self.overrides = overrides
        self.qvfr = qvfr
        self.images = {}
        self.by_id = {e["id"]: e for e in manifest["entries"]}
        self.res = {}

    def img(self, module):
        if module not in self.images:
            path = self.overrides.get(module.lower()) or os.path.join(self.bin_dir, module)
            self.images[module] = dcs_sig.Image(path) if os.path.isfile(path) else None
        return self.images[module]

    def new_value(self, id):
        r = self.res.get(id)
        return r["new"] if r and r["status"] in ("unchanged", "moved", "changed") else None

    # ------------------------------------------------------------------
    def run(self):
        for e in sorted(self.m["entries"], key=lambda e: ORDER[e["kind"]]):
            r = {"id": e["id"], "module": e["module"], "kind": e["kind"], "old": ival(e.get("value")), "new": None,
                 "status": None, "how": None, "notes": [], "failed_checks": []}
            self.res[e["id"]] = r
            if e["kind"] == "external":
                self.external(e, r)
                continue
            im = self.img(e["module"])
            if im is None:
                r["status"], r["how"] = "skipped", f"{e['module']} not found"
                continue
            getattr(self, "loc_" + e["kind"])(e, im, r)
            if r["new"] is not None and r["status"] in (None, "unchanged", "moved"):
                self.run_checks(e, im, r)
                if r["status"] is None:
                    r["status"] = "unchanged" if r["new"] == r["old"] else "moved"
            elif r["status"] is None:
                r["status"] = "lost"
        return self.res

    # ---- locators ----
    def sig_locations(self, im, sig):
        if not sig:
            return []
        t = sig.get("type")
        if t == "import_thunk":
            return im.find_import_thunks(sig["dll"], sig["name"])
        if t in ("pattern", None) and sig.get("pattern"):
            return im.resolve_signature(sig)
        if t == "rip_xref":
            return sorted(set(im.resolve_data_signature(sig)))
        return []

    def sig_holds_at(self, im, sig, rva):
        if sig.get("type") == "import_thunk":
            return im.import_thunk(rva) == (sig["dll"], sig["name"])
        if not sig.get("pattern"):
            return False
        a = rva - sig.get("offset", 0)
        n = len(sig["pattern"].split())
        return dcs_sig.find_pattern(im.mem, sig["pattern"], a, a + n, 1) == [a]

    def pick(self, r, cands, how):
        cands = sorted(set(c for c in cands if c is not None))
        if len(cands) == 1:
            r["new"], r["how"] = cands[0], how
        elif len(cands) > 1:
            r["status"], r["how"] = "ambiguous", f"{how}: {len(cands)} matches " + ", ".join(hx(c) for c in cands[:6])
        else:
            r["status"], r["how"] = "lost", f"{how}: no match"

    def loc_vtable(self, e, im, r):
        sig = e["signature"]
        name, off = sig["name"], sig.get("col_offset", 0)
        old = r["old"]
        at_old = im.rtti_of_vtable(old) if old is not None and old + 8 < im.size else None
        col = im.ptr_rva(old - 8) if at_old else None
        if at_old == name and col is not None and im.u32(col + 4) == off:
            if e.get("export") and im.exports.get(e["export"]) not in (None, old):
                r["notes"].append("export disagrees with RTTI location")
            r["new"], r["how"] = old, "RTTI at recorded RVA"
            return
        if e.get("export") and e["export"] in im.exports:
            r["new"], r["how"] = im.exports[e["export"]], "export"
            return
        hits = [h["vtable"] for h in im.vtables_for_rtti(name) if h["offset"] == off]
        if not hits and e.get("rtti_prefix") and e.get("rtti"):
            hits = [h["vtable"] for h in im.vtables_for_rtti(e["rtti"], prefix=True) if h["offset"] == off]
            if hits:
                r["notes"].append("full RTTI name changed; matched by the source's prefix")
        if not hits:
            hits = [h["vtable"] for h in im.vtables_for_rtti_normalized(name) if h["offset"] == off]
            if hits:
                r["notes"].append("RTTI name matched with lambda/scope numbers ignored; confirm the class")
        self.pick(r, hits, f"RTTI {name[:60]} (COL offset {off})")

    def loc_vtable_slot(self, e, im, r):
        sig = e["signature"]
        vt = self.new_value(e["vtable"])
        slot = e["slot"]
        found = self.sig_locations(im, sig)
        uniq = found[0] if len(found) == 1 and not sig.get("weak") else None
        if vt is not None:
            target = im.slot(vt, slot)
            if target is None:
                r["status"], r["how"] = "lost", f"vtable slot {slot} empty"
                return
            r["new"], r["how"] = target, f"vtable {hx(vt)} slot {slot}"
            if uniq is not None and uniq != target:
                # the function exists but the slot holds something else: did the slot index move?
                n, slots = 0, []
                while im.in_code(im.slot(vt, n) or 0) and n < 512:
                    if im.slot(vt, n) == uniq:
                        slots.append(n)
                    n += 1
                r["notes"].append(f"signature finds {hx(uniq)}, slot {slot} holds {hx(target)}"
                                  + (f"; function is now at slot(s) {slots}" if slots else ""))
                if slots:
                    r["new_slot"] = slots
                r["status"] = "changed"
                r["failed_checks"].append("slot target != signature")
            elif uniq is not None:
                r["how"] += ", signature agrees"
            elif sig.get("weak"):
                r["how"] += " (signature too thin to cross-check)"
            if e.get("export") and im.exports.get(e["export"]) != target:
                r["notes"].append("slot target differs from the export")
            return
        self.pick(r, found, "vtable lost; signature")
        if r["new"] is not None:
            r["notes"].append("located by signature only")

    def loc_code(self, e, im, r):
        sig = e["signature"]
        old = r["old"]
        if sig.get("type") == "anchor":
            p = self.new_value(sig["anchor"])
            if p is None:
                r["status"], r["how"] = "lost", f"anchor {sig['anchor']} not located"
                return
            r["new"], r["how"] = p + ival(sig["delta"]), f"{sig['anchor']} + {sig['delta']}"
            return
        if e.get("export") and e["export"] in im.exports:
            r["new"], r["how"] = im.exports[e["export"]], "export"
            if not self.sig_holds_at(im, sig, r["new"]):
                r["notes"].append("byte pattern no longer matches at the export")
            return
        if old is not None and self.sig_holds_at(im, sig, old):
            r["new"], r["how"] = old, "pattern at recorded RVA"
            return
        found = self.sig_locations(im, sig)
        if len(found) == 1 or not e.get("container_export"):
            self.pick(r, found, "pattern search" + (" (weak pattern)" if sig.get("weak") else ""))
            if sig.get("weak") and r["new"] is not None:
                r["notes"].append("weak pattern: confirm by hand")
        if r["new"] is None and e.get("container_export"):
            cont = im.exports.get(e["container_export"])
            tgt = self.new_value(e["call_target"])
            calls = calls_to(im, cont, tgt) if cont is not None and tgt is not None else []
            i = e["call_index"]
            r["status"] = None
            if i < len(calls):
                r["new"], r["how"] = calls[i], f"call #{i} to {e['call_target']} in {e['container_export'][:30]}"
                if len(calls) != 4:
                    r["notes"].append(f"container now has {len(calls)} such calls")
            else:
                r["status"], r["how"] = "lost", "pattern and call index both failed"

    loc_function = loc_instruction = loc_code_hash = loc_call_site = loc_code

    def loc_data(self, e, im, r):
        sig = e["signature"]
        if sig.get("type") == "export":
            v = im.exports.get(sig["export"])
            r["new"], r["how"] = v, "export"
            if v is None:
                r["status"], r["how"] = "lost", "export missing"
            return
        self.pick(r, self.sig_locations(im, sig), "RIP-relative reference pattern")

    def loc_iat_import(self, e, im, r):
        dll, name = e["import"]
        v = im.iat_slot(dll, name)
        if v is None:
            r["status"], r["how"] = "lost", f"no import {dll}!{name[:50]}"
        else:
            r["new"], r["how"] = v, "import by name"

    def loc_export(self, e, im, r):
        v = im.exports.get(e["export"])
        if v is None:
            r["status"], r["how"] = "lost", "export missing"
            return
        r["new"], r["how"] = v, "export by name"
        for s, fname in (e.get("slots") or {}).items():
            fv = im.exports.get(fname)
            if fv is None or im.slot(v, int(s)) != fv:
                r["status"] = "changed"
                r["failed_checks"].append(f"slot {s} != export {fname[:40]}")

    def external(self, e, r):
        if not self.qvfr:
            r["status"], r["how"] = "skipped", "not a DCS module (pass --qvfr PATH to check it)"
            return
        im = dcs_sig.Image(self.qvfr)
        r["new"], r["how"] = r["old"], "recorded RVA"
        self.run_checks(e, im, r)
        r["status"] = r["status"] or "unchanged"

    # ---- install-time checks ----
    def run_checks(self, e, im, r):
        v = r["new"]
        for c in e.get("checks", []):
            t = c["type"]
            ok, detail = True, None
            if t == "bytes":
                at = v + c.get("at", 0)
                ok = any(dcs_sig.find_pattern(im.mem, p, at, at + len(p.split()), 1) == [at]
                         for p in [c["hex"]] + c.get("alt", []))
                if not ok:
                    n = len(c["hex"].split())
                    detail = f"bytes at +{c.get('at', 0)}: expected {c['hex']}, found {im.read(at, n).hex(' ').upper()}"
            elif t == "fnv":
                h = dcs_sig.fnv1a64(im.read(v + c.get("at", 0), c["len"]))
                ok = h == int(c["hash"], 16)
                if not ok:
                    detail = f"FNV over {c['len']} bytes: expected {c['hash']}, now 0x{h:016x}"
                    r.setdefault("new_hashes", []).append({"refs": c.get("refs", []), "begin": hx(v),
                                                           "end": hx(v + c["len"]), "hash": f"0x{h:016x}"})
                elif v != r["old"]:
                    r.setdefault("new_hashes", []).append({"refs": c.get("refs", []), "begin": hx(v),
                                                           "end": hx(v + c["len"]), "hash": c["hash"],
                                                           "note": "same bytes, new address"})
            elif t == "call_target":
                insn = im.insn_at(v)
                tgt = self.new_value(c["target"])
                ok = insn is not None and insn.mnemonic == "call" and im.rip_target(insn) == tgt
                if not ok:
                    detail = f"not a call to {c['target']}"
            elif t == "vtable_len":
                n = 0
                while im.in_code(im.slot(v, n) or 0) and n < 1024:
                    n += 1
                ok = n == c["value"]
                if not ok:
                    detail = f"vtable has {n} code entries, source expects {c['value']}"
            if not ok:
                r["failed_checks"].append(detail or t)
        if r["failed_checks"]:
            r["status"] = "changed"


def calls_to(im, func, target):
    fb = im.func_of(func)
    if not fb:
        return []
    return [i.address for i in im.insns(fb[0], fb[1]) if i.mnemonic == "call" and im.rip_target(i) == target]


# ---------------------------------------------------------------------------
def report(manifest, checker, res, show_all):
    lines = []
    order = ["lost", "ambiguous", "changed", "moved", "unchanged", "skipped"]
    counts = {s: 0 for s in order}
    for r in res.values():
        counts[r["status"]] += 1
    by_mod = {}
    for e in manifest["entries"]:
        by_mod.setdefault(e["module"], []).append(e)
    lines.append(f"DCS bin: {checker.bin_dir}")
    for mod, im in sorted(checker.images.items()):
        if im is None:
            continue
        rec = manifest["dcs_build"].get(mod, {}).get("timestamp")
        now = hx(im.timestamp)
        lines.append(f"  {mod:26s} timestamp {now}" + ("" if rec == now else f"  (manifest derived from {rec})"))
    lines.append("")
    for mod in sorted(by_mod):
        rows = []
        for e in by_mod[mod]:
            r = res[e["id"]]
            if r["status"] == "unchanged" and not show_all:
                continue
            arrow = hx(r["old"]) if r["new"] in (None, r["old"]) else f"{hx(r['old'])} -> {hx(r['new'])}"
            rows.append(f"  [{r['status']:9s}] {e['id']:58s} {arrow}  ({r['how']})")
            for f in r["failed_checks"]:
                rows.append(f"               check failed: {f}")
            for n in r["notes"]:
                rows.append(f"               note: {n}")
        n_un = sum(1 for e in by_mod[mod] if res[e["id"]]["status"] == "unchanged")
        lines.append(f"{mod}: {len(by_mod[mod])} locations, {n_un} unchanged")
        lines += rows
    lines.append("")
    lines.append("summary: " + ", ".join(f"{k} {v}" for k, v in counts.items() if v))
    return "\n".join(lines), counts


def suggestion(manifest, checker, res):
    changes = []
    for e in manifest["entries"]:
        r = res[e["id"]]
        if r["status"] == "unchanged":
            continue
        item = {"id": e["id"], "module": e["module"], "kind": e["kind"], "status": r["status"],
                "old": hx(r["old"]), "new": hx(r["new"]), "how": r["how"],
                "value_hard_coded": e.get("value_from") == "source",
                "edit": [f"{x['file']}:{x['line']} {x['symbol']}" for x in e.get("refs", [])]}
        if r["failed_checks"]:
            item["failed_checks"] = r["failed_checks"]
        if r["notes"]:
            item["notes"] = r["notes"]
        if r.get("new_hashes"):
            item["new_hashes"] = r["new_hashes"]
        if r.get("new_slot"):
            item["new_slot"] = r["new_slot"]
        changes.append(item)
    return {"dcs_bin": checker.bin_dir,
            "builds": {m: {"manifest": manifest["dcs_build"].get(m, {}).get("timestamp"), "now": hx(i.timestamp)}
                       for m, i in sorted(checker.images.items()) if i},
            "changes": changes}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dcs-bin", help="DCS bin folder (default: $DCS_BIN, else the installed DCS from the registry)")
    ap.add_argument("--manifest", default=os.path.join(HERE, "hooks_manifest.json"))
    ap.add_argument("--json", default="dcs_patch_suggestion.json", help="patch suggestion output")
    ap.add_argument("--module", action="append", default=[], metavar="NAME=PATH", help="use PATH for module NAME")
    ap.add_argument("--qvfr", help="Quad-Views-Foveated layer DLL, to check the pose-sweep detour too")
    ap.add_argument("--all", action="store_true", help="list unchanged locations too")
    a = ap.parse_args()
    bin_dir = dcs_sig.dcs_bin_dir(a.dcs_bin)
    with open(a.manifest, encoding="utf-8") as f:
        manifest = json.load(f)
    overrides = {}
    for spec in a.module:
        name, _, path = spec.partition("=")
        overrides[name.lower()] = path
    ck = Checker(manifest, bin_dir, overrides, a.qvfr)
    res = ck.run()
    text, counts = report(manifest, ck, res, a.all)
    print(text)
    with open(a.json, "w", encoding="utf-8", newline="\n") as f:
        json.dump(suggestion(manifest, ck, res), f, indent=1)
        f.write("\n")
    print(f"patch suggestion: {a.json}")
    if counts["lost"] or counts["ambiguous"]:
        sys.exit(2)
    if counts["moved"] or counts["changed"]:
        sys.exit(1)


if __name__ == "__main__":
    main()
