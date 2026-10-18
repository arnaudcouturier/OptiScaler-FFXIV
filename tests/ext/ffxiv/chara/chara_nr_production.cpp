// Preview NR, production entry (NR on the super-resolution output): the pure parts. What this proves: the model call of a production
// run (output-size colour, render-size guides built by GuidesOf from the same inputs as the super-resolution evaluate, the upscaler's
// depth convention and motion scale), the recipe of a production run (no zero-motion pass, the same states as the bring-up), the stream
// shape (a mode change rebuilds the stream, a bring-up consumer is never given a production result) and the checks on the images a
// production run copies. The decisions of one render (production vs bring-up, the
// alpha input, the withdrawal) are tested in chara_nr_render.
// What it cannot prove (the game session does): the D3D12 state tracking of the copied inputs, the model's behaviour for guides smaller
// than the colour, the image quality on alpha-masked characters.
#include "check.h"

#include "ext/ffxiv/chara/CharaDlssNgx.h"
#include "ext/ffxiv/chara/CharaNrGuides.h"
#include "ext/ffxiv/chara/CharaNrPlumbing.h"
#include "ext/ffxiv/chara/CharaNrRecipe.h"
#include "ext/ffxiv/chara/CharaNrRules.h"

#include <string>
#include <variant>
#include <vector>

using namespace ext::ffxiv::chara::nr;
namespace ngx = ext::ffxiv::chara::dlss::ngx;

namespace
{
constexpr uint32_t kRenderW = 576, kRenderH = 960;
constexpr uint32_t kOutW = kRenderW * ngx::kOutputScale, kOutH = kRenderH * ngx::kOutputScale;

// The recipe against a fake that records the passes and the model call.
struct Fake
{
    std::vector<std::string> ops;
    ModelCall call;
    int passes = 0;
    DlssNrConstants constants[4];
    void TimerStart() {}
    void TimerEnd() {}
    void Barrier(Tex, State, State) {}
    bool Pass(const DlssNrConstants& c, Tex, Tex, Tex, Tex, Tex)
    {
        if (passes < 4)
            constants[passes] = c;
        ++passes;
        ops.push_back("pass:" + std::to_string(c.Mode));
        return true;
    }
    bool Evaluate(const ModelCall& m)
    {
        call = m;
        ops.push_back("evaluate");
        return true;
    }
    bool Has(const std::string& op) const
    {
        for (const auto& o : ops)
            if (o == op)
                return true;
        return false;
    }
};

// The inputs of one super-resolution evaluate at the render size (the same object feeds the evaluate and the production plan).
ngx::EvaluateInputs SrInputs(uint32_t width, uint32_t height, bool reset)
{
    ngx::EvaluateInputs in;
    in.width = width;
    in.height = height;
    in.reset = reset;
    return in;
}

// A production plan for the render the evaluate `sr` was made for: the colour is the evaluate's output (twice its size), the guides come from the
// same inputs, the reset follows the evaluate's.
FramePlan ProductionPlan(const ngx::EvaluateInputs& sr, bool reversedZ)
{
    FramePlan p;
    p.width = sr.width * ngx::kOutputScale;
    p.height = sr.height * ngx::kOutputScale;
    p.initGuides = false; // production never zeroes the motion image: it holds the preview's own motion
    p.reset = sr.reset;
    p.guides = GuidesOf(sr, reversedZ);
    return p;
}

// The values the super-resolution evaluate is given, recorded through its own composition.
struct NgxFake
{
    float mvX = -1.0f, mvY = -1.0f;
    uint32_t subW = 0, subH = 0;
    void SetUInt(const char* n, uint32_t v)
    {
        if (std::string(n) == ngx::kSubrectWidth)
            subW = v;
        if (std::string(n) == ngx::kSubrectHeight)
            subH = v;
    }
    void SetInt(const char*, int) {}
    void SetFloat(const char* n, float v)
    {
        if (std::string(n) == ngx::kMvScaleX)
            mvX = v;
        if (std::string(n) == ngx::kMvScaleY)
            mvY = v;
    }
    void SetResource(const char*, const void*) {}
};

void TestModelCall()
{
    // one render: the super-resolution evaluate and the production plan are made from the same inputs
    const ngx::EvaluateInputs in = SrInputs(kRenderW, kRenderH, true);
    Fake f;
    CHECK(RecordRun(f, ProductionPlan(in, false)));
    const ModelCall& m = f.call;
    // the model sees the 2x image; depth and motion are the render-size sub-rectangles starting at 0
    CHECK(m.width == kOutW && m.height == kOutH);
    CHECK(m.guideWidth == kRenderW && m.guideHeight == kRenderH && m.motionWidth == kRenderW && m.motionHeight == kRenderH);
    CHECK(m.depthBaseX == 0 && m.depthBaseY == 0 && m.motionBaseX == 0 && m.motionBaseY == 0);
    // the motion scale is the one the super-resolution evaluate is given
    NgxFake sr;
    ngx::ApplyEvaluate<NgxFake, const void*>(sr, in, nullptr, nullptr, nullptr, nullptr);
    CHECK(sr.mvX == static_cast<float>(kRenderW) && sr.mvY == static_cast<float>(kRenderH));
    CHECK(m.mvScaleX == sr.mvX && m.mvScaleY == sr.mvY);
    CHECK(sr.subW == kRenderW && sr.subH == kRenderH); // and the render size is the guides' size
    CHECK(m.guideWidth == sr.subW && m.guideHeight == sr.subH);
    CHECK(m.depthInverted == 0 && m.reset == 1);

    // the depth convention follows the feature's flags
    Fake g;
    CHECK(RecordRun(g, ProductionPlan(SrInputs(kRenderW, kRenderH, false), true)));
    CHECK(g.call.depthInverted == 1 && g.call.reset == 0);
    CHECK((ngx::CreateFlags(true) & ngx::kFlagDepthInverted) != 0 && (ngx::CreateFlags(false) & ngx::kFlagDepthInverted) == 0);
    // and the motion vectors are declared low resolution, which is what makes the guides' own size the reference of the motion scale
    CHECK((ngx::CreateFlags(false) & ngx::kFlagMotionLowRes) != 0);

    // the bring-up call is unchanged by the new argument
    const ModelCall b = MakeModelCall(kRenderW, kRenderH, false, ModelTuning {});
    CHECK(b.guideWidth == kRenderW && b.guideHeight == kRenderH && b.motionWidth == kRenderW && b.motionHeight == kRenderH);
    CHECK(b.depthInverted == 0 && b.mvScaleX == 1.0f && b.mvScaleY == 1.0f);
}

// The guides as they were computed before GuidesOf existed, from a loose width and height: the oracle of the equivalence sweep.
GuideSetup OldProductionGuides(uint32_t renderWidth, uint32_t renderHeight, bool reversedZ)
{
    return GuideSetup { renderWidth, renderHeight, reversedZ, static_cast<float>(renderWidth), static_cast<float>(renderHeight) };
}

// GuidesOf is the one place the production NR guides come from, and ApplyEvaluate (the super-resolution evaluate's set-up) uses the same
// EvaluateGuidesOf. The guides of a production run therefore equal what the evaluate of the same render is given, whatever the size.
void TestGuidesOf()
{
    const uint32_t sizes[][2] = { { 1, 1 },       { 2, 3 },       { 576, 960 },   { 960, 576 },   { 1280, 720 },
                                  { 1920, 1080 }, { 2048, 2048 }, { 4095, 1 },    { 1, 4096 } };
    for (const auto& size : sizes)
        for (const bool reversedZ : { false, true })
            for (const bool reset : { false, true })
            {
                const uint32_t w = size[0], h = size[1];
                const ngx::EvaluateInputs in = SrInputs(w, h, reset);
                const GuideSetup got = GuidesOf(in, reversedZ);

                // behaviour is unchanged: field by field the same as the old computation from the loose width and height
                const GuideSetup old = OldProductionGuides(w, h, reversedZ);
                CHECK(got.width == old.width && got.height == old.height && got.depthInverted == old.depthInverted &&
                      got.mvScaleX == old.mvScaleX && got.mvScaleY == old.mvScaleY);

                // what the super-resolution evaluate of this render is given, recorded through its own composition
                NgxFake sr;
                ngx::ApplyEvaluate<NgxFake, const void*>(sr, in, nullptr, nullptr, nullptr, nullptr);
                CHECK(got.mvScaleX == sr.mvX && got.mvScaleY == sr.mvY);
                CHECK(got.width == sr.subW && got.height == sr.subH);
                // the convention itself, typed independently of the helper: the render size, in pixels, and not the output size
                CHECK(got.width == w && got.height == h);
                CHECK(got.mvScaleX == static_cast<float>(w) && got.mvScaleY == static_cast<float>(h));
                CHECK(got.depthInverted == reversedZ);

                // the model call of a production run for this render carries them
                Fake rec;
                CHECK(RecordRun(rec, ProductionPlan(in, reversedZ)));
                CHECK(rec.call.guideWidth == sr.subW && rec.call.guideHeight == sr.subH && rec.call.motionWidth == sr.subW &&
                      rec.call.motionHeight == sr.subH);
                CHECK(rec.call.mvScaleX == sr.mvX && rec.call.mvScaleY == sr.mvY);
                CHECK(rec.call.width == w * ngx::kOutputScale && rec.call.height == h * ngx::kOutputScale);
                CHECK(rec.call.depthInverted == (reversedZ ? 1 : 0) && rec.call.reset == (reset ? 1 : 0));

                // the check above has teeth: inputs at the OUTPUT size (the mistake of passing the colour image's size) give other guides
                const GuideSetup wrong = GuidesOf(SrInputs(w * ngx::kOutputScale, h * ngx::kOutputScale, reset), reversedZ);
                CHECK(wrong.mvScaleX != sr.mvX && wrong.mvScaleY != sr.mvY);
                CHECK(wrong.width != sr.subW && wrong.height != sr.subH);
            }
}

void TestRecipe()
{
    // production: encode -> model -> resolve only; the motion image is never overwritten
    Fake f;
    CHECK(RecordRun(f, ProductionPlan(SrInputs(kRenderW, kRenderH, false), false)));
    CHECK(f.passes == 2 && f.Has("evaluate"));
    CHECK(!f.Has("pass:11")); // DlssNrMode_ZeroMotion
    CHECK(f.constants[0].Mode == DlssNrMode_Encode && f.constants[0].Width == kOutW && f.constants[0].Height == kOutH);
    CHECK(f.constants[1].Mode == DlssNrMode_Resolve && f.constants[1].Width == kOutW && f.constants[1].Height == kOutH);
    CHECK(f.constants[0].Passthrough == 1 && f.constants[1].Passthrough == 1); // the SR output is already display referred
}

StreamShape BringUp()
{
    StreamShape s;
    s.width = kRenderW;
    s.height = kRenderH;
    s.preset = 2;
    s.style = 0;
    return s;
}

StreamShape Production()
{
    StreamShape s = BringUp();
    s.width = kOutW;
    s.height = kOutH;
    s.production = true;
    s.guideWidth = kRenderW;
    s.guideHeight = kRenderH;
    return s;
}

void TestStreamModes()
{
    // the modes differ in the shape, so a mode change is a rebuild, after the change has held still
    CHECK(!(BringUp() == Production()));
    StreamShape other = Production();
    other.guideWidth = 288; // a guide size change alone is a rebuild too
    CHECK(!(other == Production()));

    StreamState st;
    st.built = BringUp();
    st.hasBuilt = true;
    CHECK(DecideRebuild(st, BringUp(), 0) == RebuildAction::Keep);
    CHECK(DecideRebuild(st, Production(), 1000) == RebuildAction::Settle);
    CHECK(DecideRebuild(st, Production(), 1000 + kRebuildSettleMs - 1) == RebuildAction::Settle);
    CHECK(DecideRebuild(st, Production(), 1000 + kRebuildSettleMs) == RebuildAction::Rebuild);
    // Both options on, SR evaluating only some of the renders: the glue no longer runs the bring-up while the SR path is in charge of the
    // preview (SrInCharge / RunBringUpNow, chara_nr_plumbing), so a production stream is never asked for the bring-up shape by the renders
    // SR skipped. Here: with the SR path in charge the bring-up shape is never wanted ...
    PlumbingFacts both;
    both.srWanted = both.nrWanted = true;
    both.srGateOpen = true;
    StreamState flap;
    flap.built = Production();
    flap.hasBuilt = true;
    uint64_t t = 0;
    for (int i = 0; i < 50; ++i)
    {
        t += 16;
        const bool srEvaluatedThisRender = (i & 1) == 0;                // SR evaluates every other render
        const bool bringUp = RunBringUpNow(both, true, true, SrInCharge(true, false, false));
        CHECK(!bringUp);                                                 // the SR path is in charge: never
        const bool wantsProduction = RunProductionNow(both, srEvaluatedThisRender, true);
        if (wantsProduction)
            CHECK(DecideRebuild(flap, Production(), t) == RebuildAction::Keep); // the production stream stays as it is
    }
    // ... and one stray bring-up call (the SR path latched for a moment, a failed slot) does not rebuild a production stream: the next
    // production call wants the built shape again and the pending change is dropped
    StreamState stray;
    stray.built = Production();
    stray.hasBuilt = true;
    CHECK(DecideRebuild(stray, BringUp(), 1000) == RebuildAction::Settle);
    CHECK(DecideRebuild(stray, Production(), 1016) == RebuildAction::Keep && !stray.hasPending);
    CHECK(DecideRebuild(stray, BringUp(), 1032 + kRebuildSettleMs) == RebuildAction::Settle); // a new change starts its own clock
    // a bring-up that holds still for the settle time (the SR path is really out) is a real change: the stream is rebuilt
    CHECK(DecideRebuild(stray, BringUp(), 1032 + 2 * kRebuildSettleMs) == RebuildAction::Rebuild);

    // a bring-up consumer is only given a bring-up result
    StreamState bring;
    bring.built = BringUp();
    bring.hasBuilt = true;
    bring.everRan = true;
    bring.attempts = bring.ranAttempt = 3;
    CHECK(ResultKindOf(bring) == ResultKind::BringUp && ServesBringUpConsumer(ResultKindOf(bring)));
    StreamState prod = bring;
    prod.built = Production();
    CHECK(ResultKindOf(prod) == ResultKind::Production);
    CHECK(!ServesBringUpConsumer(ResultKindOf(prod)));
    // a stale result (a later attempt without a result) is no kind at all, for either mode
    prod.NoteRenderWithoutResult();
    CHECK(ResultKindOf(prod) == ResultKind::None && !ServesBringUpConsumer(ResultKindOf(prod)));
    bring.NoteRenderWithoutResult();
    CHECK(ResultKindOf(bring) == ResultKind::None);
    // a stream that was never built has no kind
    StreamState fresh;
    fresh.everRan = true;
    fresh.attempts = fresh.ranAttempt = 1;
    CHECK(ResultKindOf(fresh) == ResultKind::None);

    // the production stream is bound like the bring-up stream: by the preview image's pointer, one per image
    StreamTable table;
    const int a = table.Bind(0x1000, kOutW, kOutH, 10);
    CHECK(a != kNoStream && table.Bind(0x1000, kRenderW, kRenderH, 11) == a); // the same image finds the same stream in either mode
    CHECK(table.Bind(0x2000, kOutW, kOutH, 12) != a);
}

void TestInputsFit()
{
    const ImageInfo colour { kOutW, kOutH, kFormatRgba16Float }, depth { kRenderW, kRenderH, kFormatR32Float },
        motion { kRenderW, kRenderH, kFormatRg16Float };
    const StreamShape s = Production();
    CHECK(ProductionInputsFit(s, colour, depth, motion));
    // each mismatch is caught: a copy between different sizes or formats is not allowed by Direct3D
    CHECK(!ProductionInputsFit(s, { kOutW / 2, kOutH, kFormatRgba16Float }, depth, motion));
    CHECK(!ProductionInputsFit(s, { kOutW, kOutH / 2, kFormatRgba16Float }, depth, motion));
    CHECK(!ProductionInputsFit(s, { kOutW, kOutH, 28 /* RGBA8 */ }, depth, motion));
    CHECK(!ProductionInputsFit(s, colour, { kRenderW * 2, kRenderH, kFormatR32Float }, motion));
    CHECK(!ProductionInputsFit(s, colour, { kRenderW, kRenderH, 40 /* D32 */ }, motion));
    CHECK(!ProductionInputsFit(s, colour, depth, { kRenderW, kRenderH * 2, kFormatRg16Float }));
    CHECK(!ProductionInputsFit(s, colour, depth, { kRenderW, kRenderH, kFormatR32Float }));
    // a stream that is not a production stream, or has no guide size, never fits
    CHECK(!ProductionInputsFit(BringUp(), colour, depth, motion));
    StreamShape noGuides = s;
    noGuides.guideWidth = 0;
    CHECK(!ProductionInputsFit(noGuides, colour, depth, motion));
}

} // namespace

int main()
{
    TestModelCall();
    TestGuidesOf();
    TestRecipe();
    TestStreamModes();
    TestInputsFit();
    return TEST_RESULT("chara_nr_production");
}
