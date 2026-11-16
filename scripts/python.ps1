# The one way these scripts start Python 3. Dot-source it, then: Invoke-Python <script.py> <arguments...>
#   . (Join-Path $PSScriptRoot 'python.ps1')
#   Invoke-Python (Join-Path $PSScriptRoot 'patch-cheeky.py') --force
# The Python launcher (py -3, installed with python.org's Python) comes first: on a PC without Python on PATH, plain
# "python" is the Microsoft Store stub, which only opens the Store. A real python on PATH is used when py is missing.
# By hand the same applies: py -3 scripts\package-sources.py
# Not yet used by scripts/build-upstream.ps1 (it still runs plain "python" for patch-cheeky.py): that file is an input of
# Cheeky's build stamp (scripts/build-stamp.ps1), so editing it makes build-release.ps1 refuse the Cheeky binaries until
# they are rebuilt. Switch it to Invoke-Python together with the next change that rebuilds Cheeky anyway.
function Get-PythonCommand {
    if (Get-Command py -CommandType Application -ErrorAction SilentlyContinue) { return @('py', '-3') }
    $python = Get-Command python -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($python -and $python.Source -notmatch '\\WindowsApps\\') { return @($python.Source) }
    throw 'Python 3 was not found: install it from python.org (its launcher, py, is what these scripts use).'
}
function Invoke-Python {
    $command = @(Get-PythonCommand)
    $executable = $command[0]
    $before = @($command | Select-Object -Skip 1)
    & $executable @before @args
}
