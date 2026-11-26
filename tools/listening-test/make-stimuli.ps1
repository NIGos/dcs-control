# Part A's sounds: 24 short stimuli (8 directions at cockpit distance x Off, Subtle, Normal of the generic cockpit),
# rendered by the native render tool with your HRTF. Writes seed.txt, trials.json (what the runner plays), key.json
# (the answers, for the analysis only: do not open it before the test), dry\ and stimuli\ into the results folder.
#   .\make-stimuli.ps1 [-Folder <results>] [-Tool <binaural_room_render.exe>] [-Hrtf <set.dcshrtf>] [-DryRun]
# -DryRun prints the render tool's command lines and renders nothing.
param([string]$Folder, [string]$Tool, [string]$Hrtf, [switch]$DryRun, [string]$Cli)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"
$exe = Find-Cli $Cli
$folder = Get-ResultsFolder $Folder
if (-not $Tool -and -not $DryRun) {
  $Tool = Get-ChildItem -Path (Join-Path $RepoRoot 'artifacts') -Filter binaural_room_render.exe -Recurse -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
  if (-not $Tool) { throw 'binaural_room_render.exe was not found under artifacts\. Build native/binaural (scripts\build-binaural.ps1) or give it with -Tool.' }
}
$arguments = @('audio-test', 'stimuli', '--folder', $folder)
if ($Tool) { $arguments += @('--tool', $Tool) }
if ($Hrtf) { $arguments += @('--hrtf', $Hrtf) }
if ($DryRun) { $arguments += '--dry-run' }
# The render tool runs below normal priority (the CLI sets it), so the PC stays usable.
Invoke-Cli $exe $arguments
Write-Host "Results folder: $folder"
