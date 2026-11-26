# Part A of the listening test (about 5 minutes, DCS closed, headphones on). Plays the stimuli made by
# make-stimuli.ps1 in the order of trials.json and records your answers in answers.csv. Nothing on the screen tells you
# which sound is which.
#   A1 (24 sounds): after each sound, how far outside your head it was, 0..4 on the number row:
#       0 inside my head, 1 at my head, 2 just outside, 3 clearly outside, 4 where it should be
#     then front or back: Up arrow = front, Down arrow = back. R plays the sound again.
#   A2 (8 trials): A, B and X play in turn. Is X the same as A or as B? 1 = X is A, 2 = X is B. R plays all three again.
# Esc stops; answers given so far are kept, and running it again continues where you stopped.
#   .\run-part-a.ps1 [-Folder <results>] [-GapMs 400]
param([string]$Folder, [int]$GapMs = 400)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"
$folder = Get-ResultsFolder $Folder
$trialsFile = Join-Path $folder 'trials.json'
if (-not (Test-Path -LiteralPath $trialsFile)) { throw "No trials.json in $folder. Run make-stimuli.ps1 first." }
$trials = Get-Content -LiteralPath $trialsFile -Raw | ConvertFrom-Json
$stimuli = Join-Path $folder 'stimuli'
$answers = Join-Path $folder 'answers.csv'
if (-not (Test-Path -LiteralPath $answers)) { Set-Content -LiteralPath $answers -Value 'part,trial,stimulus,answer1,answer2' -Encoding utf8 }
$done = @{}
foreach ($line in Get-Content -LiteralPath $answers | Select-Object -Skip 1) { $f = $line.Split(','); if ($f.Count -ge 2) { $done["$($f[0]):$($f[1])"] = $true } }

Add-Type -AssemblyName System.Windows.Extensions -ErrorAction SilentlyContinue
function Play([string]$id) {
  $file = Join-Path $stimuli "$id.wav"
  if (-not (Test-Path -LiteralPath $file)) { throw "Missing $file. Run make-stimuli.ps1 again." }
  $player = New-Object System.Media.SoundPlayer $file
  try { $player.PlaySync() } finally { $player.Dispose() }
}
function Tick { [Console]::Beep(1200, 40) }
function ReadKey([string[]]$allowed) {
  while ($true) {
    $k = [Console]::ReadKey($true)
    $name = switch ($k.Key) { 'UpArrow' { 'up' } 'DownArrow' { 'down' } 'Escape' { 'esc' } default { [string]$k.KeyChar } }
    if ($name -eq 'esc') { Write-Host "`nStopped. Your answers so far are kept."; exit 0 }
    if ($allowed -contains $name.ToLowerInvariant()) { return $name.ToLowerInvariant() }
  }
}

Write-Host 'Part A1: rate each sound. 0 inside my head .. 4 where it should be; then Up = front, Down = back. R = again, Esc = stop.'
Write-Host 'Press any key to start.'; [void][Console]::ReadKey($true)
$total = $trials.a1.Count
foreach ($t in $trials.a1) {
  if ($done["a1:$($t.trial)"]) { continue }
  Write-Host -NoNewline "`rSound $($t.trial) of $total   "
  Start-Sleep -Milliseconds $GapMs
  do { Play $t.stimulus; $rating = ReadKey @('0', '1', '2', '3', '4', 'r') } while ($rating -eq 'r')
  Tick
  do { $fb = ReadKey @('up', 'down', 'r'); if ($fb -eq 'r') { Play $t.stimulus } } while ($fb -eq 'r')
  Tick
  Add-Content -LiteralPath $answers -Value ("a1,{0},{1},{2},{3}" -f $t.trial, $t.stimulus, $rating, $(if ($fb -eq 'up') { 'front' } else { 'back' })) -Encoding utf8
}
Write-Host "`nPart A2: A, B and X play. 1 = X is A, 2 = X is B. R = all three again."
Write-Host 'Press any key to start.'; [void][Console]::ReadKey($true)
$total = $trials.a2.Count
foreach ($t in $trials.a2) {
  if ($done["a2:$($t.trial)"]) { continue }
  do {
    Write-Host -NoNewline "`rTrial $($t.trial) of ${total}: A   "; Play $t.a; Start-Sleep -Milliseconds $GapMs
    Write-Host -NoNewline "`rTrial $($t.trial) of ${total}: B   "; Play $t.b; Start-Sleep -Milliseconds $GapMs
    Write-Host -NoNewline "`rTrial $($t.trial) of ${total}: X   "; Play $t.x
    $answer = ReadKey @('1', '2', 'r')
  } while ($answer -eq 'r')
  Tick
  Add-Content -LiteralPath $answers -Value ("a2,{0},,{1}," -f $t.trial, $(if ($answer -eq '1') { 'A' } else { 'B' })) -Encoding utf8
}
Write-Host "`nPart A done. Answers: $answers"
