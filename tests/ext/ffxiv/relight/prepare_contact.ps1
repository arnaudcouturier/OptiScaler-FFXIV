param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$shader = Join-Path $Repo 'OptiScaler\ext\ffxiv\relight\shaders\ContactShadows.hlsl'
$fxc = Join-Path $Repo 'OptiScaler\shaders\shader_tools\fxc.exe'
# The same recipe as ext\ext.props: fxc -nologo -T <profile> -E <entry> -O3, one blob per entry point.
foreach ($target in @(@('vs_5_0', 'VS', 'Vs'), @('ps_5_0', 'PS_March', 'March'), @('ps_5_0', 'PS_Combine', 'Combine'))) {
    $profile = $target[0]; $entry = $target[1]; $name = $target[2]
    $bin = Join-Path $Out "ContactShadows_$name.bin"
    $header = Join-Path $Out "ContactShadows_$name.h"
    & $fxc -nologo -T $profile -E $entry -O3 $shader -Fo $bin
    if ($LASTEXITCODE -ne 0) { throw "ContactShadows.hlsl $entry compilation failed." }
    & pwsh -NoProfile -File (Join-Path $Repo 'tools\bin2h.ps1') -In $bin -Out $header -Name "ContactShadows_${name}_cso"
    if ($LASTEXITCODE -ne 0) { throw "ContactShadows.hlsl $entry header generation failed." }
}
