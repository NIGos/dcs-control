#Requires -Version 7
# Fetches the generic HRTF of the binaural audio (docs/BINAURAL_AUDIO.md 4.1) from its official source and converts it
# into native/binaural/data/sadie2_ku100.dcshrtf, the set embedded in the engine payload:
#   SADIE II database (University of York), subject D1 = Neumann KU100 dummy head, 48 kHz SOFA, Apache-2.0.
#   Zenodo record 12092466, version v2-2, file D1_HRIR_SOFA.zip.
# The archive is pinned by the size and MD5 Zenodo publishes for it, and by its SHA-256 (recorded on the first fetch in
# docs/THIRD_PARTY.md and $PinnedSha256 below). It is unpacked into .cache/hrtf (untracked, never run), the 48 kHz SOFA
# is converted by DcsVr.Cli hrtf-convert (src/DcsVr.Core/Hrtf), and the licence text and attribution are kept next to the
# converted file. Re-running gives a bit-identical .dcshrtf.
# Not run yet: downloading the archive needs the user's go, and the SOFA reader behind `hrtf-convert` needs the PureHDF
# package (MIT, NuGet), whose download needs the same go (docs/BINAURAL_AUDIO.md, task A, "Pending").
# -Cli: an already built DcsVr.Cli.exe (default: build it with the repository's SDK).
param([string]$Cli)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$workspaceRoot = Split-Path -Parent $PSScriptRoot
$recordUrl = 'https://zenodo.org/api/records/12092466/files/D1_HRIR_SOFA.zip/content'
$expectedBytes = 38151804
$expectedMd5 = '4850c1eb8e63e2d4f605edcdb4d5c883'
$PinnedSha256 = ''  # filled in from the first verified fetch (then every later fetch must match it)

$cache = Join-Path $workspaceRoot '.cache/hrtf'
$archive = Join-Path $cache 'D1_HRIR_SOFA.zip'
New-Item -ItemType Directory -Force -Path $cache | Out-Null
if (-not (Test-Path -LiteralPath $archive)) {
    Invoke-WebRequest -Uri $recordUrl -OutFile "$archive.part"
    Move-Item -LiteralPath "$archive.part" -Destination $archive -Force
}
$size = (Get-Item -LiteralPath $archive).Length
$md5 = (Get-FileHash -LiteralPath $archive -Algorithm MD5).Hash.ToLowerInvariant()
$sha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
if ($size -ne $expectedBytes -or $md5 -ne $expectedMd5) {
    Remove-Item -LiteralPath $archive -Force
    throw "D1_HRIR_SOFA.zip does not match the Zenodo record (size $size, MD5 $md5): deleted, nothing converted."
}
if ($PinnedSha256 -and $sha256 -ne $PinnedSha256) { throw "D1_HRIR_SOFA.zip SHA-256 $sha256 differs from the pinned $PinnedSha256." }
Write-Output "D1_HRIR_SOFA.zip verified: $size bytes, MD5 $md5, SHA-256 $sha256"

# Unpack into its own folder (data only: nothing from it is executed).
$unpacked = Join-Path $cache 'D1_HRIR_SOFA'
if (Test-Path -LiteralPath $unpacked) { Remove-Item -LiteralPath $unpacked -Recurse -Force }
Expand-Archive -LiteralPath $archive -DestinationPath $unpacked
$sofa = @(Get-ChildItem -LiteralPath $unpacked -Recurse -File -Filter '*.sofa' | Where-Object { $_.Name -match '48K' })
if ($sofa.Count -ne 1) { throw "Expected one 48 kHz SOFA file in the archive, found $($sofa.Count): $($sofa.Name -join ', ')" }
$sofaSha = (Get-FileHash -LiteralPath $sofa[0].FullName -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "SOFA: $($sofa[0].Name), SHA-256 $sofaSha"

if (-not $Cli) {
    $dotnet = & (Join-Path $PSScriptRoot 'bootstrap-sdk.ps1') -Workspace $workspaceRoot
    & $dotnet build (Join-Path $workspaceRoot 'src/DcsVr.Cli/DcsVr.Cli.csproj') -c Release -nologo -v q
    if ($LASTEXITCODE -ne 0) { throw 'DcsVr.Cli build failed.' }
    $Cli = Join-Path $workspaceRoot 'src/DcsVr.Cli/bin/Release/net10.0-windows/DcsVr.Cli.exe'
}
$data = Join-Path $workspaceRoot 'native/binaural/data'
New-Item -ItemType Directory -Force -Path $data | Out-Null
$output = Join-Path $data 'sadie2_ku100.dcshrtf'
& $Cli hrtf-convert $sofa[0].FullName $output --name 'SADIE II KU100' --source 'SADIE II v2-2 (Univ. of York) D1, Apache-2.0, modified' --generic
if ($LASTEXITCODE -ne 0) { throw 'The conversion failed.' }

# Licence and attribution next to the data (Apache-2.0 section 4: the licence, the notice, the statement of changes).
# The repository carries the Apache-2.0 text as SADIE_II_LICENSE.txt; a licence file in the archive replaces it. Without
# either, or without the NOTICE, the converted set is removed: it is never kept without its licence.
$licence = @(Get-ChildItem -LiteralPath $unpacked -Recurse -File | Where-Object { $_.Name -match '^(LICEN[CS]E|COPYING)' })
if ($licence.Count -ge 1) { Copy-Item -LiteralPath $licence[0].FullName -Destination (Join-Path $data 'SADIE_II_LICENSE.txt') -Force }
foreach ($required in 'SADIE_II_LICENSE.txt', 'NOTICE') {
    if (-not (Test-Path -LiteralPath (Join-Path $data $required))) {
        Remove-Item -LiteralPath $output -Force
        throw "native/binaural/data/$required is missing: the converted set was removed (it is not kept without its Apache-2.0 licence and notice)."
    }
}
Write-Output "Converted: $output ($((Get-Item -LiteralPath $output).Length) bytes, SHA-256 $((Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()))"
Write-Output "Record the archive's SHA-256 in `$PinnedSha256 and docs/THIRD_PARTY.md if it is not there yet."
