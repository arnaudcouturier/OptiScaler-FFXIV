#pragma once
// S10 package E: ReShade presets applied in place at the UI boundary (docs/HUDLESS.md section 16, Session 1 verdicts F1-F5).
//
// The user's ReShade runs its effects in its Present hook, BEFORE the bridge copy, so a UI-free snapshot taken at the first
// UI draw lacks the presets. ApplyPresetsInPlace() calls effect_runtime::render_effects on the render target the boundary
// detector is about to snapshot; ReShade then skips its own end-of-frame pass for that frame (documented behaviour of
// render_effects, confirmed 5 of 5 in Session 1). The HUD is drawn afterwards and is not filtered (REST behaviour).
//
// No ReShade, D3D or ImGui types here (the header is included by the FFXIV boundary and by tests).

#include <cstdint>
#include <string>
#include <string_view>

struct ID3D11DeviceContext;
struct ID3D11Resource;

namespace ext::reshade
{
enum class ApplyResult : uint8_t
{
    Applied,        // render_effects ran in place on the target (ReShade's own end-of-frame pass is skipped this frame)
    NotEnabled,     // [HudLess] ReShadePresets = off: no call
    NoReShade,      // OptiScaler is not registered as a ReShade add-on (no ReShade, wrong version, registration refused)
    NoRuntime,      // registered, but no live effect runtime for this D3D11 context
    NothingToApply, // effects are disabled, no technique is enabled, effects not loaded yet, or already applied this frame
    Failed,         // ReShade should have applied presets but the call was refused or failed: the caller must not use the frame
};
inline constexpr size_t kApplyResults = 6;

// Short text for logs ("applied", "off by config", ...). Static strings.
const char* ApplyResultName(ApplyResult result);

// [HudLess] ReShadePresets: auto (default) | on | off.
//   auto = apply when registered, a runtime exists, effects are enabled and >= 1 technique is enabled
//   on   = the same, but also when the technique count says zero (the count is not trusted)
//   off  = never
enum class PresetsMode : uint8_t
{
    Auto,
    On,
    Off,
};
// "on"/"true"/"1" -> On, "off"/"false"/"0" -> Off, anything else (including "auto" and empty) -> Auto. Case-insensitive.
PresetsMode ParsePresetsMode(std::string_view text);
const char* PresetsModeName(PresetsMode mode);

// Entry point for the boundary detector (immediate context, render thread, before the boundary draw runs). `renderTarget` must
// be the resource bound as render target 0 on `context`. Never throws, never lets an exception escape; calls ReShade at most
// once per game frame (a second call before FrameEnded() returns NothingToApply). Restores the D3D11 pipeline state.
ApplyResult ApplyPresetsInPlace(ID3D11DeviceContext* context, ID3D11Resource* renderTarget);

// What ApplyPresetsInPlace would return at this point, without calling ReShade and without counting a call (render thread, before
// a draw). Applied = it would run render_effects (the bound render target is not checked: the caller asks before the draw whose
// target is not the one the effects would be applied to). The early UI anchor uses it to keep the presets where they always run.
// Never throws.
ApplyResult PresetsWouldApply(ID3D11DeviceContext* context);

// End of the game frame (bridge Present exit): re-arms the once-per-frame gate. Cheap, any thread.
void FrameEnded();

// Counters and the last detail text (why the last non-Applied result was not Applied). Cheap enough for a 600-frame log line.
struct ApplyStats
{
    uint64_t calls = 0;
    uint64_t results[kApplyResults] {};
    uint64_t callUsMax = 0;    // render_effects submission time (CPU), microseconds
    uint64_t callUsTotal = 0;
    uint64_t runtimeEvents = 0; // init + destroy events seen
    bool inert = false;         // latched after an exception or a failed state restore
    std::string lastDetail;
};
ApplyStats Stats();
} // namespace ext::reshade
