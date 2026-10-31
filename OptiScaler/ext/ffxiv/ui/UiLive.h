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
//  * R1b by default (S10 E2): the first UI-tagged draw after the last scene/post stage whose render target IS the swapchain backbuffer
//    (UiBoundaryRules.h). UI draws into other render targets before it (an off-screen UI target, 352x352, drawn in every in-world frame;
//    probably the minimap) are ignored, not rejected: rejecting them made ReShade's own pass filter the HUD on those frames and skipped
//    it on the others (visible flicker). No such draw in the frame = NotThisFrame. R1/R2/R3 never act.
//  * [HudLess] AnchorRule = early (SetAnchorRule): on a frame whose R1 goes to such an off-screen target, the capture is taken at R1,
//    before that draw runs, by copying the swapchain BACKBUFFER (fetched for the copy only, Env::backbuffer, and required to be the very
//    texture the core compares render targets with); R1b of that frame then does nothing. It is taken only when the effects call would
//    apply nothing there (Env::predictEffects): presets that would run, or fail, keep the frame on R1b so their place never changes.
//    Any other doubt (no backbuffer identity, a failed fetch, another texture, the guard, no snapshot texture) leaves the frame to R1b
//    untouched (counted by cause in the summary). A later scene/post stage that drops R1 before any UI draw reached the backbuffer takes
//    the early action back (nothing was applied there) and the frame acts again at its next R1 off-screen or R1b (re-armed); one that
//    drops R1b after it rejects the frame (Superseded), as with r1b; a frame that never draws UI into the backbuffer after it gets today's
//    outcome (NotThisFrame, counted as UI only off the backbuffer; its early counters are taken back, the summary's early detail counts it).
//    Frames with R1 == R1b take the R1b path whatever the rule.
//  * Acts only if the engine asked for a frame in the PREVIOUS Present (Take() was called): frame generation off, an unsupported
//    provider, the debug tint or the coverage guard make the engine skip Take(); then nothing is applied or copied.
//  * Acts only on frames that showed a scene stage (menu / loading frames have no UI-free image).
//  * The engine may say in a Present that it will not use the next frame's image (SkipNextCopy: a tripped coverage guard probes one
//    frame in N). That frame is acted on as usual (the presets are applied at the boundary, so their place never alternates with
//    ReShade's own end-of-frame pass) but nothing is copied, and Take() returns NotThisFrame saying so. One frame per call.
//  * Once per frame. A rejected frame has NO side effects when the guard fails; a frame whose R1b candidate is superseded after the
//    capture is rejected (the effects already ran: Session 1 never saw this, 0 in 24,000 frames).
//  * Order inside a capture: snapshot texture ensured -> effects -> copy, so the snapshot contains the preset-processed target
//    and nothing the later (UI) draws write.
// Threading: OnDraw on the render thread, Take/EndFrame on the Present thread; one mutex (held across the effects call: it runs
// once per frame).

#include "ext/ffxiv/ui/UiBoundaryCore.h"
#include "ext/ffxiv/ui/UiCapture.h"
#include "ext/hudless/HudlessMode.h"
#include "ext/hudless/HudlessSource.h"
#include "ext/reshade/ReShadeBoundary.h"

#include <algorithm>
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

using ext::hudless::AnchorRule;

// The early anchor ([HudLess] AnchorRule = early): this draw set R1 into a render target that is not the backbuffer (the frame
// conditions are the same as for the R1b draw).
inline bool ShouldActEarly(const FrameFacts& f, AnchorRule rule, bool r1OffscreenJustSet)
{
    return rule == AnchorRule::Early && f.armed && r1OffscreenJustSet && f.sawSceneStage && !f.handled;
}

// The ReShade gate of the early anchor, on what the effects call would return there (ReShadeBoundary.h PresetsWouldApply): only a
// frame whose boundary applies nothing is moved. Applied (presets would run in place) and Failed keep R1b.
inline bool EarlyAllowedBy(ext::reshade::ApplyResult predicted)
{
    switch (predicted)
    {
    case ext::reshade::ApplyResult::NotEnabled:
    case ext::reshade::ApplyResult::NoReShade:
    case ext::reshade::ApplyResult::NoRuntime:
    case ext::reshade::ApplyResult::NothingToApply:
        return true;
    default:
        return false;
    }
}

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
using PredictFn = ext::reshade::ApplyResult (*)(ID3D11DeviceContext*);
// The swapchain backbuffer with a reference the caller releases (the early anchor holds it for the copy only); false = unknown.
using BackbufferTextureFn = bool (*)(ID3D11Texture2D** texture);

struct Env
{
    EffectsFn effects = nullptr;           // null = no effects layer (presets not applied)
    capture::CopyFn copy = nullptr;        // un-hooked CopyResource; null = the context's own
    std::string (*effectsDetail)() = nullptr; // why the last effects call was not Applied (composes log lines only)
    PredictFn predictEffects = nullptr;    // what `effects` would return now, without running it; null = unknown
    BackbufferTextureFn backbuffer = nullptr; // the early anchor's copy source; null = no early anchor
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
    // R1 off-screen (the early anchor): this draw set R1 into another render target than the backbuffer / dropped it again
    bool r1Offscreen = false;
    bool r1OffscreenCleared = false;
    uint32_t bbWidth = 0, bbHeight = 0; // swapchain backbuffer of this frame (0 = unknown)
    const void* bbPointer = nullptr;    // ... and its identity (null = unknown)
    uint64_t frame = 0;
};

// The hook glue, shared with the tests: what the boundary core's answer for one draw means to the live source. Called after
// Core::OnDraw for the draw (the draw itself has not run yet). Both flags false = nothing to do (the common case).
inline DrawEvent EventOf(const Core& core, const Core::DrawResult& res)
{
    DrawEvent ev;
    if (!res.counted || (!res.r1bChanged && !res.r1OffscreenChanged))
        return ev;
    if (res.r1bChanged)
    {
        ev.r1bSet = res.r1b != 0 && res.r1b == res.ordinal;
        ev.r1bCleared = res.r1b == 0;
        ev.anchorLater = ev.r1bSet && res.cand.r1 != 0 && res.cand.r1 != res.r1b;
    }
    if (res.r1OffscreenChanged)
    {
        ev.r1Offscreen = res.r1Offscreen != 0 && res.r1Offscreen == res.ordinal;
        ev.r1OffscreenCleared = res.r1Offscreen == 0;
    }
    ev.sawSceneStage = core.Tracker().Summary().sawSceneStage;
    ev.bbWidth = core.BackbufferShape().width;
    ev.bbHeight = core.BackbufferShape().height;
    ev.bbPointer = core.BackbufferPointer();
    ev.frame = core.Frame();
    return ev;
}

// The draws the hook hands to the live source (FfxivUiBoundary.cpp OnDraw and the WARP harness use this one test).
inline bool IsLiveEvent(const DrawEvent& ev) { return ev.r1bSet || ev.r1bCleared || ev.r1Offscreen || ev.r1OffscreenCleared; }

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

// Why the early anchor did not act at an R1 off-screen draw (log only: the frame stayed with R1b), in the order ActEarly checks them
enum class EarlyDecline : uint8_t
{
    UnknownBackbuffer, // the core knows no backbuffer identity or size this frame (or there is no fetch)
    Presets,           // ReShade presets would run (or fail) at the boundary, or what they would do is unknown
    Fetch,             // the backbuffer fetch failed or named another texture than the core's backbuffer
    Target,            // the backbuffer's description fails the render-target guard
    Texture,           // the snapshot texture could not be made
    Count,
};
inline constexpr size_t kEarlyDeclines = static_cast<size_t>(EarlyDecline::Count);

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
    uint64_t copySkipped = 0;  // armed frames whose copy the engine did not want (SkipNextCopy: a tripped guard probes one frame in N)
    uint64_t earlyAnchors = 0; // frames captured at R1 from the backbuffer ([HudLess] AnchorRule = early)
    uint64_t earlyRearmed = 0; // early actions taken back: a later scene stage dropped R1 before any UI draw reached the backbuffer
    uint64_t earlyOffscreenOnly = 0; // early actions whose frame drew no UI into the backbuffer afterwards (taken back at the frame end)
    std::array<uint64_t, kEarlyDeclines> earlyDeclined {}; // R1 off-screen draws where the early anchor did not act, by cause
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
        if (ev.r1bSet && _early)
            _earlyConfirmed = true; // the frame draws UI into the backbuffer after the early capture: it stands for this R1b
        if (ev.r1OffscreenCleared && !ev.r1bCleared && _early && !_earlyConfirmed)
        {
            // A later scene/post stage dropped the early R1 before any UI draw reached the backbuffer. Nothing was applied there (the
            // presets gate lets only frames with nothing to apply through), so unlike a superseded R1b the frame is not lost: the
            // early action is taken back and the frame's next R1 off-screen or R1b acts, as it would with r1b.
            UndoEarly();
            return;
        }
        if (ev.r1bCleared && (_captured || _copySkipped))
        {
            // The effects (and the copy, unless the engine skipped it) already happened at the earlier candidate: this frame cannot be
            // used, and it is counted the same way whether or not it was copied. An early capture confirmed by its R1b is stale once that
            // R1b is dropped (the same stage drops R1; before its R1b the take-back above applies).
            _captured = false;
            _copySkipped = false;
            _early = false;
            Reject(Reason::Superseded);
            return;
        }
        FrameFacts f;
        f.armed = _armed;
        f.r1bJustSet = ev.r1bSet;
        f.sawSceneStage = ev.sawSceneStage;
        f.handled = _handled;
        if (ev.r1Offscreen)
        {
            if (ShouldActEarly(f, _rule, true))
                ActEarly(c, ev);
            return;
        }
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

        if (!_copyArmed)
        {
            // The engine does not want this frame's image: the presets ran where they always run, the copy is left out
            _handled = true;
            _copySkipped = true;
            ++_stats.copySkipped;
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
        if (_early && !_earlyConfirmed)
        {
            // The frame acted on at R1 never drew UI into the backbuffer afterwards: today's outcome for it (UI only off the backbuffer)
            _lastReason = "no boundary in this frame";
            return ext::hudless::TakeResult::NotThisFrame;
        }
        if (_copySkipped)
        {
            _lastReason = "copy skipped (the coverage guard probes one frame in N)";
            return ext::hudless::TakeResult::NotThisFrame;
        }
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

    // [HudLess] AnchorRule, live. Present thread, between frames.
    void SetAnchorRule(AnchorRule rule)
    {
        std::lock_guard lock(_mutex);
        _rule = rule;
    }

    // The engine will not use the next frame's image (IHudlessSource::SkipNextCopy). Present thread, after Take(); covers one frame.
    void SkipNextCopy()
    {
        std::lock_guard lock(_mutex);
        _skipNext = true;
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
        // an early action whose frame drew no UI into the backbuffer afterwards counts as the frame it is for today (see Take): its
        // counters are taken back, and only the early detail of the summary keeps it (its copy was made for nothing)
        const bool earlyAlone = _early && !_earlyConfirmed;
        if (earlyAlone)
        {
            TakeBackEarlyCounts();
            ++_stats.earlyOffscreenOnly;
        }
        if (_armed)
        {
            ++_stats.frames;
            ++_window;
            if (_captured && !_consumed && !earlyAlone)
                ++_stats.unused;
            if ((!_hadEvent && !_handled) || earlyAlone)
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
        _copyArmed = !_skipNext;
        _skipNext = false;
        _captured = _handled = _consumed = _hadEvent = _anchorLaterFrame = _copySkipped = false;
        _early = _earlyConfirmed = false;
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

        // [HudLess] AnchorRule in force (from the menu or the ini): one line per change
        if (_rule != _announcedRule)
        {
            addLine(std::string("FFXIV UI boundary: AnchorRule ") + ext::hudless::AnchorRuleName(_announcedRule) + " -> " +
                    ext::hudless::AnchorRuleName(_rule));
            _announcedRule = _rule;
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
        _armed = _tookThisFrame = _captured = _handled = _consumed = _hadEvent = _anchorLaterFrame = _copySkipped = _skipNext = false;
        _early = _earlyConfirmed = false;
        _copyArmed = true;
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
    // [HudLess] AnchorRule = early, at the draw that set R1 into an off-screen render target (it has not run yet). Every check comes
    // before any side effect; a failed one leaves the frame to R1b as if the rule were r1b.
    void ActEarly(ID3D11DeviceContext* c, const DrawEvent& ev)
    {
        auto decline = [this](EarlyDecline why) { ++_stats.earlyDeclined[static_cast<size_t>(why)]; };
        if (ev.bbPointer == nullptr || ev.bbWidth == 0 || ev.bbHeight == 0 || !_env.backbuffer)
            return decline(EarlyDecline::UnknownBackbuffer); // the core does not know the backbuffer: no off-screen target to tell apart
        // What the effects call would return here. The presets are applied in place on the bound target, which at R1 is the off-screen
        // one: a frame with presets to apply (or a call that would fail) keeps R1b, where they always run.
        ext::reshade::ApplyResult predicted = ext::reshade::ApplyResult::NotEnabled; // no effects layer: what the R1b path records
        if (_env.effects)
        {
            if (!_env.predictEffects)
                return decline(EarlyDecline::Presets);
            predicted = _env.predictEffects(c);
        }
        if (!EarlyAllowedBy(predicted))
            return decline(EarlyDecline::Presets);

        Microsoft::WRL::ComPtr<ID3D11Texture2D> backbuffer; // held for this capture only (a kept reference makes ResizeBuffers fail)
        if (!_env.backbuffer(backbuffer.GetAddressOf()) || !backbuffer || static_cast<const void*>(backbuffer.Get()) != ev.bbPointer)
            return decline(EarlyDecline::Fetch);
        capture::Bound bound;
        if (CheckTarget(capture::Describe(backbuffer.Get(), bound), bound.desc, ev.bbWidth, ev.bbHeight) != Reason::None)
            return decline(EarlyDecline::Target); // R1b's guard sees the same backbuffer and decides
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        c->GetDevice(&device);
        if (!device || !capture::EnsureTexture(device.Get(), bound, false, _snapshot))
            return decline(EarlyDecline::Texture);

        _hadEvent = true;
        _early = true;
        _earlyEffects = predicted; // what UndoEarly takes back
        _earlyPrevEffects = _lastEffects;
        _earlyPrevHaveEffects = _haveEffects;
        ++_stats.effects[static_cast<size_t>(predicted)]; // the effects result of this boundary, as the R1b path records its call
        _lastEffects = predicted;
        _haveEffects = true;
        if (!_copyArmed)
        {
            // The engine does not want this frame's image: nothing to apply, nothing to copy
            _handled = true;
            _copySkipped = true;
            ++_stats.copySkipped;
            return;
        }

        if (_env.copy)
            _env.copy(c, _snapshot.Get(), backbuffer.Get());
        else
            c->CopyResource(_snapshot.Get(), backbuffer.Get());

        _format = capture::TypedFormat(bound);
        _width = bound.desc.Width;
        _height = bound.desc.Height;
        _sequence = ev.frame;
        _captured = true;
        _handled = true;
        _consumed = false;
        ++_stats.captured;
        ++_stats.earlyAnchors;
    }

    // Every counter ActEarly moved, and the effects state it recorded, taken back (UndoEarly; EndFrame for an early frame that drew no UI
    // into the backbuffer afterwards). Reads `_captured` / `_copySkipped`, so it runs before they are reset.
    void TakeBackEarlyCounts()
    {
        --_stats.effects[static_cast<size_t>(_earlyEffects)];
        _lastEffects = _earlyPrevEffects;
        _haveEffects = _earlyPrevHaveEffects;
        if (_captured)
        {
            --_stats.captured;
            --_stats.earlyAnchors;
        }
        if (_copySkipped)
            --_stats.copySkipped;
    }

    // Takes back an early action whose R1 a later scene/post stage dropped before R1b (OnDraw): its counters and the frame state, so the
    // frame is as if the early draw had not acted. Its copy stays in the snapshot texture, unused (the next action overwrites it).
    void UndoEarly()
    {
        TakeBackEarlyCounts();
        _captured = _copySkipped = _handled = _hadEvent = _early = false;
        ++_stats.earlyRearmed;
    }

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
        // The early anchor's detail, only once it has run (never with r1b): its take-backs and why it did not act
        std::string early;
        const auto& d = _stats.earlyDeclined;
        if (_stats.earlyRearmed != 0 || _stats.earlyOffscreenOnly != 0 || std::any_of(d.begin(), d.end(), [](uint64_t n) { return n != 0; }))
        {
            auto n = [&d](EarlyDecline why) { return std::to_string(d[static_cast<size_t>(why)]); };
            early = " (re-armed " + std::to_string(_stats.earlyRearmed) + ", off-screen only " + std::to_string(_stats.earlyOffscreenOnly) +
                    "; declined: backbuffer unknown " + n(EarlyDecline::UnknownBackbuffer) +
                    ", presets " + n(EarlyDecline::Presets) + ", fetch " + n(EarlyDecline::Fetch) + ", target " + n(EarlyDecline::Target) +
                    ", texture " + n(EarlyDecline::Texture) + ")";
        }
        const auto& e = _stats.effects;
        std::string line = "FFXIV UI boundary: " + std::to_string(_stats.frames) + " frames: captured " + std::to_string(_stats.captured) +
                           " (delivered " + std::to_string(_stats.delivered) + ", unused " + std::to_string(_stats.unused) + "), rejected " +
                           std::to_string(_stats.rejected) + (reasons.empty() ? "" : " (" + reasons + ")") + ", no boundary " +
                           std::to_string(_stats.noBoundary) + ", UI only off the backbuffer " + std::to_string(_stats.offscreenOnly) +
                           ", anchor after off-screen UI draws " + std::to_string(_stats.anchorLater) + ", copy skipped by the engine " +
                           std::to_string(_stats.copySkipped) + ", early anchors " + std::to_string(_stats.earlyAnchors) + early +
                           "; ReShade presets: applied " +
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
    bool _skipNext = false;    // SkipNextCopy() in this Present: the next frame is acted on without a copy
    bool _copyArmed = true;    // this frame may be copied (no SkipNextCopy in the previous Present)
    bool _copySkipped = false; // this frame was acted on and its copy left out
    bool _early = false;          // this frame was acted on at R1 (the early anchor)
    bool _earlyConfirmed = false; // ... and drew UI into the backbuffer afterwards (its R1b came)
    ext::reshade::ApplyResult _earlyEffects = ext::reshade::ApplyResult::NotEnabled; // the effects result the early action recorded
    ext::reshade::ApplyResult _earlyPrevEffects = ext::reshade::ApplyResult::NotEnabled; // ... and the state before it (UndoEarly)
    bool _earlyPrevHaveEffects = false;
    AnchorRule _rule = AnchorRule::R1b, _announcedRule = AnchorRule::R1b;
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
