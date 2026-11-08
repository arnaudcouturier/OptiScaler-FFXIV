<#
.SYNOPSIS
  Compiles the product's screen-space reflection shader for the offline frame test, with the march's diagnostic output.
.DESCRIPTION
  The test drives the product's own SsrPass.cpp, which includes the generated Ssr_Vs.h / Ssr_Ssr.h. This script compiles
  ext\ffxiv\engine\shaders\Ssr.hlsl into this test's own build folder with the product's fxc recipe, and with SSR_MARCH_DIAG defined for the
  pixel shader so the target's red channel carries the march's exit code and its green channel the hit's distance. The product's build never
  defines that name (ssr.shaders.props): its shader is the same file with the diagnostics removed, byte for byte.
  Usage (the test runner): pwsh -File prepare_ssr_frame.ps1 -Repo <repo root> -Out <build folder>
#>
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$shader = Join-Path $Repo 'OptiScaler\ext\ffxiv\engine\shaders\Ssr.hlsl'
$camera = Join-Path $Repo 'OptiScaler\ext\ffxiv\relight\shaders'
$fxc = Join-Path $Repo 'OptiScaler\shaders\shader_tools\fxc.exe'
# The same recipe as ext\ext.props: fxc -nologo -T <profile> -E <entry> -O3 -I <camera registers>, one blob per entry point.
foreach ($target in @(@('vs_5_0', 'VS', 'Vs', @()), @('ps_5_0', 'PS_Ssr', 'Ssr', @('-DSSR_MARCH_DIAG')), @('ps_5_0', 'PS_SsrFinal', 'Final', @('-DSSR_MARCH_DIAG')))) {
    $profile = $target[0]; $entry = $target[1]; $name = $target[2]; $defines = $target[3]
    $bin = Join-Path $Out "Ssr_$name.bin"
    $header = Join-Path $Out "Ssr_$name.h"
    & $fxc -nologo -T $profile -E $entry -O3 -I $camera @defines $shader -Fo $bin
    if ($LASTEXITCODE -ne 0) { throw "Ssr.hlsl $entry compilation failed." }
    & pwsh -NoProfile -File (Join-Path $Repo 'tools\bin2h.ps1') -In $bin -Out $header -Name "Ssr_${name}_cso"
    if ($LASTEXITCODE -ne 0) { throw "Ssr.hlsl $entry header generation failed." }
}
