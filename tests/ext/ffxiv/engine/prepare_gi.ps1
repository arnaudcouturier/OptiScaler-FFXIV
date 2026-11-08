param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$shader = Join-Path $Repo 'OptiScaler\ext\ffxiv\engine\shaders\Gi.hlsl'
$fxc = Join-Path $Repo 'OptiScaler\shaders\shader_tools\fxc.exe'
# The same recipe as ext\ext.props: fxc -nologo -T <profile> -E <entry> -O3, one blob per entry point. The pass's own march file rides the
# source's own directory include (Gi.hlsl's quoted include of GiMarch.hlsl), so no -I is added here.
foreach ($target in @(@('vs_5_0', 'VS', 'Vs'), @('ps_5_0', 'PS_Gi', 'Gi'), @('ps_5_0', 'PS_Guide', 'Guide'), @('ps_5_0', 'PS_Clamp', 'Clamp'), @('ps_5_0', 'PS_Atrous0', 'Atrous0'), @('ps_5_0', 'PS_Atrous1', 'Atrous1'), @('ps_5_0', 'PS_Atrous2', 'Atrous2'), @('ps_5_0', 'PS_Atrous3', 'Atrous3'), @('ps_5_0', 'PS_Apply', 'Apply'))) {
    $profile = $target[0]; $entry = $target[1]; $name = $target[2]
    $bin = Join-Path $Out "Gi_$name.bin"
    $header = Join-Path $Out "Gi_$name.h"
    & $fxc -nologo -T $profile -E $entry -O3 $shader -Fo $bin
    if ($LASTEXITCODE -ne 0) { throw "Gi.hlsl $entry compilation failed." }
    & pwsh -NoProfile -File (Join-Path $Repo 'tools\bin2h.ps1') -In $bin -Out $header -Name "Gi_${name}_cso"
    if ($LASTEXITCODE -ne 0) { throw "Gi.hlsl $entry header generation failed." }
}
