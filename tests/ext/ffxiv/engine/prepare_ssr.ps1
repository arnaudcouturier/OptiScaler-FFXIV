param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$shader = Join-Path $Repo 'OptiScaler\ext\ffxiv\engine\shaders\Ssr.hlsl'
$camera = Join-Path $Repo 'OptiScaler\ext\ffxiv\relight\shaders'
$fxc = Join-Path $Repo 'OptiScaler\shaders\shader_tools\fxc.exe'
# The same recipe as ext\ext.props: fxc -nologo -T <profile> -E <entry> -O3, one blob per entry point, with the include folders the props pass.
foreach ($target in @(@('vs_5_0', 'VS', 'Vs'), @('ps_5_0', 'PS_Ssr', 'Ssr'))) {
    $profile = $target[0]; $entry = $target[1]; $name = $target[2]
    $bin = Join-Path $Out "Ssr_$name.bin"
    $header = Join-Path $Out "Ssr_$name.h"
    & $fxc -nologo -T $profile -E $entry -O3 -I $camera $shader -Fo $bin
    if ($LASTEXITCODE -ne 0) { throw "Ssr.hlsl $entry compilation failed." }
    & pwsh -NoProfile -File (Join-Path $Repo 'tools\bin2h.ps1') -In $bin -Out $header -Name "Ssr_${name}_cso"
    if ($LASTEXITCODE -ne 0) { throw "Ssr.hlsl $entry header generation failed." }
}
# The bytecode the recognition is parsed from: one blob per shape the water shader comes in, compiled from a fixture that declares the same names.
# The second fixture is the one shape a rename produces: a water anchor called `g_SamplerWaveMap1`, which the parse must not take for the anchor.
$fixtures = @(
    @((Join-Path $Repo 'tests\ext\ffxiv\engine\ssr_stamp_fixture.hlsl'), 'PS_Capture', 'Capture'),
    @((Join-Path $Repo 'tests\ext\ffxiv\engine\ssr_stamp_fixture.hlsl'), 'PS_Water', 'Water'),
    @((Join-Path $Repo 'tests\ext\ffxiv\engine\ssr_stamp_fixture.hlsl'), 'PS_NoWave', 'NoWave'),
    @((Join-Path $Repo 'tests\ext\ffxiv\engine\ssr_stamp_fixture.hlsl'), 'PS_NoMap', 'NoMap'),
    @((Join-Path $Repo 'tests\ext\ffxiv\engine\ssr_wave_name_fixture.hlsl'), 'PS_WaveName1', 'WaveName1'))
foreach ($fixture in $fixtures) {
    $source = $fixture[0]; $entry = $fixture[1]; $name = $fixture[2]
    $bin = Join-Path $Out "Ssr_Stamp$name.bin"
    $header = Join-Path $Out "Ssr_Stamp$name.h"
    & $fxc -nologo -T ps_5_0 -E $entry -O3 $source -Fo $bin
    if ($LASTEXITCODE -ne 0) { throw "$source $entry compilation failed." }
    & pwsh -NoProfile -File (Join-Path $Repo 'tools\bin2h.ps1') -In $bin -Out $header -Name "Ssr_Stamp${name}_cso"
    if ($LASTEXITCODE -ne 0) { throw "$source $entry header generation failed." }
}
