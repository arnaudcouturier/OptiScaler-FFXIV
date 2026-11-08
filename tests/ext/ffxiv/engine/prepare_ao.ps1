param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$shader = Join-Path $Repo 'OptiScaler\ext\ffxiv\engine\shaders\Ao.hlsl'
$camera = Join-Path $Repo 'OptiScaler\ext\ffxiv\relight\shaders'
$fxc = Join-Path $Repo 'OptiScaler\shaders\shader_tools\fxc.exe'
# The same recipe as ext\ext.props: fxc -nologo -T <profile> -E <entry> -O3, one blob per entry point, with the include folders the props pass.
foreach ($target in @(@('vs_5_0', 'VS', 'Vs'), @('ps_5_0', 'PS_Ao', 'Ao'), @('ps_5_0', 'PS_Final', 'Final'))) {
    $profile = $target[0]; $entry = $target[1]; $name = $target[2]
    $bin = Join-Path $Out "Ao_$name.bin"
    $header = Join-Path $Out "Ao_$name.h"
    & $fxc -nologo -T $profile -E $entry -O3 -I $camera $shader -Fo $bin
    if ($LASTEXITCODE -ne 0) { throw "Ao.hlsl $entry compilation failed." }
    & pwsh -NoProfile -File (Join-Path $Repo 'tools\bin2h.ps1') -In $bin -Out $header -Name "Ao_${name}_cso"
    if ($LASTEXITCODE -ne 0) { throw "Ao.hlsl $entry header generation failed." }
}
