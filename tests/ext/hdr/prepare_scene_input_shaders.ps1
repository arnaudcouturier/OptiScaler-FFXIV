param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$shaderRoot = Join-Path $Repo 'OptiScaler\ext\hdr\shaders'
$commonRoot = Join-Path $Repo 'OptiScaler\ext\shaders\common'
$fxc = Join-Path $Repo 'OptiScaler\shaders\shader_tools\fxc.exe'
$convert = Join-Path $Repo 'tools\bin2h.ps1'
$shaders = @(
    @{ File = 'DlssSceneInput.hlsl'; Name = 'DlssSceneInput_cso'; Header = 'DlssSceneInput_Shader.h' },
    @{ File = 'NrSceneInput.hlsl'; Name = 'NrSceneInput_cso'; Header = 'NrSceneInput_Shader.h' }
)
foreach ($shader in $shaders) {
    $input = Join-Path $shaderRoot $shader.File
    $bin = Join-Path $Out ($shader.Name + '.bin')
    $header = Join-Path $Out $shader.Header
    & $fxc -nologo -T cs_5_0 -E CSMain -O3 -I $commonRoot $input -Fo $bin
    if ($LASTEXITCODE -ne 0) { throw "Scene input shader compilation failed: $($shader.File)" }
    & pwsh -NoProfile -File $convert -In $bin -Out $header -Name $shader.Name
    if ($LASTEXITCODE -ne 0) { throw "Scene input shader header generation failed: $($shader.File)" }
}
