"""Reproduce the adapter from pristine pinned files, without changing the live checkout."""
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

root = Path(__file__).resolve().parent.parent
upstream = root / 'external/cheeky'
expected = 'd6c18b23d31f7339735114c621bc3f9066e1a32d'
revision = subprocess.check_output(['git', '-C', str(upstream), 'rev-parse', 'HEAD'], text=True).strip()
if revision != expected:
    raise RuntimeError('The reproduction check requires the pinned Cheeky revision.')

fixture = root / 'artifacts/patch-reproduction' / uuid.uuid4().hex
fixture.mkdir(parents=True)
# Every pinned file the patch changes, and every file it adds, from the live checkout: a hand-kept list fell behind.
inputs = [p for p in subprocess.check_output(['git', '-C', str(upstream), 'diff', '--name-only', '-z', expected], text=True).split('\0') if p]
added = [p for p in subprocess.check_output(['git', '-C', str(upstream), 'ls-files', '--others', '--exclude-standard', '-z'], text=True).split('\0') if p]
for relative in inputs:
    path = fixture / 'external/cheeky' / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(subprocess.check_output(['git', '-C', str(upstream), 'show', f'{expected}:{relative}']))

(fixture / 'scripts').mkdir()
shutil.copyfile(root / 'scripts/patch-cheeky.py', fixture / 'scripts/patch-cheeky.py')
shutil.copytree(root / 'patches/cheeky', fixture / 'patches/cheeky')
script = fixture / 'scripts/patch-cheeky.py'
subprocess.run([sys.executable, str(script)], check=True)
outputs = inputs + added
first = {relative: (fixture / 'external/cheeky' / relative).read_text(encoding='utf-8') for relative in outputs}
for relative, value in first.items():
    if value != (upstream / relative).read_text(encoding='utf-8'):
        raise RuntimeError(f'Pristine-source reproduction differs from the built checkout: {relative}')

subprocess.run([sys.executable, str(script)], check=True)
for relative, value in first.items():
    if value != (fixture / 'external/cheeky' / relative).read_text(encoding='utf-8'):
        raise RuntimeError(f'Second patch changed the source: {relative}')
print(f'PASS pristine pinned-source reproduction and idempotence: {len(outputs)} files')
# Kept only when the check fails, to look at.
shutil.rmtree(fixture, ignore_errors=True)
