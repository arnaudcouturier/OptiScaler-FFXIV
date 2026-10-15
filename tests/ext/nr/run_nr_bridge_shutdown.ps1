# Runner (tools\test.ps1 `Runner`): NR bridge shutdown / CRT teardown ownership on the real production generation
# definitions, Shutdown, SuspendForBridgeShutdown and ResumeAfterBridgeInit, with a negative control that restores the
# original exit-time-destroyed containers. No NVIDIA runtime or game needed (NGX is simulated).
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/slice_common.ps1"
$ngxInclude = Join-Path $Repo 'external/nvngx_dlss_sdk'
$deferred = Read-Source $Repo $script:NrDeferredSr
$nr = Read-Source $Repo $script:NrDx12
# Use the real definitions through Collect(), omitting rendering functions that require a model. The test exercises
# real ownership/destructors, not a copy.
$end = $deferred.IndexOf('unsigned UInt(')
if ($end -lt 0) { throw 'Production generation definitions changed; update test extraction.' }
$production = $deferred.Substring(0, $end) + (Read-Function $deferred 'void Shutdown()') + "`n}`n" +
    (Read-Function $nr 'void SuspendForBridgeShutdown()') + "`n" + (Read-Function $nr 'void ResumeAfterBridgeInit()')
$include = Join-Path $Out 'nr_shutdown_production.inl'
[IO.File]::WriteAllText($include, $production)
function Build-Test([string]$name) {
    Invoke-Cl 'NR shutdown test' @('/nologo', '/std:c++20', '/EHsc', '/UNDEBUG', '/DNOMINMAX', "/I$ngxInclude", "/I$Repo/OptiScaler", "/I$Out", "/Fo$Out/$name.obj", "/Fe$Out/$name.exe", "$PSScriptRoot/nr_bridge_shutdown.cpp")
}
Build-Test 'current'
& "$Out/current.exe"
if ($LASTEXITCODE -ne 0) { throw 'NR shutdown lifecycle test failed.' }
& "$Out/current.exe" 'late'
if ($LASTEXITCODE -ne 0) { throw 'NR ownership called NGX during CRT teardown.' }
# Negative control: restore precisely the original two global owning containers.
# The same late-runtime test must detect the original exit-time driver callback.
$legacy = $production.Replace('std::unique_ptr<Generation>& current = *new std::unique_ptr<Generation>;', 'std::unique_ptr<Generation> current;').Replace('std::vector<std::unique_ptr<Generation>>& retired = *new std::vector<std::unique_ptr<Generation>>;', 'std::vector<std::unique_ptr<Generation>> retired;')
if ($legacy -ceq $production) { throw 'Negative control no longer matches production ownership.' }
[IO.File]::WriteAllText($include, $legacy)
Build-Test 'legacy'
& "$Out/legacy.exe" 'late'
if ($LASTEXITCODE -ne 99) { throw 'Negative control failed to reproduce the late NGX release.' }
Write-Output 'CRT teardown regression passed; original ownership reproduced the late NGX release (99).'
