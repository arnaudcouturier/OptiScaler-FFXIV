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
$drop = $dx12.IndexOf('void DropReuseCaches()', $host_)
Check ($drop -ge 0 -and $dx12.Substring($drop, 140).Contains('DlssNrNative::OnModelSwitch()')) 'DropReuseCaches must call DlssNrNative::OnModelSwitch()'

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

# 5b. The character previews follow the selection: the backend is re-acquired before the availability is read, and a stream built from
#     another revision is released (after the GPU is done with it) and rebuilt; the revision of a stream is recorded when its model is made.
$chara = [IO.File]::ReadAllText((Join-Path $Repo 'OptiScaler\ext\ffxiv\chara\CharaNr.cpp'))
$sync = $chara.IndexOf('SyncRuntime(); //')
$avail = $chara.IndexOf('if (Refresh() != Availability::Available || !EnsureInit(req.context))')
Check ($sync -ge 0 -and $avail -ge 0 -and $sync -lt $avail) 'RunCoreSteps must call SyncRuntime() before it reads the availability'
$reprobe = $chara.IndexOf('m.model == Probe::Missing && DlssNr::PreviewRuntimeRevision() != m.probedRuntime')
Check ($reprobe -ge 0 -and $chara.Substring($reprobe, 220).Contains('m.forwarder = m.model = Probe::Unknown;')) 'a preview module that started without a model must probe again when the selection moves'
Check ($chara.Contains('m.probedRuntime = m.backend.revision;')) 'EnsureInit must record the revision it probed'
$stale = $chara.IndexOf('m.streamRuntime[index] != m.backend.revision')
Check ($stale -ge 0) 'RunCoreSteps must compare the stream revision with the backend revision'
if ($stale -ge 0) {
    $block = $chara.Substring($stale, 700)
    $gpu = $block.IndexOf('GpuDone(m.table.At(index).lastSubmitted)')
    $rel = $block.IndexOf('ReleaseStreamObjects(m.streams[index], true)')
    Check ($gpu -ge 0 -and $rel -ge 0 -and $gpu -lt $rel) 'a stream of another revision is released only after the GPU is done with it'
}
Check ($chara.Contains('m.streamRuntime[index] = b.revision;')) 'CreateFeatureStep must record the revision the model was built for'

# 5c. The card draws its combo and status lines from the pinned text module, and the combo is disabled while switching is locked.
$menu = [IO.File]::ReadAllText((Join-Path $nr 'menu\DlssNr_Menu.cpp'))
Check ($menu.Contains('rt::text::BuildChoices(') -and $menu.Contains('rt::text::StatusLines(')) 'the card must use NrRuntimeText.h for its entries and status lines'
Check ($menu.Contains('ImGui::BeginDisabled(lock != nullptr);')) 'the combo must be disabled while live switching is locked'
Check ($menu.Contains('rt::RequestRuntime(choices.values[(size_t) selected]);')) 'a pick must go through RequestRuntime (the coalescing rule)'

# 5d. The glue: the NGX search folder is the one AppendNgxSearchPaths really added (no folder recorded = nothing to disagree with),
#     the catalog is hashed only when the card asks for it, and the session lock is evaluated outside the module mutex.
Check ($seams.Contains('runtime::NoteNgxSearchFolder(')) 'AppendNgxSearchPaths must record the folder it added'
$glue = [IO.File]::ReadAllText((Join-Path $nr 'runtime\NrRuntime.cpp'))
Check ($glue.Contains('!s.Available() || G().ngxFolder.empty() ||')) 'FollowsNgxSearchPath must treat a missing folder as following'
$build = $glue.IndexOf('void BuildCatalogLocked()')
$buildEnd = $glue.IndexOf('void StartHashingLocked()')
Check ($build -ge 0 -and $buildEnd -gt $build -and -not $glue.Substring($build, $buildEnd - $build).Contains('std::thread')) 'building the catalog must not start the hash worker'
$cat = $glue.IndexOf('std::vector<Entry> Catalog()')
Check ($cat -ge 0 -and $glue.Substring($cat, 400).Contains('StartHashingLocked();')) 'Catalog() (the card) starts the hash worker'
$cur = $glue.IndexOf('std::shared_ptr<const Snapshot> Current()')
Check ($cur -ge 0 -and $glue.IndexOf('LockedBySession(nullptr);', $cur) -lt $glue.IndexOf('std::lock_guard<std::mutex> lock(g.mutex);', $cur)) 'Current() must evaluate the session lock before it takes the module mutex'

# 6. The runtime choice is not part of the saved NR presets (the allowlist in NrUserPresets.h), and the config key is bound.
$presets = [IO.File]::ReadAllText((Join-Path $nr 'presets\NrUserPresets.h'))
Check ($presets -notmatch '(?i)NR_FIELD\(\s*Runtime\b|\.runtime\b') 'the saved NR presets must not carry the runtime choice'
$cfg = [IO.File]::ReadAllText((Join-Path $nr 'NrConfig.cpp'))
Check ($cfg.Contains('b.Add("DlssNr", "Runtime", runtime);')) '[DlssNr] Runtime must be bound'

if ($failures.Count -ne 0) { throw "$($failures.Count) wiring check(s) failed" }
Write-Output 'PASS: NR runtime wiring'
