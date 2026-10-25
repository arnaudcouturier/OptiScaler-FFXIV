param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
# Builds the stand-in ReShade DLL and the late-registration test exe (with the production ReShadeRegister.cpp, ReShadeDepthLate.cpp,
# ReShadeDepthEvents.cpp, ReShadeDepthCore.cpp and ReShadeD3D11.cpp), then runs the exe once per mode: a process registers only once
# because the ReShade helpers cache their module (and every event's entry point) in statics.
$here = $PSScriptRoot
$dll = Join-Path $Out 'FakeReShade64.dll'
& cl.exe /nologo /std:c++20 /EHsc /W4 /MD /LD (Join-Path $here 'fake_reshade.cpp') "/Fo$Out\fake_reshade.obj" "/Fe$dll" "/link" "/IMPLIB:$Out\FakeReShade64.lib"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$exe = Join-Path $Out 'reshade_depth_late_fake.exe'
$ext = Join-Path $Repo 'OptiScaler\ext\reshade'
& cl.exe /nologo /std:c++latest /EHsc /W4 /MD /DNOMINMAX /DUNDEBUG /DDEPTH_TEST_LOG "/I$here\stubs" "/I$Repo\OptiScaler" `
    (Join-Path $here 'reshade_depth_late_fake.cpp') (Join-Path $ext 'ReShadeRegister.cpp') (Join-Path $ext 'ReShadeDepthLate.cpp') `
    (Join-Path $ext 'ReShadeDepthEvents.cpp') (Join-Path $ext 'ReShadeDepthCore.cpp') (Join-Path $ext 'ReShadeD3D11.cpp') (Join-Path $ext 'ReShadeRegistrationState.cpp') `
    "/Fo$Out\" "/Fe$exe" d3d11.lib dxgi.lib
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
foreach ($mode in 'late', 'load', 'bound', 'refuse', 'crash', 'failure', 'race') {
    & $exe $mode $dll
    if ($LASTEXITCODE -ne 0) { Write-Host "mode $mode failed with $LASTEXITCODE"; exit 1 }
}
exit 0
