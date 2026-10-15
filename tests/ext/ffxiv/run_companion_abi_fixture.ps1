param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot 'companion_abi_fixture.cpp'
$output = Join-Path $Out 'CompanionBridgeFixture.dll'
& cl.exe /nologo /std:c++20 /EHsc /W4 /MD /LD $source "/Fo$Out\companion_abi_fixture.obj" "/Fe$output" "/link" "/IMPLIB:$Out\CompanionBridgeFixture.lib"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if (-not (Test-Path $output)) { throw 'Fixture DLL was not produced' }