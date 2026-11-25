"""Generates src/audio_sigs.h: the runtime signatures of the ed_sound.dll locations the binaural audio module hooks
(docs/BINAURAL_AUDIO_HOOKS.md), and checks a DCS build against them.

    python tools/gen_audio_sigs.py [--dcs-bin DIR] [--out FILE]   write src/audio_sigs.h
    python tools/gen_audio_sigs.py --check [--dcs-bin DIR]        header in step with the installed ed_sound.dll?
    python tools/gen_audio_sigs.py --report [--dcs-bin DIR]       after a DCS update: where each location is now

Why a table of its own (not tools/hooks_manifest.json / src/hook_sigs.h): ed_sound is hooked only by the audio
module, its locations are call sites and functions found by pattern, and the shared manifest pipeline cannot be
regenerated from this tree today (extract_manifest.py stops at "binding pacer.h:pacer::kSiteRva: not found by the
source scan"). The format and the rules are the same as hook_sigs.h's kCode entries: a unique wildcard pattern
(location = match + patOffset), then a masked FNV-1a 64 of the location's whole function (its .pdata range, chained
entries merged) with
every byte that changes when only addresses move zeroed (rel32 branch and call targets, RIP-relative displacements,
base-relocated bytes). The masks are stored so the runtime needs no disassembler (src/audio_hooks.h).

The DLL is only read. Output is deterministic.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import sys

import capstone
import pefile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "src", "audio_sigs.h")

# id, recorded RVA, pattern, offset of the location in the pattern, what it is
LOCS = [
    ("P3a", 0x4D40, "49 8B 1C 24 48 8B 8B F0 01 00 00 48 85 C9 74 07 E8 ?? ?? ?? ?? EB", 16,
     "call panner in SND_process's per-voice loop"),
    ("P3b", 0x1BA00, "C7 83 BC 0E 00 00 00 00 80 3B 88 83 C2 0E 00 00 E8 ?? ?? ?? ?? 0F 10 4B 30", 16,
     "call panner when a voice is created"),
    ("P4", 0x1B1FB, "45 8B C4 48 8B D3 48 8B CF FF C5 E8 ?? ?? ?? ?? 85 C0 75", 11,
     "call voice_render in the mix loop"),
    ("P5", 0x20183, "48 8B 55 ?? 48 8D 4B 08 44 8B C7 E8 ?? ?? ?? ?? 48 8B 8B ?? ?? 00 00", 11,
     "call mix in the WASAPI thread loop"),
    ("Panner", 0x1C1C0, "48 8B C4 53 55 56 48 81 EC E0 00 00 00 48 8B 59 10 48 8B E9 48 8B 71 08", 0,
     "panner(Voice*)"),
    ("VoiceRender", 0x165D0, "40 53 48 83 EC 40 80 B9 C0 0E 00 00 00 48 8B D9", 0,
     "int voice_render(Voice*, float* out, uint32 frames)"),
    ("Mix", 0x1B180, "48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 20 48 8B F1 4C 8D 79 20", 0,
     "void mix(Renderer*, float* out, uint32 frames)"),
]


def find_bin(arg):
    for d in (arg, os.environ.get("DCS_BIN")):
        if d and os.path.isfile(os.path.join(d, "ed_sound.dll")):
            return d
    sys.path.insert(0, HERE)
    try:
        import dcs_sig  # noqa: E402

        d = dcs_sig.find_dcs_bin()
        if d and os.path.isfile(os.path.join(d, "ed_sound.dll")):
            return d
    except Exception:  # noqa: BLE001
        pass
    sys.exit("ed_sound.dll not found: pass --dcs-bin or set DCS_BIN")


def parse(pat):
    b, care = [], []
    for t in pat.split():
        if t.startswith("?"):
            b.append(0)
            care.append(False)
        else:
            b.append(int(t, 16))
            care.append(True)
    return b, care


class Image:
    def __init__(self, path):
        self.path = path
        self.pe = pefile.PE(path)
        self.img = self.pe.get_memory_mapped_image()
        self.text = [(s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize) for s in self.pe.sections
                     if s.Characteristics & 0x20000000]
        # .pdata entries; a function split into chained entries (UNW_FLAG_CHAININFO) is one range (primary entry
        # plus the contiguous chained ones after it).
        ents = sorted((e.struct.BeginAddress, e.struct.EndAddress, bool(e.unwindinfo and e.unwindinfo.Flags & 4))
                      for e in self.pe.DIRECTORY_ENTRY_EXCEPTION)
        self.funcs = []
        for lo, hi, chained in ents:
            if chained and self.funcs and self.funcs[-1][1] == lo:
                self.funcs[-1] = (self.funcs[-1][0], hi)
            else:
                self.funcs.append((lo, hi))
        self.relocs = set()
        if hasattr(self.pe, "DIRECTORY_ENTRY_BASERELOC"):
            for blk in self.pe.DIRECTORY_ENTRY_BASERELOC:
                for e in blk.entries:
                    if e.type == 10:  # DIR64
                        self.relocs.add(e.rva)
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.md.detail = True

    def matches(self, pat):
        b, care = parse(pat)
        first = care.index(True)
        out = []
        for lo, hi in self.text:
            i = lo
            while True:
                i = self.img.find(bytes([b[first]]), i + (first if i == lo else 0), hi)
                if i < 0:
                    break
                s = i - first
                if s >= lo and s + len(b) <= hi and all(not c or self.img[s + k] == b[k] for k, c in enumerate(care)):
                    out.append(s)
                i += 1
        return out

    def chunk(self, rva):
        for lo, hi in self.funcs:
            if lo <= rva < hi:
                return lo, hi
        return None

    def masks(self, lo, hi):
        """[(at, len)] relative to lo: rel32 targets, RIP-relative displacements, DIR64 relocations."""
        m = []
        for ins in self.md.disasm(self.img[lo:hi], lo):
            enc = ins.bytes
            x = ins
            # rel32 branches / calls: E8, E9, 0F 8x
            if (enc[0] in (0xE8, 0xE9) and ins.size == 5) or (len(enc) >= 6 and enc[0] == 0x0F and 0x80 <= enc[1] <= 0x8F):
                m.append((ins.address + ins.size - 4 - lo, 4))
            elif x.modrm and ((x.modrm & 0xC7) == 0x05) and x.disp_offset:
                m.append((ins.address + x.disp_offset - lo, 4))
        for r in sorted(self.relocs):
            if lo <= r < hi:
                m.append((r - lo, 8))
        m.sort()
        merged = []
        for at, ln in m:
            if merged and at <= merged[-1][0] + merged[-1][1]:
                pa, pl = merged[-1]
                merged[-1] = (pa, max(pa + pl, at + ln) - pa)
            else:
                merged.append((at, ln))
        return merged

    def masked_fnv(self, lo, hi, masks):
        buf = bytearray(self.img[lo:hi])
        for at, ln in masks:
            for k in range(at, min(at + ln, len(buf))):
                buf[k] = 0
        h = 0xCBF29CE484222325
        for v in buf:
            h ^= v
            h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
        return h


def build(img: Image):
    rows, all_masks, problems = [], [], []
    for lid, rva, pat, off, what in LOCS:
        hits = img.matches(pat)
        if len(hits) != 1:
            problems.append(f"{lid}: pattern {'not unique' if hits else 'not found'} ({len(hits)} matches)")
            rows.append(None)
            continue
        loc = hits[0] + off
        ch = img.chunk(loc)
        if not ch:
            problems.append(f"{lid}: no .pdata function at 0x{loc:x}")
            rows.append(None)
            continue
        masks = img.masks(*ch)
        h = img.masked_fnv(ch[0], ch[1], masks)
        rows.append(dict(id=lid, rva=loc, recorded=rva, pat=pat, off=off, fnAt=ch[0] - loc, fnLen=ch[1] - ch[0],
                         hash=h, maskFirst=len(all_masks), maskCount=len(masks), what=what))
        all_masks.extend(masks)
    return rows, all_masks, problems


def render(img: Image, rows, masks):
    pe = img.pe
    sha = hashlib.sha256(open(img.path, "rb").read()).hexdigest()
    w = []
    w.append("// Runtime signatures of the ed_sound.dll locations the binaural audio module hooks.")
    w.append("// GENERATED by tools/gen_audio_sigs.py from ed_sound.dll; do not edit. src/audio_hooks.h reads it.")
    w.append(f"// Analysed build: ed_sound.dll TimeDateStamp 0x{pe.FILE_HEADER.TimeDateStamp:08x}, SizeOfImage "
             f"0x{pe.OPTIONAL_HEADER.SizeOfImage:x},")
    w.append(f"// SHA-256 {sha}.")
    w.append("#pragma once")
    w.append("")
    w.append("#include <cstdint>")
    w.append("")
    w.append("namespace audsig {")
    w.append("")
    w.append("struct Mask {  // bytes zeroed before hashing, relative to the function chunk's start")
    w.append("  uint16_t at, len;")
    w.append("};")
    w.append("struct Loc {")
    w.append("  const char* id;")
    w.append("  uint32_t rva;         // on the analysed build")
    w.append("  const char* pattern;  // location = match + patOffset")
    w.append("  int32_t patOffset;")
    w.append("  int32_t fnAt;         // the function (.pdata range, chained entries merged) holding the location: [location + fnAt, + fnLen)")
    w.append("  uint32_t fnLen;")
    w.append("  uint64_t fnHash;      // masked FNV-1a 64 of that chunk")
    w.append("  uint32_t maskFirst, maskCount;  // into kMasks")
    w.append("};")
    w.append("")
    w.append(f"inline constexpr uint32_t kTimestamp = 0x{pe.FILE_HEADER.TimeDateStamp:08x}u;")
    w.append(f"inline constexpr uint32_t kSizeOfImage = 0x{pe.OPTIONAL_HEADER.SizeOfImage:x}u;")
    w.append("")
    w.append("enum Id : uint8_t { " + ", ".join(f"k{r['id']}" for r in rows) + ", kCount };")
    w.append("")
    w.append("inline constexpr Mask kMasks[] = {")
    line = "   "
    for at, ln in masks:
        item = f" {{{at}, {ln}}},"
        if len(line) + len(item) > 116:
            w.append(line)
            line = "   "
        line += item
    if line.strip():
        w.append(line)
    w.append("};")
    w.append("")
    w.append("inline constexpr Loc kLocs[] = {")
    for r in rows:
        w.append(f"    // {r['what']}")
        w.append(f"    {{\"{r['id']}\", 0x{r['rva']:x}u, \"{r['pat']}\", {r['off']}, {r['fnAt']}, {r['fnLen']}u,")
        w.append(f"     0x{r['hash']:016x}ull, {r['maskFirst']}u, {r['maskCount']}u}},")
    w.append("};")
    w.append("static_assert(sizeof(kLocs) / sizeof(kLocs[0]) == kCount, \"one row per id\");")
    w.append("")
    w.append("}  // namespace audsig")
    return "\n".join(w) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dcs-bin")
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--report", action="store_true")
    a = ap.parse_args()
    path = os.path.join(find_bin(a.dcs_bin), "ed_sound.dll")
    img = Image(path)
    rows, masks, problems = build(img)
    stamp = img.pe.FILE_HEADER.TimeDateStamp
    if a.report:
        print(f"ed_sound.dll 0x{stamp:08x}")
        for (lid, rva, _p, _o, what), r in zip(LOCS, rows):
            if not r:
                continue
            moved = "" if r["rva"] == rva else f" (moved from 0x{rva:x})"
            print(f"  {lid:<12} 0x{r['rva']:x}{moved}  chunk {r['fnLen']} bytes, {r['maskCount']} masks  - {what}")
        for p in problems:
            print("  NOT FOUND", p)
        if os.path.isfile(a.out):
            text = open(a.out, encoding="utf-8").read()
            for r in rows:
                if r and f"0x{r['hash']:016x}ull" not in text:
                    print(f"  {r['id']}: function chunk differs from src/audio_sigs.h (the audio module stays off "
                          "until the hooks are re-checked and this header regenerated)")
        return 1 if problems else 0
    if problems:
        for p in problems:
            print("ERROR", p)
        return 1
    text = render(img, rows, masks)
    if a.check:
        cur = open(a.out, encoding="utf-8").read() if os.path.isfile(a.out) else ""
        if cur != text:
            print(f"src/audio_sigs.h is not in step with {path} (0x{stamp:08x}); run tools/gen_audio_sigs.py")
            return 1
        print(f"src/audio_sigs.h in step with ed_sound.dll 0x{stamp:08x}")
        return 0
    with open(a.out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print(f"wrote {os.path.relpath(a.out)}: {len(rows)} locations, {len(masks)} masked ranges (ed_sound 0x{stamp:08x})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
