#pragma once
// Automatic ReShade scene depth: the pure decisions of the capture glue (ReShadeDepth.cpp, which reads the NGX parameters, finds the
// native context and calls the copy pass). No NGX, ReShade, D3D or ImGui types here, so every rule is unit-tested on its own
// (tests/ext/reshade/reshade_depth_glue.cpp).

#include "ext/reshade/ReShadeDepthCore.h"
#include "ext/reshade/ReShadeRegister.h"
#include "ext/upscaler/RenderGeometry.h"

#include <atomic>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace ext::reshade::depth::glue
{
// ---- sizes ----------------------------------------------------------------------------------------------------------------------
struct Extent
{
    uint32_t w = 0, h = 0;
    constexpr bool Valid() const { return w != 0 && h != 0; }
    friend constexpr bool operator==(const Extent& a, const Extent& b) { return a.w == b.w && a.h == b.h; }
};

// The render size of this evaluate. NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_* is the part of the buffer that holds the
// frame, but FFXIV reports its buffers' own size there, while Width/Height are what it rendered: the smaller one per axis is the render
// size (ext::upscaler::ResolveRenderExtentValue, the geometry seam's rule, with its gate forced on: this feature always wants it).
// Without a subrect the NGX Width/Height are the render size. NOT the feature's RenderWidth(), which is the buffer size by default.
inline Extent RenderExtent(uint32_t subrectW, uint32_t subrectH, uint32_t paramW, uint32_t paramH)
{
    if (subrectW == 0 && subrectH == 0)
        return Extent { paramW, paramH }; // (a half pair is no pair: the result is not valid)
    unsigned int w = subrectW, h = subrectH;
    ext::upscaler::ResolveRenderExtentValue(true, paramW, paramH, w, h);
    return Extent { w, h };
}

// The output size: OutWidth/OutHeight, else the Output resource's, else the display size the feature was created with. A source is
// taken as a pair, never mixed with another one.
inline Extent OutputExtent(uint32_t outW, uint32_t outH, Extent texture, Extent display)
{
    if (outW != 0 && outH != 0)
        return Extent { outW, outH };
    if (texture.Valid())
        return texture;
    return display.Valid() ? display : Extent {};
}

// The base of the depth subrect, from the NGX parameter's value. NEVER clamped: a negative value becomes a huge one, which the pass
// refuses (the render region would not lie inside the source), instead of silently reading from the top-left corner.
constexpr uint32_t BaseFromInt(int value) { return static_cast<uint32_t>(value); }

// The character-preview filter: a capture is published only when the NGX Output has the size of the effect runtime's back buffer (the
// 576x960 preview entries never match). Also the size of the copy.
inline bool OutputIsConsumer(Extent output, const ConsumerInfo& consumer)
{
    return consumer.seen && output.Valid() && output.w == consumer.width && output.h == consumer.height;
}

// ---- the gates, cheapest first ---------------------------------------------------------------------------------------------------
// The word the seam reads first: bit 0 = the option is on, the others are requests for the evaluate thread (the thread that owns the
// copy pass and its context), left by whoever switched the option off or ended NGX. ONE relaxed atomic load decides "nothing to do".
class EvalGate
{
  public:
    static constexpr uint32_t kEnabled = 1;
    static constexpr uint32_t kRequestWithdraw = 2; // publish valid = false
    static constexpr uint32_t kRequestRelease = 4;  // give the pass's resources (and its reference to the device) back
    static constexpr uint32_t kRequests = kRequestWithdraw | kRequestRelease;

    bool Idle() const noexcept { return _bits.load(std::memory_order_relaxed) == 0; }
    bool Enabled() const noexcept { return (_bits.load(std::memory_order_relaxed) & kEnabled) != 0; }
    uint32_t Bits() const noexcept { return _bits.load(std::memory_order_relaxed); }

    // Switching on does not touch the requests; switching off from on leaves the two requests for the evaluate thread.
    void SetEnabled(bool on) noexcept
    {
        if (on)
            _bits.fetch_or(kEnabled, std::memory_order_relaxed);
        else if ((_bits.fetch_and(~kEnabled, std::memory_order_relaxed) & kEnabled) != 0)
            _bits.fetch_or(kRequests, std::memory_order_relaxed);
    }
    void Request(uint32_t flags) noexcept { _bits.fetch_or(flags & kRequests, std::memory_order_relaxed); }
    uint32_t TakeRequests() noexcept { return _bits.fetch_and(~kRequests, std::memory_order_relaxed) & kRequests; }

  private:
    std::atomic<uint32_t> _bits { 0 };
};

enum class Gate : uint8_t
{
    Idle,           // the option is off and there is nothing to do
    ShuttingDown,
    NotRegistered,  // ReShade is not registered (yet): the late attempt may run
    NoConsumer,     // no effect runtime has asked for depth lately
    NoSizes,        // the render or the output size is unknown
    NotConsumerSize, // the Output is not the consumer's size: a character preview, or a window the effects do not cover
    Go,
};

// `bits` is the EvalGate word AFTER the requests were carried out. Without the option nothing else may run (a late registration
// attempt or the event subscription with the option off), also if a request arrived in the meantime.
inline Gate FirstGate(uint32_t bits, bool shuttingDown, bool registered)
{
    if ((bits & EvalGate::kEnabled) == 0)
        return Gate::Idle;
    if (shuttingDown)
        return Gate::ShuttingDown;
    return registered ? Gate::Go : Gate::NotRegistered;
}

inline Gate ConsumerGate(const ConsumerInfo& consumer) { return consumer.seen ? Gate::Go : Gate::NoConsumer; }

inline Gate SizeGate(Extent render, Extent output, const ConsumerInfo& consumer)
{
    if (!render.Valid() || !output.Valid())
        return Gate::NoSizes;
    return OutputIsConsumer(output, consumer) ? Gate::Go : Gate::NotConsumerSize;
}

// ---- the pass's draw -------------------------------------------------------------------------------------------------------------
// The raw (unhooked) Draw trampoline of the lighting hooks is valid only with the native context they hooked. With any other
// context, or with no hooks at all, the pass draws with the context's own Draw inside the bypass scope.
inline bool UseRawDraw(const void* hookedContext, const void* nativeContext) { return hookedContext != nullptr && hookedContext == nativeContext; }

// ---- log lines -------------------------------------------------------------------------------------------------------------------
// What the first capture and every change of it log (one line per change, never the same twice in a row).
struct CaptureSignature
{
    uint32_t srcW = 0, srcH = 0, format = 0, bindFlags = 0; // the game's depth as read
    uint32_t baseX = 0, baseY = 0;
    uint32_t renderW = 0, renderH = 0;
    uint32_t outW = 0, outH = 0;
    friend constexpr bool operator==(const CaptureSignature& a, const CaptureSignature& b)
    {
        return a.srcW == b.srcW && a.srcH == b.srcH && a.format == b.format && a.bindFlags == b.bindFlags && a.baseX == b.baseX &&
               a.baseY == b.baseY && a.renderW == b.renderW && a.renderH == b.renderH && a.outW == b.outW && a.outH == b.outH;
    }
};
class ChangeTracker
{
  public:
    bool Changed(const CaptureSignature& s)
    {
        if (_have && _last == s)
            return false;
        _last = s;
        _have = true;
        return true;
    }

  private:
    CaptureSignature _last;
    bool _have = false;
};

// The line for a result that is not Ok: at once when the result changes, then at most one per kIntervalMs while it repeats. Ok (0) is
// never a line and ends the run. A clock that went backwards logs nothing (its difference is negative, so below the interval).
class ResultLog
{
  public:
    static constexpr int64_t kIntervalMs = 60'000;
    bool ShouldLog(uint32_t code, int64_t nowMs)
    {
        if (code == 0)
        {
            _code = 0;
            return false;
        }
        if (code != _code)
        {
            _code = code;
            _lastMs = nowMs;
            return true;
        }
        if (nowMs - _lastMs < kIntervalMs)
            return false;
        _lastMs = nowMs;
        return true;
    }

  private:
    uint32_t _code = 0;
    int64_t _lastMs = 0;
};

// ---- the game gate -----------------------------------------------------------------------------------------------------------------
// The automatic depth is for FFXIV only (plan 4.1): the executable name, whole and case-insensitive. The facade reads State::gameExe once
// (after the one-load gate), so another game with the ini key on builds nothing and does nothing.
inline bool IsFfxivExe(std::string_view exe)
{
    constexpr std::string_view kName = "ffxiv_dx11.exe";
    if (exe.size() != kName.size())
        return false;
    for (size_t i = 0; i < kName.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(exe[i])) != kName[i])
            return false;
    return true;
}

// ---- the status line of the menu -------------------------------------------------------------------------------------------------
// The text DepthBinder::Status() writes for a pass that is bound starts with this.
inline constexpr char kBoundPrefix[] = "Automatic scene depth:";

struct StatusInputs
{
    bool enabled = false;
    bool gameSupported = true;      // false: not the FFXIV executable
    bool registered = false;
    ext::reshade::RegisterStatus reason = ext::reshade::RegisterStatus::NoModule; // why not, when !registered
    bool lateGaveUp = false;        // the late registration used up its attempts
    bool subscriptionFailed = false;
    bool nativeContextMissing = false; // the last evaluate had no native immediate context
    const char* captureRefusal = nullptr; // why the last capture was refused, null when it was not
    std::string binderStatus;       // DepthBinder::Status()
    std::string gbuffers;           // "normals, motion": the G-buffers realigned by the last begin, empty when none
};

inline std::string ComposeStatus(const StatusInputs& in)
{
    if (!in.enabled)
        return "Automatic scene depth: off";
    if (!in.gameSupported)
        return "Automatic scene depth: not available for this game";
    if (!in.registered)
    {
        // "not detected" is for a ReShade that is not there; one that is there and cannot be used says why
        switch (in.reason)
        {
        case ext::reshade::RegisterStatus::Refused:
            return "ReShade found but refused add-on API 20 (needs ReShade 6.8 or newer)";
        case ext::reshade::RegisterStatus::ExportsMissing:
            return "ReShade found but without add-on support (needs ReShade 6.8 or newer with add-on support)";
        case ext::reshade::RegisterStatus::Exception:
            return "ReShade found but the registration failed";
        case ext::reshade::RegisterStatus::NoSelf:
            return "ReShade found but OptiScaler's own module is unknown";
        case ext::reshade::RegisterStatus::Ok: // (registered is false: cannot happen)
        case ext::reshade::RegisterStatus::NoModule:
            break;
        }
        return in.lateGaveUp ? "ReShade not detected (stopped looking)" : "ReShade not detected";
    }
    if (in.subscriptionFailed)
        return "ReShade events could not be subscribed; generic depth retained";
    if (in.nativeContextMissing)
        return "Automatic scene depth: native context not available";
    if (in.binderStatus.rfind(kBoundPrefix, 0) == 0)
        return in.gbuffers.empty() ? in.binderStatus : in.binderStatus + "; G-buffers realigned: " + in.gbuffers;
    if (in.captureRefusal != nullptr)
        return std::string("Scene depth capture refused (") + in.captureRefusal + "); generic depth retained";
    // no binder yet (it is built at the first evaluate; the ini can say true on the title screen): waiting, not off and not empty
    return in.binderStatus.empty() ? std::string("Waiting for ReShade depth effects") : in.binderStatus;
}
} // namespace ext::reshade::depth::glue
