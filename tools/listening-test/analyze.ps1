# The results of a listening test session: per level the median rating and the share rated 3 or 4, front/back
# reversals, Wilcoxon signed rank Off vs Subtle and Subtle vs Normal, A/B/X with its binomial p, and the verdict on the
# default. Writes summary.md and summary.json into the results folder and prints the report.
#   .\analyze.ps1 [-Folder <results>]
param([string]$Folder, [string]$Cli)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"
$exe = Find-Cli $Cli
$folder = Get-ResultsFolder $Folder
Invoke-Cli $exe @('audio-test', 'analyze', '--folder', $folder)
