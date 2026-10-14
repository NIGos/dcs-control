"""Generates src/hook_sigs.h: the runtime signatures of the DCS locations the
production hooks use (src/reloc.h resolves them when DCS moved them).

    python tools/gen_hook_sigs.py [--dcs-bin DIR] [--manifest FILE] [--out FILE] [--check]

Inputs: tools/hooks_manifest.json (patterns, RTTI names, import/export names,
FNV checks) and the DCS binaries the manifest was derived from (read only;
their timestamps must match the manifest's `dcs_build`). The output is
deterministic: same manifest and binaries, same file. Never edit
src/hook_sigs.h by hand; re-run this after extract_manifest.py.

Per location it stores:
  - the module and recorded RVA, and how to find it again: RTTI name + COL
    offset (vtables), import or export name, a unique wildcard pattern, or
    slot N of an in-scope vtable (tiny functions without a unique pattern),
    or a `jmp [IAT]` thunk by import name;
  - for code: a masked FNV-1a hash over the location's function (its .pdata
    range, widened to cover the FNV ranges the hooks check), with every byte
    that changes when only addresses move zeroed: rel32 branch targets,
    RIP-relative displacements, switch-table RVAs, base-relocated bytes, and
    the bytes DcsQvCull itself patches (the allocate detour, the triangle
    counter's lock prefix, the page-size immediate). The byte ranges are
    stored so the runtime needs no disassembler;
  - each FNV check of the hooks (src CodeIs calls): its range, the hash the
    source has, and the same range's masked hash.

Scope: the hooks that run for users by default, i.e. main.cpp's LoadConfig
defaults plus the profile DCS VR Control ships (see SCOPE below). Name-based
hooks (frame heap, timer cache, the Scene collect hook) already look their
targets up by name and need no entry.

--check only verifies: src/hook_sigs.h is up to date, and every
reloc::Rva / reloc::CodeIs use in src/ passes the value the table records.
Exit code 1 on any difference.
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
import src_scan  # noqa: E402

# The production hooks and the locations they read, in output order.
#   main.cpp LoadConfig defaults: ShadowInstancing, ShadowBatching,
#     ShadowPlanAsync, ShadowTextureSkip, BigModelPages, ShaderTimeCache = 1;
#   DCS VR Control's shipped profile adds AllocSlabs, FrameHeapSlabs,
#     PlainTriangleCounter, StreamDedupe, SkipSameConstantBuffer,
#     TaskQueueClock, CostWeights (and PartitionBoost, which hooks nothing).
# Off in both (not here): LowPowerPacer, GBufferBatching, ParallelUpload,
# DirectUpload, SkipSameConstantUpload, the D3D meter, the suites and every
# Diagnostics=1 measurement hook.
SCOPE = [
    # alloc_slab.h: allocslab (AllocSlabs), tricount (PlainTriangleCounter)
    "NG.StructBufferManager.vtbl",
    "NG.StructBufferManager.allocate",
    "NG.StructBufferManager.scan",
    "NG.StructBufferManager.reset",
    "NG.iat.ed_mutex_lock",
    "NG.iat.ed_mutex_unlock",
    "NG.SceneRenderable.render.triCounterAdd",
    # big_pages.h (BigModelPages)
    "NG.StructBufferManager.scan.pageBytesImm",
    # edtime.h (TaskQueueClock)
    "ED.ED_get_time",
    "ED.SyncTaskQueue.proceed.getTimeCall0",
    "ED.SyncTaskQueue.proceed.getTimeCall1",
    "ED.SyncTaskQueue.proceed.getTimeCall2",
    "ED.SyncTaskQueue.proceed.getTimeCall3",
    # tex_bind.h: texbind (StreamDedupe), cbskip (SkipSameConstantBuffer)
    "DX.DX11Texture.vtbl",
    "DX.DX11Texture.requestMip",
    "DX.SConstantBuffer.vtbl",
    "DX.SConstantBuffer.SetConstantBuffer",
    # part_weights.h (CostWeights)
    "VIS.GraphSceneObject.vtbl",
    "VIS.GraphSceneObject.collectRenderables",
    "VIS.GraphSceneObject.partitionWeight",
    "ANIM.CharacterInstanceGraphicsReflection.vtbl",
    "ANIM.CharacterInstanceGraphicsReflection.collectRenderables",
    "ANIM.CharacterInstanceGraphicsReflection.partitionWeight",
    # inst_count.h, shadow_batch.h, shadow_pass.h (ShadowInstancing, ShadowBatching)
    "NG.ShadowMapRenderable.vtbl",
    "NG.ShadowMapRenderable.render",
    "NG.ModelMaterialMT.vtbl",
    "GC.CascadeShadowPass.vtbl",
    "GC.CascadeShadowPass.execute",
    "DX.DX11Renderer.vtbl",
    "DX.DX11Renderer.draw",
    # shadow_tex.h (ShadowTextureSkip)
    "NG.ModelMaterialMT.drawShadow",
    "NG.ModelMaterialMT.submit",
    "NG.model_globals",
    "NG.iat.PropertiesSet_getTexture",
    "NG.iat.Texture2dProperties_valid",
    "DX.DX11Shader.vtbl",
    "DX.DX11Shader.setTexture",
    "DX.DX11Shader.dtor",
    "DX.DX11Texture.getDesc",
    "DX.DX11Texture.compatible",
    "DX.DX11Texture.getSRV",
    "DX.DX11Texture.slot18",
    "DX.DX11Texture.slot11",
    "DX.DX11TextureFromFileInternalImpl.vtbl",
    "DX.DX11TextureArrayFromFileInternalImpl.vtbl",
    "DX.DX11TextureInternalDummy.vtbl",
    "DX.DX11TextureFromFileInternalImpl.slot4",
    "DX.DX11TextureInternalDummy.slot4",
    "DX.ret0_stub",
    "DX.STechnique.vtbl",
    "DX.SPassBlock.vtbl",
    # shadow_inst.h (ShadowInstancing)
    "DX.CEffect.vtbl",
    "DX.CEffect.GetDevice",
    "DX.SShaderGlobalVariable.vtbl",
    "DX.SShaderGlobalVariable.GetVertexShader",
    "DX.DX11Shader.ctor.fieldStores",
    "DX.DX11Shader.keyBuilder",
    "DX.DX11Shader.macroBuilder",
]

# Bytes DcsQvCull itself rewrites in DCS code (id, offset, length): masked in
# the hashes and wildcards in the patterns, so a location is still found
# after a payload reload left its patch in place.
SELF_PATCHED = [
    ("NG.StructBufferManager.allocate", 0, 14),          # alloc_slab.h: 14-byte detour
    ("NG.SceneRenderable.render.triCounterAdd", 0, 1),   # alloc_slab.h: F0 -> 3E prefix
    ("NG.StructBufferManager.scan.pageBytesImm", 1, 4),  # big_pages.h: page-size immediate
    # edtime.h's call sites rewrite rel32 operands, which are masked anyway.
]

CODE_KINDS = ("function", "instruction", "code_hash", "call_site", "vtable_slot")


def ival(v):
    return v if isinstance(v, int) else int(v, 16)


def cname(id):
    return re.sub(r"\W", "_", id)


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def merge(ranges):
    out = []
    for a, b in sorted(ranges):
        if out and a <= out[-1][1]:
            out[-1][1] = max(out[-1][1], b)
        else:
            out.append([a, b])
    return out


class Gen:
    def __init__(self, manifest, bin_dir):
        self.m = manifest
        self.by_id = {e["id"]: e for e in manifest["entries"]}
        self.bin_dir = bin_dir
        self.images = {}
        self.errors = []

    def img(self, module):
        if module not in self.images:
            im = dcs_sig.Image(os.path.join(self.bin_dir, module))
            rec = self.m["dcs_build"].get(module, {})
            if rec.get("timestamp") != f"0x{im.timestamp:x}" or rec.get("size_of_image") != f"0x{im.size:x}":
                raise SystemExit(f"{module}: timestamp 0x{im.timestamp:x} / size 0x{im.size:x} differ from the "
                                 f"manifest's {rec}; run extract_manifest.py first")
            self.images[module] = im
        return self.images[module]

    def self_patched(self, module):
        """Absolute (begin, end) RVA ranges DcsQvCull patches in `module`."""
        out = []
        for id, at, n in SELF_PATCHED:
            e = self.by_id[id]
            if e["module"] == module:
                v = ival(e["value"]) + at
                out.append((v, v + n))
        return out

    def masks(self, im, begin, end, extra):
        """Masked (begin, end) ranges inside [begin, end): decoded linearly
        from begin; every instruction must decode."""
        out = []
        pos = begin
        for insn in im.insns(begin, end):
            if insn.address != pos:
                break
            for k, keep in enumerate(im.insn_mask(insn)):
                if not keep:
                    out.append((pos + k, pos + k + 1))
            pos += insn.size
        if pos < end:
            self.errors.append(f"0x{begin:x}..0x{end:x}: decoding stopped at 0x{pos:x}")
        for a, b in extra:
            a, b = max(a, begin), min(b, end)
            if a < b:
                out.append((a, b))
        return [tuple(r) for r in merge(out)]

    @staticmethod
    def masked_fnv(im, begin, end, masks):
        data = bytearray(im.read(begin, end - begin))
        for a, b in masks:
            a, b = max(a, begin), min(b, end)
            for k in range(a, b):
                data[k - begin] = 0
        return dcs_sig.fnv1a64(bytes(data))

    def pattern(self, im, e, extra):
        """The manifest pattern with self-patched bytes wildcarded; grown
        forward from the location while that is not unique."""
        sig = e["signature"]
        loc = ival(e["value"])
        start = loc - sig["offset"]
        data, mask = dcs_sig.parse_pattern(sig["pattern"])
        mask = [m and not any(a <= start + k < b for a, b in extra) for k, m in enumerate(mask)]
        pat = dcs_sig._trim(dcs_sig.pattern_to_str(data, mask))
        if mask == list(dcs_sig.parse_pattern(sig["pattern"])[1]) or len(im.code_matches(pat, 2)) == 1:
            return pat, sig["offset"]
        data, mask, pos = bytearray(), [], start
        for insn in im.insns(start, start + 1024):
            if insn.address != pos:
                break
            m = im.insn_mask(insn)
            data += insn.bytes
            mask += [k and not any(a <= pos + i < b for a, b in extra) for i, k in enumerate(m)]
            pos += insn.size
            if pos <= loc or sum(mask) < 16:
                continue
            pat = dcs_sig._trim(dcs_sig.pattern_to_str(bytes(data), mask))
            if len(im.code_matches(pat, 2)) == 1:
                return pat, loc - start
        self.errors.append(f"{e['id']}: no unique pattern with the self-patched bytes wildcarded")
        return pat, loc - start

    def via_slot(self, e):
        """(vtable id, slot) to locate a code entry without a usable pattern."""
        if e["kind"] == "vtable_slot":
            return e["vtable"], e["slot"]
        for o in self.m["entries"]:
            if (o["kind"] == "vtable_slot" and o["module"] == e["module"] and o["value"] == e["value"] and
                    o["vtable"] in SCOPE):
                return o["vtable"], o["slot"]
        self.errors.append(f"{e['id']}: no unique pattern and no in-scope vtable slot holds it")
        return None, 0

    def build(self):
        locs, masks, hashes, modules = [], [], [], []
        index = {id: i for i, id in enumerate(SCOPE)}
        for id in SCOPE:
            e = self.by_id.get(id)
            if e is None:
                raise SystemExit(f"{id}: not in the manifest")
            mod = e["module"]
            im = self.img(mod)
            if mod not in modules:
                modules.append(mod)
            loc = ival(e["value"])
            sig = e["signature"]
            r = {"id": id, "module": modules.index(mod), "rva": loc, "kind": "kCode", "via": "kPattern",
                 "rtti": None, "col": 0, "prefix": False, "vlen": 0, "pattern": None, "patoff": 0, "vtable": 0,
                 "slot": 0, "dll": None, "name": None, "fnat": 0, "fnlen": 0, "fnhash": 0, "mfirst": len(masks),
                 "mcount": 0}
            k = e["kind"]
            if k == "vtable":
                r["kind"] = "kVtable"
                r["prefix"] = bool(e.get("rtti_prefix"))
                r["rtti"] = e["rtti"] if r["prefix"] else e["rtti_full"]
                r["col"] = e.get("col_offset", 0)
                for c in e.get("checks", []):
                    if c["type"] == "vtable_len":
                        r["vlen"] = c["value"]
                hits = [h for h in im.vtables_for_rtti(r["rtti"], r["prefix"]) if h["offset"] == r["col"]]
                if [h["vtable"] for h in hits] != [loc]:
                    self.errors.append(f"{id}: RTTI lookup gives {[hex(h['vtable']) for h in hits]}")
            elif k == "iat_import":
                r["kind"] = "kImport"
                r["dll"], r["name"] = e["import"]
            elif k in ("export", "data"):
                if not e.get("export"):
                    raise SystemExit(f"{id}: data without an export is not supported")
                r["kind"] = "kExport"
                r["name"] = e["export"]
            elif k in CODE_KINDS:
                extra = self.self_patched(mod)
                if sig.get("type") == "import_thunk":
                    r["via"] = "kImportThunk"
                    r["dll"], r["name"] = sig["dll"], sig["name"]
                else:
                    if sig.get("type") == "pattern" and sig.get("matches") == 1 and not sig.get("weak"):
                        r["pattern"], r["patoff"] = self.pattern(im, e, extra)
                    else:
                        vt, slot = self.via_slot(e)
                        r["via"] = "kViaVtable"
                        r["vtable"], r["slot"] = (index.get(vt, -1), slot)
                        if r["vtable"] < 0:
                            self.errors.append(f"{id}: vtable {vt} not in scope")
                        # the pattern is then only verified at the slot's target
                        if sig.get("pattern"):
                            r["pattern"], r["patoff"] = sig["pattern"], sig["offset"]
                    # gate: the function (widened to the hooks' FNV ranges)
                    fb, fe = im.extent(loc)
                    for c in e.get("checks", []):
                        if c["type"] == "fnv":
                            fb = min(fb, loc + c["at"])
                            fe = max(fe, loc + c["at"] + c["len"])
                    for a, b in extra:
                        if fb <= a < fe:
                            fe = max(fe, b)
                    ms = self.masks(im, fb, fe, extra)
                    r["fnat"], r["fnlen"] = fb - loc, fe - fb
                    r["fnhash"] = self.masked_fnv(im, fb, fe, ms)
                    for a, b in ms:
                        masks.append((a - loc, b - a))
                    r["mcount"] = len(ms)
                    for c in e.get("checks", []):
                        if c["type"] != "fnv":
                            continue
                        a, n = loc + c["at"], c["len"]
                        if dcs_sig.fnv1a64(im.read(a, n)) != ival(c["hash"]):
                            self.errors.append(f"{id}: FNV check at +{c['at']} does not hold on this binary")
                        hashes.append({"loc": id, "at": c["at"], "len": n, "hash": ival(c["hash"]),
                                       "masked": self.masked_fnv(im, a, a + n, ms)})
                # the pattern still finds the recorded location
                if r["pattern"]:
                    hits = [h + r["patoff"] for h in im.code_matches(r["pattern"], 4)]
                    if r["via"] == "kPattern" and hits != [loc]:
                        self.errors.append(f"{id}: pattern finds {[hex(h) for h in hits]}")
                    if loc not in hits:
                        self.errors.append(f"{id}: pattern does not match at the recorded RVA")
            else:
                raise SystemExit(f"{id}: kind {k} not supported")
            locs.append(r)
        return locs, masks, hashes, modules

    def render(self):
        locs, masks, hashes, modules = self.build()
        L = []
        w = L.append
        w("// Runtime signatures of the DCS locations the production hooks use.")
        w("// GENERATED by tools/gen_hook_sigs.py from tools/hooks_manifest.json and the")
        w("// DCS binaries listed in kModules; do not edit. src/reloc.h reads it.")
        w("#pragma once")
        w("")
        w("#include <cstdint>")
        w("")
        w("namespace hooksig {")
        w("")
        w("enum Kind : uint8_t { kVtable, kCode, kImport, kExport };")
        w("// How a kCode location is found again: unique pattern, slot of a vtable")
        w("// entry (pattern verified at the target), or `jmp [IAT]` by import name.")
        w("enum Via : uint8_t { kPattern, kViaVtable, kImportThunk };")
        w("")
        w("struct Module {")
        w("  const wchar_t* name;")
        w("  uint32_t timestamp, sizeOfImage;  // the analysed build")
        w("};")
        w("struct Mask {  // bytes zeroed before hashing, relative to the location")
        w("  int32_t at;")
        w("  uint16_t len;")
        w("};")
        w("struct Loc {")
        w("  const char* id;")
        w("  uint8_t module;")
        w("  Kind kind;")
        w("  uint32_t rva;  // recorded")
        w("  const char* rtti;  // kVtable: RTTI type name, COL offset, prefix match, entry count (0 = unchecked)")
        w("  uint32_t colOffset;")
        w("  bool rttiPrefix;")
        w("  uint16_t vtableLen;")
        w("  Via via;  // kCode")
        w("  const char* pattern;  // location = match + patOffset")
        w("  int32_t patOffset;")
        w("  uint16_t vtable, slot;  // kViaVtable: Loc index and slot")
        w("  const char* dll;   // kImport, kImportThunk")
        w("  const char* name;  // kImport, kImportThunk, kExport")
        w("  int32_t fnAt;  // kCode gate: masked FNV of [location + fnAt, + fnLen)")
        w("  uint32_t fnLen;")
        w("  uint64_t fnHash;")
        w("  uint32_t maskFirst, maskCount;  // into kMasks")
        w("};")
        w("struct Hash {  // a CodeIs check: [location + at, + len)")
        w("  uint16_t loc;")
        w("  int32_t at;")
        w("  uint32_t len;")
        w("  uint64_t hash, masked;")
        w("};")
        w("")
        w("enum Id : uint16_t {")
        for r in locs:
            w(f"  {cname(r['id'])},")
        w("  kCount")
        w("};")
        w("")
        w("inline constexpr Module kModules[] = {")
        for mod in modules:
            im = self.images[mod]
            w(f"    {{L\"{mod}\", 0x{im.timestamp:08x}u, 0x{im.size:x}u}},")
        w("};")
        w("")
        w("inline constexpr Mask kMasks[] = {")
        row = []
        for at, n in masks:
            row.append(f"{{{at}, {n}}}")
            if len(row) == 8:
                w("    " + ", ".join(row) + ",")
                row = []
        if row:
            w("    " + ", ".join(row) + ",")
        if not masks:
            w("    {0, 0},")
        w("};")
        w("")
        w("inline constexpr Loc kLocs[kCount] = {")
        nul = "nullptr"
        for r in locs:
            w(f"    // {r['id']}")
            w(f"    {{{cstr(r['id'])}, {r['module']}, {r['kind']}, 0x{r['rva']:x}u,")
            w(f"     {cstr(r['rtti']) if r['rtti'] else nul}, {r['col']}u, {'true' if r['prefix'] else 'false'}, "
              f"{r['vlen']},")
            w(f"     {r['via']}, {cstr(r['pattern']) if r['pattern'] else nul}, {r['patoff']}, "
              f"{r['vtable']}, {r['slot']},")
            w(f"     {cstr(r['dll']) if r['dll'] else nul}, {cstr(r['name']) if r['name'] else nul},")
            w(f"     {r['fnat']}, {r['fnlen']}u, 0x{r['fnhash']:016x}ull, {r['mfirst']}u, {r['mcount']}u}},")
        w("};")
        w("")
        w("inline constexpr Hash kHashes[] = {")
        for h in hashes:
            w(f"    {{{cname(h['loc'])}, {h['at']}, {h['len']}u, 0x{h['hash']:016x}ull, 0x{h['masked']:016x}ull}},")
        w("};")
        w("")
        w("}  // namespace hooksig")
        w("")
        return "\n".join(L), locs, hashes


# ---------------------------------------------------------------------------
# Source uses: reloc::Rva(hooksig::X, value) and reloc::CodeIs(hooksig::X, base, begin, end, hash)
# ---------------------------------------------------------------------------

RE_RVA = re.compile(r"reloc::Rva\(\s*hooksig::(\w+)\s*,\s*([\w:\[\]]+)\s*\)")
RE_CODEIS = re.compile(r"reloc::CodeIs\(\s*hooksig::(\w+)\s*,\s*\w+\s*,\s*([\w:]+)\s*,\s*([\w:]+)\s*,\s*([\w:]+)\s*\)")


def check_sources(src_dir, locs, hashes):
    """Every value a source passes next to an id must be the one recorded."""
    consts = {}
    for f in src_scan.scan(src_dir):
        if f["value"] is not None and "::" in f["symbol"]:
            consts.setdefault((f["file"], f["symbol"]), f["value"])
    by_name = {cname(r["id"]): r for r in locs}
    errors, uses = [], 0

    def value(file, ns, tok):
        if tok.startswith("0x"):
            return int(tok, 16)
        return consts.get((file, tok if "::" in tok else f"{ns}::{tok}"))

    for name in sorted(os.listdir(src_dir)):
        if not name.endswith((".h", ".cpp")) or name in src_scan.GENERATED:
            continue
        ns_stack = []
        with open(os.path.join(src_dir, name), encoding="utf-8", errors="replace") as fh:
            for no, line in enumerate(fh, 1):
                m = src_scan.RE_NS.match(line)
                if m:
                    ns_stack.append(m.group(1))
                ns = ns_stack[-1] if ns_stack else ""
                if src_scan.RE_NS_END.match(line) and ns_stack:
                    ns_stack.pop()
                for idn, tok in RE_RVA.findall(line):
                    uses += 1
                    r = by_name.get(idn)
                    v = value(name, ns, tok)
                    if r is None:
                        errors.append(f"{name}:{no}: hooksig::{idn} is not generated")
                    elif v is None:
                        errors.append(f"{name}:{no}: cannot read the value of {tok}")
                    elif v != r["rva"]:
                        errors.append(f"{name}:{no}: {tok} = 0x{v:x} but hook_sigs.h records 0x{r['rva']:x} for {idn}")
                for idn, b, en, h in RE_CODEIS.findall(line):
                    uses += 1
                    r = by_name.get(idn)
                    vb, ve, vh = value(name, ns, b), value(name, ns, en), value(name, ns, h)
                    ok = r is not None and None not in (vb, ve, vh) and any(
                        x["loc"] == r["id"] and r["rva"] + x["at"] == vb and x["len"] == ve - vb and x["hash"] == vh
                        for x in hashes)
                    if not ok:
                        errors.append(f"{name}:{no}: CodeIs({idn}, {b}, {en}, {h}) has no matching hash entry")
    return errors, uses


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dcs-bin")
    ap.add_argument("--manifest", default=os.path.join(HERE, "hooks_manifest.json"))
    ap.add_argument("--src", default=os.path.join(HERE, "..", "src"))
    ap.add_argument("--out", default=None, help="default: <src>/hook_sigs.h")
    ap.add_argument("--check", action="store_true", help="verify only; write nothing")
    a = ap.parse_args()
    out = a.out or os.path.join(a.src, "hook_sigs.h")
    with open(a.manifest, encoding="utf-8") as f:
        manifest = json.load(f)
    g = Gen(manifest, dcs_sig.dcs_bin_dir(a.dcs_bin))
    text, locs, hashes = g.render()
    errors = list(g.errors)
    if a.check:
        old = open(out, encoding="utf-8").read() if os.path.isfile(out) else ""
        if old != text:
            errors.append(f"{out} is not up to date (run tools/gen_hook_sigs.py)")
    elif not errors:
        with open(out, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    src_errors, uses = check_sources(os.path.abspath(a.src), locs, hashes)
    errors += src_errors
    n_mask = sum(r["mcount"] for r in locs)
    print(f"{len(locs)} locations in {len({r['module'] for r in locs})} modules, {len(hashes)} FNV checks, "
          f"{n_mask} masked ranges; {uses} reloc uses in src/" + ("" if a.check else f" -> {os.path.relpath(out)}"))
    for x in errors:
        print("ERROR " + x)
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
