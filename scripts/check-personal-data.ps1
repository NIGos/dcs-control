#Requires -Version 7
# Refuses personal data in what is about to be committed. The repository holds only generic patterns (e-mail
# addresses, Steam IDs, a Steam library drive path); the values that identify one person (their user name, machine
# name, device serials, local folder names) live OUTSIDE the repository, in a local file, so the checker never
# publishes what it protects. By default it scans the staged content of the staged files (what the commit will hold,
# not the working copy); -All scans every tracked and untracked, non-ignored file as it is on disk; -Paths scans the
# given files on disk. Exit 1 on a hit, printing file:line.
#
# The local file: DCSC_PERSONAL_PATTERNS, else %USERPROFILE%\.dcs-control\personal-data.txt. One entry per line
# (# comments):
#     pattern <name> <regex>        a value to refuse (PCRE, case-insensitive, as git grep -P -i reads it)
#     allow <name> <path glob>      a false positive of that pattern ("*" = every pattern)
# Without the file the generic patterns still run, with a warning: create it on every PC you commit from.
# scripts/personal-data-allow.txt lists allowed hits of the generic patterns, same "<name> <path glob>" form.
# The pre-commit hook (.githooks/pre-commit) runs it; enable with: git config core.hooksPath .githooks
param([switch]$All, [string[]]$Paths)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$patterns = [ordered]@{
    # Any Windows user folder written as a path is fine (docs use %USERPROFILE%); an e-mail address is not.
    'email'          = '\b[\w.+-]+@(gmail|outlook|hotmail|live|yahoo|icloud|proton)\.\w+\b'
    'steam-library'  = '\b[A-Z]:[\\/]+SteamLibrary\b'
    # Any Steam ID (SteamID64): Steam's API prints the pilot's into DCS's output, which the rig runs capture.
    'steam-id'       = '(?<!\d)7656119\d{10}(?!\d)'
}
$allow = @()
$allowFile = Join-Path $PSScriptRoot 'personal-data-allow.txt'
if (Test-Path -LiteralPath $allowFile) {
    $allow = @(Get-Content -LiteralPath $allowFile | ForEach-Object { $_.Trim() } | Where-Object { $_ -and -not $_.StartsWith('#') } | ForEach-Object {
        $name, $glob = $_ -split '\s+', 2
        [pscustomobject]@{ Name = $name; Glob = $glob.Replace('\', '/') }
    })
}
$localFile = if ($env:DCSC_PERSONAL_PATTERNS) { $env:DCSC_PERSONAL_PATTERNS } else { Join-Path $env:USERPROFILE '.dcs-control\personal-data.txt' }
if (Test-Path -LiteralPath $localFile) {
    foreach ($line in Get-Content -LiteralPath $localFile) {
        $line = $line.Trim(); if (-not $line -or $line.StartsWith('#')) { continue }
        $kind, $name, $value = $line -split '\s+', 3
        if (-not $value) { throw "personal-data patterns: bad line in the local file: $line" }
        if ($kind -eq 'pattern') { $patterns[$name] = $value }
        elseif ($kind -eq 'allow') { $allow += [pscustomobject]@{ Name = $name; Glob = $value.Replace('\', '/') } }
        else { throw "personal-data patterns: unknown entry '$kind' in the local file (pattern or allow)" }
    }
}
else { Write-Host "WARNING: no local personal-data patterns ($localFile): only the generic ones are checked." }
# The checker and its allow list name only generic patterns; the local file is not in the repository.
$allow += [pscustomobject]@{ Name = '*'; Glob = 'scripts/check-personal-data.ps1' }
function Test-Allowed([string]$name, [string]$path) {
    foreach ($entry in $allow) { if (($entry.Name -eq '*' -or $entry.Name -eq $name) -and $path -like $entry.Glob) { return $true } }
    return $false
}

$mode = @('--cached')
if ($Paths) {
    # Files on disk, possibly outside the repository (a peer's tree before vendoring): scanned here, not by git grep.
    $hits = [Collections.Generic.List[string]]::new()
    foreach ($path in $Paths) {
        $full = (Resolve-Path -LiteralPath $path).Path
        $shown = [IO.Path]::GetRelativePath($root, $full).Replace('\', '/')
        $stream = [IO.File]::OpenRead($full)
        try { $head = [byte[]]::new([Math]::Min(8000, $stream.Length)); [void]$stream.Read($head, 0, $head.Length) } finally { $stream.Dispose() }
        if ($head -contains 0) { continue }  # binary
        foreach ($name in $patterns.Keys) {
            if (Test-Allowed $name $shown) { continue }
            foreach ($match in (Select-String -LiteralPath $full -Pattern $patterns[$name])) { $hits.Add("${shown}:$($match.LineNumber): [$name]") }
        }
    }
    foreach ($hit in $hits) { Write-Host $hit }
    if ($hits.Count) { Write-Host "Personal data found ($($hits.Count) line(s))."; exit 1 }
    Write-Host "No personal data found ($(@($Paths).Count) file(s))."
    exit 0
}
elseif ($All) { $files = @(); $mode = @('--untracked') }
else {
    $files = @(git -C $root diff --cached --name-only --diff-filter=ACMR)
    if ($LASTEXITCODE -ne 0) { throw 'git diff --cached failed.' }
    if (-not $files.Count) { Write-Host 'No staged files to check for personal data.'; exit 0 }
}

$hits = [Collections.Generic.List[string]]::new()
# The pathspec list is split so a large commit never exceeds the command line; -All passes none (every file).
$batches = [Collections.Generic.List[object]]::new()
if ($files.Count) { for ($i = 0; $i -lt $files.Count; $i += 200) { $batches.Add(@($files[$i..([Math]::Min($i + 199, $files.Count - 1))])) } }
else { $batches.Add(@()) }
foreach ($name in $patterns.Keys) {
    foreach ($batch in $batches) {
        # -I skips binary files; -P for \b.
        $arguments = @('-C', $root, '-c', 'core.quotepath=off', 'grep', '-n', '-I', '-i', '-P') + $mode + @('-e', $patterns[$name], '--')
        $arguments += @($batch | ForEach-Object { ":(literal)$_" })
        $output = @(& git @arguments)
        if ($LASTEXITCODE -gt 1) { throw "git grep failed ($LASTEXITCODE) for $name." }
        foreach ($line in $output) {
            if ($line -notmatch '^(?<path>[^:]+):(?<line>\d+):') { continue }
            if (Test-Allowed $name $Matches.path) { continue }
            $hits.Add("$($Matches.path):$($Matches.line): [$name]")
        }
    }
}
if ($hits.Count) {
    foreach ($hit in $hits) { Write-Host $hit }
    Write-Host "Personal data found ($($hits.Count) line(s)). Remove it, or allow a false positive in scripts/personal-data-allow.txt."
    exit 1
}
Write-Host "No personal data found ($(if ($All) { 'all files' } else { "$($files.Count) staged file(s)" }))."
exit 0
