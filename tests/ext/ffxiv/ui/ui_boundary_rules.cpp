// S10-A: boundary rules R1 / R2 / R3 (docs/HUDLESS.md section 10) on synthetic draw sequences, and the property that the
// online tracker equals the retrospective definition on every input.
#include "check.h"
#include "ext/ffxiv/ui/UiBoundaryRules.h"

#include <vector>

using namespace ext::ffxiv::ui;

namespace
{
struct Seq
{
    std::vector<DrawIn> d;
    Seq& Untagged(int n = 1, bool bb = false)
    {
        for (int i = 0; i < n; ++i)
            d.push_back({ kStageNone, bb });
        return *this;
    }
    Seq& Stage(uint8_t s, bool bb = false)
    {
        d.push_back({ s, bb });
        return *this;
    }
    Seq& Ui(int n = 1, bool bb = true)
    {
        for (int i = 0; i < n; ++i)
            d.push_back({ kUiStage, bb });
        return *this;
    }
    Seq& Unknown(int n = 1, bool bb = false)
    {
        for (int i = 0; i < n; ++i)
            d.push_back({ kStageUnknown, bb });
        return *this;
    }
    uint32_t Next() const { return static_cast<uint32_t>(d.size()) + 1; } // ordinal of the next draw to be added
};

struct Run
{
    BoundaryTracker t;
    int moves = 0;
    Candidates retro;
    uint32_t retroR1b = 0;
};

Run Play(const Seq& s)
{
    Run r;
    for (const auto& d : s.d)
        if (r.t.OnDraw(d))
            ++r.moves;
    r.retro = Retrospective(s.d);
    r.retroR1b = RetrospectiveR1b(s.d);
    CHECK_EQ(r.t.R1b(), r.retroR1b); // online equals retrospective on every hand written sequence too
    return r;
}

void Typical()
{
    Seq s;
    s.Untagged(10).Stage(1).Stage(2).Stage(3).Stage(4).Stage(5, true);
    const uint32_t firstUi = s.Next();
    s.Ui(5);
    const auto r = Play(s);
    CHECK_EQ(r.t.Current().r1, firstUi);
    CHECK_EQ(r.t.Current().r2, firstUi);
    CHECK_EQ(r.t.Current().r2any, firstUi);
    CHECK_EQ(r.t.Current().r3, firstUi);
    CHECK(r.t.Current() == r.retro);
    CHECK(r.t.Summary().sawSceneStage);
    CHECK_EQ(r.t.Summary().uiDraws, 5u);
    CHECK_EQ(r.t.Summary().firstUi, firstUi);
    CHECK_EQ(r.t.Summary().lastStage5, firstUi - 1);
    CHECK_EQ(r.moves, 1); // one capture opportunity: both rules fire on the same draw
}

void UntaggedBackbufferDrawBeforeUi()
{
    // An overlay (Dalamud-like) or a pre-hook UI shader draws into the backbuffer right after the gamma pass.
    Seq s;
    s.Stage(1).Stage(5, true);
    const uint32_t early = s.Next();
    s.Untagged(1, true);
    s.Untagged(3, false);
    const uint32_t ui = s.Next();
    s.Ui(2);
    const auto r = Play(s);
    CHECK_EQ(r.t.Current().r2, early);
    CHECK_EQ(r.t.Current().r1, ui);
    CHECK_EQ(r.t.Current().r3, early); // the union takes the earlier one
    CHECK(r.t.Current() == r.retro);
}

void ShadersCreatedBeforeTheHook()
{
    Seq s;
    s.Unknown(60);
    const auto r = Play(s);
    CHECK_EQ(r.t.Current().r1, 0u);
    CHECK_EQ(r.t.Current().r2, 0u);
    CHECK_EQ(r.t.Current().r3, 0u);
    CHECK_EQ(r.t.Summary().unknownDraws, 60u);
    CHECK(!r.t.Summary().sawSceneStage);
    CHECK_EQ(r.moves, 0);
    CHECK(r.t.Current() == r.retro);

    // Only the gamma shader predates the hook: stages 1..4 and UI are known, R2 is blind, R1 still works.
    Seq p;
    p.Stage(1).Stage(2).Stage(3).Stage(4).Unknown(1, true);
    const uint32_t ui = p.Next();
    p.Ui(3);
    const auto q = Play(p);
    CHECK_EQ(q.t.Current().r1, ui);
    CHECK_EQ(q.t.Current().r2, 0u);
    CHECK_EQ(q.t.Current().r3, ui);
    CHECK(q.t.Current() == q.retro);

    // Only the UI shaders predate the hook: R1 is blind, R2 (stage 5 known, position based) still finds the boundary.
    Seq u;
    u.Stage(1).Stage(5, true);
    const uint32_t first = u.Next();
    u.Unknown(4, true);
    const auto v = Play(u);
    CHECK_EQ(v.t.Current().r1, 0u);
    CHECK_EQ(v.t.Current().r2, first);
    CHECK_EQ(v.t.Current().r3, first);
    CHECK(v.t.Current() == v.retro);
}

void FadeFrame()
{
    // Scene stages, then a full-screen fade quad into the backbuffer with an untagged shader, no UI shader at all.
    Seq s;
    s.Stage(1).Stage(2).Stage(5, true);
    const uint32_t fade = s.Next();
    s.Untagged(1, true);
    const auto r = Play(s);
    CHECK_EQ(r.t.Current().r1, 0u);
    CHECK_EQ(r.t.Current().r2, fade);
    CHECK_EQ(r.t.Current().r3, fade);
    CHECK(r.t.Current() == r.retro);
}

void MenuOnlyFrame()
{
    Seq s;
    s.Untagged(3, false);
    const uint32_t ui = s.Next();
    s.Ui(8);
    const auto r = Play(s);
    CHECK(!r.t.Summary().sawSceneStage);
    CHECK_EQ(r.t.Current().r1, ui); // the policy "no scene stage means no usable boundary" belongs to the consumer
    CHECK_EQ(r.t.Current().r2, 0u);
    CHECK_EQ(r.t.Current().r3, ui);
    CHECK(r.t.Current() == r.retro);
}

void NoUiShaderAtAll()
{
    Seq s;
    s.Untagged(20).Stage(1).Stage(2).Stage(3).Stage(4).Stage(5, true);
    const auto r = Play(s);
    CHECK_EQ(r.t.Current().r1, 0u);
    CHECK_EQ(r.t.Current().r2, 0u);
    CHECK_EQ(r.t.Current().r2any, 0u);
    CHECK_EQ(r.t.Current().r3, 0u);
    CHECK_EQ(r.moves, 0);
    CHECK(r.t.Current() == r.retro);
}

void UiDrawBeforeTheLastSceneStage()
{
    Seq s;
    s.Untagged(2).Ui(3, false); // e.g. a render-to-texture portrait drawn early
    s.Stage(1).Stage(2);
    s.Ui(1, false); // still before the last stage
    s.Stage(5, true);
    const uint32_t ui = s.Next();
    s.Ui(4);
    const auto r = Play(s);
    CHECK_EQ(r.t.Current().r1, ui);
    CHECK_EQ(r.t.Summary().uiBeforeLastScene, 4u);
    CHECK_EQ(r.t.Summary().r1Supersessions, 2u); // the candidate was dropped by stage 1 and later by stage 5
    CHECK(r.t.Current() == r.retro);
}

void SecondGammaPassSupersedesR2()
{
    Seq s;
    s.Stage(1).Stage(5, true).Ui(2);
    s.Stage(5, true);
    const uint32_t ui = s.Next();
    s.Ui(2);
    const auto r = Play(s);
    CHECK_EQ(r.t.Current().r1, ui);
    CHECK_EQ(r.t.Current().r2, ui);
    CHECK_EQ(r.t.Summary().r2Supersessions, 1u);
    CHECK_EQ(r.t.Summary().lastStage5, ui - 1);
    CHECK(r.t.Current() == r.retro);
}

void GammaIntoIntermediateTarget()
{
    // Stage 5 writes an intermediate texture; the first backbuffer draw comes later. r2any is the draw right after the
    // anchor, r2 the first draw into the backbuffer.
    Seq s;
    s.Stage(1).Stage(5, false);
    const uint32_t afterAnchor = s.Next();
    s.Untagged(2, false);
    const uint32_t bb = s.Next();
    s.Untagged(1, true);
    s.Ui(1);
    const auto r = Play(s);
    CHECK_EQ(r.t.Current().r2any, afterAnchor);
    CHECK_EQ(r.t.Current().r2, bb);
    CHECK(r.t.Current() == r.retro);
}

void SceneStageAfterTheAnchor()
{
    // A stage 3 draw after the last stage-5 draw is not an anchor; the UI draw right after the anchor already claimed R2,
    // and the stage 3 draw drops R1.
    Seq s;
    s.Stage(5, true).Ui(1);
    s.Stage(3, true);
    const auto r = Play(s);
    CHECK_EQ(r.t.Current().r2, 2u);
    CHECK_EQ(r.t.Current().r1, 0u);
    CHECK(r.t.Current() == r.retro);
}

void WantsRenderTargetFollowsTheAnchor()
{
    BoundaryTracker t;
    CHECK(!t.WantsRenderTarget());
    t.OnDraw({ 1, false });
    CHECK(!t.WantsRenderTarget());
    t.OnDraw({ 5, false });
    CHECK(t.WantsRenderTarget()); // after the anchor until R2 is found
    t.OnDraw({ kStageNone, false });
    CHECK(t.WantsRenderTarget());
    t.OnDraw({ kStageNone, true });
    CHECK(!t.WantsRenderTarget()); // R2 found
    t.OnDraw({ 5, false });
    CHECK(t.WantsRenderTarget()); // a new anchor restarts the search
    t.Reset();
    CHECK(!t.WantsRenderTarget());
    CHECK_EQ(t.Summary().draws, 0u);
}

void CaptureOpportunities()
{
    // The return value marks the draws at which a capture would be taken (R3 changed).
    BoundaryTracker t;
    CHECK(!t.OnDraw({ kStageNone, false }));
    CHECK(!t.OnDraw({ 1, false }));
    CHECK(t.OnDraw({ kUiStage, false }));  // R1 fires
    CHECK_EQ(t.LastChange(), 1);           // bit 0: R1 changed
    CHECK(!t.OnDraw({ kUiStage, false })); // later UI draws change nothing
    CHECK_EQ(t.LastChange(), 0);
    CHECK(t.OnDraw({ 5, false }));         // a later stage drops the candidate: the capture was stale
    CHECK_EQ(t.LastChange(), 1);           // R1 cleared; a consumer checks Current().r1 == ordinal before capturing
    CHECK(t.OnDraw({ kUiStage, true }));   // new candidate
    CHECK_EQ(t.LastChange(), 3);           // R1 and R2 on the same draw
    CHECK_EQ(t.Summary().candidateMoves, 3u);
}

// ---- R1b: the live anchor (S10 E2) -------------------------------------------------------------------------------------------
void R1bIsR1WhenTheFirstUiDrawIsOnTheBackbuffer()
{
    // The 99.5% case of Session 1: the HUD starts on the backbuffer.
    Seq s;
    s.Untagged(10).Stage(1).Stage(2).Stage(4);
    s.Untagged(10, false); // the scene resolve into the backbuffer is not tagged and is not a UI draw
    const uint32_t first = s.Next();
    s.Ui(5, true);
    const auto r = Play(s);
    CHECK_EQ(r.t.Current().r1, first);
    CHECK_EQ(r.t.R1b(), first);
}

void R1bSkipsPreviewRenderTargets()
{
    // The 103 frames of Session 1: the character preview (352x352 B8G8R8A8, UI shaders) is drawn before the HUD. R1 sits on the
    // preview, R1b on the first UI draw into the backbuffer.
    Seq s;
    s.Stage(1).Stage(2).Stage(4);
    const uint32_t preview = s.Next();
    s.Ui(9, false);
    s.Untagged(6, false);
    const uint32_t hud = s.Next();
    s.Ui(7, true);
    const auto r = Play(s);
    CHECK_EQ(r.t.Current().r1, preview);
    CHECK_EQ(r.t.R1b(), hud);
    CHECK(r.t.R1b() > r.t.Current().r1);

    // the preview can also come after the first HUD draw (frame 20300 of Session 1): R1b == R1, the later preview changes nothing
    Seq t;
    t.Stage(1).Stage(4);
    const uint32_t first = t.Next();
    t.Ui(2, true);
    t.Ui(9, false);
    t.Ui(5, true);
    const auto q = Play(t);
    CHECK_EQ(q.t.Current().r1, first);
    CHECK_EQ(q.t.R1b(), first);
}

void R1bNeedsABackbufferUiDraw()
{
    // Only preview-target UI draws: R1 exists, R1b does not (the live source reports no boundary, not a rejection).
    Seq s;
    s.Stage(1).Stage(4).Ui(12, false);
    const auto r = Play(s);
    CHECK(r.t.Current().r1 != 0);
    CHECK_EQ(r.t.R1b(), 0u);

    // a backbuffer draw that is not UI-tagged is not an anchor
    Seq u;
    u.Stage(1).Stage(5, true).Untagged(3, true).Ui(2, false);
    const auto v = Play(u);
    CHECK_EQ(v.t.R1b(), 0u);

    // pre-hook shaders (unknown stage) into the backbuffer are not UI draws either
    Seq w;
    w.Stage(1).Unknown(4, true);
    CHECK_EQ(Play(w).t.R1b(), 0u);
}

void R1bResetsOnEverySceneStage()
{
    // A scene/post stage draw drops R1b like it drops R1; the anchor is the first backbuffer UI draw after the LAST one.
    BoundaryTracker t;
    t.OnDraw({ 1, false });
    CHECK(!t.R1bChanged());
    t.OnDraw({ kUiStage, false });
    CHECK_EQ(t.R1b(), 0u);
    CHECK(!t.R1bChanged());                 // a preview draw is not an event for R1b
    t.OnDraw({ kUiStage, true });
    CHECK_EQ(t.R1b(), 3u);
    CHECK(t.R1bChanged());                  // set: the capture opportunity of the live source (the draw has not run yet)
    t.OnDraw({ kUiStage, true });
    CHECK_EQ(t.R1b(), 3u);
    CHECK(!t.R1bChanged());                 // later HUD draws change nothing
    t.OnDraw({ 3, false });
    CHECK_EQ(t.R1b(), 0u);
    CHECK(t.R1bChanged());                  // cleared: a capture taken at 3 is stale
    t.OnDraw({ 3, false });
    CHECK(!t.R1bChanged());                 // nothing to clear
    t.OnDraw({ kUiStage, false });
    t.OnDraw({ kUiStage, true });
    CHECK_EQ(t.R1b(), 8u);                  // the new anchor, after the new last scene stage (draw 7 was a preview draw)
    CHECK(t.R1bChanged());
    // R1 / R2 / R3 and LastChange() are untouched by R1b (frames.txt and the D1 tests are keyed on them)
    CHECK_EQ(t.Current().r1, 7u);
    CHECK_EQ(t.LastChange(), 0);
    t.Reset();
    CHECK_EQ(t.R1b(), 0u);
    CHECK(!t.R1bChanged());
}

void R1bMenuOnlyFrame()
{
    // the tracker has no policy about scene stages; the live source ignores R1b events of frames without a scene stage
    Seq s;
    s.Untagged(3, false);
    const uint32_t ui = s.Next();
    s.Ui(4, true);
    const auto r = Play(s);
    CHECK(!r.t.Summary().sawSceneStage);
    CHECK_EQ(r.t.R1b(), ui);
}

// Deterministic generator for the property test.
struct Lcg
{
    uint64_t s = 0x9E3779B97F4A7C15ull;
    uint32_t Next(uint32_t n)
    {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>((s >> 33) % n);
    }
};

void OnlineEqualsRetrospective()
{
    Lcg rng;
    for (int iter = 0; iter < 5000; ++iter)
    {
        Seq s;
        const uint32_t len = rng.Next(80);
        for (uint32_t i = 0; i < len; ++i)
        {
            static const uint8_t kinds[] = { kStageNone, kStageNone, kStageNone, 1, 2, 3, 4, 5, kUiStage, kUiStage, kStageUnknown };
            s.d.push_back({ kinds[rng.Next(sizeof kinds)], rng.Next(2) == 1 });
        }
        BoundaryTracker t;
        for (const auto& d : s.d)
            t.OnDraw(d);
        const auto retro = Retrospective(s.d);
        const uint32_t retroR1b = RetrospectiveR1b(s.d);
        if (!(t.Current() == retro) || t.R1b() != retroR1b)
        {
            std::printf("mismatch at iteration %d (len %u): online r1=%u r2=%u r2any=%u r3=%u r1b=%u, retro r1=%u r2=%u r2any=%u r3=%u r1b=%u\n",
                        iter, len, t.Current().r1, t.Current().r2, t.Current().r2any, t.Current().r3, t.R1b(), retro.r1, retro.r2,
                        retro.r2any, retro.r3, retroR1b);
            CHECK(false);
            return;
        }
        // R1b is never earlier than R1 and is a UI draw into the backbuffer
        CHECK(t.R1b() == 0 || (t.Current().r1 != 0 && t.R1b() >= t.Current().r1));
        if (t.R1b() != 0)
        {
            CHECK(s.d[t.R1b() - 1].stage == kUiStage);
            CHECK(s.d[t.R1b() - 1].rtBackbuffer);
        }
        CHECK_EQ(t.Summary().draws, len);
    }
}
} // namespace

int main()
{
    Typical();
    UntaggedBackbufferDrawBeforeUi();
    ShadersCreatedBeforeTheHook();
    FadeFrame();
    MenuOnlyFrame();
    NoUiShaderAtAll();
    UiDrawBeforeTheLastSceneStage();
    SecondGammaPassSupersedesR2();
    GammaIntoIntermediateTarget();
    SceneStageAfterTheAnchor();
    WantsRenderTargetFollowsTheAnchor();
    CaptureOpportunities();
    R1bIsR1WhenTheFirstUiDrawIsOnTheBackbuffer();
    R1bSkipsPreviewRenderTargets();
    R1bNeedsABackbufferUiDraw();
    R1bResetsOnEverySceneStage();
    R1bMenuOnlyFrame();
    OnlineEqualsRetrospective();
    return Finish("ui_boundary_rules");
}
