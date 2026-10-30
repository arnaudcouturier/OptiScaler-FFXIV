# Builds the real S10 HUD-less shaders (OptiScaler\ext\hudless\shaders) into {out} with the exact recipe, header names
# and array names of hudless.shaders.props (fxc -O3), so the production UiImage.cpp / UiPaste.cpp compile unchanged.
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$shaderRoot = Join-Path $Repo 'OptiScaler\ext\hudless\shaders'
$fxc = Join-Path $Repo 'OptiScaler\shaders\shader_tools\fxc.exe'
$jobs = @(
    @{ File = 'UiPasteProduce.hlsl'; Profile = 'cs_5_0'; Entry = 'CSMain'; Header = 'UiPasteProduce_Shader.h'; Name = 'UiPasteProduce_cso' },
    @{ File = 'UiRecomposition.hlsl'; Profile = 'cs_5_0'; Entry = 'CSMain'; Header = 'UiRecomposition_Shader.h'; Name = 'UiRecomposition_cso' },
    @{ File = 'UiPasteDraw.hlsl'; Profile = 'vs_5_0'; Entry = 'VSMain'; Header = 'UiPasteVs_Shader.h'; Name = 'UiPasteVs_cso' },
    @{ File = 'UiPasteDraw.hlsl'; Profile = 'ps_5_0'; Entry = 'PSMain'; Header = 'UiPastePs_Shader.h'; Name = 'UiPastePs_cso' },
    @{ File = 'UiPlateDraw.hlsl'; Profile = 'vs_5_0'; Entry = 'VSMain'; Header = 'UiPlateVs_Shader.h'; Name = 'UiPlateVs_cso' },
    @{ File = 'UiPlateDraw.hlsl'; Profile = 'ps_5_0'; Entry = 'PSMain'; Header = 'UiPlatePs_Shader.h'; Name = 'UiPlatePs_cso' }
)
foreach ($j in $jobs) {
    $bin = Join-Path $Out ($j.Header + '.bin')
    & $fxc -nologo -T $j.Profile -E $j.Entry -O3 -I $shaderRoot (Join-Path $shaderRoot $j.File) -Fo $bin
    if ($LASTEXITCODE -ne 0) { throw "fxc failed for $($j.File) $($j.Entry)" }
    & pwsh -NoProfile -File (Join-Path $Repo 'tools\bin2h.ps1') -In $bin -Out (Join-Path $Out $j.Header) -Name $j.Name
    if ($LASTEXITCODE -ne 0) { throw "bin2h failed for $($j.Header)" }
}
