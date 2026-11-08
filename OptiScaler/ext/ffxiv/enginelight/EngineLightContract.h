#pragma once

// Engine light controls (2026-11-07): the shared contract between the two writers of the slice.
//   Writer 1 (branch eng-cb): the constant-buffer patch family - the moon at its light draw (and the water's copy), the water's ambient block,
//     the ambient tint - plus the F9 dumps of those buffers. It never owns config keys, the menu or the weight: it receives Params + the weight
//     through PublishCbFamily and applies them.
//   Writer 2 (branch eng-lamp): the lamp arm (blend factor on the point-light volumes), the exposure compensation stage in the tone-map chain,
//     the night weight (its own feed request, independent of NightGrade), the config keys, the INI, the schema, the menu card with its master
//     toggle and the engine / reference A/B, the force-night-weight key; it calls PublishCbFamily whenever Params or the weight change.
// Both branches carry this file byte for byte; neither edits it (the orchestrator owns it).

namespace ext::ffxiv::enginelight
{

struct Params
{
    bool master = true;           // the card's master toggle: false = every arm at the engine's own value
    bool referenceAB = false;     // live A/B: true = the engine's own values for one look (every factor 1, no compensation)
    float moonIntensity = 0.1f;   // the moon (directional light) colour multiplier, 0.05 .. 1 (1 = the engine's)
    float moonTint[3] = { 1.0f, 1.0f, 1.0f };   // per-channel multipliers on the moon colour, 0.5 .. 1.5
    float ambientIntensity = 0.5f;              // the ambient (SH) multiplier, 0.1 .. 1
    float ambientTint[3] = { 1.0f, 1.0f, 1.0f };// per-channel multipliers on the ambient, 0.5 .. 1.5
    float waterMoon = 0.2f;       // the water pass's own moon term multiplier, 0.05 .. 1
    float waterAmbient = 0.5f;    // the water's own ambient block multiplier, 0.1 .. 1
    float waterTint[3] = { 1.0f, 1.0f, 1.0f };  // per-channel multipliers on the water's ambient block (toward neutral), 0.5 .. 1.5
    float lampIntensity = 3.0f;   // the point-light volumes' contribution multiplier, 1 .. 5
    int exposureMode = 1;         // 0 = off (the engine adapts freely), 1 = auto hold, 2 = manual factor
    float exposureManual = 1.0f;  // the manual compensation factor, 0.25 .. 4
    bool forceNightWeight = false;// INI only: the weight is 1 whatever the night curve says (the benchmark never reads a light)
};

// The effective factor of one arm at a weight w in [0, 1]: lerp(1, factor, w); 1 when the master is off or the A/B shows the engine's values.
constexpr float Effective(const Params& p, float factor, float weight) noexcept
{
    if (!p.master || p.referenceAB)
        return 1.0f;
    const float w = weight < 0.0f ? 0.0f : (weight > 1.0f ? 1.0f : weight);
    return 1.0f + (factor - 1.0f) * w;
}

// Writer 1's module sets this at init; writer 2's card / config / weight update calls it (render thread) with the raw Params and the smoothed weight.
using PublishCbFamilyFn = void (*)(const Params& params, float weight);
inline PublishCbFamilyFn publishCbFamily = nullptr;

} // namespace ext::ffxiv::enginelight
