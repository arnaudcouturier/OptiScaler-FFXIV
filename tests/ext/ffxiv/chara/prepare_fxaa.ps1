param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$shader = Join-Path $Repo 'OptiScaler\ext\ffxiv\chara\shaders\chara_fxaa.hlsl'
$fxc = Join-Path $Repo 'OptiScaler\shaders\shader_tools\fxc.exe'
# The same recipe as ext\ext.props: fxc -nologo -T <profile> -E <entry> -O3, one blob per entry point.
foreach ($target in @(@('vs_5_0', 'VS'), @('ps_5_0', 'PS'), @('ps_5_0', 'PSTint'))) {
    $profile = $target[0]; $entry = $target[1]
    $bin = Join-Path $Out "Chara_Fxaa_$entry.bin"
    $header = Join-Path $Out "Chara_Fxaa_$entry.h"
    & $fxc -nologo -T $profile -E $entry -O3 $shader -Fo $bin
    if ($LASTEXITCODE -ne 0) { throw "chara_fxaa.hlsl $entry compilation failed." }
    & pwsh -NoProfile -File (Join-Path $Repo 'tools\bin2h.ps1') -In $bin -Out $header -Name "Chara_Fxaa_${entry}_cso"
    if ($LASTEXITCODE -ne 0) { throw "chara_fxaa.hlsl $entry header generation failed." }
}
