# Part B's test mission: a copy of a mission DCS saved, with the cockpit space test added (one MISSION START trigger
# that runs the test script; an F10 radio menu for the trials and your answers; no key bindings). The source mission
# is only read. Pick one with your aircraft cold on the ramp, for example the module's "Cold and Dark" instant action
# mission (<DCS>\Mods\aircraft\<aircraft>\Missions\QuickStart\...). The copy goes to artifacts\cspace-listening\ in
# this checkout (or -Out); open it in DCS with Mission > Open, or copy it to your Missions folder yourself.
#   .\make-mission.ps1 -Source <mission.miz> [-Out <test mission.miz>]
param([Parameter(Mandatory = $true)][string]$Source, [string]$Out, [string]$Cli)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"
$exe = Find-Cli $Cli
if (-not (Test-Path -LiteralPath $Source)) { throw "No mission at $Source." }
if (-not $Out) {
  $dir = Join-Path $RepoRoot 'artifacts\cspace-listening'
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  $Out = Join-Path $dir ([IO.Path]::GetFileNameWithoutExtension($Source) + ' - cockpit space test.miz')
}
Invoke-Cli $exe @('audio-test', 'mission', '--source', (Resolve-Path -LiteralPath $Source).Path, '--out', $Out)
Write-Host "Test mission: $Out"
