# Test prepare step (tools\test.ps1 `Prepare`): stages the NR HLSL sources with the shared include directory next to
# them, so the tests' D3DCompileFromFile(D3D_COMPILE_STANDARD_FILE_INCLUDE) finds `HdrCompatibility.hlsli`
# (ext\shaders\common) as well as dlssnr_spatial_warp.hlsli (ext\nr\shaders). The tests then compile {out}\shaders\*.hlsl.
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$dst = Join-Path $Out 'shaders'
New-Item -ItemType Directory -Path $dst -Force | Out-Null
Copy-Item -Path (Join-Path $Repo 'OptiScaler\ext\nr\shaders\*') -Destination $dst
Copy-Item -Path (Join-Path $Repo 'OptiScaler\ext\shaders\common\*') -Destination $dst
# The resolve shader as it was before the depth-aware enlargement was added: the reference the new shader must match bit for bit
# while the feature is off (nr_community_shader).
Copy-Item -Path (Join-Path $Repo 'tests\ext\nr\fixtures\dlssnr_v040_reference.hlsl') -Destination $dst
