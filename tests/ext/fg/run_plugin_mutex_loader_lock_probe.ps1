# Runner (tools\test.ps1 `Runner`) for plugin_mutex_loader_lock_probe: stages the product DLL with an ini that turns the file
# log on at trace level, builds the probe exe, an empty sl.dlss_g.dll in an ad-hoc plugin folder and the DLL whose DllMain
# holds the loader lock, then runs the probe as a CHILD of the hang launcher. A hang (the plugin mutex is held across the
# load) is a FAIL: the launcher prints every thread stack and the owner of the child's loader lock from outside the process
# and exits 3. -ProductDll runs the same probe against another build (the unfixed one is the negative control).
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out,
      [string]$ProductDll = '', [int]$Runs = 10, [int]$TimeoutSeconds = 30)
$ErrorActionPreference = 'Stop'
if (-not $ProductDll) { $ProductDll = Join-Path $Repo 'x64\Release\a\OptiScaler.dll' }
if (-not (Test-Path $ProductDll)) { Write-Error "no product build here: $ProductDll"; exit 1 }

$stage = Join-Path $Out 'stage'
$plugins = Join-Path $stage 'plugins'
New-Item -ItemType Directory -Force -Path $stage, $plugins | Out-Null
Copy-Item $ProductDll (Join-Path $stage 'OptiScaler.dll')
[IO.File]::WriteAllText((Join-Path $stage 'OptiScaler.ini'), "[Log]`r`nLogToFile=true`r`nLogLevel=0`r`n")

function Build([string]$source, [string]$target, [string[]]$extra) {
    & cl.exe /nologo /std:c++20 /EHsc /W4 "/Fo$Out\" "/Fe$target" "$PSScriptRoot\$source" @extra
    if ($LASTEXITCODE -ne 0) { throw "build failed: $source" }
}
Build 'hang_launcher.cpp' (Join-Path $Out 'hang_launcher.exe') @()
Build 'plugin_mutex_loader_lock_probe.cpp' (Join-Path $Out 'plugin_mutex_loader_lock_probe.exe') @()
Build 'sl_dlss_g_stub.cpp' (Join-Path $plugins 'sl.dlss_g.dll') @('/LD')
Build 'lock_holder.cpp' (Join-Path $Out 'lock_holder.dll') @('/LD')

$failed = 0
for ($i = 1; $i -le $Runs; $i++) {
    # local symbol folders only (never a symbol server): the stage folder is where the product DLL sits
    & (Join-Path $Out 'hang_launcher.exe') $TimeoutSeconds $stage (Join-Path $Out 'plugin_mutex_loader_lock_probe.exe') `
        (Join-Path $stage 'OptiScaler.dll') (Join-Path $plugins 'sl.dlss_g.dll') (Join-Path $Out 'lock_holder.dll')
    $code = $LASTEXITCODE
    if ($code -eq 3) { Write-Output "run ${i}/${Runs}: HANG (exit 3)" }
    else { Write-Output "run ${i}/${Runs}: exit $code" }
    if ($code -ne 0) { $failed++ }
}
if ($failed -ne 0) {
    Write-Output "FAIL: $failed of $Runs run(s) did not finish (exit 3 is the loader-lock hang, see the stacks above)"
    exit 1
}
Write-Output "PASS: $Runs of $Runs run(s) finished"
exit 0
