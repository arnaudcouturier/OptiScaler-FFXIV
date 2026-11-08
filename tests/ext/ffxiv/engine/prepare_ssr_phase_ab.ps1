param([Parameter(Mandatory)][string]$Repo,[Parameter(Mandatory)][string]$Out)
$ErrorActionPreference='Stop'
$variant=$env:OPTISCALER_SSR_PHASE_VARIANT
if($variant -notin @('original','half')){throw 'OPTISCALER_SSR_PHASE_VARIANT must be original or half'}
$source=Join-Path $Repo 'OptiScaler\ext\ffxiv\engine\shaders'
$include=Join-Path $Repo 'OptiScaler\ext\ffxiv\relight\shaders'
$copy=Join-Path $Out 'phase-src'
New-Item -ItemType Directory -Force -Path (Join-Path $copy 'engine\shaders'),(Join-Path $copy 'relight\shaders') | Out-Null
Copy-Item (Join-Path $source '*') (Join-Path $copy 'engine\shaders') -Recurse -Force
Copy-Item (Join-Path $include '*') (Join-Path $copy 'relight\shaders') -Recurse -Force
$march=Join-Path $copy 'engine\shaders\SceneMarch.hlsl'
if($variant -eq 'half'){
  $s=Get-Content $march -Raw
  $old='const float2 projected = float2(window.x + (ndc.x * 0.5 + 0.5) * window.z, window.y + (0.5 - ndc.y * 0.5) * window.w);'
  $new='const float2 projected = float2(window.x + (ndc.x * 0.5 + 0.5) * window.z, window.y + (0.5 - ndc.y * 0.5) * window.w) + float2(0.5, 0.5);'
  if(-not $s.Contains($old)){throw 'SceneMarchProject target expression not found exactly'}
  Set-Content -NoNewline -Path $march -Value $s.Replace($old,$new)
}
$shader=Join-Path $copy 'engine\shaders\Ssr.hlsl'
$camera=Join-Path $copy 'relight\shaders'
$fxc=Join-Path $Repo 'OptiScaler\shaders\shader_tools\fxc.exe'
$diag=$env:OPTISCALER_SSR_PHASE_DIAG -eq '1'
foreach($t in @(@('vs_5_0','VS','Vs',@()),@('ps_5_0','PS_Ssr','Ssr',$(if($diag){@('-DSSR_MARCH_DIAG')}else{@()})),@('ps_5_0','PS_SsrFinal','Final',$(if($diag){@('-DSSR_MARCH_DIAG')}else{@()})))){
  & $fxc -nologo -T $t[0] -E $t[1] -O3 -I $camera @($t[3]) $shader -Fo (Join-Path $Out "Ssr_$($t[2]).bin")
  if($LASTEXITCODE -ne 0){throw "Ssr.hlsl $($t[1]) $variant compilation failed"}
  & pwsh -NoProfile -File (Join-Path $Repo 'tools\bin2h.ps1') -In (Join-Path $Out "Ssr_$($t[2]).bin") -Out (Join-Path $Out "Ssr_$($t[2]).h") -Name "Ssr_$($t[2])_cso"
  if($LASTEXITCODE -ne 0){throw 'shader header generation failed'}
}
