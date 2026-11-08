param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$shader = Join-Path $Repo 'OptiScaler\ext\ffxiv\engine\shaders\CbScale.hlsl'
$fxc = Join-Path $Repo 'OptiScaler\shaders\shader_tools\fxc.exe'
# The same recipe as ext\ext.props: fxc -nologo -T cs_5_0 -E CSMain -O3, then bin2h into the header CbPatch.cpp includes.
$bin = Join-Path $Out 'CbScale_CS.bin'
$header = Join-Path $Out 'CbScale_CS.h'
& $fxc -nologo -T cs_5_0 -E CSMain -O3 $shader -Fo $bin
if ($LASTEXITCODE -ne 0) { throw 'CbScale.hlsl compilation failed.' }
& pwsh -NoProfile -File (Join-Path $Repo 'tools\bin2h.ps1') -In $bin -Out $header -Name 'CbScale_CS_cso'
if ($LASTEXITCODE -ne 0) { throw 'CbScale.hlsl header generation failed.' }
