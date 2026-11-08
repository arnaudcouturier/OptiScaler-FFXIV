param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$fxc = Join-Path $Repo 'OptiScaler\shaders\shader_tools\fxc.exe'
# The same recipe as ext\ext.props: fxc -nologo -T <profile> -E <entry> -O3, one blob per entry point.
# ShadowLift.hlsl includes ShadowLiftCommon.hlsli (the tent and the remap rules the contact combine shares), so the
# include path adds the file's own directory the way the ext build does (-I its own directory).
$shadow = Join-Path $Repo 'OptiScaler\ext\ffxiv\shadows\shaders\ShadowLift.hlsl'
foreach ($target in @(@('vs_5_0', 'VS', 'Vs'), @('ps_5_0', 'PS_Lift', 'Ps'))) {
    $profile = $target[0]; $entry = $target[1]; $name = $target[2]
    $bin = Join-Path $Out "ShadowLift_$name.bin"
    $header = Join-Path $Out "ShadowLift_$name.h"
    & $fxc -nologo -T $profile -E $entry -O3 -I (Join-Path $Repo 'OptiScaler\ext\ffxiv\shadows\shaders') $shadow -Fo $bin
    if ($LASTEXITCODE -ne 0) { throw "ShadowLift.hlsl $entry compilation failed." }
    & pwsh -NoProfile -File (Join-Path $Repo 'tools\bin2h.ps1') -In $bin -Out $header -Name "ShadowLift_${name}_cso"
    if ($LASTEXITCODE -ne 0) { throw "ShadowLift.hlsl $entry header generation failed." }
}
# The contact pass's shaders too: the warp test runs the real combine with the lift stage integrated in it.
$contact = Join-Path $Repo 'OptiScaler\ext\ffxiv\relight\shaders\ContactShadows.hlsl'
foreach ($target in @(@('vs_5_0', 'VS', 'Vs'), @('ps_5_0', 'PS_March', 'March'), @('ps_5_0', 'PS_Combine', 'Combine'))) {
    $profile = $target[0]; $entry = $target[1]; $name = $target[2]
    $bin = Join-Path $Out "ContactShadows_$name.bin"
    $header = Join-Path $Out "ContactShadows_$name.h"
    & $fxc -nologo -T $profile -E $entry -O3 -I (Join-Path $Repo 'OptiScaler\ext\ffxiv\relight\shaders') $contact -Fo $bin
    if ($LASTEXITCODE -ne 0) { throw "ContactShadows.hlsl $entry compilation failed." }
    & pwsh -NoProfile -File (Join-Path $Repo 'tools\bin2h.ps1') -In $bin -Out $header -Name "ContactShadows_${name}_cso"
    if ($LASTEXITCODE -ne 0) { throw "ContactShadows.hlsl $entry header generation failed." }
}
