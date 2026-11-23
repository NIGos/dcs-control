# How many compile jobs a native build runs at once: the logical processors less two (at least 1), or DCSC_BUILD_JOBS=<n>.
#   . (Join-Path $PSScriptRoot 'build-jobs.ps1')
#   cmake --build $build --parallel (Get-BuildJobs)
# The job count changes how fast a build is, never what it produces. Builds that share the PC with someone are started
# at BelowNormal priority (docs/DEVELOPING.md): every compiler and test inherits it and yields to the user's programs,
# however many jobs there are.
function Get-BuildJobs {
    $override = $env:DCSC_BUILD_JOBS
    if ($override) {
        $jobs = 0
        if (-not [int]::TryParse($override.Trim(), [ref]$jobs) -or $jobs -lt 1) { throw "DCSC_BUILD_JOBS='$override' is not a whole number of 1 or more." }
        return $jobs
    }
    return [Math]::Max(1, [Environment]::ProcessorCount - 2)
}
