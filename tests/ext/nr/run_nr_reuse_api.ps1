# Runner (tools\test.ps1 `Runner`): NR bottleneck-reuse driver wrapper (LaunchEx, BeginEvaluate) on the real production
# functions of DlssNrNative.cpp, with two negative controls (the previous global lock scope; a downstream pass clearing
# every feature's cache).
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/slice_common.ps1"
$nvapiInclude = Join-Path $Repo 'external/nvapi'
$source = Read-Source $Repo $script:NrNative
$extract = (Read-Function $source 'template<class Kernel>') + "`n" + (Read-Function $source 'NvAPI_Status __cdecl LaunchEx(') + "`n" + (Read-Function $source 'void BeginEvaluate(')
$include = Join-Path $Out 'nr_reuse_api_production.inl'
function Build-Test([string]$name, [string[]]$extra) {
    Invoke-Cl 'Reuse API test' (@('/nologo', '/std:c++20', '/EHsc') + $extra + @("/I$nvapiInclude", "/I$Repo/OptiScaler", "/I$Out", "/Fo$Out/$name.obj", "/Fe$Out/$name.exe", "$PSScriptRoot/nr_reuse_api.cpp"))
}
[IO.File]::WriteAllText($include, $extract)
Build-Test 'test' @('/W4')
& "$Out/test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Reuse API regression failed' }
# Negative control: the previous wrapper held this mutex around driver calls.
$bad = $extract.Replace('auto&s=S();const auto driver=s.launchEx.load();', 'auto&s=S();std::lock_guard<std::recursive_mutex> oldLock(s.mutex);const auto driver=s.launchEx.load();')
if ($bad -ceq $extract) { throw 'Lock-scope negative control no longer matches production.' }
[IO.File]::WriteAllText($include, $bad)
Build-Test 'legacy' @()
& "$Out/legacy.exe"
if ($LASTEXITCODE -ne 1) { throw 'Old global lock was not rejected' }
Write-Output 'PASS: negative control detects the previous startup lock scope'
# Negative control: a full downstream pass used to clear every feature's cache.
$bad = $extract.Replace('s.vit.Invalidate(feature);', 's.vit.Clear();')
if ($bad -ceq $extract) { throw 'Cache-clear negative control no longer matches production.' }
[IO.File]::WriteAllText($include, $bad)
Build-Test 'global-clear' @()
& "$Out/global-clear.exe"
if ($LASTEXITCODE -ne 1) { throw 'Global cache clearing was not rejected' }
Write-Output 'PASS: negative control detects downstream passes cancelling first-pass reuse'
# Negative control: a capture that only watches launches the override is not allowed to drop would miss exactly the launches
# an override run discards (recording has to precede the drop decision).
$bad = $extract.Replace('if(watching)capture.Observe(k[i]);', 'if(watching&&!s.allowUnverified)capture.Observe(k[i]);')
if ($bad -ceq $extract) { throw 'Capture-order negative control no longer matches production.' }
[IO.File]::WriteAllText($include, $bad)
Build-Test 'late-capture' @()
& "$Out/late-capture.exe"
if ($LASTEXITCODE -ne 1) { throw 'A capture that skips override-dropped launches was not rejected' }
Write-Output 'PASS: negative control detects a capture that records after the drop decision'
