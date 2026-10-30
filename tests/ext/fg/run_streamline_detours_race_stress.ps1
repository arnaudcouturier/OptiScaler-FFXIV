# Runner (tools\test.ps1 `Runner`) for streamline_detours_race_stress: stages the product DLL, ini and Streamline plugins
# (prepare script), builds the stress exe and an out-of-process watchdog launcher, then runs the stress exe as a CHILD of the
# launcher. A hang (no exit within the timeout) is a FAIL: the launcher prints every thread's stack and the owner of the
# child's loader lock from outside the process, and exits with 3. -ProductDll runs the same stress against another build (the
# unfixed one is the negative control).
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out,
      [string]$ProductDll = '', [int]$TimeoutSeconds = 60, [int]$Iterations = 300, [int]$Threads = 3)
$ErrorActionPreference = 'Stop'
& pwsh -NoProfile -File "$PSScriptRoot/prepare_streamline_detours_race_stress.ps1" -Repo $Repo -Out $Out -ProductDll $ProductDll
if ($LASTEXITCODE -ne 0) { throw 'prepare failed' }
function Build([string]$source, [string]$exe, [string[]]$extra) {
    & cl.exe /nologo /std:c++20 /EHsc /W4 "/Fo$Out\" "/Fe$Out\$exe" "$PSScriptRoot\$source" @extra
    if ($LASTEXITCODE -ne 0) { throw "build failed: $source" }
}
Build 'hang_launcher.cpp' 'hang_launcher.exe' @()
Build 'streamline_detours_race_stress.cpp' 'streamline_detours_race_stress.exe' @()
$stage = Join-Path $Out 'stage'
# local symbol folders only (never a symbol server)
$symbols = "$stage;$stage\plugins"
& "$Out\hang_launcher.exe" $TimeoutSeconds $symbols "$Out\streamline_detours_race_stress.exe" "$stage\OptiScaler.dll" "$stage\plugins" $Iterations $Threads
$code = $LASTEXITCODE
if ($code -eq 3) { Write-Output 'FAIL: the stress test hung (see the stacks above)' }
exit $code
