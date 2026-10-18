#pragma once
// The GPU recipe of one preview NR run, as pure data and a recording template, so the same sequence drives the real D3D12 command
// list (CharaNr.cpp) and a state-tracking fake in the tests.
//
// The simple display-referred (SDR) path of the main NR pass, reproduced for a preview image that is not part of the game's frame:
//
//   Encode   the colour image -> a proxy the model is shown, plus an untouched copy ("original"); the preview is already tone
//            mapped, so the encode is a pass-through (the main pass does the same when the game's buffer is not linear HDR)
//   Model    proxy + depth + motion -> the model's answer (reuse every frame, one pass, model size = image size)
//   Resolve  proxy + answer + original -> the result image: the original with the model's edit transferred onto it
//
// Constants not listed below keep the values of the main pass for the SDR path (no game exposure texture, no spatial packing, no
// working-scale change, one model pass). Diagnostic overlays of the main pass (debug view, compare, skin-mask view) are never
// applied to a preview.
//
// Resource states. Shared images (colour, depth, motion, result) are owned by both devices: they start and end every list in
// COMMON, the state the D3D11 side expects. Private images (proxy, original, answer) rest in UNORDERED_ACCESS, the state the
// main pass leaves its scratch images in.

#include <cstdint>

#include "ext/nr/pass/DlssNr_Common.h"

namespace ext::ffxiv::chara::nr
{
enum class Tex : uint8_t
{
    Colour,   // shared: the native preview image copied in (read)
    Proxy,    // private
    Original, // private
    Depth,    // shared: a cleared depth image (bring-up) or the preview's depth (production)
    Motion,   // shared: a zeroed motion image (bring-up) or the preview's motion (production)
    ModelOut, // private: the model's answer
    Result,   // shared: the result image the D3D11 side draws (one of two, chosen by the caller)
    None,
};

enum class State : uint8_t
{
    Common,
    Uav,
    Srv, // NON_PIXEL_SHADER_RESOURCE
};

constexpr bool IsShared(Tex t) { return t == Tex::Colour || t == Tex::Depth || t == Tex::Motion || t == Tex::Result; }
constexpr State RestState(Tex t) { return IsShared(t) ? State::Common : State::Uav; }

// The values of [DlssNr] that shape the result image (the same keys and meaning as the main pass).
struct Settings
{
    float whitePointScale = 1.0f;
    float transferStrength = 1.0f;
    float colourStrength = 1.0f;
    float maxRatio = 2.0f;
    uint32_t transfer = 1;
    bool skinProtection = false;
    bool skinToneEnabled = true;
    float skinDetail = 1.0f;
    float skinColour = 1.0f;
    float environmentDetail = 1.0f;
    float environmentColour = 1.0f;
    uint32_t reversibleMode = 0;
    bool applyModel = true;
};

constexpr float Strength01(float v) { return (v == v && v >= 0.0f) ? (v > 1.0f ? 1.0f : v) : (v == v ? 0.0f : 1.0f); }

inline DlssNrConstants MakeEncode(uint32_t width, uint32_t height, const Settings& s)
{
    DlssNrConstants c {};
    c.Mode = DlssNrMode_Encode;
    c.Passthrough = 1;
    c.WhitePoint = s.whitePointScale;
    c.ReversibleMode = s.reversibleMode;
    c.Width = width;
    c.Height = height;
    return c;
}

inline DlssNrConstants MakeResolve(uint32_t width, uint32_t height, const Settings& s)
{
    DlssNrConstants c {};
    c.Mode = DlssNrMode_Resolve;
    c.WhitePoint = s.whitePointScale;
    c.Width = width;
    c.Height = height;
    c.TransferStrength = s.transferStrength;
    c.SkinProtection = s.skinProtection ? 1u : 0u;
    c.ShowSkinMask = 0;
    c.SkinDetail = Strength01(s.skinDetail);
    c.SkinColour = s.skinToneEnabled ? Strength01(s.skinColour) : 0.0f;
    c.EnvironmentDetail = Strength01(s.environmentDetail);
    c.EnvironmentColour = Strength01(s.environmentColour);
    c.ColourStrength = s.colourStrength;
    c.DebugView = 0;
    c.MaxRatio = s.maxRatio;
    c.Transfer = s.transfer;
    c.DebugScale = s.whitePointScale;
    c.Passthrough = 1;
    c.ReversibleMode = s.reversibleMode;
    c.ApplyModel = s.applyModel ? 1u : 0u;
    c.CompareMode = 0;
    c.CompareSplit = 0.5f;
    c.CompareZoom = 1.0f;
    c.CompareSwap = 0;
    return c;
}

// Writes zero into the target, whatever the source is (the source only needs a valid view).
inline DlssNrConstants MakeZeroMotion(uint32_t width, uint32_t height)
{
    DlssNrConstants c {};
    c.Mode = DlssNrMode_ZeroMotion;
    c.Width = width;
    c.Height = height;
    return c;
}

// The model's creation-time tuning is read when the feature is built; the per-evaluate values are passed with every call.
struct ModelTuning
{
    float intensity = 1.0f;
    int style = 0;
    float structure = 1.0f;
    float tone = 1.0f;
    float skin = -1.0f;
    bool autoMask = true;
};

// The positional arguments of the forwarder's evaluate call after the four resources.
struct ModelCall
{
    uint32_t width = 0, height = 0;                 // the model's input size
    uint32_t guideWidth = 0, guideHeight = 0;       // depth sub-rectangle
    uint32_t motionWidth = 0, motionHeight = 0;     // motion sub-rectangle
    uint32_t depthBaseX = 0, depthBaseY = 0;
    uint32_t motionBaseX = 0, motionBaseY = 0;
    int depthInverted = 0;
    int reset = 0;
    float intensity = 1.0f;
    int style = 0;
    float structure = 1.0f, tone = 1.0f, skin = -1.0f;
    int autoMask = 1;
    float mvScaleX = 1.0f, mvScaleY = 1.0f;
};

// The model works at the image's size; depth and motion cover the whole image; the depth is not inverted (the bring-up depth is
// cleared to far); the motion scale is 1 (the motion image is zero in the bring-up).
inline ModelCall MakeModelCall(uint32_t width, uint32_t height, bool reset, const ModelTuning& t)
{
    ModelCall m;
    m.width = m.guideWidth = m.motionWidth = width;
    m.height = m.guideHeight = m.motionHeight = height;
    m.reset = reset ? 1 : 0;
    m.intensity = t.intensity;
    m.style = t.style;
    m.structure = t.structure;
    m.tone = t.tone;
    m.skin = t.skin;
    m.autoMask = t.autoMask ? 1 : 0;
    return m;
}

struct FramePlan
{
    uint32_t width = 0;
    uint32_t height = 0;
    bool initGuides = false; // the first run of a stream: the motion image is zeroed on the GPU
    bool reset = false;
    Settings settings {};
    ModelTuning tuning {};
    Tex result = Tex::Result;
};

// `R` provides
//   void TimerStart(); void TimerEnd();
//   void Barrier(Tex, State from, State to);
//   bool Pass(const DlssNrConstants&, Tex source, Tex model, Tex original, Tex target, Tex keep);  // Tex::None = unused
//   bool Evaluate(const ModelCall&);                                                               // proxy, depth, motion -> answer
// The recording stops at the first failing step and returns false; the caller then discards the list.
template <class R> bool RecordRun(R& r, const FramePlan& p)
{
    r.TimerStart();
    r.Barrier(Tex::Colour, State::Common, State::Srv);

    if (p.initGuides)
    {
        r.Barrier(Tex::Motion, State::Common, State::Uav);
        if (!r.Pass(MakeZeroMotion(p.width, p.height), Tex::Colour, Tex::None, Tex::None, Tex::Motion, Tex::None))
            return false;
        r.Barrier(Tex::Motion, State::Uav, State::Common);
    }

    if (!r.Pass(MakeEncode(p.width, p.height, p.settings), Tex::Colour, Tex::None, Tex::None, Tex::Proxy, Tex::Original))
        return false;
    r.Barrier(Tex::Colour, State::Srv, State::Common);
    r.Barrier(Tex::Proxy, State::Uav, State::Srv);
    r.Barrier(Tex::Original, State::Uav, State::Srv);

    r.Barrier(Tex::Depth, State::Common, State::Srv);
    r.Barrier(Tex::Motion, State::Common, State::Srv);
    if (!r.Evaluate(MakeModelCall(p.width, p.height, p.reset, p.tuning)))
        return false;
    r.Barrier(Tex::ModelOut, State::Uav, State::Srv);
    r.Barrier(Tex::Depth, State::Srv, State::Common);
    r.Barrier(Tex::Motion, State::Srv, State::Common);

    r.Barrier(p.result, State::Common, State::Uav);
    if (!r.Pass(MakeResolve(p.width, p.height, p.settings), Tex::Proxy, Tex::ModelOut, Tex::Original, p.result, Tex::None))
        return false;
    r.Barrier(p.result, State::Uav, State::Common);

    r.Barrier(Tex::ModelOut, State::Srv, State::Uav);
    r.Barrier(Tex::Proxy, State::Srv, State::Uav);
    r.Barrier(Tex::Original, State::Srv, State::Uav);
    r.TimerEnd();
    return true;
}
} // namespace ext::ffxiv::chara::nr
