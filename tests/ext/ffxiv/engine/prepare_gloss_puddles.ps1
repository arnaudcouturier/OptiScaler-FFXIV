param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$shader = Join-Path $Repo 'OptiScaler\ext\ffxiv\engine\shaders\GlossPuddles.hlsl'
$camera = Join-Path $Repo 'OptiScaler\ext\ffxiv\relight\shaders'
$engineShaders = Join-Path $Repo 'OptiScaler\ext\ffxiv\engine\shaders'
$fxc = Join-Path $Repo 'OptiScaler\shaders\shader_tools\fxc.exe'
# The same recipe as ext\ext.props: fxc -nologo -T <profile> -E <entry> -O3, one blob per entry point, with the include folders the props pass.
foreach ($target in @(@('vs_5_0', 'VS', 'Vs'), @('ps_5_0', 'PS', 'Ps'))) {
    $profile = $target[0]; $entry = $target[1]; $name = $target[2]
    $bin = Join-Path $Out "GlossPuddles_$name.bin"
    $header = Join-Path $Out "GlossPuddles_$name.h"
    & $fxc -nologo -T $profile -E $entry -O3 -I $camera -I $engineShaders $shader -Fo $bin
    if ($LASTEXITCODE -ne 0) { throw "GlossPuddles.hlsl $entry compilation failed." }
    & pwsh -NoProfile -File (Join-Path $Repo 'tools\bin2h.ps1') -In $bin -Out $header -Name "GlossPuddles_${name}_cso"
    if ($LASTEXITCODE -ne 0) { throw "GlossPuddles.hlsl $entry header generation failed." }
}
