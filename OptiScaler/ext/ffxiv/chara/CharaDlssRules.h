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

// Decides which log lines of the DLSS path are written: each reason once per burst of preview activity, and at most
// `maxLines` times over the session, so a stalled path shows where it stalls without a line per draw.
class BurstLimiter
{
  public:
    static constexpr size_t kReasons = 16;

    bool ShouldLog(size_t reason, uint32_t burst, uint32_t maxLines = 24)
    {
        if (reason >= kReasons || _lines[reason] >= maxLines || (_lines[reason] != 0 && _burst[reason] == burst))
            return false;
        _burst[reason] = burst;
        ++_lines[reason];
        return true;
    }

  private:
    uint32_t _burst[kReasons] {};
    uint32_t _lines[kReasons] {};
};

// What may be done with the NGX objects (feature handle, parameter object) a slot holds. They belong to the NGX instance that
// created them: once NGX was shut down and initialised again (the generation changed) they must neither be used nor released
// through the driver, because the new instance does not know them. They are dropped without a driver call (the shutdown
// reclaimed them) and the next finish creates a new feature.
enum class FeatureUse : uint8_t
{
    Keep,                 // nothing to do: no NGX objects, or they are in use
    ReleaseThroughDriver, // objects of the live NGX instance: release them with the driver
    Forget,               // objects of an earlier (or ending) NGX instance: drop the pointers, no driver call
};

constexpr FeatureUse DecideFeatureUse(bool hasObjects, uint32_t slotGeneration, uint32_t currentGeneration, bool ngxInitialised,
                                      bool ngxShuttingDown, bool releasing)
{
    if (!hasObjects)
        return FeatureUse::Keep;
    if (slotGeneration != currentGeneration || !ngxInitialised || ngxShuttingDown)
        return FeatureUse::Forget;
    return releasing ? FeatureUse::ReleaseThroughDriver : FeatureUse::Keep;
}

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
// Float depth (R32_TYPELESS and the like) is deliberately not mapped: the reference build records the preview depth view as
// R24_UNORM_X8_TYPELESS, a float buffer would come with a reversed-Z convention nobody has seen in this renderer, and a wrong
// guess produces wrong guides silently, where an unknown format keeps the preview native.

// What was learnt about the pass's depth texture, so that an unreadable one is looked at (and logged) once instead of once per pass.
// The creation of the shader resource view may fail for a readable format; it is tried a few times and then given up on.
class DepthMemo
{
  public:
    static constexpr uint8_t kMaxViewFailures = 3;

    // True when the texture must be examined now: it is not the one remembered, or it has no view yet and may still get one.
    bool ShouldExamine(uintptr_t texture, bool haveView) const { return texture != _texture || (!haveView && !_rejected); }

    // A texture is being examined: a new one starts with a clean record.
    void Start(uintptr_t texture)
    {
        if (texture == _texture)
            return;
        _texture = texture;
        _failures = 0;
        _rejected = false;
    }

    void Reject() { _rejected = true; }                // the format cannot be read: never examine this texture again
    bool ViewFailed()                                  // a view could not be created; true when this was the last try
    {
        if (++_failures >= kMaxViewFailures)
            _rejected = true;
        return _rejected;
    }
    bool Rejected() const { return _rejected; }
    void Reset() { *this = DepthMemo {}; }

  private:
    uintptr_t _texture = 0;
    uint8_t _failures = 0;
    bool _rejected = false;
};
} // namespace ext::ffxiv::chara::dlss
