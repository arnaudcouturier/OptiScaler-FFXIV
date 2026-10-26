#pragma once
// S10 package E: the LIVE half of the FFXIV boundary source (docs/HUDLESS.md sections 4.1, 10, 16): at the R1b boundary draw it
// (1) checks the bound render target, (2) applies the ReShade presets in place, (3) snapshots the target into one reused
// texture, and later hands that texture to the engine through IHudlessSource::Take.
//
// Pure D3D11 class, no OptiScaler dependencies (no pch, config, log): tests/ext/ffxiv/ui builds it standalone (cpu decision test,
// WARP order test). The effects function and the copy function are injected (production: ext::reshade::ApplyPresetsInPlace and
// the lighting hook's un-hooked CopyResource trampoline).
//
// Rules (all decided in S10-E-LOG.md):
//  * R1b only (S10 E2): the first UI-tagged draw after the last scene/post stage whose render target IS the swapchain backbuffer
//    (UiBoundaryRules.h). UI draws into other render targets before it (the character preview, 103 of 24,000 frames in Session 1)
//    are ignored, not rejected: rejecting them made ReShade's own pass filter the HUD on those frames and skipped it on the others
//    (visible flicker). No such draw in the frame = NotThisFrame. R1/R2/R3 never act.
//  * Acts only if the engine asked for a frame in the PREVIOUS Present (Take() was called): frame generation off, an unsupported
//    provider, the debug tint or the coverage guard make the engine skip Take(); then nothing is applied or copied.
//  * Acts only on frames that showed a scene stage (menu / loading frames have no UI-free image).
//  * Once per frame. A rejected frame has NO side effects when the guard fails; a frame whose R1b candidate is superseded after the
//    capture is rejected (the effects already ran: Session 1 never saw this, 0 in 24,000 frames).
//  * Order inside a capture: snapshot texture ensured -> effects -> copy, so the snapshot contains the preset-processed target
//    and nothing the later (UI) draws write.
// Threading: OnDraw on the render thread, Take/EndFrame on the Present thread; one mutex (held across the effects call: it runs
// once per frame).

#include "ext/ffxiv/ui/UiBoundaryCore.h"
#include "ext/ffxiv/ui/UiCapture.h"
#include "ext/hudless/HudlessSource.h"
#include "ext/reshade/ReShadeBoundary.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <string>

namespace ext::ffxiv::ui::live
{
// Why a frame was not delivered. The first group is the guard ("render target is not the full-size backbuffer").
enum class Reason : uint8_t
{
    None,
    NoRenderTarget,
    NotTexture2D,
    Multisampled,
    ArrayOrMips,
    NoBackbuffer,
    SizeMismatch,
    FormatUnsupported,
    EffectsFailed,
    Superseded,
    CopyFailed,
    Count
};
inline constexpr size_t kReasons = static_cast<size_t>(Reason::Count);

// Names for the counters in the log.
inline const char* ReasonName(Reason r)
{
    switch (r)
    {
    case Reason::None: return "none";
    case Reason::NoRenderTarget: return "no render target";
    case Reason::NotTexture2D: return "not a 2D texture";
    case Reason::Multisampled: return "multisampled";
    case Reason::ArrayOrMips: return "array or mips";
    case Reason::NoBackbuffer: return "backbuffer unknown";
    case Reason::SizeMismatch: return "size differs from the backbuffer";
    case Reason::FormatUnsupported: return "format not RGBA8";
    case Reason::EffectsFailed: return "ReShade effects failed";
    case Reason::Superseded: return "boundary moved after the capture";
    case Reason::CopyFailed: return "copy failed";
    default: return "?";
    }
}

// IHudlessSource::LastReason() text (static).
inline const char* ReasonText(Reason r)
{
    switch (r)
    {
    case Reason::None: return "";
    case Reason::EffectsFailed: return "ReShade effects failed";
    case Reason::Superseded: return "boundary moved after the capture";
    case Reason::CopyFailed: return "could not copy the render target";
    default: return "render target is not the full-size backbuffer";
    }
}

inline bool IsGuardReason(Reason r) { return r >= Reason::NoRenderTarget && r <= Reason::FormatUnsupported; }

// ---- pure decisions (cpu test) ---------------------------------------------------------------------------------------------
// The frame-level conditions for acting on an R1 event.
struct FrameFacts
{
    bool armed = false;         // the engine called Take() in the previous Present
    bool r1bJustSet = false;    // this draw is the R1b candidate (set by this draw)
    bool sawSceneStage = false; // a scene/post stage draw came before it in this frame
    bool handled = false;       // this frame already has a verdict (captured or rejected)
};
inline bool ShouldAct(const FrameFacts& f) { return f.armed && f.r1bJustSet && f.sawSceneStage && !f.handled; }

// RGBA8 family the engine can use (HudlessEngine.cpp TypedFormat maps typeless and sRGB to UNORM).
inline bool IsRgba8(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_TYPELESS;
}

// The bound render target against the swapchain backbuffer (bbWidth == 0: backbuffer unknown). `r` is the result of the bound
// target query (UiCapture.h GetBound) with `desc` filled whenever it got that far.
inline Reason CheckTarget(capture::Result r, const D3D11_TEXTURE2D_DESC& desc, uint32_t bbWidth, uint32_t bbHeight)
{
    switch (r)
    {
    case capture::Result::NoRenderTarget: return Reason::NoRenderTarget;
    case capture::Result::NotTexture2D: return Reason::NotTexture2D;
    case capture::Result::Multisampled: return Reason::Multisampled;
    case capture::Result::ArrayOrMips: return Reason::ArrayOrMips;
    case capture::Result::Ok: break;
    default: return Reason::NotTexture2D;
    }
    if (bbWidth == 0 || bbHeight == 0)
        return Reason::NoBackbuffer;
    if (desc.Width != bbWidth || desc.Height != bbHeight)
        return Reason::SizeMismatch;
    if (!IsRgba8(desc.Format))
        return Reason::FormatUnsupported;
    return Reason::None;
}

// ---- the source -------------------------------------------------------------------------------------------------------------
using EffectsFn = ext::reshade::ApplyResult (*)(ID3D11DeviceContext*, ID3D11Resource*);

struct Env
{
    EffectsFn effects = nullptr;           // null = no effects layer (presets not applied)
    capture::CopyFn copy = nullptr;        // un-hooked CopyResource; null = the context's own
    std::string (*effectsDetail)() = nullptr; // why the last effects call was not Applied (composes log lines only)
};

// One R1b event of the draw hook.
struct DrawEvent
{
    bool r1bSet = false;      // this draw became the R1b candidate (first UI draw into the backbuffer since the last scene stage)
    bool r1bCleared = false;  // this draw dropped the R1b candidate (a later scene/post stage superseded it)
    bool sawSceneStage = false;
    // With r1bSet: UI draws into another render target came before this draw since the last scene stage (R1 sits before R1b, the same
    // r1 != r1b test as the end-of-frame "anchor after off-screen UI draws" counter). Only reported to the engine, it changes nothing here.
    bool anchorLater = false;
    uint32_t bbWidth = 0, bbHeight = 0; // swapchain backbuffer of this frame (0 = unknown)
    uint64_t frame = 0;
};

// The hook glue, shared with the tests: what the boundary core's answer for one draw means to the live source. Called after
// Core::OnDraw for the draw (the draw itself has not run yet). Both flags false = nothing to do (the common case).
inline DrawEvent EventOf(const Core& core, const Core::DrawResult& res)
{
    DrawEvent ev;
    if (!res.counted || !res.r1bChanged)
        return ev;
    ev.r1bSet = res.r1b != 0 && res.r1b == res.ordinal;
    ev.r1bCleared = res.r1b == 0;
    ev.anchorLater = ev.r1bSet && res.cand.r1 != 0 && res.cand.r1 != res.r1b;
    ev.sawSceneStage = core.Tracker().Summary().sawSceneStage;
    ev.bbWidth = core.BackbufferShape().width;
    ev.bbHeight = core.BackbufferShape().height;
    ev.frame = core.Frame();
    return ev;
}

// What the frame looked like at the end (Present exit), for the counters only.
struct FrameNote
{
    bool sawSceneStage = false;
    uint32_t r1 = 0;  // first UI draw after the last scene stage (any render target); 0 = none
    uint32_t r1b = 0; // first UI draw into the backbuffer after the last scene stage; 0 = none
};
inline FrameNote NoteOf(const FrameOutcome& outcome)
{
    FrameNote n;
    n.sawSceneStage = outcome.record.summary.sawSceneStage;
    n.r1 = outcome.record.candidates.r1;
    n.r1b = outcome.r1b;
    return n;
}

struct Stats
{
    uint64_t frames = 0;       // frames the source acted in (the engine asked in the previous Present)
    uint64_t captured = 0;     // snapshots made
    uint64_t delivered = 0;    // Take() returned Ok
    uint64_t unused = 0;       // captured but the engine did not call Take() in that Present
    uint64_t noBoundary = 0;   // acting frames without a UI draw after the last scene stage (or without a scene stage)
    uint64_t offscreenOnly = 0; // acting frames whose UI draws after the last scene stage all went to other render targets
    uint64_t anchorLater = 0;  // acting frames where R1b came after R1 (the old R1 rule would have hit a preview render target)
    uint64_t rejected = 0;
    std::array<uint64_t, kReasons> rejectedBy {};
    std::array<uint64_t, ext::reshade::kApplyResults> effects {};
    uint64_t takes = 0;
};

class LiveSource
{
  public:
    static constexpr uint64_t kSummaryEvery = 600; // acting frames
    static constexpr uint64_t kMinStateLines = 300; // frames between two state-change lines

    explicit LiveSource(Env env) : _env(env) {}

    // ---- render thread, before the R1 draw runs -----------------------------------------------------------------------
    void OnDraw(ID3D11DeviceContext* c, const DrawEvent& ev)
    {
        std::lock_guard lock(_mutex);
        // The frame's final R1b decides the same-frame fact for the engine (a later R1b replaces it, a dropped one takes it away)
        if (ev.r1bSet)
            _anchorLaterFrame = ev.anchorLater && ev.sawSceneStage;
        else if (ev.r1bCleared)
            _anchorLaterFrame = false;
        if (ev.r1bCleared && _captured)
        {
            // The effects and the copy already happened at the earlier candidate: this frame cannot be used.
            _captured = false;
            Reject(Reason::Superseded);
            return;
        }
        FrameFacts f;
        f.armed = _armed;
        f.r1bJustSet = ev.r1bSet;
        f.sawSceneStage = ev.sawSceneStage;
        f.handled = _handled;
        if (!ShouldAct(f))
            return;
        _hadEvent = true;

        capture::Bound bound;
        const capture::Result query = capture::GetBound(c, bound);
        const Reason guard = CheckTarget(query, bound.desc, ev.bbWidth, ev.bbHeight);
        if (guard != Reason::None)
        {
            Reject(guard); // no side effects
            return;
        }

        Microsoft::WRL::ComPtr<ID3D11Device> device;
        c->GetDevice(&device);
        if (!device || !capture::EnsureTexture(device.Get(), bound, false, _snapshot))
        {
            Reject(Reason::CopyFailed);
            return;
        }

        const ext::reshade::ApplyResult applied = _env.effects ? _env.effects(c, bound.texture.Get()) : ext::reshade::ApplyResult::NotEnabled;
        ++_stats.effects[static_cast<size_t>(applied)];
        _lastEffects = applied;
        _haveEffects = true;
        if (applied == ext::reshade::ApplyResult::Failed)
        {
            Reject(Reason::EffectsFailed);
            return;
        }

        if (_env.copy)
            _env.copy(c, _snapshot.Get(), bound.texture.Get());
        else
            c->CopyResource(_snapshot.Get(), bound.texture.Get());

        _format = capture::TypedFormat(bound);
        _width = bound.desc.Width;
        _height = bound.desc.Height;
        _sequence = ev.frame;
        _captured = true;
        _handled = true;
        _consumed = false;
        ++_stats.captured;
    }

    // ---- Present thread -------------------------------------------------------------------------------------------------
    ext::hudless::TakeResult Take(ext::hudless::FrameImage& out)
    {
        std::lock_guard lock(_mutex);
        out = {};
        out.anchoredAfterOffscreenUi = _anchorLaterFrame;
        _tookThisFrame = true;
        ++_stats.takes;
        if (_captured && !_consumed && _snapshot)
        {
            _consumed = true;
            out.texture = _snapshot;
            out.format = _format;
            out.width = _width;
            out.height = _height;
            out.sequence = _sequence;
            ++_stats.delivered;
            _lastReason = "";
            return ext::hudless::TakeResult::Ok;
        }
        if (_handled && !_captured)
        {
            _lastReason = ReasonText(_reason);
            return ext::hudless::TakeResult::Rejected;
        }
        _lastReason = _armed ? "no boundary in this frame" : "waiting for the engine to ask for frames";
        return ext::hudless::TakeResult::NotThisFrame;
    }

    const char* LastReason()
    {
        std::lock_guard lock(_mutex);
        return _lastReason;
    }

    // Bridge Present exit. Accounts the frame, forgets it, arms the next one iff Take() ran in this Present. Returns the log lines
    // that are due ("" = none, lines separated by '\n'). `note` = how the frame looked (counters only).
    std::string EndFrame(const FrameNote& note = {})
    {
        std::lock_guard lock(_mutex);
        std::string lines;
        ++_framesSinceStateLine;
        if (_armed)
        {
            ++_stats.frames;
            ++_window;
            if (_captured && !_consumed)
                ++_stats.unused;
            if (!_hadEvent && !_handled)
            {
                // No R1b event acted: either no UI draw after the last scene stage, or all of them went to other render targets.
                if (note.sawSceneStage && note.r1 != 0 && note.r1b == 0)
                    ++_stats.offscreenOnly;
                else
                    ++_stats.noBoundary;
            }
            if (note.sawSceneStage && note.r1b != 0 && note.r1b != note.r1)
                ++_stats.anchorLater;
        }

        const bool wasArmed = _armed;
        _armed = _tookThisFrame;
        _tookThisFrame = false;
        _captured = _handled = _consumed = _hadEvent = _anchorLaterFrame = false;
        _reason = Reason::None;
        ++_framesSinceEffectsLine;

        auto addLine = [&lines](const std::string& text)
        {
            if (!lines.empty())
                lines += '\n';
            lines += text;
        };

        // State changes, rate limited (the first one of each kind is always written).
        if (_armed != _announcedArmed && (!_announced || _framesSinceStateLine >= kMinStateLines))
        {
            addLine(_armed ? "FFXIV UI boundary: live capture armed (the engine asks for UI-free frames)"
                           : "FFXIV UI boundary: live capture idle (the engine does not ask for frames: frame generation off, "
                             "unsupported provider, debug tint or coverage guard)");
            _announced = true;
            _announcedArmed = _armed;
            _framesSinceStateLine = 0;
        }
        if (wasArmed && _haveEffects)
        {
            if (!_effectsAnnounced)
            {
                _announcedEffects = _lastEffects; // baseline, shown by the summary
                _effectsAnnounced = true;
            }
            else if (_lastEffects != _announcedEffects && _framesSinceEffectsLine >= kMinStateLines)
            {
                std::string text = std::string("FFXIV UI boundary: ReShade presets at the boundary: ") +
                                   ext::reshade::ApplyResultName(_announcedEffects) + " -> " + ext::reshade::ApplyResultName(_lastEffects);
                if (_lastEffects != ext::reshade::ApplyResult::Applied && _env.effectsDetail)
                    text += " (" + _env.effectsDetail() + ")";
                addLine(text);
                _announcedEffects = _lastEffects;
                _framesSinceEffectsLine = 0;
            }
        }

        if (_window >= kSummaryEvery)
        {
            addLine(SummaryLocked());
            _window = 0;
        }
        return lines;
    }

    // The engine stopped caring (feature off) or the device went away: drops the snapshot texture.
    void Release()
    {
        std::lock_guard lock(_mutex);
        _snapshot.Reset();
        _armed = _tookThisFrame = _captured = _handled = _consumed = _hadEvent = _anchorLaterFrame = false;
    }

    Stats GetStats()
    {
        std::lock_guard lock(_mutex);
        return _stats;
    }

    std::string Summary()
    {
        std::lock_guard lock(_mutex);
        return SummaryLocked();
    }

    bool Armed()
    {
        std::lock_guard lock(_mutex);
        return _armed;
    }

  private:

    void Reject(Reason reason)
    {
        _handled = true;
        _reason = reason;
        ++_stats.rejected;
        ++_stats.rejectedBy[static_cast<size_t>(reason)];
    }

    std::string SummaryLocked() const
    {
        std::string reasons;
        for (size_t i = 1; i < kReasons; ++i)
            if (_stats.rejectedBy[i])
            {
                if (!reasons.empty())
                    reasons += ", ";
                reasons += std::string(ReasonName(static_cast<Reason>(i))) + " " + std::to_string(_stats.rejectedBy[i]);
            }
        const auto& e = _stats.effects;
        std::string line = "FFXIV UI boundary: " + std::to_string(_stats.frames) + " frames: captured " + std::to_string(_stats.captured) +
                           " (delivered " + std::to_string(_stats.delivered) + ", unused " + std::to_string(_stats.unused) + "), rejected " +
                           std::to_string(_stats.rejected) + (reasons.empty() ? "" : " (" + reasons + ")") + ", no boundary " +
                           std::to_string(_stats.noBoundary) + ", UI only off the backbuffer " + std::to_string(_stats.offscreenOnly) +
                           ", anchor after off-screen UI draws " + std::to_string(_stats.anchorLater) + "; ReShade presets: applied " +
                           std::to_string(e[static_cast<size_t>(ext::reshade::ApplyResult::Applied)]) + ", nothing to apply " +
                           std::to_string(e[static_cast<size_t>(ext::reshade::ApplyResult::NothingToApply)]) + ", failed " +
                           std::to_string(e[static_cast<size_t>(ext::reshade::ApplyResult::Failed)]) + ", other " +
                           std::to_string(e[static_cast<size_t>(ext::reshade::ApplyResult::NotEnabled)] +
                                          e[static_cast<size_t>(ext::reshade::ApplyResult::NoReShade)] +
                                          e[static_cast<size_t>(ext::reshade::ApplyResult::NoRuntime)]);
        return line;
    }

    Env _env;
    std::mutex _mutex;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _snapshot; // the one reused texture, recreated on size / format change

    // frame state
    bool _armed = false, _tookThisFrame = false;
    bool _handled = false, _captured = false, _consumed = false, _hadEvent = false;
    bool _anchorLaterFrame = false; // the frame's R1b came after off-screen UI draws (handed to the engine with Take())
    Reason _reason = Reason::None;
    DXGI_FORMAT _format = DXGI_FORMAT_UNKNOWN;
    uint32_t _width = 0, _height = 0;
    uint64_t _sequence = 0;
    const char* _lastReason = "waiting for the engine to ask for frames";

    // accounting
    Stats _stats;
    uint64_t _window = 0;
    uint64_t _framesSinceStateLine = 1000000, _framesSinceEffectsLine = 1000000;
    bool _announced = false, _announcedArmed = false;
    bool _haveEffects = false, _effectsAnnounced = false;
    ext::reshade::ApplyResult _lastEffects = ext::reshade::ApplyResult::NotEnabled;
    ext::reshade::ApplyResult _announcedEffects = ext::reshade::ApplyResult::NotEnabled;
};
} // namespace ext::ffxiv::ui::live
