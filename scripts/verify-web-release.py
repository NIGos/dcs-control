"""Verify archives, packaged assets and recorded offline results without launching software."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile

from product_version import product_version

parser = argparse.ArgumentParser()
parser.add_argument('--fixture', required=True)
parser.add_argument('--manager', required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
version = product_version()
release = root / f'artifacts/release/DcsControl-{version}-win-x64'
result = {'version': version, 'passed': True, 'actualDcsLaunched': False}

def digest(data):
    return hashlib.sha256(data).hexdigest()

for kind, archive in [('binary', Path(str(release)+'.zip')), ('source', root/f'artifacts/release/DcsControl-{version}-sources.zip')]:
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None, 'Archive CRC failed'
        assert not any(n.lower().endswith('nvngx_dlssnr.dll') for n in z.namelist()), 'Neural runtime must not be bundled'
        if kind == 'binary':
            assert not any('CustomHeadset-1.3.0-Pimax-UI.zip' in n for n in z.namelist()), 'Sboys private binary was bundled'
            policy = json.loads(z.read(release.name+'/files/distribution-policy.json'))
            assert any(c['id'] == 'sboys' and c['delivery'] == 'OfficialDownloadOrImport' for c in policy['components'])
            for n in z.namelist():
                if n.endswith('.zip'):
                    import io
                    with zipfile.ZipFile(io.BytesIO(z.read(n))) as nested:
                        assert nested.testzip() is None
                        assert not any(p.lower().endswith(('nvngx_dlssnr.dll','nvofapi64.dll','_nvngx.dll')) for p in nested.namelist()), 'Vendor runtime accidentally bundled'
            manifest = json.loads(z.read(release.name+'/release-manifest.json'))
            for f in manifest['files']:
                data = z.read(release.name+'/'+f['path'].replace('\\','/'))
                assert len(data) == f['bytes'] and digest(data) == f['sha256'], f['path']
            for asset in ['ui/index.html','ui/app.js','ui/setup.js','ui/app.css','ui/theme.css','ui/keyart.svg','WebView2Loader.dll','Microsoft.Web.WebView2.Core.dll','licenses/WebView2-SDK.txt','licenses/QuadViews-THIRD-PARTY.txt','licenses/WIL-MIT.txt','docs/SETUP.md','docs/FRAME_PACING.md']:
                assert release.name+'/files/'+asset in z.namelist(), asset
            assert not any('interface-fixture/' in n for n in z.namelist()), 'Test fixture accidentally bundled'
            # No text file in the package may carry the build machine's user profile path (a user name).
            home = str(Path.home()).lower().encode()
            for n in z.namelist():
                if n.lower().endswith(('.json', '.txt', '.md', '.cmd', '.ps1', '.ini', '.cfg', '.lua', '.html', '.js', '.css')):
                    assert home not in z.read(n).lower() and home.replace(b'\\', b'/') not in z.read(n).lower(), 'Build machine path in the package: ' + n
            result['manifestFiles'] = len(manifest['files'])
        else:
            assert not any(n.startswith('upstream/') and n.lower().endswith(('.exe','.dll','.lib','.pdb')) for n in z.namelist()), 'General-purpose vendor build binaries were bundled'
            current = subprocess.check_output(['git','-c','core.autocrlf=false','ls-files','-z','--cached','--others','--exclude-standard'],cwd=root).decode().split('\0')
            own = [p for p in current if p and (root/p).is_file()]
            for p in own:
                assert z.read('manager/'+p) == (root/p).read_bytes(), 'Source differs: '+p
            assert 'manager/src/DcsVr.App/MainWindow.xaml' not in z.namelist(), 'Old GUI still packaged'
            result['managerSourceFiles'] = len(own)
        result[kind] = {'path':str(archive), 'bytes':archive.stat().st_size, 'sha256':digest(archive.read_bytes()), 'entries':len(z.namelist())}
        assert Path(str(archive)+'.sha256').read_text().strip() == result[kind]['sha256']

result['manager'] = json.loads(Path(args.manager).read_text(encoding='utf-8-sig'))
assert result['manager']['passed'] == result['manager']['total'] and result['manager']['total'] >= 124
fixture = Path(args.fixture)
result['installer'] = json.loads((fixture/'result.json').read_text(encoding='utf-8-sig'))
assert result['installer']['passed'] and result['installer']['previousReleaseUpgradeChecked']
# Both runs pass every check they make; the installed run (with the NR runtime) makes at least as many, and every
# render fits its window. The counts come from the runs themselves, never from numbers kept here.
for name, prefix in [('packagedWebView',release/'files/docs/interface'),('installedWebView',fixture/'gui')]:
    checks = json.loads(Path(str(prefix)+'-checks.json').read_text())
    renders = json.loads(Path(str(prefix)+'-renders.json').read_text())
    assert checks['passed'] and checks['count'] > 0 and renders, name
    assert all(not r['layout']['overflow'] and r['layout']['footerVisible'] and r['layout']['headerVisible'] for r in renders), name+': a view overflows'
    result[name] = {'checks':checks['count'],'renders':len(renders),'passed':True}
assert result['installedWebView']['checks'] >= result['packagedWebView']['checks'] and result['installedWebView']['renders'] == result['packagedWebView']['renders']

old = root/'artifacts/release/DcsVrControl-0.2.3-preview-win-x64'
for f in json.loads((release/'release-manifest.json').read_text(encoding='utf-8-sig'))['files']:
    p = f['path'].replace('\\','/')
    if not p.startswith('files/'): continue  # the launcher, the installer command, README.txt
    p = p.removeprefix('files/')
    # Components added after 0.2.3 (the deferred OFXR layer) have no baseline to compare against.
    # Components rebuilt on purpose from patched sources (Cheeky OpenXR layer with the DCS quad layout export, the
    # focus adapter) must match their own published hash instead; every other component stays byte-identical.
    rebuilt = p.removesuffix('.sha256') in ('components/CheekyOpenXRLayer.dll', 'components/quadviews/XR_APILAYER_MBUCCHIA_quad_views_foveated.dll') or p.startswith('components/cheeky-focus/') or p.startswith('components/dcsqvcull/') or p.startswith('components/pupilshift/')
    if rebuilt:
        sidecar = release/'files'/(p + '.sha256')
        if not p.endswith('.sha256') and sidecar.exists():
            assert digest((release/'files'/p).read_bytes()) == sidecar.read_text().strip().lower(), 'Rebuilt component hash mismatch: '+p
    elif p.startswith(('components/','packages/')) and (old/p).exists():
        assert digest((release/'files'/p).read_bytes()) == digest((old/p).read_bytes()), 'Native render component changed: '+p
result['nativeRenderComponentsUnchanged'] = True
result['distributionPolicyChecked'] = True
# The pilot's own DCS settings on this PC are untouched by the build and the verification.
baseline = json.loads((root/'artifacts/fps-actual-before.json').read_text(encoding='utf-8-sig')) # written by build-release.ps1
for path, expected in baseline.items():
    assert digest(Path(path).read_bytes()) == expected.lower(), 'Actual settings changed during final verification: '+path
result['actualSettingsReadOnlyCheck'] = baseline
output = root/f'artifacts/release-verification-{version.split("-")[0]}.json'
output.write_text(json.dumps(result,indent=2),encoding='utf-8')
print(json.dumps({'passed':True,'evidence':str(output),'managerChecks':result['manager']['total'],'webViewChecks':result['packagedWebView']['checks'],'installedWebViewChecks':result['installedWebView']['checks'],'rendersPerRun':result['packagedWebView']['renders'],'binaryBytes':result['binary']['bytes']}))
