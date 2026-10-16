param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
# Builds the stand-in ReShade DLL and the registration test exe (with the production ReShadeRegister.cpp), then runs the
# exe once per mode: a process can register only once because the ReShade helpers cache their module in statics.
$here = $PSScriptRoot
$dll = Join-Path $Out 'FakeReShade64.dll'
& cl.exe /nologo /std:c++20 /EHsc /W4 /MD /LD (Join-Path $here 'fake_reshade.cpp') "/Fo$Out\fake_reshade.obj" "/Fe$dll" "/link" "/IMPLIB:$Out\FakeReShade64.lib"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$exe = Join-Path $Out 'reshade_registration_fake.exe'
& cl.exe /nologo /std:c++latest /EHsc /W4 /MD /DNOMINMAX "/I$here\stubs" "/I$Repo\OptiScaler" (Join-Path $here 'reshade_registration_fake.cpp') (Join-Path $Repo 'OptiScaler\ext\reshade\ReShadeRegister.cpp') "/Fo$Out\" "/Fe$exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
foreach ($mode in 'ok', 'refuse') {
    & $exe $mode $dll
    if ($LASTEXITCODE -ne 0) { Write-Host "mode $mode failed with $LASTEXITCODE"; exit 1 }
}
exit 0
