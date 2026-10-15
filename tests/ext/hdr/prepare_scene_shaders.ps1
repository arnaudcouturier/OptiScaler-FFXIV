param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$shaderRoot = Join-Path $Repo 'OptiScaler\ext\hdr\shaders\renodx'
$fxc = Join-Path $Repo 'OptiScaler\shaders\shader_tools\fxc.exe'
$convert = Join-Path $Repo 'tools\bin2h.ps1'
$shaders = @(
    @{ File = 'Tonemap_0x85E777EF.ps_5_0.hlsl'; Name = 'Tonemap_0x85E777EF'; Header = 'Tonemap_0x85E777EF_Shader.h' },
    @{ File = 'PostTonemapPreLUT_0xF8F57F0A.ps_5_0.hlsl'; Name = 'PostTonemapPreLUT_0xF8F57F0A'; Header = 'PostTonemapPreLUT_0xF8F57F0A_Shader.h' },
    @{ File = 'LUT_0x27EBC404.ps_5_0.hlsl'; Name = 'LUT_0x27EBC404'; Header = 'LUT_0x27EBC404_Shader.h' },
    @{ File = 'LUT_0x1F264D17.ps_5_0.hlsl'; Name = 'LUT_0x1F264D17'; Header = 'LUT_0x1F264D17_Shader.h' },
    @{ File = 'FullscreenGammaCorrection_0xF6E81A1B.ps_5_0.hlsl'; Name = 'FullscreenGammaCorrection_0xF6E81A1B'; Header = 'FullscreenGammaCorrection_0xF6E81A1B_Shader.h' }
)
foreach ($shader in $shaders) {
    $input = Join-Path $shaderRoot $shader.File
    $bin = Join-Path $Out ($shader.Name + '.bin')
    $header = Join-Path $Out $shader.Header
    & $fxc -nologo -T ps_5_0 -E main -O3 -I $shaderRoot $input -Fo $bin
    if ($LASTEXITCODE -ne 0) { throw "Scene shader compilation failed: $($shader.File)" }
    & pwsh -NoProfile -File $convert -In $bin -Out $header -Name $shader.Name
    if ($LASTEXITCODE -ne 0) { throw "Scene shader header generation failed: $($shader.File)" }
}
