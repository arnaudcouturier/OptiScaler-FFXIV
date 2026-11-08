param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$shader = Join-Path $Repo 'OptiScaler\ext\ffxiv\engine\shaders\Godrays.hlsl'
$fxc = Join-Path $Repo 'OptiScaler\shaders\shader_tools\fxc.exe'
# The same recipe as ext\ext.props: fxc -nologo -T <profile> -E <entry> -O3, one blob per entry point.
foreach ($target in @(@('vs_5_0', 'VS', 'Vs'), @('ps_5_0', 'PS_March', 'March'), @('ps_5_0', 'PS_SunColour', 'SunColour'), @('ps_5_0', 'PS_Composite', 'Composite'), @('cs_5_0', 'CS_Stats', 'Stats'))) {
    $profile = $target[0]; $entry = $target[1]; $name = $target[2]
    $bin = Join-Path $Out "Godrays_$name.bin"
    $header = Join-Path $Out "Godrays_$name.h"
    & $fxc -nologo -T $profile -E $entry -O3 $shader -Fo $bin
    if ($LASTEXITCODE -ne 0) { throw "Godrays.hlsl $entry compilation failed." }
    & pwsh -NoProfile -File (Join-Path $Repo 'tools\bin2h.ps1') -In $bin -Out $header -Name "Godrays_${name}_cso"
    if ($LASTEXITCODE -ne 0) { throw "Godrays.hlsl $entry header generation failed." }
}
