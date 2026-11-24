#Requires -Version 7
# ctest binaural_avx2_object: kernels_avx2.cpp is the only file built with /arch:AVX2. Any external function it defines
# outside binaural::kernels (an inline std::floor / std::fma / std::min instantiated there) is a COMDAT the linker may
# keep instead of the SSE2 copy from another object, which then runs AVX2 code on a CPU without it (DCS's audio thread
# would crash). This lists the object's external, defined symbols and fails on any such function.
# Usage: check_avx2_object.ps1 <object files...> (the target's objects; only *kernels_avx2*.obj is checked)
$ErrorActionPreference = 'Stop'
$objects = @($args | ForEach-Object { $_ -split ';' } | Where-Object { $_ -match 'kernels_avx2[^\\/]*\.obj$' })
if ($objects.Count -ne 1) { Write-Output "avx2 object: expected one kernels_avx2 object, got $($objects.Count): 1 failure(s)"; exit 1 }
$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if (-not $dumpbin) { Write-Output 'avx2 object: dumpbin.exe not found (run through scripts/msvc.cmd): 1 failure(s)'; exit 1 }
$lines = & $dumpbin.Source /nologo /symbols $objects[0]
if ($LASTEXITCODE -ne 0) { Write-Output "avx2 object: dumpbin failed: 1 failure(s)"; exit 1 }
$defined = @($lines | Where-Object { $_ -match '\bSECT[0-9A-F]+\b' -and $_ -match '\bExternal\b' -and $_ -match '\(\)' })
$bad = @()
foreach ($l in $defined) {
    # "... notype ()    External     | ?name@... (demangled)": function symbols only (data constants such as __real@ are
    # identical bit patterns in every object, whichever copy is kept).
    $demangled = if ($l -match '\|\s*\S+\s+\((.*)\)\s*$') { $Matches[1] } else { ($l -split '\|')[-1].Trim() }
    if ($demangled -notmatch 'binaural::kernels::') { $bad += $demangled }
}
Write-Output "avx2 object: $($defined.Count) external function(s) defined"
foreach ($b in $bad) { Write-Output "  outside binaural::kernels: $b" }
Write-Output "avx2 object: $($bad.Count) failure(s)"
if ($bad.Count) { exit 1 }
