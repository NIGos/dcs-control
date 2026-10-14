# Hook signature tools

DcsQvCull hooks a fixed set of locations in DCS's DLLs (vtable slots, functions,
imports, structure offsets). These tools record those locations with
recompile-robust byte signatures. After a DCS update they let you check whether
the locations are still valid, and if not, find where they moved.

The DLLs are only read: nothing in the DCS install is written.

## Requirements

- Python 3.10 or newer
- `pip install capstone pefile`
- DCS World installed. The tools find its `bin` folder automatically: Steam
  libraries first, then Eagle Dynamics' standalone installer. Override with
  `--dcs-bin <folder>` or the `DCS_BIN` environment variable.

## Files

| File | Role |
|---|---|
| `dcs_sig.py` | Shared helpers: PE image model, RTTI vtable lookup, wildcard byte patterns, signature derivation and search, DCS folder detection |
| `src_scan.py` | Finds the hard-coded DCS constants, tables and symbol names in `src/` (file and line) |
| `hooks_curated.py` | Hand-written notes for locations the scanner can't describe on its own |
| `extract_manifest.py` | Builds `hooks_manifest.json` from `src/` plus the current DCS binaries |
| `hooks_manifest.json` | Every location, with its source lines, install-time checks and signature |
| `gen_hook_sigs.py` | Generates `src/hook_sigs.h` (the runtime relocation table) from the manifest |
| `dcs_patch_check.py` | Checks the manifest against a DCS build and writes `dcs_patch_suggestion.json` with the new locations |
| `test_patch_check.py` | Self-test on copies of the DLLs shifted in memory (optional) |

## Steps

1. **After changing hooks in `src/`**, with the DCS version they were written for:

       python tools/extract_manifest.py
       python tools/gen_hook_sigs.py

   The first command rewrites `tools/hooks_manifest.json`, the second rewrites
   `src/hook_sigs.h`. `python tools/gen_hook_sigs.py --check` only verifies
   that both are in step.

2. **After a DCS update:**

       python tools/dcs_patch_check.py

   This lists every location that changed. For each one whose signature is
   found again, it gives the new address in `dcs_patch_suggestion.json`. Apply
   the suggestions in `src/`, then repeat step 1 against the new DCS.

3. **Optional self-tests:**

       python tools/test_patch_check.py

   The runtime side of the relocation is tested by the module's own tests
   (`DcsQvCullSigscanTests`, run by `scripts/build-dcsqvcull.ps1`).

## License

MIT, like the rest of DcsQvCull.
