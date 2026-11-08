param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$shader = Join-Path $Repo 'OptiScaler\ext\ffxiv\meter\shaders\Meter.hlsl'
$fxc = Join-Path $Repo 'OptiScaler\shaders\shader_tools\fxc.exe'
# The same recipe as ext\ext.props: fxc -nologo -T <profile> -E <entry> -O3, one blob per entry point.
foreach ($target in @(@('vs_5_0', 'VS', 'Vs'), @('ps_5_0', 'PS', 'Ps'))) {
    $profile = $target[0]; $entry = $target[1]; $name = $target[2]
    $bin = Join-Path $Out "Meter_$name.bin"
    $header = Join-Path $Out "Meter_$name.h"
    # The pixel shader is compiled with -Gis (IEEE strictness): the isnan() test that keeps a NaN texel out of the sums must not be folded away.
    [string[]]$extra = if ($name -eq 'Ps') { @('/Gis') } else { @() }
    & $fxc -nologo -T $profile -E $entry -O3 @extra $shader -Fo $bin
    if ($LASTEXITCODE -ne 0) { throw "Meter.hlsl $entry compilation failed." }
    & pwsh -NoProfile -File (Join-Path $Repo 'tools\bin2h.ps1') -In $bin -Out $header -Name "Meter_${name}_cso"
    if ($LASTEXITCODE -ne 0) { throw "Meter.hlsl $entry header generation failed." }
}
