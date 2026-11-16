#Requires -Version 7
# Checks the files of this version that get published, without starting anything. scripts/test-release.ps1 runs it last.
#   - Neither archive (the release ZIP, and the sources ZIP when there is one) holds a private key or the build
#     machine's user profile path, in UTF-8 or UTF-16, in any file or in a ZIP inside it.
#   - The setup is the setup program followed by exactly this release ZIP and the trailer it checks before unpacking.
#   - The setup's and the ZIP's signatures verify with the public key the app embeds (src/DcsVr.Core/Updates.cs), as
#     the updater checks them. An unsigned developer build passes with a warning; -Signed makes it fail.
#   - The sources ZIP, when there is one, holds every tracked file of this checkout byte for byte.
# -ReleaseFolder: where the archives are (default artifacts/release). Returns what it checked.
param([string]$ReleaseFolder, [switch]$Signed)
$ErrorActionPreference = 'Stop'
$workspaceRoot = Split-Path -Parent $PSScriptRoot
$version = & (Join-Path $PSScriptRoot 'version.ps1')
if (-not $ReleaseFolder) { $ReleaseFolder = Join-Path $workspaceRoot 'artifacts/release' }
$releaseZip = Join-Path $ReleaseFolder "DcsControl-$version-win-x64.zip"
$sourcesZip = Join-Path $ReleaseFolder "DcsControl-$version-sources.zip"
$setup = Join-Path $ReleaseFolder "DcsControl-$version-Setup.exe"
foreach ($file in $releaseZip, $setup) { if (-not (Test-Path -LiteralPath $file)) { throw "Missing release file: $file" } }
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$latin1 = [Text.Encoding]::Latin1

# What must never be in a published file. Bytes are searched as Latin-1 text, so UTF-16 is the same text with a NUL
# after each character. The key marker is put together here: this script is in the sources ZIP it checks.
$userProfile = [Environment]::GetFolderPath('UserProfile')
$needles = [Collections.Generic.List[object]]::new()
foreach ($form in $userProfile, $userProfile.Replace('\', '/')) {
    $needles.Add(@{ Text = $form; What = 'the build machine''s user profile path'; Comparison = [StringComparison]::OrdinalIgnoreCase })
    $needles.Add(@{ Text = $latin1.GetString([Text.Encoding]::Unicode.GetBytes($form)); What = 'the build machine''s user profile path (UTF-16)'; Comparison = [StringComparison]::OrdinalIgnoreCase })
}
$needles.Add(@{ Text = 'PRIVATE' + ' KEY'; What = 'a private key'; Comparison = [StringComparison]::Ordinal })
function Read-Entry($entry) {
    $stream = $entry.Open()
    try { $buffer = [IO.MemoryStream]::new(); $stream.CopyTo($buffer); return ,$buffer.ToArray() } finally { $stream.Dispose() }
}
function Assert-Clean([string]$name, [byte[]]$bytes) {
    if ($bytes.Length -eq 0) { return }
    $text = $latin1.GetString($bytes)
    foreach ($needle in $needles) {
        if ($text.IndexOf([string]$needle.Text, [StringComparison]$needle.Comparison) -ge 0) { throw "$name holds $($needle.What)." }
    }
}
function Test-Archive([string]$path) {
    $entries = 0
    $zip = [IO.Compression.ZipFile]::OpenRead($path)
    try {
        foreach ($entry in $zip.Entries) {
            if ($entry.FullName.EndsWith('/')) { continue }
            $bytes = Read-Entry $entry
            $name = (Split-Path -Leaf $path) + ': ' + $entry.FullName
            Assert-Clean $name $bytes
            $entries++
            if ($entry.FullName.EndsWith('.zip', [StringComparison]::OrdinalIgnoreCase)) {
                $inner = [IO.Compression.ZipArchive]::new([IO.MemoryStream]::new($bytes, $false), [IO.Compression.ZipArchiveMode]::Read)
                try { foreach ($nested in $inner.Entries) { if (-not $nested.FullName.EndsWith('/')) { Assert-Clean ($name + '/' + $nested.FullName) (Read-Entry $nested); $entries++ } } }
                finally { $inner.Dispose() }
            }
        }
    } finally { $zip.Dispose() }
    return $entries
}
function Get-Sha256Hex([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Assert-Sidecar([string]$path) {
    $sidecar = $path + '.sha256'
    if (-not (Test-Path -LiteralPath $sidecar) -or (Get-Content -LiteralPath $sidecar -Raw).Trim() -ne (Get-Sha256Hex $path)) { throw "$(Split-Path -Leaf $sidecar) is missing or is not the file's SHA-256." }
}

$result = [ordered]@{ version = $version }
$result.releaseEntries = Test-Archive $releaseZip
Assert-Sidecar $releaseZip

# The sources ZIP: clean like the release ZIP, and every tracked file of this checkout is in it with the bytes on disk.
$result.sourcesChecked = Test-Path -LiteralPath $sourcesZip
if ($result.sourcesChecked) {
    $result.sourcesEntries = Test-Archive $sourcesZip
    Assert-Sidecar $sourcesZip
    $tracked = @((& git -C $workspaceRoot -c core.quotepath=false ls-files -z --cached) -join "`n" -split "`0" | Where-Object { $_ })
    if ($LASTEXITCODE -ne 0 -or $tracked.Count -eq 0) { throw 'git ls-files failed.' }
    $zip = [IO.Compression.ZipFile]::OpenRead($sourcesZip)
    try {
        $byName = @{}; foreach ($entry in $zip.Entries) { $byName[$entry.FullName] = $entry }
        $sha = [Security.Cryptography.SHA256]::Create()
        foreach ($relative in $tracked) {
            $onDisk = Join-Path $workspaceRoot $relative
            if (-not (Test-Path -LiteralPath $onDisk -PathType Leaf)) { continue } # a gitlink, or a tracked file deleted on disk
            $entry = $byName['manager/' + $relative]
            if (-not $entry) { throw "The sources ZIP does not have $relative. Make it again (py -3 scripts/package-sources.py)." }
            $packed = Read-Entry $entry
            if ([Convert]::ToHexString($sha.ComputeHash($packed)) -ne (Get-FileHash -LiteralPath $onDisk -Algorithm SHA256).Hash) { throw "The sources ZIP differs from this checkout: $relative. Make it again (py -3 scripts/package-sources.py)." }
        }
    } finally { $zip.Dispose() }
    $result.sourcesFiles = $tracked.Count
} else { Write-Warning "No sources ZIP for $version next to the release (py -3 scripts/package-sources.py makes it): not checked." }

# The setup: the setup program, then this release's ZIP, its SHA-256 (32 bytes), offset and length (8 bytes each) and
# the marker the setup looks for before it unpacks anything.
Assert-Sidecar $setup
$zipLength = (Get-Item -LiteralPath $releaseZip).Length
$stream = [IO.File]::OpenRead($setup)
try {
    if ($stream.Length -lt 56) { throw 'The setup is too short to carry a package.' }
    $trailer = [byte[]]::new(56); $stream.Position = $stream.Length - 56; $stream.ReadExactly($trailer, 0, 56)
    $offset = [BitConverter]::ToUInt64($trailer, 32); $length = [BitConverter]::ToUInt64($trailer, 40)
    if ([Text.Encoding]::ASCII.GetString($trailer, 48, 8) -ne 'DCSCSFX1' -or $offset + $length + 56 -ne $stream.Length) { throw 'The setup''s trailer is wrong.' }
    if ($length -ne $zipLength) { throw 'The setup does not carry this release ZIP (length).' }
    $program = [byte[]]::new($offset); $stream.Position = 0; $stream.ReadExactly($program, 0, [int]$offset)
    $sha = [Security.Cryptography.SHA256]::Create()
    $package = [byte[]]::new(1MB); $left = [long]$length
    while ($left -gt 0) {
        $read = $stream.Read($package, 0, [int][Math]::Min($package.Length, $left)); if ($read -le 0) { throw 'The setup ended inside its package.' }
        [void]$sha.TransformBlock($package, 0, $read, $null, 0); $left -= $read
    }
    [void]$sha.TransformFinalBlock([byte[]]::new(0), 0, 0)
    $carried = [Convert]::ToHexString($sha.Hash).ToLowerInvariant()
} finally { $stream.Dispose() }
if ($carried -ne (Get-Sha256Hex $releaseZip) -or $carried -ne [Convert]::ToHexString($trailer, 0, 32).ToLowerInvariant()) { throw 'The setup does not carry this release ZIP.' }
if ($program[0] -ne 0x4D -or $program[1] -ne 0x5A -or -not $latin1.GetString($program).Contains('requestedExecutionLevel level="asInvoker"')) { throw 'The setup must run without administrator rights.' }
Assert-Clean (Split-Path -Leaf $setup) $program
$keys = @(Get-ChildItem -LiteralPath $ReleaseFolder -File | Where-Object { $_.Extension -in '.pem', '.key', '.pfx' })
if ($keys.Count) { throw "A key file next to the release files: $($keys[0].Name)" }
$result.setupBytes = (Get-Item -LiteralPath $setup).Length

# Signatures: base64 of the IEEE P1363 (r, s) ECDSA P-256 / SHA-256 signature, checked with the embedded public key.
$source = Get-Content -LiteralPath (Join-Path $workspaceRoot 'src/DcsVr.Core/Updates.cs') -Raw
$pem = [regex]::Match($source, '-----BEGIN PUBLIC KEY-----[\s\S]+?-----END PUBLIC KEY-----')
if (-not $pem.Success) { throw 'The public key was not found in src/DcsVr.Core/Updates.cs.' }
$key = [Security.Cryptography.ECDsa]::Create()
$key.ImportFromPem((($pem.Value -split "`n" | ForEach-Object { $_.Trim() }) -join "`n"))
$signedFiles = 0
foreach ($file in $setup, $releaseZip) {
    $signatureFile = $file + '.sig'
    if (-not (Test-Path -LiteralPath $signatureFile)) {
        if ($Signed) { throw "Not signed: $(Split-Path -Leaf $file) (build-release.ps1 -Sign)." }
        Write-Warning "$(Split-Path -Leaf $file) is not signed: a developer build. The updater will not install it."
        continue
    }
    $signature = [Convert]::FromBase64String((Get-Content -LiteralPath $signatureFile -Raw).Trim())
    $data = [IO.File]::OpenRead($file)
    try { $valid = $signature.Length -eq 64 -and $key.VerifyData($data, $signature, [Security.Cryptography.HashAlgorithmName]::SHA256, [Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation) }
    finally { $data.Dispose() }
    if (-not $valid) { throw "The release signature does not verify: $(Split-Path -Leaf $file)" }
    $signedFiles++
}
$result.signedFiles = $signedFiles
[pscustomobject]$result
