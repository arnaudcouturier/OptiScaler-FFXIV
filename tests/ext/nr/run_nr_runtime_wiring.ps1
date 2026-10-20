# Runner (tools\test.ps1 `Runner`): pins how the NR runtime picker is wired into the production sources, so a later edit cannot quietly
# bring back the recursive model search (which would pick an NR.<version> folder by search order) or move the switch check behind the
# failure latch. Plain text checks on OptiScaler\ext\nr; no build needed.
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$nr = Join-Path $Repo 'OptiScaler\ext\nr'
$failures = @()
function Check([bool]$ok, [string]$what) { if (-not $ok) { $script:failures += $what; Write-Output "FAIL: $what" } }

# 1. Nothing in ext\nr finds the NR model with the recursive Util::FindFilePath (the forwarder file nvngx.dll_dlssnr.dll may keep it).
foreach ($f in Get-ChildItem $nr -Recurse -File -Include *.cpp, *.h, *.inl) {
    $text = [IO.File]::ReadAllText($f.FullName)
    Check (-not [regex]::IsMatch($text, 'FindFilePath\s*\([^;]*"nvngx_dlssnr\.dll"')) "$($f.Name) still searches for nvngx_dlssnr.dll with FindFilePath"
}

$dx12 = [IO.File]::ReadAllText((Join-Path $nr 'pass\DlssNr_Dx12.cpp'))

# 2. The scene pass applies a new revision before it looks at the failure latch.
$dispatch = $dx12.IndexOf('void DlssNr_Dx12::Dispatch(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour,')
Check ($dispatch -ge 0) 'Dispatch not found'
$poll = $dx12.IndexOf('PollRuntime();', $dispatch)
$latch = $dx12.IndexOf('if (g_nr.failed || cmdList == nullptr', $dispatch)
Check ($poll -ge 0 -and $latch -ge 0 -and $poll -lt $latch) 'Dispatch must call PollRuntime() before it tests g_nr.failed'

# 3. A switch parks the scene feature and every extra-pass feature through ParkNrFeature, and drops the reuse caches.
$host_ = $dx12.IndexOf('struct RuntimeHost')
Check ($host_ -ge 0) 'RuntimeHost not found'
$park = $dx12.IndexOf('void ParkFeatures()', $host_)
$parkEnd = $dx12.IndexOf('void ClearHistory()', $park)
$parkBody = $dx12.Substring($park, $parkEnd - $park)
Check ($parkBody.Contains('ParkNrFeature(g_nr.feature)')) 'ParkFeatures must park the scene feature'
Check ($parkBody.Contains('ParkNrFeature(g_nr.passFeature[i])')) 'ParkFeatures must park the extra-pass features'
Check ($parkBody -notmatch 'release\(') 'ParkFeatures must not release a feature directly (the GPU may still use it)'
Check ($dx12.Contains('void DropReuseCaches() { DlssNrNative::OnModelSwitch(); }')) 'DropReuseCaches must call DlssNrNative::OnModelSwitch()'

# 4. The model path comes from the selection: FindSelectedModel and the preview backend read the runtime snapshot.
Check ($dx12.Contains('return snapshot->path;')) 'FindSelectedModel must return the snapshot path'
$acquire = $dx12.IndexOf('PreviewBackendStatus AcquirePreviewBackend(')
$acquireBody = $dx12.Substring($acquire, 1400)
Check ($acquireBody.Contains('ext::nr::runtime::Current()') -and $acquireBody.Contains('out.revision = runtime->revision;')) 'AcquirePreviewBackend must take path and revision from the runtime selection'

# 5. The pinned paths (first NGX search path, native Vulkan) follow the startup selection, never a later switch.
$seams = [IO.File]::ReadAllText((Join-Path $nr 'NrSeams.cpp'))
Check ($seams.Contains('runtime::Startup()')) 'AppendNgxSearchPaths must use the startup selection'
$vk = [IO.File]::ReadAllText((Join-Path $nr 'vk\DlssNrFeature_Vk.cpp'))
Check ($vk.Contains('ext::nr::runtime::Startup()')) 'Vulkan FindSnippet must use the startup selection'

# 6. The runtime choice is not part of the saved NR presets (the allowlist in NrUserPresets.h), and the config key is bound.
$presets = [IO.File]::ReadAllText((Join-Path $nr 'presets\NrUserPresets.h'))
Check ($presets -notmatch '(?i)NR_FIELD\(\s*Runtime\b|\.runtime\b') 'the saved NR presets must not carry the runtime choice'
$cfg = [IO.File]::ReadAllText((Join-Path $nr 'NrConfig.cpp'))
Check ($cfg.Contains('b.Add("DlssNr", "Runtime", runtime);')) '[DlssNr] Runtime must be bound'

if ($failures.Count -ne 0) { throw "$($failures.Count) wiring check(s) failed" }
Write-Output 'PASS: NR runtime wiring'
