param([Parameter(Mandatory)][string]$Repo,[Parameter(Mandatory)][string]$Out)
$ErrorActionPreference='Stop'
$shader=Join-Path $Repo 'OptiScaler\ext\ffxiv\engine\shaders\Ssr.hlsl'
$camera=Join-Path $Repo 'OptiScaler\ext\ffxiv\relight\shaders'
$fxc=Join-Path $Repo 'OptiScaler\shaders\shader_tools\fxc.exe'
foreach($t in @(@('vs_5_0','VS','Vs',@()),@('ps_5_0','PS_Ssr','Ssr',@('-DSSR_MARCH_DIAG')),@('ps_5_0','PS_SsrFinal','Final',@('-DSSR_MARCH_DIAG')))) {
  & $fxc -nologo -T $t[0] -E $t[1] -O3 -I $camera @($t[3]) $shader -Fo (Join-Path $Out "Ssr_$($t[2]).bin")
  if($LASTEXITCODE -ne 0){throw "Ssr.hlsl $($t[1]) compilation failed"}
  & pwsh -NoProfile -File (Join-Path $Repo 'tools\bin2h.ps1') -In (Join-Path $Out "Ssr_$($t[2]).bin") -Out (Join-Path $Out "Ssr_$($t[2]).h") -Name "Ssr_$($t[2])_cso"
  if($LASTEXITCODE -ne 0){throw 'shader header generation failed'}
}
