# How OptiScaler-FFXIV attaches to OptiScaler

This repo is OptiScaler `master` (nightly) plus a stack of fork commits. This document is the contract that keeps the stack cheap to rebase. **Read it before changing any upstream file.**

## 1. The invariant

1. **Everything the fork owns lives in fork-only paths:**
   - `OptiScaler/ext/**`
   - `docs/**`
   - `tools/**`
   - `tests/ext/**`
   - `.github/README.md`
   - `.github/workflows/ffxiv-*.yml`
2. **Upstream files may only contain:**
   - `#include "ext/Seams.h"`. This is the only fork header upstream files include.
   - **Seam calls:** one line each, `ext::<group>::<Name>(...)`, as listed in section 4.
   - **Friend line:** one `friend struct ext::Access;` per upstream class whose private state the fork must reach (section 3.3).
   - **Upstream-candidate fixes:** self-contained `[UP]` commits (section 3.5).
   - **Marked inline changes:** inline changes that cannot be seams, each marked with a trailing `// ext: <ID> <why>` comment.
3. **Every upstream line counts.** `tools/footprint.ps1` prints the diff against the upstream base, excluding fork-only paths. Every wave reports it.
   - Target, revised after the catalog projection (2026-10-15): **≤ 600 changed lines of persistent surface** (seam calls, `// ext:` inline lines, friend/`_ext` lines). This compares with the reference merge's 114 files and 6.8k lines.
   - `[UP]` commits are reported separately (projected ~700 lines). Each is self-contained, so a conflict in one stays inside that commit, and it disappears when upstream ships an equivalent.
   - The touched-file count is reported but has no target, because seams are naturally spread across the files they hook.
   - Any single upstream edit of more than ~10 lines needs a justification in the commit message.

## 2. Layout

```
OptiScaler/ext/
  Seams.h, Seams.cpp       seam declarations; inline fast-path guards; ext::Access
  ExtFeatures.h            feature bits for the fast-path guards
  core/                    ExtConfig + ini binding, MenuRegistry + widgets, HooksBypass scope,
                           version banner, DetourLock, build info
  game/                    game-module interfaces (IRenderGeometry, INativeQuality, ISceneLighting,
                           ISceneHdr, IFgMarkers) + ext::game::Current()
  gpu/                     C6 submission ownership, GpuLifetime, command-list Reset/Execute wrappers
  nr/                      A1–A12, B1–B5, B7, B8, B9; nr/forwarder/, nr/shaders/, nr/amd/ (E4)
  fg/                      D1–D5, E3; fg/dlssg/, fg/xefg/, fg/streamline/
  hdr/                     C8 OptiHDR, C9 HDR PNG + ICC, C10 RenoDX scene HDR (+ NOTICE), F3, F4
  hudless/                 C11 + G: HudlessEngine, HudlessSource
  ffxiv/                   GameBuild.h (all RVAs + byte guards), FfxivModule (implements game/
                           interfaces), C1–C5, C7 companion, B6 lighting scan, F5 CharaView,
                           G FfxivUiBoundary
  ini/                     ini template fragments per section (merged into OptiScaler.ini by tools/package.ps1)
  ext.props                imported once by OptiScaler.vcxproj
```

FFXIV-specific code lives only in `ext/ffxiv/`. Feature code (`nr`, `fg`, `hdr`, `hudless`, `gpu`) is game-agnostic and reaches the game only through `ext/game/` interfaces. `ext::game::Current()` returns null pointers for every game except a matched FFXIV build, so a seam that needs the game costs one pointer test, never a per-frame `gameExe` string compare.

## 3. Mechanics

### 3.1 Seam declaration

- **Declaration:** `ext/Seams.h` declares every seam as a free function in `namespace ext::<group>`.
- **Hot-path seams:** per draw, per resource or per command list. These are `inline` wrappers that test a feature bit (`ext::Active(ext::F::Nr)`, a relaxed atomic load) and call out of line only when a feature needs them. Cost when off: one load and one branch.
- **Decision seams** return the value the upstream flow branches on: `bool` = "handled, skip upstream", or an adjusted value. They never throw. On any internal failure they return the upstream-default answer.
- **No containers on hot paths:** no `std::function`, registries or heap allocation in per-frame or per-resource seams.
- **Free functions over hooks in the class:** seams are free functions taking the upstream object (`ext::fg::AdjustDlssgOptions(this, opts, reflex)`). This avoids adding virtuals to upstream classes.

### 3.2 State the fork attaches to upstream objects

When fork state must live per upstream object (e.g. per `IFGFeature_Dx12`), the class gets **one** member: `std::unique_ptr<ext::fg::Dx12Ext> _ext;`, marked `// ext:`. All fork state hangs off it. Never add individual fields.

### 3.3 Access to upstream internals

- Use one `friend struct ext::Access;` line per upstream class.
- `ext::Access` (in `Seams.h`/`Seams.cpp`) is the only fork code that names private upstream members. It exposes narrow static accessors.
- When upstream renames a member, `ext::Access` is the single place that breaks.

### 3.4 Config: `ExtConfig`

- **Ownership:** each feature owns a config struct in its own directory (`ext/nr/NrConfig.h`, `ext/fg/FgConfig.h`, …).
  - Fields use upstream's `CustomOptional<T, Default>`, so the semantics match (`value_or_default`, `has_value`).
  - A declarative binding table maps each field to its `(section, key)`.
- **Access:** `ext::Cfg()` aggregates the structs: `ext::Cfg().nr.passes`, `ext::Cfg().hdr.ffxivHdr`. The port renames `Config::Instance()->DlssNrX` call sites to `ext::Cfg().nr.x` (~460 sites). The rename is mechanical, and afterwards no fork member is left in `Config.h`.
- **Ini compatibility:** section and key names stay **identical** to today's, including fork keys inside upstream sections (`[XeFG]`, `[DLSSG]`, `[HDR]`, `[Hotfix]`, `[QualityOverrides]`, `[LowLatency]`, `[Vulkan]`). Existing user ini files keep working unchanged.
- **Seams:**
  - `ext::config::Load(ini)` at the end of `Config::Reload`.
  - `ext::config::Save(ini)` in `Config::SaveIni`.
  - `ext::config::SaveXeFG(ini)` in `Config::SaveXeFG`, so fork XeFG keys persist too.
- **Ini template:** `ext/ini/*.ini` fragments are inserted into the matching sections of the upstream `OptiScaler.ini` by `tools/package.ps1`. The shipped file has one header per section, and the upstream template is never edited in git.
- **Dropped keys:** drop-listed keys (E1 `ProxyProbe`/`UseProxy`/`ScanAnchor*`, E2 `ProbeD3D11`, E6 `ExternalFrameGeneration`, dead `FGDLSSGAdaBlackwellKernels`) are removed together with their code branches.
  - Migration-only keys (`WhitePointFromExposure`, legacy anchors) are dropped.
  - `AutoCapture` becomes a real key (default false = the current behaviour: the oracle never ran the capture; when true, one capture after 180 frames).

### 3.5 Commit classes

- **`[UP]` upstream-candidate fix:**
  - It is a generic bug fix inside upstream code that is better kept inline than wrapped.
  - Each one is a separate, self-contained commit at the bottom of the stack (series S1), written as if it were an upstream PR.
  - When upstream merges an equivalent, `git rebase` drops the commit as empty.
- **`[<ID>]` feature commit:** a feature series (`[A4] nr: exposure scan via restrack seam`). Every series builds.
- **`[EXT]` infrastructure:** scaffolding (`Seams.h`, `ExtConfig`, `MenuRegistry`, `ext.props`, tools).

### 3.6 Menu: `MenuRegistry`

- **Merge point:** upstream `MenuCommon::GetMenuBoxes` builds its static array once. The single seam `ext::menu::MergeBoxes(base, count)` returns a merged array.
- **Fork cards:** each fork card registers `{id, tab, name, draw, after = "<upstream id>", visible(ctx)}` from its own feature directory. `after` places it relative to upstream cards; `visible` is a predicate such as FFXIV-only, DLSS-G-active or HDR-active.
- **Draw callbacks:** they receive `ext::menu::Ctx`, a narrow view built from `RenderMenuContext` through `ext::Access`, and draw through upstream's `ScopedCard`/`RenderCard`. This makes filtering and pinning work for every fork card, which fixes the reference build's NR/Companion bypass.
- **Inline extras:** extras that belong inside an upstream card (XeFG extras, FG output options) use one in-card seam each: `ext::menu::XeFgExtras(ctx)`, `ext::menu::AdjustFgOutputOptions(...)`.
- **Widgets:** fork widgets (tooltip, section title, keybind row) live in `ext/core/MenuWidgets`. Upstream private helpers are not exposed.
- Cosmetic text tweaks to upstream menus are **not carried**.

### 3.7 Build: `ext.props`

- **Import:** `OptiScaler.vcxproj` gets one `<Import Project="ext\ext.props" />`.
- **What `ext.props` does:**
  - Wildcard-compiles `ext/**/*.cpp`.
  - Sets per-file `PrecompiledHeader=NotUsing` where needed (`AmdPreSr.cpp`, `DlssNrNative.cpp`).
  - Adds an `AfterBuild` target that copies the fork's readme/attribution text files.
  - Adds a shader-generation target that compiles HLSL to headers before `ClCompile`. The recipes come from the U6 ledger. Generated headers go to `$(IntDir)ext_gen\` and are never committed.
- **Forwarder:** `nvngx.dll_dlssnr.dll` is the second project in `OptiScaler.sln`. It is the only accepted `.sln` edit.
- **Submodule:** `external/RTX40MFG-Unlock` is not added. The fork's MFG unlock does not build from it.

### 3.8 Game modules

`ext/game/` defines small, separate interfaces. A future game implements only what it needs:

| Interface | Methods (initial) | Used by |
|---|---|---|
| `IRenderGeometry` | `ResolveRenderExtent(unsigned int paramW, unsigned int paramH, unsigned int& w, unsigned int& h)` (S7 C2/C3, corrected from the initial `Extent&` placeholder), `ColorIsDisplayAllocation(unsigned int renderWidth, unsigned int renderHeight, uint64_t allocWidth, unsigned int allocHeight, unsigned int subrectWidth, unsigned int subrectHeight)` (S7 C4, corrected from the initial zero-arg placeholder) | C2, C3, C4 |
| `INativeQuality` | `EnsureHooks` (lazy, `call_once`), `Live`, `DlaaRequired` (per-frame cached), `Request(int preset)` | C1, C5 |
| `ISceneLighting` | `OnUpscaleBoundary`, `ConsumeHistoryReset` | B6, A3 |
| `ISceneHdr` | `Open`, `PublishReconstructed`, `EndFrame` | C10 |
| `IFgMarkers` | `PresentationGuideDelay`, `OnUpscale`, `BeforePresent`, `NextFrame` | B5, D2, C5 |
| `INgxLifecycle` | `IsolatesShutdown` | C6 |

`ext::game::Current()` returns a struct of these pointers, selected once at init.

**`ext/ffxiv/GameBuild.h` is the only file with FFXIV addresses:**
- Every RVA, struct offset and byte guard, keyed by PE timestamp + image size.
- This includes the offsets now hard-coded inside `ProcessFfxivQualityRequest`.
- A game patch means editing this one file, per the REA runbook.

### 3.9 Hook bypass scope

`ext::core::HooksBypassed()` is an inline `thread_local` test. It replaces the six `FfxivNameplateLiveScope::active` guards. Any fork code that issues its own native draws or device calls wraps them in `ext::core::BypassScope`. It is not FFXIV-specific.

### 3.10 Logging and version

`ext::core::Banner()` is called once after upstream's version line. It logs `OptiScaler-FFXIV <tag> (<fork sha>) on upstream <upstream sha>`. The upstream version line is not edited.

## 4. Seam catalog

The catalog is in [SEAMS-catalog.md](SEAMS-catalog.md), one table per group: `config`, `menu`, `upscaler`, `dx11bridge`, `nr`, `fg`, `present`, `restrack`, `streamline`, `gpu`, `hdr`, `hudless`, `game`, `core`.
- **What it records:** for each seam, its form (hook/decision), host file(s), owner IDs and frequency.
- **Implementation:** porting agents implement the catalog and may refine signatures. A seam that is added, removed or moved must update the catalog in the same commit.
- **What the catalog is for:** it is the conflict surface. On every nightly sync, review the catalog's host files and nothing else.

## 5. Decisions on the reference build's oddities and open questions

| Topic | Decision |
|---|---|
| `ResTrack_dx12` `Reset` hooked twice (nightly `hkReset` + fork `o_LateReset`), wrong detach order, leaked `ExecuteCommandLists` detour | **One** `hkReset` with an `ext::restrack::OnCommandListReset` callback. Installed when `persistentBindings` or `ext::restrack::WantsCommandListReset()` is true, through one idempotent `EnsureResetHook(device)` for late devices (replacing `HookLateNrQueue`). Detached once, in reverse order of attach. The fork's `ExecuteCommandLists` hook is owned by `ext/gpu` and always detached |
| `isHdrActive` (fork) vs `hdrOutputActive`/`outputColorSpace` (nightly) | **Nightly's fields are the single truth.** The D3D11 bridge fills them through `ext::dx11bridge::SetColorSpace`. `Hdr10::Active()` reads them. `isHdrActive` and its logs are deleted |
| NR/Companion menu bypassing `RenderCard` | Fixed by `MenuRegistry` (section 3.6) |
| Nightly's Present removed the DXVK early return | D6 is covered by nightly (`State::frameCount` is the epoch). Not carried |
| `IFeature_Dx11wDx12::Evaluate` raw-pointer state backup | Take nightly's ComPtr version (the fork's raw version predates it) |
| `DLSSG_Common` has no extension points | Free-function seams called from `DLSSG_Common.cpp`. No virtuals are added to the header |
| 64-bit low-latency `sleep` frame ids | **Dropped**: no 64-bit producer exists. The 16–18 tech files stay untouched |
| `ConcurrentInputSelection` (D5) | `[UP]` commit. The globals become a synchronized proxy type at their declaration, so the ~44 call sites in `input_common.cpp` stay unchanged. The tech mutex uses a lock-free read first and locks only when switching |
| Generic bug fixes found in the ledgers (A11 output-scaling/UAV, recorded-Init, per-API NGX init flags, `WaitForUIAllocator`, XeFG barrier, HUD-less format loop, HC/RUI null guards, overlay fence ring, anisotropy `ModifyComparison`, `FrameLimit` timer, Vulkan spoofing DXVK re-entry, D5 Reflex/Streamline hardening, `PFN_D3D11_Shutdown1` typedef) | Each is an `[UP]` commit in S1, unless the U6 check shows nightly already has it |
| A4 exposure-scan hook shape | Keep the resource-creation seam (2 tail-wrapper lines in `D3D12_Hooks.cpp`) to preserve behaviour. The seam checks `riid` before treating the object as a resource |
| E5 swapchain sizing | Upstream already fixed the 0x0 overlay misclassification. **Drop the 0x0→window resolve; keep only the `CreateSwapChainForComposition` hook** (`ext::present::HookFactoryComposition`, `ResolveCompositionHandle`) |
| `hdrPresenter` lost `activeFgInput != NvngxFG` | Restore upstream's term unless the originating fork commit justifies the removal (S6 checks `git log -S`) |
| Nameplate live-scope guards | `ext::core::HooksBypassed()` (section 3.9) |
| Forwarder name `nvngx.dll_dlssnr.dll` | **Kept (S2 check done).** The name is not our convention: NVIDIA's `nvngx_dlssnr.dll` resolves the module owning its caller's return address and returns `FAIL_PlatformError` unless that path contains `nvngx.dll`. Any other name makes the model refuse every call, so the substring is part of an external contract. Consequently the `Kernel_Hooks` `!path.contains("nvngx.dll_")` carve-out stays (S2b, `// ext: A1`, otherwise the nvngx spoof redirects the forwarder to the driver `nvngx.dll`; this crashed Dragon's Dogma 2) and `tools/package.ps1`'s default stays |
| Dynamic markers thread-local bridge flag | Replace with a raw-send function on the fork's Reflex path. No per-marker check on the upstream hook |
| `ext::present::BeforeOverlay` | A general pre-overlay slot, used by HudlessEngine paste and HDR |
| C3 `RenderSizeFromWidthHeight` | Restore Siren's gate from `adfbea9d` (both halves, default off). Its readers were lost in Siren's merge `c131de52`. Default-off behaviour must equal the oracle |
| `GameQuirk::ScaleDisplayColorForForcedQuality` | Appended at the end of the enum (no shift of upstream values). The FFXIV `GameConfigs.cpp` entry is a 1-line upstream extension point |
| HDR menu in `imgui_impl_dx12` (vendored) | Prefer zero edits: composite the menu into an SDR target, then convert. Visual equivalence is checked in-game in Phase 4. Fall back to ≤ 3 seams only if equivalence fails |
| `OS_Dx12` vs `OS_Vk` resource-sized dispatch | Both gate on the NR scaler override; upstream behaviour is unchanged for Output Scaling and Magnifier |
| XeFG max interpolated frames | Code is authoritative (1..7). The ini text is fixed |
| `delayMenuRenderBy`, `_device2`, stray declarations, BOM/whitespace hunks, log-only tweaks, the upstream README banner, `setup_windows.bat` edits | Dropped |
| C11 engine | Moves to `ext/hudless` with a `HudlessSource` contract (`Active()`, `Take() -> ref-counted texture, newest wins`). The engine owns the shared-handle ring. The add-on, `.fx` marker and `StatusV1` are dropped. Rejection strings lose the add-on wording. Recomposition depends on `HudlessEngine::SourceActive()` |

## 6. Build, shader and repo decisions (from the U6 ledger)

| Topic | Decision |
|---|---|
| Shader compilers | Use the fxc/dxc that upstream already tracks in `shader_tools/`, so builds need no SDK. Recipes are in U6 §2: `cs_5_0 /E CSMain /O3`; RenoDX `ps_5_0 /E main`; `SPATIAL_GUIDES=1` variant; SPIR-V via dxc `-spirv -T cs_6_0 -Qstrip_debug -Cc -Vi -D VK_MODE`. Generated output must be byte-identical to the oracle's checked-in blobs (checked once in S2) |
| `dlssnr.hlsl` including `../../hdr/HdrCompatibility.hlsli` | Shared HLSL includes live in `ext/shaders/common/`, so `nr` and `hdr` both include them without crossing feature dirs |
| `VK_MODE` branches in `dlssnr_spatial.hlsl` | Source kept unchanged; no SPIR-V is generated for it (there's no Vulkan consumer) |
| Runtime-compiled HLSL strings (AMD PreSR, UiPaste, hudless normalize, UE_Common, HdrMenu, HdrMenuLayer) | Moved to `.hlsl` files compiled at build like the others. Where runtime defines vary, the `.hlsl` is embedded as a text resource and still compiled at runtime. There's one shader source of truth either way |
| Forwarder | Keep the vcxproj (v143, `/MT`) as the second solution project. Drop CMake. Its rename follows section 5 |
| Docs | The 59 notes are condensed into `docs/` (design, MFG unlock/pacing, AMD HIP, HDR, companion, testing). zh-CN READMEs are dropped (unmaintainable). Third-party licences are kept next to their code (RenoDX NOTICE, `external/peripheral_warp`) |
| Tests | Keep the 17 CPU-only and 23 WARP tests under `tests/ext/`, built and run by `tools/test.ps1` (one runner). The 10 text-slicing tests are rewritten against the real headers when that's cheap, otherwise dropped. The 4 hardware-dependent tests are kept as manual tests and documented |
| Dead files | `menu_hdr_colors.h`, `DLSSFeature_VkOn12.*`, `DlssNr_Proxy.*` (E1), `optional/reshade-hudless/*`, PR review notes and `tests/compile_scene_hdr.py` are not carried |
| A11 | Held-key input and GPU-probe recursion are already upstream; not carried. The output-scaling dispatch is carried as `[UP]`, gated on the NR scaler override in both Dx12 and Vk. The NGX init flag is carried only for DLSSD (nightly's DLSS-SR flag is already per instance) |
| ImGui | The overlay fence ring mirrors ImGui's DX12 backend frame index. `[UP]` commit with a `static_assert` on the vendored ImGui version |

## 7. Decisions on the catalog's open conflicts (2026-10-15)

| # | Topic | Decision |
|---|---|---|
| A8 | NR-owned resampler with resource-sized dispatch; upstream `OS_*` untouched | **NR owns its resampler.** `ext/nr/NrResampler{Dx12,Vk}` is derived from upstream's OS classes, with NR's override and resource sizing. Upstream `OS_*` stay untouched (0 lines), and the A11 output-scaling `[UP]` is not carried: upstream callers keep nightly's behaviour |
| 1 | Recorded-Init `[UP]` vs C6/C10 needs | The `[UP]` commit uses raw calls. C6 and C10 later add one RAII line each (`ext::gpu::Scope`, `ext::hdr::BridgeInitScope`) |
| 2 | Bridge fence/allocator hardening | `[UP]` |
| 3 | Menu tone-map under OptiHDR | Resolved in C8: `UseSdrUiTarget()` makes `getUiTargetMode()` return SDR while the separate menu layer composites into the HDR output. Phase 4 verifies visual parity. |
| 4 | XeFG camera build | Seam: it keeps `BuildView` validation, so behaviour matches the oracle |
| 5 | Detours lock | Keep only the `[UP]` file-local Streamline mutex. No shared `DetourLock` |
| 6 | Forwarder rename | Not renamed: the model's caller check needs `nvngx.dll` in the module path (section 5). The `Kernel_Hooks` carve-out is carried |
| 7 | `ReleaseBackbufferOnce` | Upstream quirk mechanism (`GameConfigs`), appended enum value |
| 8 | `FrameLimit` interval / timer | `[UP]` |
| 9 | `menuOverlayIsVulkan` / Vulkan DLSS-G menu interlock | S1 checks nightly. Dropped if covered, otherwise `[UP]` |
| 10 | Generic menu behaviours | Preset live-apply is kept only if it belongs to fork presets (A12), via the A12 card. OptiFG state text and SaveIni result text are dropped (cosmetic) |
| 11 | Footprint target | Restated in section 1.3 |
| 12 | `OS_Dx12` sizing policy | Moot (see A8) |
| 13–15 | `timingQueue` signature, XeFG interpolation write-back, `OnUpscaleStart` cast | The owning series follows the oracle's behaviour. Write-back changes need a one-line justification in PORT-LOG |
| 16 | C3 gate | S7, per section 5 |
