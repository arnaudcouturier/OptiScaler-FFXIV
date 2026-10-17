# HUD-less engine (S10): design

Status: **draft for the user's approval (W6-0), 2026-10-16.** Nothing here is implemented yet except the two contract headers (`OptiScaler/ext/hudless/HudlessSource.h`, `HudlessGpu.h`) and the `Game::hudless` pointer. Plan, decisions and waves: workspace `S10-PLAN.md`. Seam IDs P0-P8 come from `phase3-ledger/U5-dx11bridge-shaders-menu.md`.

Statements marked **(inferred)** come from reading code and have not been run; the diagnostic build D1 (section 12) exists to turn them into facts.

## 1. Goal and decisions

With DLSS-G at 2-4x the HUD stays sharp (no smear/ghost) in fast camera motion: frame generation interpolates the **UI-free image** and the UI is **pasted back at the real frame rate**. Nothing else about the picture changes.

| Decision (user, 2026-10-16) | Consequence in this design |
|---|---|
| The oracle's REST HUD never ran (the ReShade add-on was never built) | Nothing to stay compatible with; no old keys carried |
| Clean redesign; oracle code and its WARP tests are a reference only | New modules; the oracle's mask maths and tests become test data (section 13) |
| Provider parity with the oracle | Full engine (UI-free FG + paste) on DLSS-G only; FSR-FG / XeFG get the hud-less / UI image tags (section 9) |
| Difference-based UI mask | Threshold 0.008, dilation 1, three-level alpha cleanup (section 7) |
| `[HudLess] Mode = off | recomposition | ui_free`, default off | Section 11 |
| Fail open | Any doubt means today's behaviour (section 5) |
| HDR via SDR capture + OptiHDR conversion; scene HDR keeps priority on frames it imports | Section 8 |
| ReShade integrated as a built-in add-on (decided 2026-10-16, replaces option A) | Section 16: presets are applied in place at the UI boundary, the HUD stays unfiltered; fail open if ReShade is absent or the integration fails |
| Dalamud overlays count as UI if the hook order allows | Measured by D1 |
| Base branch `w5` | Footprint baseline 746 |

## 2. Frame flow

Today (bridge, `Dx11wDx12SC::Present`, `with_dx12/dx11_with_dx12_sc.cpp` ~361-441, read 2026-10-16):

```
game draws scene -> game draws UI -> game calls Present (bridge wrapper)
  _RequestSharedBackBuffer -> _CopyDx11BackBufferToShared (backbuffer WITH UI) -> _WaitDx11ThenDx12
  -> _CopyDx11SharedToDx12FGBackBuffer -> _WaitForInteropCopyOnPresentQueue
  -> hidden real D3D11 Present (no wait) -> FG swapchain Present (DLSS-G dispatches; generated frames follow)
```

With S10 (`ui_free`):

```
game draws scene -> [boundary: first UI draw] source snapshots the bound scene RT (UI-free)
  -> game draws UI -> Present:
  PreInteropCopy      engine copies the snapshot into the slot's shared hud-less texture (same D3D11 context, before the signal)
  existing copies     backbuffer WITH UI -> shared -> D3D12 (unchanged)
  UiFreeFrame         decision: use the hud-less copy as the FG base? (all guards of section 5 pass)
  PostInteropCopy     on the FG queue: tag HudlessColor (+ UIColor in recomposition), build the paste image
  FG swapchain Present, then for EVERY output present (real and generated):
  BeforeOverlay       UiPaste::Paste draws the newest completed paste image over the backbuffer
```

`recomposition` mode skips the base swap and the paste: DLSS-G receives HudlessColor + UIColor and recomposes the UI itself (it still interpolates the UI layer; documented limitation of that mode).

## 3. Components and ownership

```
OptiScaler/ext/hudless/                 generic engine, nothing FFXIV-specific
  HudlessSource.h                       contract 4.1 (committed in W6-0)
  HudlessGpu.h                          contract 4.2 (committed in W6-0)
  HudlessState.{h,cpp}                  the shared Wanted() flag (committed in W6-0; B writes it, A and the hook sites read it)
  HudlessEngine.{h,cpp}                 B: state machine, ring, fences, decisions, summary log, seam entry points
  HudlessConfig.{h,cpp}                 B: [HudLess] binder
  HudlessMenu.cpp                       B: MenuRegistry card
  adapters/DlssgAdapter.cpp             B: tags, recomposition, base swap
  adapters/FsrFgAdapter.cpp, XeFgAdapter.cpp   B: tags only
  UiImage.cpp, UiPaste.cpp              C: GPU work behind HudlessGpu.h
  shaders/*.hlsl, hudless.shaders.props C: mask / paste shaders (props picked up automatically by ext.props)
OptiScaler/ext/ffxiv/ui/                A: FFXIV boundary detector (+ D1 diagnostics)
  FfxivUiBoundary.{h,cpp}               implements IHudlessSource
  UiShaderCatalog.h                     UI pixel-shader CRC set
  UiDiag.{h,cpp}                        D1 logger / probes / snapshots, diag menu card
OptiScaler/ext/ini/HudLess.ini          B: ini template fragment
tests/ext/hudless/, tests/ext/ffxiv/ui/ B / C / A
```

Existing fork-owned files that gain a few lines: `ext/ffxiv/lighting/FfxivLightingCapture.h` (A: hook consumer, section 10), `ext/game/Game.cpp` (A: `ffxiv.hudless = &ui::Provider()`), `ext/Seams.h` / `ext/Seams.cpp` (B). Upstream files: bridge seam calls, `wrapped_swapchain.cpp` (BeforeOverlay), `framegen/IFGFeature.cpp` (index fix, section 9.4), and the removal of `ext/fg/HudlessLink.*` (replaced by the engine's state, section 9.1).

## 4. Contracts

### 4.1 `IHudlessSource` (game side; header committed)

`Active()` (cheap atomic), `Take(FrameImage&)` returning `NoSource | NotThisFrame | Rejected | Ok` (newest wins, never blocks), `LastReason()`, `EndFrame()`. `FrameImage` = D3D11 texture + typed format + size + sequence. The source snapshots the scene RT **at the boundary** into its own texture (one reused texture); the engine copies it into the shared slot at Present. This is one extra 4K RGBA8 copy per frame (about 33 MB read + write; negligible next to the frame's other traffic). A single-copy variant (source writes straight into the shared slot) is possible later if D1's cost numbers say so.

### 4.2 GPU API (header committed)

`UiPaste::{Produce, Invalidate, Ready, Release, Paste, SetDebugTint, SetSteadyTiming, Snapshot}` and `UiImage::{Build, Release}` with `MaskParams` / `PasteStatus`. Semantics follow the oracle's `UiPaste` (section 7) with two additions: `PasteStatus::coverage` (share of pixels flagged, from a one-counter readback that is read two frames later without stalling) and an explicit `hdrFinalImage`.

### 4.3 Engine entry points (B owns the exact signatures; they are registered in `SEAMS-catalog.md`)

| Seam | Site | Does |
|---|---|---|
| `ext::hudless::PreInteropCopy` (P0/P1) | bridge `Present`, before `_WaitDx11ThenDx12` | `Take()` from the source, copy into the slot's shared texture on the D3D11 immediate context |
| `ext::hudless::UiFreeFrame` (P2) | before `_CopyDx11SharedToDx12FGBackBuffer` | returns the D3D12 resource to use as FG base (hud-less) or null (normal path) |
| `ext::hudless::PostInteropCopy` (P3/P4) | after `_WaitForInteropCopyOnPresentQueue` | tags, builds UI image / paste image on the FG queue, advances the ring |
| `ext::hudless::OnReleaseInterop` (P5) | the bridge's release / resize path | drops slots and paste images |
| `ext::present::BeforeOverlay` (P6) | `LocalPresent`, before `MenuOverlayDx::Present` (`wrapped_swapchain.cpp` ~484) | `UiPaste::Paste` when mode is `ui_free` and a paste image is ready |
| `ext::hudless::Wanted()` / `SetWanted()` | hook sites; defined in `HudlessState.cpp` (committed in W6-0) | atomic: `Mode != off` and the game source exists (zero-cost early-out). B calls `SetWanted` when the config loads or the menu changes Mode; A and the hook sites only read it |
| DLSS-G option seam | `ext/fg/dlssg/DlssgSeams.cpp` `AdjustUiRecomposition` | reads engine state instead of the `HudlessLink` stub |

Persistent footprint budget for all upstream-file lines of S10: **<= 25** (baseline 746).

## 5. Engine state machine and fail-open

States (shown in the menu): `Off` (mode off, FG inactive or no source), `Searching` (armed, no capture yet), `Active` (>= 90 % of the last 60 presents delivered a usable capture), `Fallback(reason)`.

The decision is **per frame**, with hysteresis only for the displayed state:

```
useHudless = mode == ui_free
          && FG active && provider is DLSS-G
          && Take() == Ok
          && size / format match the FG backbuffer, not MSAA, not an array
          && slot free (reader fence completed)          // never block the game thread
          && paste ready (UiPaste::Ready)
          && !frameOwnedBySceneHdr                        // section 8
          && coverageGuardOk
otherwise: today's path, exactly (full-frame FG), reason recorded
```

Fallback reasons (enum + text, all counted): `ModeOff`, `NoFrameGeneration`, `ProviderUnsupported`, `NoSource`, `NoBoundaryThisFrame`, `CaptureRejected`, `SizeMismatch`, `FormatUnsupported`, `MsaaOrArray`, `SlotBusy`, `PasteNotReady`, `SceneHdrOwnsFrame`, `MaskCoverage`, `DebugTintOn`.

**Coverage guard:** the mask pass counts flagged pixels; if coverage exceeds `CoverageGuard` (initial 0.60) for 5 consecutive real frames the engine falls back with `MaskCoverage` and retries after 120 frames. Reason: black/white fades and, if ReShade presets turn out to run before the capture, preset-changed pixels would make the mask cover most of the screen. The value is a starting guess that D1/D2 calibrate. If nothing completes a mask readback within 8 frames the guard reads "unknown" and does not block.

**Never** produce a black, partial or flickering frame: every failure path leaves the normal backbuffer copy as the FG base and calls `UiPaste::Invalidate`.

### Periodic summary log (judging sessions from the log)

Every 600 real presents, same style as the scene-HDR line:

```
HudLess: mode ui_free, state Active; 600 presents: 598 captured, 2 fallbacks (SlotBusy 1, NoBoundaryThisFrame 1); mask coverage avg 4.1% max 9.7%; paste built 598 pasted 1190 skipped 6; slot waits 0; boundary: ok
```

Plus one line at every state change (`HudLess: state Searching -> Active`, `... -> Fallback(MaskCoverage)`).

## 6. Interop and synchronisation

- **Ring:** one slot per FG buffer (`BUFFER_COUNT`, keyed like the bridge's fake index). A slot = a shared-NT-handle D3D11 texture (typed, RGBA8 or the capture's format), the opened D3D12 resource, and the D3D12 value of the last queue operation that reads it.
- **Rule:** the D3D11 side may write slot i at Present N only if the D3D12 reader fence has **completed** that slot's last read (non-blocking `GetCompletedValue`). Otherwise this frame has no hud-less (`SlotBusy`). The oracle reused slots guarded only by the copy-allocator fence while FG-queue readers (the DLSS-G tag and the paste producer) were not fenced **(inferred)**; this design fences them.
- **Ordering across APIs:** the hud-less copy is recorded on the D3D11 context before the existing D3D11 -> D3D12 signal, so the single existing `_WaitDx11ThenDx12` covers both copies (the oracle did the same).
- **Paste images:** 4 slots; the producer reuses a slot only after its write and its last read completed and never waits on the GPU; the consumer picks the newest COMPLETED image, at most 250 ms old, never older than the last one pasted.
- **Lifetime:** nothing is destroyed while the GPU may still read it; teardown retires resources behind the GPU fence (existing `GpuLifetime`).

## 7. UI mask and paste algorithms (reference: oracle `UE_Common.h`, `UiPaste.cpp`; values are the oracle's tuned defaults)

Per pixel `p` of the real frame, with `F` = final image, `H` = UI-free image:

- difference `d(p) = max(|F.r-H.r|, |F.g-H.g|, |F.b-H.b|)`;
- `isUi(p)` = any pixel within the square window of radius `dilation` (clamped to the image) has `d > threshold`;
- paste image: `isUi` -> `(F.rgb, 1.0)`; else if cleanup is on and the previous paste image had alpha > 0.75 here -> `(F.rgb, 0.5)` (clears ghosts of UI that just closed); else `(0,0,0,0)`.
- Paste draw: full-screen triangle over the backbuffer, `discard` when alpha <= 0, output alpha 1, **no blend state** (pasted pixels overwrite).
- Recomposition mode: `UiImage` writes `(F.rgb, 1)` or `(0,0,0,0)`, alpha strictly 0 or 1 (valid for premultiplied and straight consumers), R8G8B8A8_UNORM.
- Known limit: full-screen black/white fades differ from the UI-free frame everywhere and read as UI (coverage guard falls back; harmless). Nameplates can look displaced at low FPS because the held UI and the generated scene represent different times (oracle doc, unverified here).

## 8. HDR

The user's HDR setup is OptiHDR with scene HDR (`[HDR] Mode = 1`, `FfxivHDR`, NR/DLSS scene input). Design (follows the oracle doc, which describes SDR/HDR gameplay tests of its own hud-less path; not re-verified here):

1. The UI-free capture and the mask are **SDR** (the game's native SDR chain).
2. The FG base in HDR is the hud-less image converted by the production OptiHDR converter (`Hdr10::Convert`, same call the bridge already makes on its copy source).
3. The paste needs the converted **final** image too (one extra convert per real frame in `ui_free` + HDR). Paste storage in HDR: `R10G10B10A2` PQ with the three-level alpha.
4. **Scene HDR priority:** on frames where `ISceneHdr::Open(presentation=true)` supplies a scene, the existing mode-1 HudlessColor re-tag (`BridgeHdr.cpp` ~94-117) keeps ownership: the engine does not swap the base on that frame (`SceneHdrOwnsFrame`) and invalidates the paste. These frames are rare in the user's last HDR session (the DLSS scene-HDR path alternated native-HDR and SDR-fallback periods; the Present-path counter showed 2 imports in 1,800 requests). Revisit with Session 2 data.
5. Not supported: native HDR10 game output with UI-free (the oracle rejected it: "Off while FFXIV HDR10 output is on ..."). The engine falls back with `FormatUnsupported`.

## 9. Providers

### 9.1 DLSS-G (full engine)

- Tags: `HudlessColor` -> `kBufferTypeHUDLessColor`, `UIColor` -> `kBufferTypeUIColorAndAlpha` (`DLSSG_Dx12.cpp` ~924-965), lifecycle `eOnlyValidNow` unless UntilPresent. `SetResource` gates (`FGDisableHudless`, `FGDisableUI`, ...) stay.
- Recomposition is a Streamline option read at feature **creation**: `AdjustUiRecomposition` (`ext/fg/dlssg/DlssgSeams.cpp`) now reads the engine (`Mode == recomposition`, last hud-less format) instead of the `HudlessLink` stub, which is deleted. Changing mode afterwards applies after a game restart (logged, as today).
- `ui_free`: base swap + paste as in section 2. The oracle's menu showed a recomposition checkbox that contradicted `ui_free`; here one mode combo replaces both (section 11).
- Config note: `[DLSSG] UIRecomposition` stays; it only matters in `recomposition` mode.

### 9.2 FSR-FG and XeFG (parity: tags only, untested here)

HudlessColor and UIColor are tagged exactly as the oracle did (FSR-FG: HUDLessColor into the FFX config, UI resource never registered; XeFG: `XEFG_SWAPCHAIN_RES_HUDLESS_COLOR` / `_UI`, UI only if the SDK exports `xefgSwapChainSetUiCompositionState` or Draw-UI-over-FG is on; XeFG HDR rejects UIColor). No UI-free, no paste for them. Labelled untested/community.

### 9.3 Tag slot

The tag slot must be the slot the provider's own Dispatch will use (next subsection).

### 9.4 Dispatch index (a pre-existing mismatch S10 must not trip over)

`IFGFeature::GetIndexWillBeDispatched()` (predictor, stateless) and `GetDispatchIndex()` (consumer, writes `_lastDispatchedFrame`, returns -1 for an already-dispatched frame) disagree in the resync branch **(read 2026-10-16)**: the consumer tests `HasResource(Depth|Velocity|UIColor|HudlessColor, GetIndex())`, the predictor only `HasResource(Depth)`. With a frame that carries only hud-less/UI resources the tag can land one frame off.

- The bridge must **not** call `GetDispatchIndex` (it would consume the frame and make the provider skip it).
- Fix: make the predictor mirror the consumer's resync predicate exactly (same four resource types, same index), still stateless. This is a small upstream-file edit (~6 lines) and an upstream-candidate fix; B implements it with a unit test (`framegen` has none today, so the test slices the two functions the way `nr_reuse_api` slices production code) and records it as `[UP]` if it stays generic.
- If B finds the mirror cannot be made exact, the fallback design is tagging at a dispatch-time seam right after the provider's own `GetDispatchIndex` call; B reports before choosing.

## 10. FFXIV boundary detector (`ext/ffxiv/ui/`, work package A)

**Hook consumer, no new Detours.** `FfxivLightingCapture.h` already hooks `CreatePixelShader`, `CreateInputLayout`, all draw entry points and `CopyResource` on the immediate context (`Around()` is the central draw wrapper, `inside` guards re-entry). A adds one early line at the top of `Around()` guarded by `ext::hudless::Wanted()` (an atomic) and one call in `CreateShader` that forwards the bytecode to `ui::TagShader`. Only the immediate context is hooked **(read)**; whether the game ever uses deferred contexts is measured by D1.

**Stage information in SDR.** The scene-HDR stage tagger (`FfxivSceneHdr::Tag`, CRC -> stage 1..5, `FfxivHdrPreview::Tag` -> stage 6) runs at shader creation unconditionally, so the tags exist in SDR; but `FfxivSceneHdr::AfterDraw` (which tracks them) runs only when HDR is requested (`Active() && hdrOutputActive`). A therefore reads the stage tag straight from the pixel shader's private data per draw (one `PSGetShader` + `GetPrivateData`, only when `Wanted()`), and keeps its own per-frame tracking: draw ordinal, last scene/post stage seen (CRCs: 0x85E777EF tonemap=1, 0xF8F57F0A post-tonemap pre-LUT=2, 0x27EBC404 LUT=3, 0x1F264D17 LUT=4, 0xF6E81A1B fullscreen gamma=5), UI-tagged draws.

**UI shader set (starting point).** The seven CRCs of the HDR "UI only" stage (3865947726, 2966694105, 3759127293, 1898885941, 4076256744, 3915247300, 1823062889); the Companion's four are a subset. They are transmittance-related and may not cover every UI draw, and a shader created before the hook installs is never tagged; D1 measures both.

**Boundary rule candidates (D1 decides, A implements the winner in v1):**
- **R1 (tag-based):** first draw whose pixel shader carries the UI tag, after the last scene/post stage of the frame.
- **R2 (position-based, hash-independent):** first draw after the last stage-5 (fullscreen gamma) draw whose render target is the backbuffer.
- **R3:** R1 or R2, whichever fires first (union), if neither alone is reliable.
The capture: before the boundary draw executes, `OMGetRenderTargets` -> RTV 0 resource; require single sample, non-array, same size as the swapchain backbuffer; copy into the reused snapshot texture. D1 logs identity/size/format of that RT against the backbuffer (the engine ring assumes the capture matches the bridge copy source).

**Arming and frame hooks (ownership, so A and B never edit the same lines).** A's `Armed()` is `[HudLess] Diagnostics` (A's own tiny binder `ext/ffxiv/ui/UiConfig`, same ini section as B's keys) **or** `ext::hudless::Wanted()`; `IHudlessSource::Active()` returns `Armed()`. D1 (A alone on top of `s10`) therefore arms from `Diagnostics = true` without any B code. A owns the bridge's `ext::dx11bridge::PresentScope` in `ext/hdr/BridgeHdr.cpp` (constructor runs at bridge `Present` entry, destructor at every exit): the constructor runs the ReShade/Dalamud probe and the per-frame bookkeeping, the destructor calls `ext::game::Current().hudless->EndFrame()` when the pointer is non-null. **B must not call `EndFrame()` and must not edit `PresentScope`.**

**Exclusions:** the Companion nameplate layer is a separate DirectComposition swapchain; it is not in the game's frame and not captured. Draws issued under `ext::core::BypassScope` (`HooksBypassed()`) are ignored. The Companion's `isolateReShade` stays false (as in the Phase 2 build; there is no ReShade marker any more).

**Cost:** with `Mode = off` the added code is one atomic load per draw. With the feature on, the per-draw cost is a pointer test plus `GetPrivateData` on candidate draws only; D1 reports draws per frame and hook ticks per frame.

## 11. Config and menu

```ini
[HudLess]
; off = today's behaviour | recomposition = DLSS-G recomposes the UI (still interpolates it) | ui_free = FG on the UI-free image, UI pasted back
Mode = off
MaskThreshold = 0.008      ; advanced: 0.001 .. 1.0
MaskDilation = 1           ; advanced: 0 .. 4
PasteCleanup = true        ; advanced: clear ghosts of UI that just closed
SteadyTiming = false       ; advanced: paste the UI of one real frame behind (steadier, slightly older)
CoverageGuard = 0.60       ; advanced: fall back when more than this share of pixels is flagged (0 = off)
ShowDetectedUi = false     ; debug tint on the pasted area
Diagnostics = false        ; D1 / diagnostic builds only (bound by A's UiConfig, the rest by B's HudlessConfig)
ReShadePresets = auto      ; auto | on | off: apply the user's ReShade presets in place at the UI boundary (S10 E); auto = a ReShade runtime exists and >= 1 technique is enabled
```

Menu card "HUD-less (UI-free frame generation)": status line (state, reason, counters), mode combo, an "Advanced" fold with the tuning keys, "Show detected UI". Changing Mode to/from `recomposition` shows "applies after a game restart". The old `[FrameGen] ExternalUI*` / `ExternalHudless` keys are not read or written.

## 12. Diagnostic build D1 (observation only; `Mode = off` behaviour unchanged)

A new `[HudLess] Diagnostics = true` turns on, per run, a folder `<game>\OptiScaler\hudless-diag\<yyyymmdd-hhmmss>\` (older runs beyond 5 pruned). Never changes a draw, a copy or a Present.

| File | Content |
|---|---|
| `probe.txt` | **ReShade / Dalamud position (title screen is enough):** at bridge `Present` entry, per frame for the first 5 frames, `CaptureStackBackTrace` -> `GetModuleHandleEx` of each caller frame (module names; looks for ReShade64 and Dalamud); the module owning the `Present` entry in the real D3D11 swapchain vtable and in the D3D12 FG swapchain vtable (`Present`, `Present1`); module list order at start-up |
| `shaders.csv` | per distinct pixel shader: CRC32, DXBC size, stage tag (0-6), UI tag, created-before-hook flag, draws total, first frame seen, set of render-target formats/sizes it drew into, share of draws into the backbuffer |
| `frames.txt` | first 600 frames, then every 100th: per frame the ordered list of tagged draws `(ordinal, stage|UI, rt class [backbuffer|other], rt size/format)`, the candidate boundary under R1 and R2 and their ordinals |
| `cost.txt` | draws per frame, hook ticks per frame (QPC), the early-out cost with `Mode = off` |
| `rt.txt` | identity / size / format of the RT bound at each candidate boundary vs the swapchain backbuffer, deferred-context usage (`CreateDeferredContext` seen or not) |
| `snap-*.png/.raw` | on the menu button "Save UI diagnostics snapshot": boundary capture and bridge-copy frame of the same frame number, plus (user toggles the game's hide-UI key between two presses) a UI-hidden / UI-shown pair; used to check a boundary capture against the UI-hidden frame and to measure mask coverage offline |

Menu card "HUD-less diagnostics": run folder path, counters, the snapshot button.

## 13. Tests

| Test | Class | Package |
|---|---|---|
| `hudless_api_headers` (committed in W6-0) | cpu | W6-0 |
| Mask / paste shaders on WARP: threshold, dilation, cleanup, unchanged scene, alpha levels, SDR + PQ, coverage counter (oracle `external_hudless_warp.cpp` is the data source) | warp | C |
| Paste ring: producer never reuses a busy slot, consumer picks newest completed <= 250 ms, never goes back in time | cpu | C |
| Engine state machine with a fake `IHudlessSource` and fake provider: every fallback reason, hysteresis, coverage guard, retry timing | cpu | B |
| Slot ring + reader-fence logic (no GPU) | cpu | B |
| Index predictor == consumer resync branch (table test) | cpu | B |
| Config binder / defaults / clamps | cpu | B |
| Boundary rule R1/R2/R3 on synthetic draw sequences incl. shaders created before the hook, fades, menus; UI shader catalog CRC set | cpu | A |
| WARP D3D11 draw-capture harness (extends `tests/ext/hdr/hdr_preview.cpp`'s pattern): boundary copy equals the render target at that draw | warp | A |
| Diag file formats (round-trip of `shaders.csv`, `frames.txt`) | cpu | A |

## 14. Work packages (wave W6-A, parallel, Sonnet 5.5 xhigh, worktrees from `s10`)

| Package | Owns | Must not touch | Depends on |
|---|---|---|---|
| **A** `FfxivUiBoundary v0` + D1 | `ext/ffxiv/ui/**` (incl. `UiConfig`), `tests/ext/ffxiv/ui/**`, the three hook lines in `FfxivLightingCapture.h`, `Game.cpp` assignment, `PresentScope` in `ext/hdr/BridgeHdr.cpp` (probe at entry, `EndFrame()` at exit) | the bridge files, `ext/hudless/**` (reads `HudlessSource.h`, `HudlessState.h`) | `HudlessSource.h`, `HudlessState.h` (committed) |
| **B** engine core | `ext/hudless/{Engine,Config,Menu}*`, `adapters/*`, bridge seams + `wrapped_swapchain.cpp` BeforeOverlay, `IFGFeature.cpp` predictor, `ext/ini/HudLess.ini`, `ext/Seams.*`, deletion of `ext/fg/HudlessLink.*`, catalog rows | shaders, `UiPaste.cpp`, `UiImage.cpp`, `ext/ffxiv/ui/**` | both headers; codes against `HudlessGpu.h` with a stub implementation behind a test flag so it builds before C lands |
| **C** mask + paste | `ext/hudless/{UiImage,UiPaste}.cpp`, `shaders/**`, `hudless.shaders.props`, WARP tests | engine, bridge, FFXIV | `HudlessGpu.h` (committed) |

Rules as in W5 (`BRIEF-w5-common.md`): every commit builds through `build-locked.ps1`, `tools\test.ps1` stays green (baseline 60 / 0 / 7 plus new tests), no push / tag / game installs, usage self-stop at 88 %, one log file per agent, report format unchanged. Contract-header changes go through the orchestrator, never silently.

Done when (wave A): each branch builds at every commit, its tests pass, upstream-file edits stay within the section 4.3 budget, and D1 (A alone on top of `w5` + the headers) is packaged for Session 1.

## 15. What the diagnostics must answer (and what happens if the answer is bad)

1. Where ReShade runs (outer / inner / on the FG chain) -> `S10-PLAN.md` section 2b table.
2. Whether R1, R2 or R3 gives a boundary that matches the UI-hidden frame in every scene.
3. How many UI shaders are created before the hook (untaggable) and what fraction of UI draws they account for; if large, the hook has to install earlier or R2 becomes the rule.
4. Whether Dalamud overlays draw before or after the boundary / the bridge copy.
5. Hook cost per frame in a city scene.
6. Whether deferred contexts are used.
7. Whether the RT at the boundary is the backbuffer (else the capture source or the ring assumption changes).

## 16. ReShade integration (built-in add-on; decided 2026-10-16)

**Why:** the user's ReShade 6.8.0 is outermost on the game's D3D11 swapchain (`ReShade.log`: it "redirects" the game's `CreateSwapChain`, one runtime, on that chain). Its effect pass therefore runs in its Present hook, before the bridge copy, on the full frame including the HUD. A UI-free image snapshotted at the first UI draw is taken before that pass, so without help FG would drop the presets and the difference mask would flag every preset-changed pixel.

**Design (REST's behaviour, native, no REST / marker / extra file):**
1. `ext/reshade/ReShadeAddon`: after OptiScaler loads ReShade (`dllmain.cpp` ~1165-1179, one seam call `ext::reshade::OnReshadeLoaded(module)`), register OptiScaler's own module with `ReShadeRegisterAddon(module, 20)` (header-only registration in ReShade's public API headers, vendored, `BSD-3-Clause OR MIT`) and subscribe to `init_effect_runtime`, `destroy_effect_runtime`, `reshade_begin_effects`, `reshade_finish_effects`, `reshade_present`.
2. At the boundary (package A's detector) call `effect_runtime::render_effects(cmd_list, rtv, rtv_srgb)` for the runtime of the game chain, in place on the scene render target; then snapshot. The HUD is drawn afterwards and is not filtered.
3. Suppress ReShade's own end-of-frame effect pass for that frame so the effects are not applied twice.
4. Engine state: new fallback reasons `ReShadeUnavailable`, `ReShadeEffectsFailed`. Any failure in steps 1-3 means: no presets applied at the boundary, the end-of-frame pass is left alone, the engine falls back to normal full-frame FG (today's behaviour). Never a half-applied frame.
5. The full integration (package D) is built only after the diagnostic build answers F1-F5 (below). The probe package D0 contains only registration, event logging and the guarded probes.

**Feasibility questions F1-F5** (answered by D1 + D0 in Session 1, written to `reshade-probe.txt`):
- **F1** registration succeeds in-process; the runtime on the game chain appears (device API, hwnd, technique names with enabled state, preset path).
- **F2** `render_effects` can be called in place from a draw-time hook on the immediate context without corrupting game or ReShade state (probe armed from the menu, one frame; compare the render target before/after; run twice).
- **F3** ReShade's end-of-frame pass can be skipped for that frame (event or effects state), checked by counting effect passes per frame.
- **F4** depth-based techniques see valid depth at the boundary (log ReShade's depth-buffer detection state).
- **F5** cost: effects run once per real frame; measured with QPC around the call.

**Not in scope:** HDR-specific ReShade handling (effects stay SDR), ReShade versions other than the user's 6.8.0 (API 20; other versions fail the registration check and fall back).
