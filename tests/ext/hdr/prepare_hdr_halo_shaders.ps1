param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
# Compiles the shader under test twice with the product's recipe (fxc cs_5_0 CSMain -O3): the current Hdr10.hlsl (Hdr10_cso) and the
# frozen copy of the shader of main 191c89b2 (Hdr10Legacy_cso), the oracle for the "legacy composition is byte-for-byte unchanged" claim.
$ErrorActionPreference = 'Stop'
$shaderRoot = Join-Path $Repo 'OptiScaler\ext\hdr\shaders'
$fxc = Join-Path $Repo 'OptiScaler\shaders\shader_tools\fxc.exe'
function Build([string]$Hlsl, [string]$Name, [string]$Header) {
    $bin = Join-Path $Out ($Header + '.bin')
    & $fxc -nologo -T cs_5_0 -E CSMain -O3 -I $shaderRoot $Hlsl -Fo $bin
    if ($LASTEXITCODE -ne 0) { throw "shader compilation failed: $Hlsl" }
    & pwsh -NoProfile -File (Join-Path $Repo 'tools\bin2h.ps1') -In $bin -Out (Join-Path $Out $Header) -Name $Name
    if ($LASTEXITCODE -ne 0) { throw "header generation failed: $Header" }
}
Build (Join-Path $shaderRoot 'Hdr10.hlsl') 'Hdr10_cso' 'Hdr10_Shader.h'
Build (Join-Path $Repo 'tests\ext\hdr\legacy\Hdr10_191c89b2.hlsl') 'Hdr10Legacy_cso' 'Hdr10Legacy_Shader.h'
