# Runner (tools\test.ps1 `Runner`): PreSR BoundResidual ownership/barrier lifecycle on the real production function.
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/slice_common.ps1"
$source = Read-Source $Repo $script:NrDeferredSr
$slice = Read-Fragment $source 'bool BoundResidual(' '// Background GPU job:'
[IO.File]::WriteAllText((Join-Path $Out 'presr-bounds-production.inl'), $slice)
Invoke-Cl 'Bounds lifecycle' @('/nologo', '/std:c++20', '/EHsc', '/UNDEBUG', "/I$Repo/OptiScaler", "/I$Out", "/Fo$Out/lifecycle.obj", "/Fe$Out/lifecycle.exe", "$PSScriptRoot/nr_presr_bounds_lifecycle.cpp")
& "$Out/lifecycle.exe"
if ($LASTEXITCODE -ne 0) { throw 'Bounds lifecycle regression failed.' }
