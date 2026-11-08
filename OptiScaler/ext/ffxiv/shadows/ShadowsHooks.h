#pragma once
// Shadow mapping (ext/ffxiv/shadows): the sink hooks the two seams read. Null-checked function pointers set once by
// the module's own static initialisers (ext/ffxiv/shadows/ShadowsDispatch.cpp, the FfxivConfig::*OnLoaded pattern),
// so the consumers - ext/ffxiv/relight and ext/ffxiv/engine - carry no link dependency on the shadows code: the
// existing contact and engine tests keep building without it, and every hook's null answer is "not armed", which is
// today's picture.
//
// The hooks, and who asks:
//  - maskWants               the relight seam asks per bind and per draw (a cheap gate behind the pointer): does the
//                            mask lift want the light draws while contact shadows is not asking for them? The
//                            relight index and tracker run the same lookups they run for contact, and the draw
//                            route hands the resolved layout to the sink below;
//  - combineLift             the relight combine asks, per served light draw, for (lift, softness): true = apply
//                            them in the combine it already renders for this draw (one copy per light draw, no
//                            second competing copy), false = the game's own mask;
//  - lightDrawRoute          the relight draw route asks, per draw whose layout the index resolved, while contact
//                            shadows is not armed: true = the module's own copy/remap/bind ran and the game's draw
//                            has been called exactly once (the brief's "when contact is off, do your own
//                            copy/remap/rebind"); false = pass the draw through untouched. A null layout answers
//                            false (only indexed light draws ask);
//  - captureLightColour      the relight path (the contact arm and the own route alike) asks once per served light
//                            draw, so the composite's curve reads the frame's own g_LightParam at the draw that
//                            binds it (the staging ring; the ShaftsSun.cpp pattern; the sink keys on the
//                            frame counter itself, so the relight caller carries no present dependency);
//  - ambientWants            the engine pass overrides' arming chain asks: has the night ambient lift key left the
//                            engine's own value, whatever the light colour says today? The curve itself is asked
//                            per composite draw, through the entries provider;
//  - ambientEntries          the engine pass overrides' ambient arm asks, per composite draw, for the two scale
//                            entries of ONE dispatch, appended AFTER the shipped knob's own entries (CbScale.hlsl
//                            multiplies every matching entry in order, so the knob's scale and the night lift
//                            compose on the 112+192k `.w` field). The curve value the dispatch ran under is
//                            returned for the run line.
//
// Cost model of the arming (the engine's own): a lift that has left the identity range turns the seams on and pays
// what a shipped knob armed at its identity pays - one relaxed load per draw, the classified draws' own work only.
// The curve moving the arms in and out happens per classified draw at the sinks below (the light-draw rate and the
// composite rate), never in a gate the whole frame's draws would run.

#include "ext/ffxiv/engine/EnginePassRules.h" // ScaleEntry: the provider's entry type (pure, no device)

#include <cstdint>

struct ID3D11Buffer;
struct ID3D11DeviceContext;

namespace ext::ffxiv::shadows
{
// The layout of a sun light draw the relight index resolved, handed to the draw route by the seam that resolved it
// (relight::Layout's own fields, read-only for the sink). `id` is the relight index id, so the sink can keep its own
// copy of the shape-refusal memory the contact pass keeps (one look per light draw shader, re-inspected after the
// relight module's kShapeRefusalSeconds).
struct RouteLayout
{
    uint32_t id = 0;
    uint32_t maskSlot = 0;
    uint32_t depthSlot = 0;
    uint32_t lightParamSlot = 0;
    uint32_t cameraParamSlot = 0;
    bool fakeSpecular = false; // the main view's sun light draw (relight::Layout::fakeSpecular): the night curve's colour source
};

using MaskWantsFn = bool (*)(void);
using CombineLiftFn = bool (*)(ID3D11DeviceContext*, float* outLift, float* outSoftness);
using LightDrawRouteFn = bool (*)(ID3D11DeviceContext*, void (*)(void*), void*, const RouteLayout* layout);
using CaptureLightColourFn = void (*)(ID3D11DeviceContext*, ID3D11Buffer*); // the sink reads the frame key itself
using AmbientWantsFn = bool (*)(void);
using AmbientEntriesFn = uint32_t (*)(ID3D11DeviceContext*, engine::ScaleEntry* out, uint32_t capacity, float* outCurve);

// The six consumers. Null while the shadows module is not linked (today's picture), assigned only by its static
// registration line in ShadowsDispatch.cpp.
struct Hooks
{
    static inline MaskWantsFn maskWants = nullptr;
    static inline CombineLiftFn combineLift = nullptr;
    static inline LightDrawRouteFn lightDrawRoute = nullptr;
    static inline CaptureLightColourFn captureLightColour = nullptr;
    static inline AmbientWantsFn ambientWants = nullptr;
    static inline AmbientEntriesFn ambientEntries = nullptr;
};

// The module's own counters, for the card's state line and for a test: what the mask route served and what the light
// colour capture did. Plain reads on the render thread (the card's thread), no lock.
struct Status
{
    bool keyOn = false;
    bool maskAsk = false;         // the route's gate (MaskWants): the lift key, or the colour feed of the night curve
    bool liftKeyOff = true;       // the shadow lift key sits at its identity
    bool ambientKeyOff = true;    // the night ambient key sits at its identity
    bool standDown = false;       // the route stood itself down for the session
    bool sceneHdr = false;        // the engine's scene HDR path is on (the module is inert)
    bool haveReading = false;     // a light colour has landed
    float curve = 0.0f;           // the curve of the last blend
    uint64_t routeDrawsSeen = 0;  // light draws the route classified
    uint32_t routeRuns = 0;       // of those, the ones that bound a lifted mask
    uint32_t routeDeclined = 0;   // of those, the ones the classification left alone
    uint32_t colourCopies = 0;    // light colour copies staged
    uint32_t colourReads = 0;     // reads that landed
    uint32_t colourNotReady = 0;  // reads that were not ready yet
    uint32_t ambientAsked = 0;    // composite draws for which the ambient provider gave entries
};
Status CurrentStatus();

// The registration line comes from ext::FfxivConfig::PostLoad, which hands the master switch's value across here
// (the module reads its own keys itself); nothing else of the config crosses this seam. Nothing here touches a
// device or logs.
void OnConfigLoaded(bool on);

// Device-side release, the engine seam's Release chain (EnginePass.cpp calls it next to the other texture passes').
void Release();
} // namespace ext::ffxiv::shadows
