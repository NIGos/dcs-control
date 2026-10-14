"""Verify archives, packaged assets and recorded offline results without launching software."""
import argparse
import base64
import hashlib
import os
import re
import json
from pathlib import Path
import struct
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

HOME = str(Path.home()).lower()
HOME_FORMS = {HOME.encode(), HOME.replace('\\', '/').encode(), HOME.encode('utf-16-le'), HOME.replace('\\', '/').encode('utf-16-le')}
def personal_data(name, data):
    """The build machine's user profile path (a user name) in UTF-8 or UTF-16, and the release signing key, which never
    leaves the release machine's %USERPROFILE%\\.dcs-control-signing."""
    low = data.lower()
    assert not any(form in low for form in HOME_FORMS), 'Build machine path in the package: ' + name
    assert b'PRIVATE' + b' KEY' not in data, 'A private key in the package: ' + name  # split: this script is in the source ZIP

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
                        for inner in nested.namelist():
                            personal_data(n + '/' + inner, nested.read(inner))
            manifest = json.loads(z.read(release.name+'/release-manifest.json'))
            for f in manifest['files']:
                data = z.read(release.name+'/'+f['path'].replace('\\','/'))
                assert len(data) == f['bytes'] and digest(data) == f['sha256'], f['path']
            for asset in ['ui/index.html','ui/app.js','ui/setup.js','ui/app.css','ui/theme.css','ui/keyart.svg','WebView2Loader.dll','Microsoft.Web.WebView2.Core.dll','licenses/WebView2-SDK.txt','licenses/QuadViews-THIRD-PARTY.txt','licenses/WIL-MIT.txt','docs/SETUP.md','docs/FRAME_PACING.md']:
                assert release.name+'/files/'+asset in z.namelist(), asset
            assert not any('interface-fixture/' in n for n in z.namelist()), 'Test fixture accidentally bundled'
            # No file in the package, binaries included (compiled-in source paths), may carry the build machine's user
            # profile path (a user name); the setup carries this same ZIP.
            for n in z.namelist():
                personal_data(n, z.read(n))
            result['manifestFiles'] = len(manifest['files'])
        else:
            assert not any(n.startswith('upstream/') and n.lower().endswith(('.exe','.dll','.lib','.pdb')) for n in z.namelist()), 'General-purpose vendor build binaries were bundled'
            current = subprocess.check_output(['git','-c','core.autocrlf=false','ls-files','-z','--cached','--others','--exclude-standard'],cwd=root).decode().split('\0')
            own = [p for p in current if p and (root/p).is_file()]
            for p in own:
                assert z.read('manager/'+p) == (root/p).read_bytes(), 'Source differs: '+p
            assert 'manager/src/DcsVr.App/MainWindow.xaml' not in z.namelist(), 'Old GUI still packaged'
            result['managerSourceFiles'] = len(own)
            # The source ZIP carries every untracked file and the upstream snapshots: the same check.
            for n in z.namelist():
                personal_data(n, z.read(n))
        result[kind] = {'path':str(archive), 'bytes':archive.stat().st_size, 'sha256':digest(archive.read_bytes()), 'entries':len(z.namelist())}
        assert Path(str(archive)+'.sha256').read_text().strip() == result[kind]['sha256']

# The setup: the setup program followed by exactly this release's ZIP, its SHA-256, offset and length and the marker the
# setup checks before it unpacks anything.
setup = root/f'artifacts/release/DcsControl-{version}-Setup.exe'
setup_bytes = setup.read_bytes()
zip_bytes = Path(str(release)+'.zip').read_bytes()
offset, length = struct.unpack('<QQ', setup_bytes[-24:-8])
assert setup_bytes[-8:] == b'DCSCSFX1' and offset + length + 56 == len(setup_bytes), 'Setup trailer'
assert setup_bytes[offset:offset+length] == zip_bytes and setup_bytes[-56:-24] == hashlib.sha256(zip_bytes).digest(), 'The setup does not carry this release ZIP'
assert setup_bytes[:2] == b'MZ' and b'requestedExecutionLevel level="asInvoker"' in setup_bytes[:offset], 'The setup must run without administrator rights'
assert Path(str(setup)+'.sha256').read_text().strip() == digest(setup_bytes)
assert not any(p.lower().endswith(('.pem', '.key', '.pfx')) for p in os.listdir(setup.parent)), 'A key file next to the release files'

# The setup and the ZIP are signed with the release key: base64 of the IEEE P1363 (r, s) ECDSA P-256 / SHA-256
# signature, checked with the public key DCS Control embeds (src/DcsVr.Core/Updates.cs), as its updater does.
public_pem = re.search(r'-----BEGIN PUBLIC KEY-----[\s\S]+?-----END PUBLIC KEY-----', (root/'src/DcsVr.Core/Updates.cs').read_text(encoding='utf-8')).group(0)
public_pem = '\n'.join(line.strip() for line in public_pem.splitlines()) + '\n'
def signature_valid(path):
    signature = base64.b64decode(Path(str(path)+'.sig').read_text(encoding='ascii').strip(), validate=True)
    assert len(signature) == 64, 'Signature length: ' + path.name
    try:
        from cryptography.exceptions import InvalidSignature
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import ec
        from cryptography.hazmat.primitives.asymmetric.utils import encode_dss_signature
    except ImportError:
        # Without the cryptography module, .NET (PowerShell 7) verifies it the way the app does.
        script = ('$k=[Security.Cryptography.ECDsa]::Create();$k.ImportFromPem($env:DCSC_PUBLIC_KEY);$s=[IO.File]::OpenRead($env:DCSC_FILE);'
                  'try{if($k.VerifyData($s,[Convert]::FromBase64String($env:DCSC_SIGNATURE),[Security.Cryptography.HashAlgorithmName]::SHA256,'
                  '[Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation)){exit 0}else{exit 1}}finally{$s.Dispose()}')
        env = dict(os.environ, DCSC_PUBLIC_KEY=public_pem, DCSC_FILE=str(path), DCSC_SIGNATURE=base64.b64encode(signature).decode())
        return subprocess.run(['pwsh', '-NoProfile', '-Command', script], env=env).returncode == 0
    key = serialization.load_pem_public_key(public_pem.encode())
    assert isinstance(key, ec.EllipticCurvePublicKey) and key.curve.name == 'secp256r1'
    try:
        key.verify(encode_dss_signature(int.from_bytes(signature[:32], 'big'), int.from_bytes(signature[32:], 'big')), path.read_bytes(), ec.ECDSA(hashes.SHA256()))
        return True
    except InvalidSignature:
        return False
for signed in (setup, Path(str(release)+'.zip')):
    assert signature_valid(signed), 'Release signature does not verify: ' + signed.name
result['setup'] = {'path': str(setup), 'bytes': len(setup_bytes), 'sha256': digest(setup_bytes), 'signed': True}

result['manager'] = json.loads(Path(args.manager).read_text(encoding='utf-8-sig'))
assert result['manager']['passed'] == result['manager']['total'] and result['manager']['total'] >= 124
fixture = Path(args.fixture)
result['installer'] = json.loads((fixture/'result.json').read_text(encoding='utf-8-sig'))
assert result['installer']['passed'] and result['installer']['previousReleaseUpgradeChecked'] and result['installer'].get('setupChecked'), 'The setup was not installed and uninstalled in the release test'
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
print(json.dumps({'passed':True,'evidence':str(output),'managerChecks':result['manager']['total'],'webViewChecks':result['packagedWebView']['checks'],'installedWebViewChecks':result['installedWebView']['checks'],'rendersPerRun':result['packagedWebView']['renders'],'binaryBytes':result['binary']['bytes'],'setupBytes':result['setup']['bytes'],'signed':True}))
