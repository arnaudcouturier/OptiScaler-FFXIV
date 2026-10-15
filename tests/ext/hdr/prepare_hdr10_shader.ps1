param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$shaderRoot = Join-Path $Repo 'OptiScaler\ext\hdr\shaders'
$fxc = Join-Path $Repo 'OptiScaler\shaders\shader_tools\fxc.exe'
$bin = Join-Path $Out 'Hdr10_Shader.bin'
$header = Join-Path $Out 'Hdr10_Shader.h'
& $fxc -nologo -T cs_5_0 -E CSMain -O3 -I $shaderRoot (Join-Path $shaderRoot 'Hdr10.hlsl') -Fo $bin
if ($LASTEXITCODE -ne 0) { throw 'HDR10 shader compilation failed.' }
& pwsh -NoProfile -File (Join-Path $Repo 'tools\bin2h.ps1') -In $bin -Out $header -Name Hdr10_cso
if ($LASTEXITCODE -ne 0) { throw 'HDR10 shader header generation failed.' }
