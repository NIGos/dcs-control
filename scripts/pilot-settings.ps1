#Requires -Version 7
# The pilot's own DCS settings (options.lua, autoexec.cfg) around a release test: nothing in the test may change them.
# -Action Record hashes them into -State when the test begins; -Action Compare checks them when it ends and throws on a
# difference. DCS itself rewrites options.lua when it runs, so a difference is only a warning when a DCS ran meanwhile:
# one was running at Record, one runs now, or dcs.log (Logs, next to Config) was written since Record.
# Compare returns what it found: unchanged and changed (the files), dcsRan.
# -ConfigFolder and -ProcessName are for the tests: a fixture folder, and a name no process has.
param(
    [Parameter(Mandatory)][ValidateSet('Record', 'Compare')][string]$Action,
    [Parameter(Mandatory)][string]$State,
    [string]$ConfigFolder = (Join-Path $env:USERPROFILE 'Saved Games/DCS/Config'),
    [string]$ProcessName = 'DCS'
)
$ErrorActionPreference = 'Stop'
function Test-DcsRunning { [bool]@(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue).Count }
if ($Action -eq 'Record') {
    $files = [ordered]@{}
    foreach ($name in 'options.lua', 'autoexec.cfg') {
        $file = Join-Path $ConfigFolder $name
        if (Test-Path -LiteralPath $file) { $files[$file] = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash }
    }
    [ordered]@{ started = [DateTime]::UtcNow.ToString('O'); dcsRunning = (Test-DcsRunning); files = $files } |
        ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $State -Encoding utf8
    return
}
$recorded = Get-Content -LiteralPath $State -Raw | ConvertFrom-Json -AsHashtable
$started = [DateTime]::Parse($recorded.started, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::RoundtripKind)
$log = Join-Path (Split-Path -Parent $ConfigFolder) 'Logs/dcs.log'
$dcsRan = [bool]$recorded.dcsRunning -or (Test-DcsRunning) -or ((Test-Path -LiteralPath $log) -and (Get-Item -LiteralPath $log).LastWriteTimeUtc -ge $started)
$unchanged = @(); $changed = @()
foreach ($file in $recorded.files.Keys) {
    if ((Test-Path -LiteralPath $file) -and (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -eq $recorded.files[$file]) { $unchanged += $file } else { $changed += $file }
}
foreach ($file in $changed) {
    if (-not $dcsRan) { throw "The pilot's own DCS settings changed during the release test: $file" }
    Write-Warning "The pilot's own DCS settings changed during the release test, and DCS ran meanwhile (DCS rewrites them): $file"
}
[pscustomobject]@{ unchanged = $unchanged; changed = $changed; dcsRan = $dcsRan }
