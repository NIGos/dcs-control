# Part B of the listening test, in DCS (about 5 minutes). Start DCS with Launch DCS (3D headphone audio on), open the
# test mission made by make-mission.ps1 and sit in the cockpit. Then run this: it follows DCS's log and sets the sound
# for each trial you start from the F10 radio menu (Other > Cockpit space test). It never shows which sound plays.
# It ends when you pick "End the test" in the menu, or with Ctrl+C here; either way your own Cockpit space setting
# plays again.
#   .\part-b.ps1 [-Folder <results>] [-Log <Saved Games>\DCS\Logs\dcs.log]
param([string]$Folder, [string]$Log, [string]$Cli)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"
$exe = Find-Cli $Cli
$folder = Get-ResultsFolder $Folder
$arguments = @('audio-test', 'watch', '--folder', $folder)
if ($Log) { $arguments += @('--log', $Log) }
Invoke-Cli $exe $arguments
