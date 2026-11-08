param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$shader = Join-Path $Repo 'OptiScaler\ext\ffxiv\engine\shaders\PuddleReflection.hlsl'
$engineShaders = Join-Path $Repo 'OptiScaler\ext\ffxiv\engine\shaders'
$camera = Join-Path $Repo 'OptiScaler\ext\ffxiv\relight\shaders'
$fxc = Join-Path $Repo 'OptiScaler\shaders\shader_tools\fxc.exe'
# The same recipe as ext\ext.props: fxc -nologo -T <profile> -E <entry> -O3, one blob per entry point, with the include folders the props pass
# (the camera registers and the shared march the water reflections use).
foreach ($target in @(@('vs_5_0', 'VS', 'Vs'), @('ps_5_0', 'PS_Puddle', 'Ps'))) {
    $profile = $target[0]; $entry = $target[1]; $name = $target[2]
    $bin = Join-Path $Out "PuddleReflection_$name.bin"
    $header = Join-Path $Out "PuddleReflection_$name.h"
    & $fxc -nologo -T $profile -E $entry -O3 -I $camera -I $engineShaders $shader -Fo $bin
    if ($LASTEXITCODE -ne 0) { throw "PuddleReflection.hlsl $entry compilation failed." }
    & pwsh -NoProfile -File (Join-Path $Repo 'tools\bin2h.ps1') -In $bin -Out $header -Name "PuddleReflection_${name}_cso"
    if ($LASTEXITCODE -ne 0) { throw "PuddleReflection.hlsl $entry header generation failed." }
}
