"""Package the corresponding source: this repository as committed, and the patched upstream trees it was built from.

Run with the Python launcher:  py -3 scripts/package-sources.py

It refuses a working tree with uncommitted changes or untracked files (the archive would not be what the commit says),
and upstream trees in external/ that were not built from the scripts and patches checked out here (external/ holds
whatever was built last, on any branch). Only tracked files of this repository are packed.

  --allow-dirty         pack the tracked files as they are on disk, uncommitted edits included
  --include-untracked   also pack untracked files that are not ignored (implies --allow-dirty)
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import zipfile

from product_version import product_version

parser = argparse.ArgumentParser(description='Package the corresponding source ZIP.')
parser.add_argument('--allow-dirty', action='store_true', help='pack although the working tree has uncommitted changes or untracked files')
parser.add_argument('--include-untracked', action='store_true', help='also pack untracked, non-ignored files (implies --allow-dirty)')
options = parser.parse_args()

root = Path(__file__).resolve().parent.parent


def git(path, *arguments):
    return subprocess.check_output(['git', '-C', str(path), *arguments]).decode()


dirty = [line for line in git(root, 'status', '--porcelain').splitlines() if line.strip()]
if dirty and not (options.allow_dirty or options.include_untracked):
    shown = '\n  '.join(dirty[:20]) + ('\n  ...' if len(dirty) > 20 else '')
    sys.exit('The working tree is not clean, so the archive would not be the committed source:\n  ' + shown
             + '\nCommit or remove these, or pass --allow-dirty (tracked files as they are on disk; --include-untracked adds the untracked ones).')

# The upstream trees must be the ones this checkout's scripts and patches produce (build stamps, OFXR patch marker).
check = subprocess.run(['pwsh', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(root / 'scripts/assert-components-current.ps1')])
if check.returncode != 0:
    sys.exit('The upstream sources in external/ do not match this checkout: nothing was packed.')

destination = root / f'artifacts/release/DcsControl-{product_version()}-sources.zip'
destination.parent.mkdir(parents=True, exist_ok=True)
revisions = {}
added = set()

with zipfile.ZipFile(destination, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
    def add(path, name):
        # General-purpose SDK/build executables have their own vendor distribution.
        # Obtain them from the pinned official checkouts; keep all modified source here.
        if name.startswith('upstream/') and path.suffix.lower() in {'.exe','.dll','.lib','.pdb'}:
            return
        if path.is_file() and name not in added:
            archive.write(path, name)
            added.add(name)

    def repository(path, prefix):
        # An upstream tree as it was built: the patches edit tracked files and add new ones, so untracked files count.
        revision = git(path, 'rev-parse', 'HEAD').strip()
        revisions[prefix] = revision
        files = git(path, 'ls-files', '-z', '--cached', '--others', '--exclude-standard').split('\0')
        for relative in filter(None, files):
            full = path / relative
            if full.is_dir() and (full / '.git').exists():
                repository(full, prefix + '/' + relative)
            else:
                add(full, prefix + '/' + relative)

    listing = ['ls-files', '-z', '--cached'] + (['--others', '--exclude-standard'] if options.include_untracked else [])
    for relative in filter(None, git(root, *listing).split('\0')):
        add(root / relative, 'manager/' + relative)
    revisions['manager'] = git(root, 'rev-parse', 'HEAD').strip() + (' (with uncommitted changes)' if dirty else '')
    for name in ['ofxr', 'ofxr-djules75', 'cheeky', 'quadviews', 'sboys']:
        repository(root / 'external' / name, 'upstream/' + name)
    # OFXR's build dependency is a separate checkout, not an upstream gitlink.
    repository(root / 'external/ofxr/external/FidelityFX-SDK-v1.1.4', 'upstream/ofxr/external/FidelityFX-SDK-v1.1.4')
    archive.writestr('UPSTREAM_REVISIONS.json', json.dumps(revisions, indent=2))
    archive.writestr('SOURCE_README.txt', 'Local preview corresponding source. Build instructions: manager/README.md and manager/docs/THIRD_PARTY.md.\nThe Cheeky focus adapter, Quad Views settings patch and FidelityFX build fixes are included. The shipped OFXR layer is upstream/ofxr-djules75 (tag 0.2.9.1) plus manager/patches/ofxr-djules75/*.patch, applied by scripts/build-ofxr-djules75.ps1.\nUPSTREAM_REVISIONS.json lists base commits; snapshots include local modifications.\nGeneral-purpose SDK executables, binary libraries and debug symbols are excluded. Obtain MSVC, Windows SDK and the FidelityFX SDK build tools from the pinned official distributions before overlaying these source snapshots.\nSboys private functionality is not included. Vendor packages retain their own terms.\n')

digest = hashlib.sha256(destination.read_bytes()).hexdigest()
destination.with_suffix('.zip.sha256').write_text(digest + '\n', encoding='ascii')
print(f'{destination}: {len(added)} source files, {destination.stat().st_size:,} bytes')
