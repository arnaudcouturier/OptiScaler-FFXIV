# Runner (tools\test.ps1 `Runner`): the main NR shader must still compile for Vulkan. The Direct3D 12 build has a sixth input (the game's depth,
# read by the depth-aware enlargement) that the Vulkan layout does not have; it is declared and used only outside VK_MODE. Nothing else compiles the
# file with -D VK_MODE (the Vulkan smoke test needs a real device), so a use of the depth input that slips out of its guard would break the Vulkan
# build without any test noticing. Same dxc and flags as the product recipe (OptiScaler\ext\nr\nr.shaders.props).
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$dxc = Join-Path $Repo 'OptiScaler\shaders\shader_tools\dxc.exe'
if (-not (Test-Path $dxc)) { throw "dxc.exe not found at $dxc" }
$dst = Join-Path $Out 'shaders'
New-Item -ItemType Directory -Path $dst -Force | Out-Null
Copy-Item -Path (Join-Path $Repo 'OptiScaler\ext\nr\shaders\*') -Destination $dst
Copy-Item -Path (Join-Path $Repo 'OptiScaler\ext\shaders\common\*') -Destination $dst

function Invoke-VkCompile([string]$Source, [string]$Output) {
    $log = & $dxc -spirv -T cs_6_0 -E CSMain -O3 -Qstrip_debug -D VK_MODE -Cc -Vi $Source -Fo $Output 2>&1
    return [pscustomobject]@{ Code = $LASTEXITCODE; Log = ($log | Out-String) }
}

# 1. The shader as it is compiles, and writes SPIR-V.
$spv = Join-Path $Out 'dlssnr_vk.spv'
$ok = Invoke-VkCompile (Join-Path $dst 'dlssnr.hlsl') $spv
if ($ok.Code -ne 0) { Write-Host $ok.Log; throw 'dlssnr.hlsl does not compile with -D VK_MODE' }
if (-not (Test-Path $spv) -or (Get-Item $spv).Length -lt 4096) { throw 'dxc reported success but wrote no usable SPIR-V' }
Write-Host ("PASS: dlssnr.hlsl compiles for Vulkan ({0} bytes of SPIR-V)" -f (Get-Item $spv).Length)

# 2. The negative control: the same file with a use of the depth input outside its guard must NOT compile, so the check above can fail.
$bad = Join-Path $dst 'dlssnr_unguarded.hlsl'
$text = [IO.File]::ReadAllText((Join-Path $dst 'dlssnr.hlsl'))
[IO.File]::WriteAllText($bad, $text + "`r`nfloat UnguardedDepthUse() { return gDepth.Load(int3(0, 0, 0)).x; }`r`n")
$broken = Invoke-VkCompile $bad (Join-Path $Out 'dlssnr_unguarded.spv')
if ($broken.Code -eq 0) { throw 'negative control: a use of the Direct3D 12-only depth input outside its guard compiled for Vulkan' }
if ($broken.Log -notmatch 'gDepth') { Write-Host $broken.Log; throw 'negative control failed for another reason than the undeclared depth input' }
Write-Host 'PASS: the check can fail (an unguarded use of the depth input is rejected)'
exit 0
