#pragma once
// Pure rules of the DLSS finish step (CharaDlss.cpp): whether the guides for a finished preview render are complete and the
// slot may be evaluated. Everything here is facts in, verdict out.

#include <cstdint>

#include "ext/ffxiv/chara/CharaSlots.h"

namespace ext::ffxiv::chara::dlss
{
inline constexpr uint32_t kMaxOutputExtent = 8192; // the 2x output must fit

struct GuideState
{
    bool replayed = false;       // the signed-motion replay ran in this pass
    bool depthTexture = false;   // the pass depth texture is known
    bool depthView = false;      // ... and a shader resource view of it exists
    bool cameraCaptured = false; // the camera block was copied in this pass
};

struct FinishFacts
{
    uint32_t width = 0; // the pass (render) size
    uint32_t height = 0;
    bool slotFound = false;
    bool slotFailed = false;
    uint64_t now = 0;
    uint64_t lastUse = 0;
    bool needsRebuild = false;
    GuideState guides {};
};

enum class FinishVerdict : uint8_t
{
    Proceed,
    Rebuild, // proceed after recreating the slot (size or depth convention changed)
    TooLarge,
    NoSlot,
    Failed,
    Stale,
    WaitingForMotion,
    WaitingForDepth,
    WaitingForDepthView,
    WaitingForCamera,
};

constexpr bool IsWaiting(FinishVerdict v) { return v >= FinishVerdict::WaitingForMotion; }

constexpr const char* FinishVerdictName(FinishVerdict v)
{
    switch (v)
    {
    case FinishVerdict::Proceed: return "evaluated";
    case FinishVerdict::Rebuild: return "slot rebuilt";
    case FinishVerdict::TooLarge: return "too large";
    case FinishVerdict::NoSlot: return "no slot yet";
    case FinishVerdict::Failed: return "slot failed";
    case FinishVerdict::Stale: return "slot not used recently";
    case FinishVerdict::WaitingForMotion: return "guides: no motion";
    case FinishVerdict::WaitingForDepth: return "guides: no depth";
    case FinishVerdict::WaitingForDepthView: return "guides: no depth view";
    case FinishVerdict::WaitingForCamera: return "guides: no camera";
    }
    return "?";
}

constexpr const char* WaitingText(FinishVerdict v)
{
    switch (v)
    {
    case FinishVerdict::WaitingForMotion: return "waiting for guides: motion";
    case FinishVerdict::WaitingForDepth: return "waiting for guides: depth";
    case FinishVerdict::WaitingForDepthView: return "waiting for guides: depth view";
    case FinishVerdict::WaitingForCamera: return "waiting for guides: camera";
    default: return "";
    }
}

constexpr FinishVerdict DecideFinish(const FinishFacts& f)
{
    if (f.width == 0 || f.height == 0 || f.width * 2ull > kMaxOutputExtent || f.height * 2ull > kMaxOutputExtent)
        return FinishVerdict::TooLarge;
    if (!f.slotFound)
        return FinishVerdict::NoSlot;
    if (f.slotFailed)
        return FinishVerdict::Failed;
    if (f.now < f.lastUse || f.now - f.lastUse > kLiveMs)
        return FinishVerdict::Stale;
    if (!f.guides.replayed)
        return FinishVerdict::WaitingForMotion;
    if (!f.guides.depthTexture)
        return FinishVerdict::WaitingForDepth;
    if (!f.guides.depthView)
        return FinishVerdict::WaitingForDepthView;
    if (!f.guides.cameraCaptured)
        return FinishVerdict::WaitingForCamera;
    return f.needsRebuild ? FinishVerdict::Rebuild : FinishVerdict::Proceed;
}

// The shader resource view format of a depth texture the guides pass can read: a typeless 24-bit depth format bound as a
// shader resource. Anything else keeps the preview native.
constexpr bool DepthReadable(uint32_t dxgiFormat, bool shaderResourceBind)
{
    return dxgiFormat == 44 /* DXGI_FORMAT_R24G8_TYPELESS */ && shaderResourceBind;
}
} // namespace ext::ffxiv::chara::dlss
