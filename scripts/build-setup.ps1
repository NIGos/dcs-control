# Builds the DCS Control setup (native/setup). Without arguments: the program alone, artifacts/native/setup/DcsControlSetup.exe.
# With -Package <release ZIP> -Output <exe>: DcsControl-<version>-Setup.exe, that program followed by the ZIP and a trailer
# (the ZIP's SHA-256, its offset and length, then the marker "DCSCSFX1"), which the setup checks before unpacking.
param([string]$Package, [string]$Output)
$ErrorActionPreference = 'Stop'
$workspaceRoot = Split-Path -Parent $PSScriptRoot
$wrapper = Join-Path $PSScriptRoot 'msvc.cmd'
$source = Join-Path $workspaceRoot 'native/setup'
$build = Join-Path $workspaceRoot 'artifacts/native/setup'
New-Item -ItemType Directory -Path $build -Force | Out-Null
# The version resource and the texts follow the product version (Directory.Build.props).
$version = & (Join-Path $PSScriptRoot 'version.ps1')
$numbers = @(([regex]::Match($version, '^\d+\.\d+\.\d+').Value -split '\.') + '0')
@("#define DCSC_VERSION $($numbers -join ',')", "#define DCSC_VERSION_TEXT `"$version`"") | Set-Content -LiteralPath (Join-Path $build 'version.h') -Encoding ascii
& $wrapper rc /nologo /i $build /fo (Join-Path $build 'setup.res') (Join-Path $source 'setup.rc')
if ($LASTEXITCODE -ne 0) { throw 'Setup resources failed.' }
$stub = Join-Path $build 'DcsControlSetup.exe'
& $wrapper cl /nologo /utf-8 /std:c++20 /EHsc /O2 /W4 /WX /MT /DUNICODE /D_UNICODE "/I$build" "/Fo:$build\" "/Fe:$stub" (Join-Path $source 'setup.cpp') (Join-Path $build 'setup.res') user32.lib shell32.lib ole32.lib comctl32.lib bcrypt.lib shlwapi.lib advapi32.lib gdi32.lib /link /SUBSYSTEM:WINDOWS /MANIFEST:NO
if ($LASTEXITCODE -ne 0) { throw 'Setup build failed.' }
if (-not $Package) { return $stub }
if (-not $Output) { throw 'Name the setup to write with -Output.' }
$zip = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Package).Path)
$program = [IO.File]::ReadAllBytes($stub)
$digest = [Security.Cryptography.SHA256]::Create().ComputeHash($zip)
$stream = [IO.File]::Create($Output)
try {
    $stream.Write($program, 0, $program.Length)
    $stream.Write($zip, 0, $zip.Length)
    $stream.Write($digest, 0, 32)
    $stream.Write([BitConverter]::GetBytes([uint64]$program.Length), 0, 8)
    $stream.Write([BitConverter]::GetBytes([uint64]$zip.Length), 0, 8)
    $stream.Write([Text.Encoding]::ASCII.GetBytes('DCSCSFX1'), 0, 8)
} finally { $stream.Dispose() }
$Output
