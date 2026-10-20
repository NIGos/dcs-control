#Requires -Version 7
# Refuses personal data in what is about to be committed: the user's paths, e-mail, machine and device names, the
# peer's local repository name and the Steam library drive. By default it scans the staged content of the staged files
# (what the commit will hold, not the working copy); -All scans every tracked and untracked, non-ignored file as it is
# on disk; -Paths scans the given files on disk. Exit 1 on a hit, printing file:line.
# scripts/personal-data-allow.txt lists allowed hits: "<pattern name> <path glob>" per line (# comments).
# The pre-commit hook (.githooks/pre-commit) runs it; enable with: git config core.hooksPath .githooks
param([switch]$All, [string[]]$Paths)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$patterns = [ordered]@{
    'user-path'      = 'Users[\\/]quali\b'
    'user-name'      = '\banigr9001\b'
    'gmail'          = '@gmail\.com\b'
    'peer-repo'      = '\bDCS-quadview-analysis\b'
    'device-serial'  = '\bP407B0D2005\b'
    'id-81939'       = '\b81939\b'
    'machine-name'   = '\bDESKTOP-LCT0TCH\b'
    'steam-library'  = '\b[A-Z]:[\\/]SteamLibrary\b'
}
$allow = @()
$allowFile = Join-Path $PSScriptRoot 'personal-data-allow.txt'
if (Test-Path -LiteralPath $allowFile) {
    $allow = @(Get-Content -LiteralPath $allowFile | ForEach-Object { $_.Trim() } | Where-Object { $_ -and -not $_.StartsWith('#') } | ForEach-Object {
        $name, $glob = $_ -split '\s+', 2
        [pscustomobject]@{ Name = $name; Glob = $glob.Replace('\', '/') }
    })
}
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
