"""Self-test for dcs_patch_check.py on a synthetically shifted DLL.

    python tools/test_patch_check.py [--dcs-bin DIR] [--module NGModel.dll] [--shift 0x1000]

Copies one DCS DLL into a temporary folder and inserts `shift` bytes of int3
padding at the start of .text, so .text's contents and every later section
move up by `shift` while all code-to-code and code-to-data distances stay the
same (as when a patch adds code in front). PE headers, data directories,
base relocations (and the absolute pointers they cover), import and export
tables, .pdata and the RTTI records (complete object locators, class
hierarchy descriptors, base class descriptors) are fixed up, which is
everything the checker reads. Not fixed (the copy is for analysis, it would
not run): unwind/EH handler data, CFG tables, resources, switch-table RVAs
encoded in instructions and jump tables.

Then runs the checker against the real bin folder with that one module
replaced and asserts:
  - every location in the shifted module is "moved" to old + shift (vtables,
    slot targets, patterns, imports, exports, data, call sites), with all
    install-time checks passing at the new address;
  - every other location is "unchanged" (or "skipped" for non-DCS modules).
Also runs the unshifted bin folder and asserts everything is unchanged.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import pefile  # noqa: E402

import dcs_patch_check  # noqa: E402
import dcs_sig  # noqa: E402


def shift_dll(src: str, dst: str, shift: int) -> None:
    pe = pefile.PE(src, fast_load=True)
    pe.parse_data_directories(directories=[
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_DEBUG"],
    ])
    oh = pe.OPTIONAL_HEADER
    assert shift % oh.SectionAlignment == 0 and shift % oh.FileAlignment == 0
    text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
    tva, traw = text.VirtualAddress, text.PointerToRawData
    base = oh.ImageBase
    raw = bytearray(pe.__data__)

    def f(rva):
        return rva + shift if rva >= tva else rva

    # file layout: original sections, mapped by RVA so fix-ups can address them
    sections = [(s.VirtualAddress, s.PointerToRawData, s.SizeOfRawData) for s in pe.sections]

    def off(rva):  # file offset in the ORIGINAL file
        for va, ptr, size in sections:
            if va <= rva < va + size:
                return ptr + rva - va
        raise ValueError(hex(rva))

    def get32(rva):
        return struct.unpack_from("<I", raw, off(rva))[0]

    def put32(rva, v):
        struct.pack_into("<I", raw, off(rva), v)

    def fix32(rva):
        put32(rva, f(get32(rva)))

    exc_rva, exc_size = oh.DATA_DIRECTORY[3].VirtualAddress, oh.DATA_DIRECTORY[3].Size
    rdata = next(s for s in pe.sections if s.Name.startswith(b".rdata"))
    rdata_rva, rdata_size, img_size = rdata.VirtualAddress, rdata.Misc_VirtualSize, oh.SizeOfImage

    # headers
    oh.AddressOfEntryPoint = f(oh.AddressOfEntryPoint)
    oh.SizeOfCode += shift
    oh.SizeOfImage += shift
    oh.CheckSum = 0
    for d in oh.DATA_DIRECTORY:
        if d.VirtualAddress and d.name != "IMAGE_DIRECTORY_ENTRY_SECURITY":
            d.VirtualAddress = f(d.VirtualAddress)
    for s in pe.sections:
        if s.VirtualAddress == tva:
            s.Misc_VirtualSize += shift
            s.SizeOfRawData += shift
        elif s.VirtualAddress > tva:
            s.VirtualAddress += shift
            s.PointerToRawData += shift
    hdr = pe.write()[:oh.SizeOfHeaders]

    # base relocations: block page RVAs and the 64-bit pointers they cover
    for blk in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []):
        for e in blk.entries:
            if e.type == 10:
                o = off(e.rva)
                v = struct.unpack_from("<Q", raw, o)[0]
                struct.pack_into("<Q", raw, o, f(v - base) + base)
        o = blk.struct.get_file_offset()
        struct.pack_into("<I", raw, o, f(blk.struct.VirtualAddress))
    # imports
    for d in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        o = d.struct.get_file_offset()
        for fld in (0, 12, 16):  # OriginalFirstThunk, Name, FirstThunk
            v = struct.unpack_from("<I", raw, o + fld)[0]
            struct.pack_into("<I", raw, o + fld, f(v))
        for arr in (d.struct.OriginalFirstThunk, d.struct.FirstThunk):
            rva = arr
            while True:
                v = struct.unpack_from("<Q", raw, off(rva))[0]
                if v == 0:
                    break
                if not v >> 63:
                    struct.pack_into("<Q", raw, off(rva), f(v))
                rva += 8
    # exports
    if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
        ed = pe.DIRECTORY_ENTRY_EXPORT.struct
        o = ed.get_file_offset()
        for fld in (12, 28, 32, 36):  # Name, AddressOfFunctions, AddressOfNames, AddressOfNameOrdinals
            struct.pack_into("<I", raw, o + fld, f(struct.unpack_from("<I", raw, o + fld)[0]))
        for i in range(ed.NumberOfFunctions):
            fix32(ed.AddressOfFunctions + 4 * i)
        for i in range(ed.NumberOfNames):
            fix32(ed.AddressOfNames + 4 * i)
    # debug directory
    for dbg in getattr(pe, "DIRECTORY_ENTRY_DEBUG", []):
        o = dbg.struct.get_file_offset()
        struct.pack_into("<I", raw, o + 20, f(dbg.struct.AddressOfRawData))
        if dbg.struct.PointerToRawData >= traw:
            struct.pack_into("<I", raw, o + 24, dbg.struct.PointerToRawData + shift)
    # .pdata
    for i in range(exc_size // 12):
        for k in range(3):
            fix32(exc_rva + 12 * i + 4 * k)
    # RTTI: complete object locators found in .rdata, then their hierarchy
    chds, bcds = set(), set()
    for rva in range(rdata_rva, rdata_rva + rdata_size - 24, 4):
        if get32(rva) == 1 and get32(rva + 20) == rva and 0 < get32(rva + 12) < img_size:
            chds.add(get32(rva + 16))
            for k in (12, 16, 20):
                fix32(rva + k)
    for chd in chds:
        n, arr = get32(chd + 8), get32(chd + 12)
        put32(chd + 12, f(arr))
        for i in range(n):
            bcds.add(get32(arr + 4 * i))
            fix32(arr + 4 * i)
    for bcd in bcds:
        fix32(bcd)        # pTypeDescriptor
        fix32(bcd + 24)   # pClassDescriptor

    out = bytearray(hdr) + raw[oh.SizeOfHeaders:traw] + bytes([0xCC]) * shift + raw[traw:]
    with open(dst, "wb") as fh:
        fh.write(out)
    pe.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dcs-bin")
    ap.add_argument("--module", action="append", help="module(s) to shift (default: NGModel.dll, dx11backend.dll)")
    ap.add_argument("--shift", default="0x1000")
    ap.add_argument("--manifest", default=os.path.join(HERE, "hooks_manifest.json"))
    a = ap.parse_args()
    bin_dir = dcs_sig.dcs_bin_dir(a.dcs_bin)
    shift = int(a.shift, 0)
    modules = a.module or ["NGModel.dll", "dx11backend.dll"]
    with open(a.manifest, encoding="utf-8") as f:
        manifest = json.load(f)
    fails = 0

    def expect(cond, what):
        nonlocal fails
        if not cond:
            fails += 1
            print("FAIL " + what)

    # 1. the real binaries: everything unchanged
    res = dcs_patch_check.Checker(manifest, bin_dir, {}).run()
    bad = [r for r in res.values() if r["status"] not in ("unchanged", "skipped")]
    expect(not bad, f"current binaries: {[(r['id'], r['status']) for r in bad]}")
    print(f"current binaries: {sum(r['status'] == 'unchanged' for r in res.values())} unchanged, "
          f"{sum(r['status'] == 'skipped' for r in res.values())} skipped")

    tmp = tempfile.mkdtemp(prefix="dcsqv_shift_")
    try:
        for mod in modules:
            dst = os.path.join(tmp, mod)
            shift_dll(os.path.join(bin_dir, mod), dst, shift)
            # the copy really moved: .text starts with the padding
            im = dcs_sig.Image(dst)
            expect(im.mem[0x1000:0x1000 + 16] == b"\xcc" * 16, f"{mod}: padding not at .text start")
            res = dcs_patch_check.Checker(manifest, bin_dir, {mod.lower(): dst}).run()
            n_moved = 0
            for e in manifest["entries"]:
                r = res[e["id"]]
                if e["module"] != mod:
                    expect(r["status"] in ("unchanged", "skipped"), f"{mod}: {e['id']} {r['status']} (other module)")
                    continue
                old = int(e["value"], 16)
                expect(r["status"] == "moved" and r["new"] == old + shift,
                       f"{mod}: {e['id']} {r['status']} {r['how']} new={r['new'] and hex(r['new'])} "
                       f"expected {hex(old + shift)} {r['failed_checks']}")
                n_moved += r["status"] == "moved"
            total = sum(e["module"] == mod for e in manifest["entries"])
            print(f"{mod} shifted by {hex(shift)}: {n_moved}/{total} locations re-found at old + shift")
            hows = {}
            for e in manifest["entries"]:
                if e["module"] == mod:
                    h = res[e["id"]]["how"].split(":")[0].split(" 0x")[0].split(" .")[0].split(" + ")[0]
                    hows[h] = hows.get(h, 0) + 1
            print("   found by: " + "; ".join(f"{k} {v}" for k, v in sorted(hows.items(), key=lambda kv: -kv[1])))

        # 3. negative cases on the shifted NGModel: a duplicated function body
        # must be reported ambiguous, a damaged one lost, a changed hashed byte changed.
        if "NGModel.dll" in modules:
            ents = {e["id"]: e for e in manifest["entries"]}
            path = os.path.join(tmp, "NGModel.dll")
            data = bytearray(open(path, "rb").read())
            im = dcs_sig.Image(path)
            pe = pefile.PE(path, fast_load=True)
            file_off = pe.get_offset_from_rva
            scan = int(ents["NG.StructBufferManager.scan"]["value"], 16) + shift
            n = len(ents["NG.StructBufferManager.scan"]["signature"]["pattern"].split())
            data[file_off(0x1100):file_off(0x1100) + n] = im.read(scan, n)          # duplicate (in the padding)
            alloc = int(ents["NG.StructBufferManager.allocate"]["value"], 16) + shift
            data[file_off(alloc):file_off(alloc) + 4] = b"\x90\x90\x90\x90"          # damage the prologue
            sub = int(ents["NG.ModelMaterialMT.submit"]["value"], 16) + shift
            off = len(ents["NG.ModelMaterialMT.submit"]["signature"]["pattern"].split()) + 8
            data[file_off(sub + off)] ^= 0xFF                                        # change a hashed byte
            bad = os.path.join(tmp, "NGModel_bad.dll")
            pe.close()
            open(bad, "wb").write(data)
            res = dcs_patch_check.Checker(manifest, bin_dir, {"ngmodel.dll": bad}).run()
            for id, want in (("NG.StructBufferManager.scan", "ambiguous"),
                             ("NG.StructBufferManager.allocate", "lost"),
                             ("NG.ModelMaterialMT.submit", "changed")):
                r = res[id]
                expect(r["status"] == want, f"negative: {id} is {r['status']} ({r['how']}), expected {want}")
                print(f"negative case: {id}: {r['status']} ({r['how']}) {r['failed_checks'][:1]}")

        # 4. a lambda renumbered by an unrelated source edit (scope number
        # ?P@ -> ?O@ in its RTTI name): still found, with a note.
        if "dx11backend.dll" in modules:
            path = os.path.join(tmp, "dx11backend.dll")
            data = bytearray(open(path, "rb").read())
            old_name = b"<lambda_1>@?P@??load@DX11SubresDecMipmapTextureDriver"
            assert data.count(old_name) == 2  # the lambda's type descriptor and the _Func_impl's
            data = bytearray(bytes(data).replace(old_name, old_name.replace(b"?P@", b"?O@")))
            ren = os.path.join(tmp, "dx11backend_renamed.dll")
            open(ren, "wb").write(data)
            res = dcs_patch_check.Checker(manifest, bin_dir, {"dx11backend.dll": ren}).run()
            ents = {e["id"]: e for e in manifest["entries"]}
            for id in ("DX.FuncImpl.mipFinalize.vtbl", "DX.FuncImpl.mipFinalize.doCall"):
                r = res[id]
                want = int(ents[id]["value"], 16) + shift
                expect(r["status"] == "moved" and r["new"] == want, f"renamed lambda: {id} {r['status']} {r['how']}")
            r = res["DX.FuncImpl.mipFinalize.vtbl"]
            print(f"renamed lambda: {r['status']} ({r['how']}) {r['notes']}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("PASS" if not fails else f"{fails} FAILURES")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
