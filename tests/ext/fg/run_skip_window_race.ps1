# Builds and runs skip_window_race with the debug CRT (/MDd, _ITERATOR_DEBUG_LEVEL=2), which tools\test.ps1 cannot select
# (it always compiles /MD). The checked containers are what catches a map that two threads touch at the same time; the test
# also carries its own deadline, so a hang ends the run with a failure instead of the suite.
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'

& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'prepare_skip_window_state.ps1') -Repo $Repo -Out $Out
if ($LASTEXITCODE -ne 0) { Write-Error 'the State.h slice could not be prepared'; exit 1 }

$exe = Join-Path $Out 'skip_window_race.exe'
$clArgs = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/MDd', "/Fo$Out\obj_", "/Fe$exe", "/I$Out",
            (Join-Path $PSScriptRoot 'skip_window_race.cpp'))
& cl.exe @clArgs
if ($LASTEXITCODE -ne 0) { Write-Error 'skip_window_race did not build'; exit 1 }

& $exe
exit $LASTEXITCODE
