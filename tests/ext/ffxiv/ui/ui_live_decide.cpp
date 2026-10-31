// S10-E / E2: the live source's decisions without a GPU: when it acts (R1b only, scene stage seen, engine asked in the previous
// Present, once per frame), what the render target guard accepts (size, format, sample count, array/mips, backbuffer known),
// the arming rule driven by Take(), the reason texts the engine shows, and the log lines (state changes, 600-frame summary).
#define NOMINMAX
#include "check.h"

#include "ext/ffxiv/ui/UiLive.h"

#include <string>

using namespace ext::ffxiv::ui;
using namespace ext::ffxiv::ui::live;
using ext::hudless::FrameImage;
using ext::hudless::TakeResult;
using ext::reshade::ApplyResult;

namespace
{
D3D11_TEXTURE2D_DESC Desc(UINT w, UINT h, DXGI_FORMAT f)
{
    D3D11_TEXTURE2D_DESC d {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    return d;
}

void Acts()
{
    FrameFacts f;
    f.armed = f.r1bJustSet = f.sawSceneStage = true;
    CHECK(ShouldAct(f));
    { auto g = f; g.armed = false; CHECK(!ShouldAct(g)); }          // the engine did not ask in the previous Present
    { auto g = f; g.r1bJustSet = false; CHECK(!ShouldAct(g)); }    // not the R1b candidate
    { auto g = f; g.sawSceneStage = false; CHECK(!ShouldAct(g)); } // menu / loading frame
    { auto g = f; g.handled = true; CHECK(!ShouldAct(g)); }        // once per frame
}

void Guard()
{
    const auto ok = Desc(3840, 2160, DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(CheckTarget(capture::Result::Ok, ok, 3840, 2160) == Reason::None);
    CHECK(CheckTarget(capture::Result::Ok, Desc(3840, 2160, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB), 3840, 2160) == Reason::None);
    CHECK(CheckTarget(capture::Result::Ok, Desc(3840, 2160, DXGI_FORMAT_R8G8B8A8_TYPELESS), 3840, 2160) == Reason::None);
    // the 103 Session 1 frames: character previews. Since S10-E2 they never reach the guard (the anchor is a UI draw into the
    // backbuffer, so UI draws into a preview target are skipped); the guard still tells them apart.
    CHECK(CheckTarget(capture::Result::Ok, Desc(352, 352, DXGI_FORMAT_B8G8R8A8_UNORM), 3840, 2160) == Reason::SizeMismatch);
    CHECK(CheckTarget(capture::Result::Ok, Desc(1024, 1024, DXGI_FORMAT_B8G8R8A8_UNORM), 3840, 2160) == Reason::SizeMismatch);
    CHECK(CheckTarget(capture::Result::Ok, Desc(3840, 2160, DXGI_FORMAT_B8G8R8A8_UNORM), 3840, 2160) == Reason::FormatUnsupported);
    CHECK(CheckTarget(capture::Result::Ok, Desc(3840, 2160, DXGI_FORMAT_R10G10B10A2_UNORM), 3840, 2160) == Reason::FormatUnsupported);
    CHECK(CheckTarget(capture::Result::Ok, Desc(3840, 2160, DXGI_FORMAT_R16G16B16A16_FLOAT), 3840, 2160) == Reason::FormatUnsupported);
    CHECK(CheckTarget(capture::Result::Ok, Desc(3840, 2159, DXGI_FORMAT_R8G8B8A8_UNORM), 3840, 2160) == Reason::SizeMismatch);
    CHECK(CheckTarget(capture::Result::Ok, ok, 0, 0) == Reason::NoBackbuffer); // backbuffer unknown: cannot verify
    CHECK(CheckTarget(capture::Result::NoRenderTarget, {}, 3840, 2160) == Reason::NoRenderTarget);
    CHECK(CheckTarget(capture::Result::NotTexture2D, {}, 3840, 2160) == Reason::NotTexture2D);
    CHECK(CheckTarget(capture::Result::Multisampled, ok, 3840, 2160) == Reason::Multisampled);
    CHECK(CheckTarget(capture::Result::ArrayOrMips, ok, 3840, 2160) == Reason::ArrayOrMips);

    // every guard reason shows the engine the same text
    for (Reason r : { Reason::NoRenderTarget, Reason::NotTexture2D, Reason::Multisampled, Reason::ArrayOrMips, Reason::NoBackbuffer,
                      Reason::SizeMismatch, Reason::FormatUnsupported })
    {
        CHECK(IsGuardReason(r));
        CHECK(std::string(ReasonText(r)) == "render target is not the full-size backbuffer");
    }
    CHECK(!IsGuardReason(Reason::EffectsFailed) && !IsGuardReason(Reason::Superseded) && !IsGuardReason(Reason::None));
    CHECK(std::string(ReasonText(Reason::EffectsFailed)) == "ReShade effects failed");
}

void Arming()
{
    LiveSource s(Env {});
    FrameImage img;
    CHECK(!s.Armed());

    // nothing asked yet: no frame, and the call itself arms the next frame
    CHECK(s.Take(img) == TakeResult::NotThisFrame);
    CHECK(img.texture == nullptr);
    CHECK(std::string(s.LastReason()).find("waiting") != std::string::npos);
    auto lines = s.EndFrame();
    CHECK(s.Armed());
    CHECK(lines.find("armed") != std::string::npos);

    // the engine did not call Take() in that Present (frame generation off, coverage guard, ...): disarmed again
    lines = s.EndFrame();
    CHECK(!s.Armed());
    CHECK(lines.empty()); // rate limited: no second state line right away
    CHECK(s.GetStats().frames == 1);
    CHECK(s.GetStats().noBoundary == 1);

    // a frame without an R1b event: the engine sees "no boundary", the frame is counted
    CHECK(s.Take(img) == TakeResult::NotThisFrame);
    s.EndFrame();
    CHECK(s.Armed());
    CHECK(s.Take(img) == TakeResult::NotThisFrame);
    CHECK(std::string(s.LastReason()) == "no boundary in this frame");
    s.EndFrame();
    CHECK(s.GetStats().noBoundary == 2);
    CHECK(s.GetStats().takes == 3);
}

// The hook glue: Core::DrawResult -> DrawEvent (the same function the game hook calls).
void Events()
{
    Core core;
    Core::DrawResult res;
    res.counted = true;
    res.ordinal = 40;

    // nothing changed for R1b with this draw (the common case): no event
    CHECK(!EventOf(core, res).r1bSet && !EventOf(core, res).r1bCleared);

    // R1b set by this very draw
    res.r1bChanged = true;
    res.r1b = 40;
    {
        const DrawEvent ev = EventOf(core, res);
        CHECK(ev.r1bSet && !ev.r1bCleared);
        CHECK_EQ(ev.frame, core.Frame());
        CHECK(!ev.sawSceneStage); // no draw went through this Core: the tracker has seen no scene stage
    }

    // R1b dropped by this draw (a later scene/post stage)
    res.r1b = 0;
    {
        const DrawEvent ev = EventOf(core, res);
        CHECK(!ev.r1bSet && ev.r1bCleared);
    }

    // R1b changed but the candidate is another draw (cannot happen with the tracker; the mapping must still not claim "set here")
    res.r1b = 12;
    {
        const DrawEvent ev = EventOf(core, res);
        CHECK(!ev.r1bSet && !ev.r1bCleared);
    }

    // a draw the core ignored (other thread, before the first Present): no event even if the flags are set
    res.counted = false;
    res.r1b = 40;
    {
        const DrawEvent ev = EventOf(core, res);
        CHECK(!ev.r1bSet && !ev.r1bCleared);
    }
}

// The same-frame "anchor after off-screen UI draws" fact: computed by the hook glue from the boundary's answer for the R1b draw (the
// same r1 != r1b condition as the end-of-frame counter), kept per frame by the live source and handed to the engine with Take().
void AnchorFact()
{
    Core core;
    Core::DrawResult res;
    res.counted = true;
    res.ordinal = 40;
    res.r1bChanged = true;
    res.r1b = 40;
    res.cand.r1 = 12; // a UI draw into another render target came first
    CHECK(EventOf(core, res).anchorLater);
    res.cand.r1 = 40; // the HUD came first
    CHECK(!EventOf(core, res).anchorLater);
    res.cand.r1 = 0;  // no R1 at all (cannot happen): nothing is claimed
    CHECK(!EventOf(core, res).anchorLater);
    res.cand.r1 = 12;
    res.r1b = 0; // the R1b candidate was dropped by this draw
    CHECK(!EventOf(core, res).anchorLater);

    LiveSource s(Env {});
    FrameImage img;
    DrawEvent ev;
    ev.r1bSet = true;
    ev.anchorLater = true;
    ev.sawSceneStage = true;
    s.OnDraw(nullptr, ev); // not armed yet: nothing acts, the fact is still kept for the engine's Take()
    CHECK(s.Take(img) == TakeResult::NotThisFrame);
    CHECK(img.anchoredAfterOffscreenUi);
    s.EndFrame();
    CHECK(s.Take(img) == TakeResult::NotThisFrame);
    CHECK(!img.anchoredAfterOffscreenUi); // forgotten at the end of the frame

    // Each case below is a frame of a source that has not been armed yet (an armed source would copy: no GPU here)
    auto flagAfter = [&](auto&& events)
    {
        LiveSource fresh(Env {});
        FrameImage out;
        events(fresh);
        fresh.Take(out);
        return out.anchoredAfterOffscreenUi;
    };
    // a menu frame (no scene stage seen) is not claimed
    DrawEvent menu = ev;
    menu.sawSceneStage = false;
    CHECK(!flagAfter([&](LiveSource& f) { f.OnDraw(nullptr, menu); }));
    // a later scene/post stage that drops the R1b candidate takes the fact with it
    DrawEvent cleared;
    cleared.r1bCleared = true;
    CHECK(!flagAfter(
        [&](LiveSource& f)
        {
            f.OnDraw(nullptr, ev);
            f.OnDraw(nullptr, cleared);
        }));
    // ... and the next R1b of the frame decides: first off-screen UI before it (claimed), then the HUD first (not)
    DrawEvent hudFirst = ev;
    hudFirst.anchorLater = false;
    CHECK(flagAfter(
        [&](LiveSource& f)
        {
            f.OnDraw(nullptr, hudFirst);
            f.OnDraw(nullptr, cleared);
            f.OnDraw(nullptr, ev);
        }));
    CHECK(!flagAfter(
        [&](LiveSource& f)
        {
            f.OnDraw(nullptr, ev);
            f.OnDraw(nullptr, cleared);
            f.OnDraw(nullptr, hudFirst);
        }));
    // an R1b that replaces the earlier one without a clear in between decides on its own
    CHECK(!flagAfter(
        [&](LiveSource& f)
        {
            f.OnDraw(nullptr, ev);
            f.OnDraw(nullptr, hudFirst);
        }));
    CHECK(flagAfter(
        [&](LiveSource& f)
        {
            f.OnDraw(nullptr, hudFirst);
            f.OnDraw(nullptr, ev);
        }));
    // released with the device: nothing stays set
    CHECK(!flagAfter(
        [&](LiveSource& f)
        {
            f.OnDraw(nullptr, ev);
            f.Release();
        }));
}

// The end-of-frame note: which counter an acting frame without a capture lands in, and the "anchor after off-screen draws" count.
void Notes()
{
    LiveSource s(Env {});
    FrameImage img;
    s.Take(img);
    s.EndFrame(); // armed from now on while Take() keeps being called

    auto frame = [&](FrameNote n)
    {
        s.Take(img);
        s.EndFrame(n);
    };
    frame({ true, 0, 0 });    // a scene frame without any UI draw (cutscene): no boundary
    frame({ false, 5, 5 });   // a menu frame (no scene stage): no boundary, whatever the UI did
    frame({ true, 9, 0 });    // UI draws after the last scene stage, all into other render targets (preview only)
    frame({ true, 9, 0 });
    frame({ true, 9, 15 });   // the preview came first, the HUD on the backbuffer later (anchor after off-screen UI draws) ...
    frame({ true, 9, 9 });    // ... and the HUD first
    const auto st = s.GetStats();
    CHECK_EQ(st.frames, 6u);
    CHECK_EQ(st.noBoundary, 4u);        // the first two, plus the last two (they had an R1b, but no event reached this source)
    CHECK_EQ(st.offscreenOnly, 2u);
    CHECK_EQ(st.anchorLater, 1u);
    CHECK_EQ(st.rejected, 0u);          // skipped preview draws are never rejections
    const auto summary = s.Summary();
    CHECK(summary.find("UI only off the backbuffer 2") != std::string::npos);
    CHECK(summary.find("anchor after off-screen UI draws 1") != std::string::npos);
    CHECK(summary.find("rejected 0") != std::string::npos);
}

// ---- the early anchor ([HudLess] AnchorRule = early) -------------------------------------------------------------------------------
// When it acts: the draw that set R1 into an off-screen render target, in a frame the engine asked for, after a scene stage, once per
// frame, and only with the rule set to early.
void EarlyActs()
{
    FrameFacts f;
    f.armed = f.sawSceneStage = true;
    CHECK(ShouldActEarly(f, AnchorRule::Early, true));
    CHECK(!ShouldActEarly(f, AnchorRule::R1b, true));   // the default never acts at R1
    CHECK(!ShouldActEarly(f, AnchorRule::Early, false)); // not the R1 off-screen draw
    { auto g = f; g.armed = false; CHECK(!ShouldActEarly(g, AnchorRule::Early, true)); }
    { auto g = f; g.sawSceneStage = false; CHECK(!ShouldActEarly(g, AnchorRule::Early, true)); }
    { auto g = f; g.handled = true; CHECK(!ShouldActEarly(g, AnchorRule::Early, true)); }
}

// The ReShade gate: the early anchor is taken only when the boundary would apply nothing (at R1 the bound target is the off-screen one,
// so presets that run would move); Applied or Failed keep the frame on R1b, where the presets always run.
void EarlyPresetsGate()
{
    CHECK(EarlyAllowedBy(ApplyResult::NotEnabled));
    CHECK(EarlyAllowedBy(ApplyResult::NoReShade));
    CHECK(EarlyAllowedBy(ApplyResult::NoRuntime));
    CHECK(EarlyAllowedBy(ApplyResult::NothingToApply));
    CHECK(!EarlyAllowedBy(ApplyResult::Applied));
    CHECK(!EarlyAllowedBy(ApplyResult::Failed));
}

// The hook glue for the R1 off-screen event, and which draws the hook hands to the live source.
void EarlyEvents()
{
    Core core;
    Core::DrawResult res;
    res.counted = true;
    res.ordinal = 9;
    {
        const DrawEvent ev = EventOf(core, res); // nothing changed with this draw
        CHECK(!ev.r1Offscreen && !ev.r1OffscreenCleared && !ev.r1bSet && !ev.r1bCleared);
        CHECK(!IsLiveEvent(ev));
    }

    res.r1OffscreenChanged = true;
    res.r1Offscreen = 9; // set by this very draw
    {
        const DrawEvent ev = EventOf(core, res);
        CHECK(ev.r1Offscreen && !ev.r1OffscreenCleared);
        CHECK(!ev.r1bSet && !ev.r1bCleared && !ev.anchorLater); // R1b is untouched
        CHECK_EQ(ev.frame, core.Frame());
        CHECK(ev.bbPointer == core.BackbufferPointer());
        CHECK(IsLiveEvent(ev));
    }
    res.r1Offscreen = 0; // dropped by this draw (a later scene/post stage)
    {
        const DrawEvent ev = EventOf(core, res);
        CHECK(!ev.r1Offscreen && ev.r1OffscreenCleared);
        CHECK(IsLiveEvent(ev));
    }
    res.r1Offscreen = 3; // changed to another draw (cannot happen with the tracker): not claimed
    {
        const DrawEvent ev = EventOf(core, res);
        CHECK(!ev.r1Offscreen && !ev.r1OffscreenCleared);
    }
    res.r1Offscreen = 0; // one scene stage drops both
    res.r1bChanged = true;
    res.r1b = 0;
    {
        const DrawEvent ev = EventOf(core, res);
        CHECK(ev.r1OffscreenCleared && ev.r1bCleared);
    }
    res.counted = false; // a draw the core ignored: no event
    res.r1Offscreen = 9;
    res.r1bChanged = false;
    {
        const DrawEvent ev = EventOf(core, res);
        CHECK(!ev.r1Offscreen && !ev.r1OffscreenCleared);
        CHECK(!IsLiveEvent(ev));
    }
    // the R1b events are hook events as before
    DrawEvent set;
    set.r1bSet = true;
    CHECK(IsLiveEvent(set));
    DrawEvent cleared;
    cleared.r1bCleared = true;
    CHECK(IsLiveEvent(cleared));
}

// The order of the early path's checks, without a GPU: every attempt below falls back to R1b before it touches the context (null here),
// and none of them is a capture, a rejection or a fact for the engine.
int g_predicts = 0, g_fetches = 0;
ApplyResult g_predicted = ApplyResult::NothingToApply;
ApplyResult CountingPredict(ID3D11DeviceContext*)
{
    ++g_predicts;
    return g_predicted;
}
bool FailingBackbuffer(ID3D11Texture2D** texture)
{
    ++g_fetches;
    *texture = nullptr;
    return false;
}
int g_effectsCalls = 0;
ApplyResult CountingEffects(ID3D11DeviceContext*, ID3D11Resource*)
{
    ++g_effectsCalls;
    return ApplyResult::NothingToApply;
}

void EarlyGateOrder()
{
    static const int kIdentity = 0;
    DrawEvent ev;
    ev.r1Offscreen = true;
    ev.sawSceneStage = true;
    ev.bbWidth = 3840;
    ev.bbHeight = 2160;
    ev.bbPointer = &kIdentity;

    Env env;
    env.effects = &CountingEffects;
    env.predictEffects = &CountingPredict;
    env.backbuffer = &FailingBackbuffer;
    LiveSource s(env);
    FrameImage img;
    s.Take(img);
    s.EndFrame(); // armed from now on while Take() keeps being called
    CHECK(s.Armed());

    // r1b (the default): an R1 off-screen event asks nothing, and the summary has no early-anchor detail
    s.OnDraw(nullptr, ev);
    CHECK_EQ(g_predicts, 0);
    CHECK_EQ(g_fetches, 0);
    CHECK(s.Summary().find("declined") == std::string::npos);
    CHECK(s.Summary().find("re-armed") == std::string::npos);

    s.SetAnchorRule(AnchorRule::Early);
    // presets would run, or the effects call would fail: the frame keeps R1b, the backbuffer is not even fetched
    g_predicted = ApplyResult::Applied;
    s.OnDraw(nullptr, ev);
    CHECK_EQ(g_predicts, 1);
    CHECK_EQ(g_fetches, 0);
    g_predicted = ApplyResult::Failed;
    s.OnDraw(nullptr, ev);
    CHECK_EQ(g_predicts, 2);
    CHECK_EQ(g_fetches, 0);
    // nothing would be applied: the backbuffer is fetched; its fetch fails here, so the frame keeps R1b as well
    for (ApplyResult r : { ApplyResult::NothingToApply, ApplyResult::NotEnabled, ApplyResult::NoReShade, ApplyResult::NoRuntime })
    {
        g_predicted = r;
        const int fetches = g_fetches;
        s.OnDraw(nullptr, ev);
        CHECK_EQ(g_fetches, fetches + 1);
    }
    CHECK_EQ(g_predicts, 6);
    // the backbuffer the core does not know (no identity, no width, no height) is never fetched: either half of the shape alone makes it
    // unknown
    g_predicted = ApplyResult::NothingToApply;
    {
        const int fetches = g_fetches;
        DrawEvent unknown = ev;
        unknown.bbPointer = nullptr;
        s.OnDraw(nullptr, unknown);
        unknown = ev;
        unknown.bbWidth = unknown.bbHeight = 0;
        s.OnDraw(nullptr, unknown);
        unknown = ev;
        unknown.bbWidth = 0; // a known height does not rescue it
        s.OnDraw(nullptr, unknown);
        unknown = ev;
        unknown.bbHeight = 0; // ... nor a known width
        s.OnDraw(nullptr, unknown);
        CHECK_EQ(g_fetches, fetches);
    }
    // a draw that does not qualify asks nothing: no scene stage before it, not the R1 off-screen draw, an R1 off-screen clear
    {
        const int predicts = g_predicts, fetches = g_fetches;
        DrawEvent menu = ev;
        menu.sawSceneStage = false;
        s.OnDraw(nullptr, menu);
        DrawEvent other = ev;
        other.r1Offscreen = false;
        s.OnDraw(nullptr, other);
        DrawEvent cleared;
        cleared.r1OffscreenCleared = true;
        cleared.sawSceneStage = true;
        s.OnDraw(nullptr, cleared);
        CHECK_EQ(g_predicts, predicts);
        CHECK_EQ(g_fetches, fetches);
    }
    // why each early anchor was not taken (log only): the presets gate twice (Applied, Failed), the fetch four times, an unknown backbuffer
    // four times; a draw that does not qualify is no decline
    {
        const auto st = s.GetStats();
        CHECK_EQ(st.earlyDeclined[size_t(EarlyDecline::Presets)], 2u);
        CHECK_EQ(st.earlyDeclined[size_t(EarlyDecline::Fetch)], 4u);
        CHECK_EQ(st.earlyDeclined[size_t(EarlyDecline::UnknownBackbuffer)], 4u);
        CHECK_EQ(st.earlyDeclined[size_t(EarlyDecline::Target)], 0u);
        CHECK_EQ(st.earlyDeclined[size_t(EarlyDecline::Texture)], 0u);
        CHECK_EQ(st.earlyRearmed, 0u);
        CHECK(s.Summary().find("early anchors 0 (re-armed 0, off-screen only 0; declined: backbuffer unknown 4, presets 2, fetch 4, target 0, texture 0); "
                               "ReShade presets") != std::string::npos);
    }
    CHECK_EQ(g_effectsCalls, 0); // the early path never runs the effects itself
    CHECK(s.Take(img) == TakeResult::NotThisFrame);
    CHECK(!img.anchoredAfterOffscreenUi); // R1 off-screen events are not the same-frame fact (that one is r1 != r1b at R1b)
    s.EndFrame({ true, 9, 0 }); // the frame ended with UI only off the backbuffer: counted as today
    {
        const auto st = s.GetStats();
        CHECK_EQ(st.offscreenOnly, 1u);
        CHECK_EQ(st.captured, 0u);
        CHECK_EQ(st.rejected, 0u);
        CHECK_EQ(st.earlyAnchors, 0u);
        CHECK_EQ(st.effects[size_t(ApplyResult::NothingToApply)], 0u); // a fallback records no effects result
    }

    // an effects layer without a predictor: what it would do is unknown, so the frame keeps R1b (no fetch)
    {
        Env noPredict = env;
        noPredict.predictEffects = nullptr;
        LiveSource u(noPredict);
        u.Take(img);
        u.EndFrame();
        u.SetAnchorRule(AnchorRule::Early);
        const int fetches = g_fetches;
        u.OnDraw(nullptr, ev);
        CHECK_EQ(g_fetches, fetches);
        CHECK_EQ(u.GetStats().earlyDeclined[size_t(EarlyDecline::Presets)], 1u);
    }
    // no effects layer at all: nothing would be applied (as the R1b path records NotEnabled), the backbuffer is fetched
    {
        Env bare;
        bare.backbuffer = &FailingBackbuffer;
        LiveSource u(bare);
        u.Take(img);
        u.EndFrame();
        u.SetAnchorRule(AnchorRule::Early);
        const int fetches = g_fetches;
        u.OnDraw(nullptr, ev);
        CHECK_EQ(g_fetches, fetches + 1);
        CHECK_EQ(u.GetStats().earlyDeclined[size_t(EarlyDecline::Fetch)], 1u);
    }
    // no backbuffer fetch at all (no source wired): the draw declines as an unknown backbuffer without calling one
    {
        Env noFetch;
        noFetch.effects = &CountingEffects;
        noFetch.predictEffects = &CountingPredict;
        LiveSource u(noFetch);
        u.Take(img);
        u.EndFrame();
        u.SetAnchorRule(AnchorRule::Early);
        const int predicts = g_predicts, fetches = g_fetches;
        u.OnDraw(nullptr, ev);
        CHECK_EQ(g_predicts, predicts); // the presets gate was not even asked
        CHECK_EQ(g_fetches, fetches);
        CHECK_EQ(u.GetStats().earlyDeclined[size_t(EarlyDecline::UnknownBackbuffer)], 1u);
        CHECK_EQ(u.GetStats().earlyDeclined[size_t(EarlyDecline::Fetch)], 0u);
        CHECK_EQ(u.GetStats().captured, 0u);
    }
    // not armed: nothing is asked
    {
        LiveSource u(env);
        u.SetAnchorRule(AnchorRule::Early);
        const int predicts = g_predicts;
        u.OnDraw(nullptr, ev);
        CHECK_EQ(g_predicts, predicts);
    }
}

// [HudLess] AnchorRule is live: one log line per change of the rule in force, none while it stays.
void AnchorRuleLine()
{
    auto count = [](const std::string& text, const std::string& what)
    {
        size_t n = 0;
        for (size_t p = 0; (p = text.find(what, p)) != std::string::npos; ++p)
            ++n;
        return n;
    };
    LiveSource s(Env {});
    CHECK_EQ(count(s.EndFrame(), "AnchorRule"), 0u); // r1b from the start: nothing to say
    s.SetAnchorRule(AnchorRule::Early);
    std::string lines = s.EndFrame();
    CHECK_EQ(count(lines, "FFXIV UI boundary: AnchorRule r1b -> early"), 1u);
    CHECK_EQ(count(lines, "AnchorRule"), 1u);
    s.SetAnchorRule(AnchorRule::Early); // read again every frame: unchanged, no line
    CHECK_EQ(count(s.EndFrame(), "AnchorRule"), 0u);
    s.Release(); // the device went away: the rule is a setting, not frame state
    CHECK_EQ(count(s.EndFrame(), "AnchorRule"), 0u);
    s.SetAnchorRule(AnchorRule::R1b);
    CHECK_EQ(count(s.EndFrame(), "FFXIV UI boundary: AnchorRule early -> r1b"), 1u);
    s.SetAnchorRule(AnchorRule::Early); // switched and switched back within one Present: the rule in force did not change
    s.SetAnchorRule(AnchorRule::R1b);
    CHECK_EQ(count(s.EndFrame(), "AnchorRule"), 0u);
}

void Lines()
{
    LiveSource s(Env {});
    FrameImage img;
    std::string all;
    for (uint64_t i = 0; i < LiveSource::kSummaryEvery + 5; ++i)
    {
        s.Take(img);
        all += s.EndFrame();
        all += '\n';
    }
    // one state line ("armed") and exactly one summary (after kSummaryEvery acting frames)
    size_t armed = 0, summaries = 0;
    for (size_t p = 0; (p = all.find("live capture armed", p)) != std::string::npos; ++p)
        ++armed;
    for (size_t p = 0; (p = all.find("FFXIV UI boundary: 600 frames:", p)) != std::string::npos; ++p)
        ++summaries;
    CHECK_EQ(armed, 1u);
    CHECK_EQ(summaries, 1u);
    const auto summary = s.Summary();
    CHECK(summary.find("captured 0") != std::string::npos);
    CHECK(summary.find("rejected 0") != std::string::npos);
    CHECK(summary.find("ReShade presets: applied 0") != std::string::npos);
    // the early anchors sit with the boundary's own counters, after the copies the engine skipped and before the ReShade group
    CHECK(summary.find("copy skipped by the engine 0, early anchors 0; ReShade presets: applied 0") != std::string::npos);

    // release drops the armed state
    s.Release();
    CHECK(!s.Armed());
}
} // namespace

int main()
{
    Acts();
    Guard();
    Arming();
    Events();
    AnchorFact();
    Notes();
    EarlyActs();
    EarlyPresetsGate();
    EarlyEvents();
    EarlyGateOrder();
    AnchorRuleLine();
    Lines();
    return Finish("ui_live_decide");
}
