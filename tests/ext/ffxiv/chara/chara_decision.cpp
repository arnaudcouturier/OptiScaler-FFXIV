#include "check.h"
#include "ext/ffxiv/chara/CharaDecision.h"

#include <initializer_list>
#include <string>

using namespace ext::ffxiv::chara;

namespace
{
// A well-formed draw of each kind (576 x 960, the preview renderer's size).
DrawFacts Gbuffer(int entry = 8)
{
    DrawFacts f;
    f.rtv0Present = true;
    f.rtv0Entry = entry;
    f.dsvPresent = true;
    f.dsvEntry = 18;
    f.samples = 1;
    f.targetWidth = 576;
    f.targetHeight = 960;
    f.viewportMatchesTarget = true;
    return f;
}

DrawFacts Prepass(int depthEntry = 18)
{
    DrawFacts f;
    f.dsvPresent = true;
    f.dsvEntry = depthEntry;
    f.vsCb0Bytes = 0x3b0;
    f.samples = 1;
    f.targetWidth = 576;
    f.targetHeight = 960;
    f.viewportMatchesTarget = true;
    return f;
}

DrawFacts Finisher()
{
    DrawFacts f;
    f.psFinisher = true;
    f.rtv0Present = true;
    f.rtv0Entry = 0;
    f.samples = 1;
    f.targetWidth = 576;
    f.targetHeight = 960;
    f.targetFormat = kFormatB8G8R8A8Unorm;
    f.passActive = true;
    f.passWidth = 576;
    f.passHeight = 960;
    return f;
}

DrawFacts Consumer()
{
    DrawFacts f;
    f.psUi = true;
    f.rtv0Present = true;
    f.srvEntry = 0;
    f.targetWidth = 1920;
    f.targetHeight = 1080;
    return f;
}

void TestKinds()
{
    CHECK(Classify(Gbuffer(8)) == DrawKind::GBuffer);
    CHECK(Classify(Gbuffer(13)) == DrawKind::GBuffer);
    CHECK(Classify(Prepass(18)) == DrawKind::Prepass);
    CHECK(Classify(Prepass(19)) == DrawKind::Prepass);
    CHECK(Classify(Finisher()) == DrawKind::Finisher);
    CHECK(Classify(Consumer()) == DrawKind::Consumer);
    CHECK(Classify(DrawFacts {}) == DrawKind::Ignore);

    // the equipment display shader is a consumer too
    auto equip = Consumer();
    equip.psUi = false;
    equip.psEquipDisplay = true;
    CHECK(Classify(equip) == DrawKind::Consumer);
}

void TestContext()
{
    for (auto make : { +[] { return Gbuffer(); }, +[] { return Prepass(); }, +[] { return Finisher(); }, +[] { return Consumer(); } })
    {
        auto f = make();
        CHECK(Classify(f) != DrawKind::Ignore);
        f.immediate = false; // deferred contexts stay native
        CHECK(Classify(f) == DrawKind::Ignore);
        f = make();
        f.reentrant = true; // the feature's own draws and other modules' replays
        CHECK(Classify(f) == DrawKind::Ignore);
    }
}

void TestPrepassRules()
{
    auto f = Prepass();
    CHECK(Classify(f) == DrawKind::Prepass);

    f.vsCb0Bytes = 0x3af; // below the camera block
    CHECK(Classify(f) == DrawKind::Ignore);
    f.vsCb0Bytes = 0x3b0;
    CHECK(Classify(f) == DrawKind::Prepass);
    f.vsCb0Bytes = 0x10000; // larger blocks are accepted
    CHECK(Classify(f) == DrawKind::Prepass);

    f = Prepass();
    f.dsvEntry = 17; // not a depth candidate
    CHECK(Classify(f) == DrawKind::Ignore);
    f.dsvEntry = 20;
    CHECK(Classify(f) == DrawKind::Ignore);
    f.dsvEntry = kEntryNone;
    CHECK(Classify(f) == DrawKind::Ignore);

    f = Prepass();
    f.rtv0Present = true; // a prepass has no colour target
    f.rtv0Entry = kEntryNone;
    CHECK(Classify(f) == DrawKind::Ignore);

    f = Prepass();
    f.dsvPresent = false;
    CHECK(Classify(f) == DrawKind::Ignore);

    // geometry acceptance applies to the prepass as well
    f = Prepass();
    f.viewportMatchesTarget = false;
    CHECK(Classify(f) == DrawKind::Ignore);
    f = Prepass();
    f.samples = 4;
    CHECK(Classify(f) == DrawKind::Ignore);
    f = Prepass();
    f.targetWidth = 4097;
    CHECK(Classify(f) == DrawKind::Ignore);
}

void TestGbufferRules()
{
    for (int entry = -1; entry <= 20; ++entry)
    {
        auto f = Gbuffer(entry);
        CHECK(Classify(f) == ((entry == 8 || entry == 13) ? DrawKind::GBuffer : DrawKind::Ignore));
    }

    auto f = Gbuffer();
    f.dsvPresent = false;
    CHECK(Classify(f) == DrawKind::Ignore);
    f = Gbuffer();
    f.samples = 2;
    CHECK(Classify(f) == DrawKind::Ignore);
    f = Gbuffer();
    f.viewportMatchesTarget = false;
    CHECK(Classify(f) == DrawKind::Ignore);
    f = Gbuffer();
    f.targetWidth = 4096; // the limit is accepted
    f.targetHeight = 4096;
    CHECK(Classify(f) == DrawKind::GBuffer);
    f.targetHeight = 4097;
    CHECK(Classify(f) == DrawKind::Ignore);
    f = Gbuffer();
    f.targetWidth = 4097;
    CHECK(Classify(f) == DrawKind::Ignore);
    f = Gbuffer();
    f.rtv0Present = false;
    CHECK(Classify(f) == DrawKind::Ignore);

    // a geometry draw the module cannot follow is not turned into a consumer by its shader
    f = Gbuffer();
    f.psUi = true;
    f.srvEntry = 0;
    f.viewportMatchesTarget = false;
    CHECK(Classify(f) == DrawKind::Ignore);
}

void TestFinisherRules()
{
    auto f = Finisher();
    CHECK(IsFinisher(f));

    // the tag is enough for any of the preview renderer's outputs, whatever else the draw looks like
    for (int entry = 0; entry <= 7; ++entry)
    {
        auto tagged = DrawFacts {};
        tagged.psFinisher = true;
        tagged.rtv0Entry = entry;
        CHECK(Classify(tagged) == DrawKind::Finisher && !IsOffTableFinisher(tagged));
    }

    // the first in-game test saw the finisher shader draw into a 960x540 target that is not in the table: not a preview draw
    for (int entry : { kEntryNone, 8, 13, 18, 19 })
    {
        auto offTable = DrawFacts {};
        offTable.psFinisher = true;
        offTable.rtv0Entry = entry;
        offTable.rtv0Present = true;
        offTable.targetWidth = 960;
        offTable.targetHeight = 540;
        CHECK(IsOffTableFinisher(offTable) && !IsFinisher(offTable) && Classify(offTable) == DrawKind::Ignore);
    }
    CHECK(!IsOffTableFinisher(DrawFacts {})); // no finisher shader, nothing to report

    // fallback for a shader created before the hooks: shape of the draw
    auto early = Finisher();
    early.psFinisher = false;
    early.anySrvIsEntry8 = true;
    CHECK(IsFinisher(early) && Classify(early) == DrawKind::Finisher);
    for (int entry = 0; entry <= 7; ++entry)
    {
        early.rtv0Entry = entry;
        CHECK(IsFinisher(early));
    }
    early.rtv0Entry = 8;
    CHECK(!IsFinisher(early));
    early.rtv0Entry = kEntryNone;
    CHECK(!IsFinisher(early));
    early = Finisher();
    early.psFinisher = false;
    early.anySrvIsEntry8 = true;
    early.passActive = false;
    CHECK(!IsFinisher(early));
    early.passActive = true;
    early.targetWidth = 288;
    CHECK(!IsFinisher(early));
    early.targetWidth = 576;
    early.targetHeight = 480;
    CHECK(!IsFinisher(early));
    early.targetHeight = 960;
    early.targetFormat = 28;
    CHECK(!IsFinisher(early));
    early.targetFormat = kFormatB8G8R8A8Unorm;
    early.anySrvIsEntry8 = false;
    CHECK(!IsFinisher(early));
    early.anySrvIsEntry8 = true;
    CHECK(IsFinisher(early));
    early.rtv0Present = false;
    CHECK(!IsFinisher(early));
}

void TestConsumerRules()
{
    auto f = Consumer();
    f.srvEntry = kEntryNone; // a UI draw that samples no preview image is no consumer
    CHECK(Classify(f) == DrawKind::Ignore);
    for (int entry = 0; entry < 20; ++entry)
    {
        f = Consumer();
        f.srvEntry = entry;
        CHECK(Classify(f) == DrawKind::Consumer);
    }
    f = Consumer();
    f.psUi = false; // not a UI shader of the set
    CHECK(Classify(f) == DrawKind::Ignore);
}

// Exhaustive: every combination of the boolean facts and the interesting entries gives a kind consistent with the rules.
void TestExhaustive()
{
    int checked = 0;
    for (unsigned mask = 0; mask < (1u << 11); ++mask)
    {
        for (int rtvEntry : { -1, 0, 7, 8, 13, 18 })
        {
            for (int dsvEntry : { -1, 18, 19, 8 })
            {
                DrawFacts f;
                f.immediate = (mask & 1) != 0;
                f.reentrant = (mask & 2) != 0;
                f.psFinisher = (mask & 4) != 0;
                f.psUi = (mask & 8) != 0;
                f.rtv0Present = (mask & 16) != 0;
                f.dsvPresent = (mask & 32) != 0;
                f.viewportMatchesTarget = (mask & 64) != 0;
                f.samples = (mask & 128) ? 1 : 4;
                f.vsCb0Bytes = (mask & 256) ? 0x3b0 : 0;
                f.srvEntry = (mask & 512) ? 3 : kEntryNone;
                f.psEquipDisplay = (mask & 1024) != 0;
                f.rtv0Entry = rtvEntry;
                f.dsvEntry = dsvEntry;
                f.targetWidth = 576;
                f.targetHeight = 960;

                const DrawKind k = Classify(f);
                ++checked;

                if (!f.immediate || f.reentrant)
                {
                    CHECK(k == DrawKind::Ignore);
                    continue;
                }
                const bool prepass = !f.rtv0Present && f.dsvPresent && (dsvEntry == 18 || dsvEntry == 19) && f.vsCb0Bytes >= 0x3b0;
                const bool gbuffer = f.rtv0Present && (rtvEntry == 8 || rtvEntry == 13) && f.dsvPresent && f.samples == 1;
                const bool geometryOk = f.samples == 1 && f.viewportMatchesTarget;
                const bool ui = (f.psUi || f.psEquipDisplay) && f.srvEntry >= 0;
                DrawKind expected = DrawKind::Ignore;
                if (prepass || gbuffer)
                    expected = geometryOk ? (prepass ? DrawKind::Prepass : DrawKind::GBuffer) : DrawKind::Ignore;
                else if (f.psFinisher && rtvEntry >= 0 && rtvEntry <= 7)
                    expected = DrawKind::Finisher;
                else if (ui)
                    expected = DrawKind::Consumer;
                CHECK(k == expected);
            }
        }
    }
    CHECK(checked == (1 << 11) * 6 * 4);
}

void TestFinisherCheck()
{
    FinisherCheck ok;
    ok.rtv0Present = true;
    ok.passActive = true;
    ok.targetWidth = ok.passWidth = 576;
    ok.targetHeight = ok.passHeight = 960;
    ok.targetFormat = kFormatB8G8R8A8Unorm;
    ok.viewportWidth = 576;
    ok.viewportHeight = 960;
    CHECK(FinisherAccepted(ok));

    auto c = ok;
    c.rtv0Present = false;
    CHECK(!FinisherAccepted(c));
    c = ok;
    c.depthOrStencilTest = true;
    CHECK(!FinisherAccepted(c));
    c = ok;
    c.passActive = false;
    CHECK(!FinisherAccepted(c));
    c = ok;
    c.targetWidth = 288;
    CHECK(!FinisherAccepted(c));
    c = ok;
    c.passHeight = 480;
    CHECK(!FinisherAccepted(c));
    c = ok;
    c.targetFormat = 28;
    CHECK(!FinisherAccepted(c));

    // viewport: top-left within [0, 0.5], full size
    c = ok;
    c.viewportX = 0.5f;
    c.viewportY = 0.5f;
    CHECK(FinisherAccepted(c));
    c.viewportX = 0.51f;
    CHECK(!FinisherAccepted(c));
    c.viewportX = 0.0f;
    c.viewportY = -0.1f;
    CHECK(!FinisherAccepted(c));
    c = ok;
    c.viewportWidth = 575;
    CHECK(!FinisherAccepted(c));
    c = ok;
    c.viewportHeight = 961;
    CHECK(!FinisherAccepted(c));
}

// The first failing condition, in the order the check lists them; every reason has a name for the log.
void TestFinisherVerdict()
{
    FinisherCheck ok;
    ok.rtv0Present = true;
    ok.passActive = true;
    ok.targetWidth = ok.passWidth = 576;
    ok.targetHeight = ok.passHeight = 960;
    ok.targetFormat = kFormatB8G8R8A8Unorm;
    ok.viewportWidth = 576;
    ok.viewportHeight = 960;
    CHECK(CheckFinisher(ok) == FinisherVerdict::Accepted);

    auto c = ok;
    c.rtv0Present = false;
    CHECK(CheckFinisher(c) == FinisherVerdict::NoRenderTarget);
    c = ok;
    c.depthOrStencilTest = true;
    CHECK(CheckFinisher(c) == FinisherVerdict::DepthOrStencilTest);
    c = ok;
    c.passActive = false;
    CHECK(CheckFinisher(c) == FinisherVerdict::NoPass);
    c = ok;
    c.targetWidth = 288;
    CHECK(CheckFinisher(c) == FinisherVerdict::SizeMismatch);
    c = ok;
    c.passHeight = 480;
    CHECK(CheckFinisher(c) == FinisherVerdict::SizeMismatch);
    c = ok;
    c.targetFormat = 91; // B8G8R8A8_UNORM_SRGB
    CHECK(CheckFinisher(c) == FinisherVerdict::FormatMismatch);
    c = ok;
    c.viewportX = 0.75f;
    CHECK(CheckFinisher(c) == FinisherVerdict::ViewportOrigin);
    c = ok;
    c.viewportY = -1.0f;
    CHECK(CheckFinisher(c) == FinisherVerdict::ViewportOrigin);
    c = ok;
    c.viewportWidth = 288;
    CHECK(CheckFinisher(c) == FinisherVerdict::ViewportSize);

    // several failures: the earlier condition is the one reported
    c = ok;
    c.passActive = false;
    c.targetFormat = 28;
    c.viewportWidth = 1;
    CHECK(CheckFinisher(c) == FinisherVerdict::NoPass);
    c.passActive = true;
    c.targetWidth = 100;
    CHECK(CheckFinisher(c) == FinisherVerdict::SizeMismatch);

    // the boolean and the reason always agree
    for (const auto& probe : { ok, c })
        CHECK(FinisherAccepted(probe) == (CheckFinisher(probe) == FinisherVerdict::Accepted));

    for (size_t i = 0; i < kFinisherVerdicts; ++i)
        CHECK(std::string(FinisherVerdictName(static_cast<FinisherVerdict>(i))) != "?");
}

void TestPassState()
{
    PassState pass;
    DrawFacts f = Gbuffer();
    CHECK(!pass.active && pass.counter == 0);
    CHECK(pass.OnGeometry(f));
    CHECK(pass.active && pass.counter == 1 && pass.width == 576 && pass.height == 960);
    CHECK(!pass.OnGeometry(f)); // the same pass continues
    CHECK(pass.counter == 1);
    pass.End();
    CHECK(!pass.active);
    f.targetWidth = 288;
    f.targetHeight = 480;
    CHECK(pass.OnGeometry(f));
    CHECK(pass.counter == 2 && pass.width == 288 && pass.height == 480);
}

// Two renders in one presented frame: an accepted finisher closes the render, so the next prepass or G-buffer draw starts a new pass
// (a new counter, hence a new jitter phase and fresh guides) instead of sharing the first render's pass.
void TestSecondRenderInOneFrame()
{
    PassState pass;
    const DrawFacts first = Gbuffer(8);
    CHECK(pass.OnGeometry(first) && pass.counter == 1);
    CHECK(!pass.OnGeometry(Gbuffer(13)));
    pass.OnFinisher(FinisherVerdict::Accepted);

    // same size: the second render is its own pass
    CHECK(pass.OnGeometry(Gbuffer(8)));
    CHECK(pass.counter == 2 && pass.active && pass.width == 576 && pass.height == 960);
    pass.OnFinisher(FinisherVerdict::Accepted);

    // another size: its own pass at its own size
    DrawFacts other = Gbuffer(8);
    other.targetWidth = 288;
    other.targetHeight = 480;
    CHECK(pass.OnGeometry(other));
    CHECK(pass.counter == 3 && pass.width == 288 && pass.height == 480);

    // a finisher that is rejected ends the pass; the next geometry draw starts a new one as before
    pass.OnFinisher(FinisherVerdict::SizeMismatch);
    CHECK(!pass.active);
    CHECK(pass.OnGeometry(first) && pass.counter == 4);

    // an accepted finisher alone (no geometry after it) leaves the facts of its render readable
    pass.OnFinisher(FinisherVerdict::Accepted);
    CHECK(pass.active && pass.width == 576 && pass.height == 960);
    pass.End();
    CHECK(!pass.active);
    CHECK(pass.OnGeometry(first) && pass.counter == 5); // End() followed by a draw: new pass, nothing stays finished
}

// A geometry draw of another size than the running pass (a second render that begins before the first one finished) does not follow
// the pass: the module leaves it native and unjittered.
void TestGeometryFollowsPass()
{
    PassState pass;
    const DrawFacts first = Gbuffer(8);
    CHECK(pass.OnGeometry(first));
    CHECK(GeometryFollowsPass(pass.width, pass.height, first.targetWidth, first.targetHeight));

    DrawFacts other = Gbuffer(8);
    other.targetWidth = 288;
    other.targetHeight = 480;
    CHECK(Classify(other) == DrawKind::GBuffer);
    CHECK(!pass.OnGeometry(other)); // it joins the running pass as far as the counter goes ...
    CHECK(pass.counter == 1 && pass.width == 576 && pass.height == 960);
    CHECK(!GeometryFollowsPass(pass.width, pass.height, other.targetWidth, other.targetHeight)); // ... but is not followed
    other = Prepass(18);
    other.targetWidth = 576; // one dimension equal is not enough
    other.targetHeight = 540;
    CHECK(!GeometryFollowsPass(pass.width, pass.height, other.targetWidth, other.targetHeight));
    other.targetWidth = 288;
    other.targetHeight = 960;
    CHECK(!GeometryFollowsPass(pass.width, pass.height, other.targetWidth, other.targetHeight));

    // no pass yet (size 0): nothing follows
    CHECK(!GeometryFollowsPass(0, 0, 0, 0));
    CHECK(!GeometryFollowsPass(0, 0, 576, 960));
}

// The second in-game test: no pass ever reached the finisher. An untagged draw that writes the same output with a depth test
// ended the pass before the tagged finisher arrived.
void TestPassStartObservedSignatures()
{
    // The preview renderer of that build has no depth prepass: the first G-buffer draw starts the pass.
    PassState pass;
    auto opaque = Gbuffer(8); // target entry 8, depth entry 18
    CHECK(Classify(opaque) == DrawKind::GBuffer);
    CHECK(pass.OnGeometry(opaque) && pass.active && pass.counter == 1 && pass.width == 576 && pass.height == 960);

    // the transparent G-buffer: a depth view that is not a table entry
    PassState other;
    auto transparent = Gbuffer(13);
    transparent.dsvEntry = kEntryNone;
    CHECK(transparent.dsvPresent);
    CHECK(Classify(transparent) == DrawKind::GBuffer);
    CHECK(other.OnGeometry(transparent) && other.active);

    // a draw into a G-buffer entry without any depth view is not followed
    transparent.dsvPresent = false;
    CHECK(Classify(transparent) == DrawKind::Ignore);
}

DrawFacts ObservedShapeOnlyDraw(bool tagKnown, const PassState& pass)
{
    // CRC C52B0FE9: no tag, writes output entry 0 at the pass size in B8G8R8A8 with a depth view, samples the G-buffer
    DrawFacts f;
    f.rtv0Present = true;
    f.rtv0Entry = 0;
    f.dsvPresent = true;
    f.dsvEntry = 18;
    f.samples = 1;
    f.targetWidth = 576;
    f.targetHeight = 960;
    f.targetFormat = kFormatB8G8R8A8Unorm;
    f.anySrvIsEntry8 = true;
    f.passActive = pass.active;
    f.passWidth = pass.width;
    f.passHeight = pass.height;
    f.finisherTagKnown = tagKnown;
    return f;
}

DrawFacts ObservedTaggedFinisher(const PassState& pass)
{
    // CRC FDB2F4FC: tagged, draws into entry 0, no depth view
    DrawFacts f = Finisher();
    f.passActive = pass.active;
    f.passWidth = pass.width;
    f.passHeight = pass.height;
    f.finisherTagKnown = true;
    return f;
}

FinisherCheck ObservedCheck(const PassState& pass, bool depthTest)
{
    FinisherCheck c;
    c.rtv0Present = true;
    c.depthOrStencilTest = depthTest;
    c.passActive = pass.active;
    c.targetWidth = c.viewportWidth = 576;
    c.targetHeight = c.viewportHeight = 960;
    c.passWidth = pass.width;
    c.passHeight = pass.height;
    c.targetFormat = kFormatB8G8R8A8Unorm;
    c.viewportX = c.viewportY = 0.5f;
    return c;
}

// The frame counter moves at an arbitrary moment (the bridge's present callback). It never ends a pass by itself: a render in progress
// completes in its own pass, and the first geometry draw after the counter moved starts the next pass.
void TestFrameCounterDoesNotCutARender()
{
    PassState pass;
    pass.NoteFrame(7);
    const DrawFacts g = Gbuffer(8);
    CHECK(pass.OnGeometry(g) && pass.counter == 1 && pass.startFrame == 7);
    CHECK(!pass.OnGeometry(Gbuffer(13)));

    // the callback fires between the G-buffer draws and the finisher
    pass.NoteFrame(8);
    CHECK(pass.active && !pass.finished); // nothing ended the pass
    FinisherCheck c = ObservedCheck(pass, false);
    CHECK(CheckFinisher(c) == FinisherVerdict::Accepted); // the finisher still finds its pass
    pass.OnFinisher(CheckFinisher(c));
    CHECK(pass.OnGeometry(g) && pass.counter == 2 && pass.startFrame == 8); // the next render: a new pass

    // more geometry draws of that render in the same frame: the same pass
    CHECK(!pass.OnGeometry(Gbuffer(13)) && pass.counter == 2);

    // a pass that never reached an accepted finisher (a render the module could not follow): the first geometry draw after the counter
    // moved starts a new pass, so a stale pass cannot stay alive across frames
    pass.NoteFrame(9);
    CHECK(pass.OnGeometry(g) && pass.counter == 3 && pass.startFrame == 9);
    pass.NoteFrame(9); // the same value again changes nothing
    CHECK(!pass.OnGeometry(g) && pass.counter == 3);

    // the counter seen before any pass (startFrame 0) does not start one by itself
    PassState fresh;
    fresh.NoteFrame(100);
    CHECK(!fresh.active && fresh.counter == 0);
}

// KNOWN LIMIT, pinned as it is today (not a wanted behaviour): a frame callback between two G-buffer draws of one render splits the render
// into two passes. The callback is the bridge's present callback, which runs on the presenting thread, the thread that issues the draws,
// and a preview render does not straddle a present, so nothing reaches this today. If the callback ever moves to another thread, the second
// draw below would start a new pass (a new jitter phase, the motion target cleared) and the rest of the render would run without jitter;
// the fix would be to restart only when the counter is two or more ahead, and this check would then change.
void TestFrameCounterBetweenGbufferDrawsKnownLimit()
{
    PassState pass;
    pass.NoteFrame(7);
    CHECK(pass.OnGeometry(Gbuffer(8)) && pass.counter == 1);
    pass.NoteFrame(8); // the callback fires between two G-buffer draws of the same render
    CHECK(pass.OnGeometry(Gbuffer(13)) && pass.counter == 2); // today: a second pass starts mid-render
}

// One render as the second in-game log shows it, step by step as the dispatcher runs it. The pass state is NOT reset between
// renders: a render starts a new pass because the previous one ended (a rejected finisher) or finished (an accepted one), exactly
// as in the game.
FinisherVerdict RunRender(PassState& pass, bool tagKnown)
{
    const uint32_t before = pass.counter;
    CHECK(pass.OnGeometry(Gbuffer(8)));
    CHECK(pass.counter == before + 1);
    CHECK(!pass.OnGeometry(Gbuffer(13)));
    CHECK(pass.counter == before + 1); // the transparent draws of the same render continue the pass

    // the draw that only has the finisher's shape, with a depth test
    const DrawFacts shape = ObservedShapeOnlyDraw(tagKnown, pass);
    if (Classify(shape) == DrawKind::Finisher)
        pass.OnFinisher(CheckFinisher(ObservedCheck(pass, true)));

    const DrawFacts tagged = ObservedTaggedFinisher(pass);
    CHECK(Classify(tagged) == DrawKind::Finisher);
    const FinisherVerdict verdict = CheckFinisher(ObservedCheck(pass, false));
    pass.OnFinisher(verdict);
    return verdict;
}

void TestPassSurvivesShapeOnlyDraw()
{
    PassState pass;
    // the tagged finisher is known: the shape-only draw is not a finisher, so the pass reaches the real one
    const auto shape = ObservedShapeOnlyDraw(true, [] { PassState p; p.OnGeometry(Gbuffer(8)); return p; }());
    CHECK(HasFinisherShape(shape) && IsShapeOnlyFinisher(shape) && !IsFinisher(shape));
    CHECK(Classify(shape) == DrawKind::Ignore);
    CHECK(RunRender(pass, true) == FinisherVerdict::Accepted);
    CHECK(pass.active); // the render is closed but its facts stay: a second finisher of the same render is still checked against it
    CHECK(RunRender(pass, true) == FinisherVerdict::Accepted); // the next render (same state object) is a new pass
    CHECK(pass.counter == 2);
    CHECK(RunRender(pass, true) == FinisherVerdict::Accepted);
    CHECK(pass.counter == 3);

    // before any tagged finisher shader exists (a finisher created before the hooks), the shape still counts: the old behaviour
    const auto early = ObservedShapeOnlyDraw(false, [] { PassState p; p.OnGeometry(Gbuffer(8)); return p; }());
    CHECK(IsFinisher(early) && !IsShapeOnlyFinisher(early) && Classify(early) == DrawKind::Finisher);
    CHECK(RunRender(pass, false) == FinisherVerdict::NoPass); // the bug of the second in-game test, kept only where it is meant
    CHECK(!pass.active);
    CHECK(pass.counter == 4);
    CHECK(RunRender(pass, true) == FinisherVerdict::Accepted); // the next render starts again from the ended pass
    CHECK(pass.counter == 5);

    // a tagged finisher that fails the check still ends the pass
    PassState rejected;
    rejected.OnGeometry(Gbuffer(8));
    rejected.OnFinisher(CheckFinisher(ObservedCheck(rejected, true)));
    CHECK(!rejected.active);
}

void TestShapeOnlyRules()
{
    PassState active;
    active.OnGeometry(Gbuffer(8));
    auto f = ObservedShapeOnlyDraw(true, active);
    CHECK(IsShapeOnlyFinisher(f));
    f.finisherTagKnown = false;
    CHECK(!IsShapeOnlyFinisher(f) && IsFinisher(f));
    f = ObservedShapeOnlyDraw(true, active);
    f.psFinisher = true; // a tagged shader is never "shape only"
    CHECK(!IsShapeOnlyFinisher(f) && IsFinisher(f));
    f = ObservedShapeOnlyDraw(true, active);
    f.anySrvIsEntry8 = false;
    CHECK(!IsShapeOnlyFinisher(f));
    f = ObservedShapeOnlyDraw(true, PassState {});
    CHECK(!IsShapeOnlyFinisher(f)); // no pass active
}

// The dispatcher skips the classification (about ten COM calls per draw) while the preview renderer's table holds no valid image. That
// is only safe if every draw classifies as Ignore then, whatever else it looks like: checked over every combination of the facts that do
// not name a table entry.
void TestEmptyTableClassifiesNothing()
{
    CHECK(ClassificationNeeded(true, false));
    CHECK(ClassificationNeeded(false, true)); // a debug aid looks at draws without the table
    CHECK(ClassificationNeeded(true, true));
    CHECK(!ClassificationNeeded(false, false));

    size_t combos = 0;
    for (unsigned bits = 0; bits < (1u << 12); ++bits)
    {
        for (const uint32_t cb : { 0u, kCameraBlockBytes, 0x10000u })
        {
            for (const uint32_t samples : { 1u, 4u })
            {
                DrawFacts f; // every table entry field stays "none": the table is empty
                f.immediate = (bits & 1u) != 0;
                f.reentrant = (bits & 2u) != 0;
                f.psGeometry = (bits & 4u) != 0;
                f.psFinisher = (bits & 8u) != 0;
                f.psEquipDisplay = (bits & 16u) != 0;
                f.psUi = (bits & 32u) != 0;
                f.rtv0Present = (bits & 64u) != 0;
                f.dsvPresent = (bits & 128u) != 0;
                f.passActive = (bits & 256u) != 0;
                f.finisherTagKnown = (bits & 512u) != 0;
                f.anySrvIsEntry8 = (bits & 1024u) != 0;
                f.viewportMatchesTarget = (bits & 2048u) != 0;
                f.vsCb0Bytes = cb;
                f.samples = samples;
                f.targetWidth = f.passWidth = 576;
                f.targetHeight = f.passHeight = 960;
                f.targetFormat = kFormatB8G8R8A8Unorm;
                CHECK(Classify(f) == DrawKind::Ignore);
                ++combos;
            }
        }
    }
    CHECK(combos == 4096u * 6u);
}

void TestNames()
{
    CHECK(std::string(DrawKindName(DrawKind::Prepass)) == "prepass");
    CHECK(std::string(DrawKindName(DrawKind::GBuffer)) == "GBuffer");
    CHECK(std::string(DrawKindName(DrawKind::Finisher)) == "finisher");
    CHECK(std::string(DrawKindName(DrawKind::Consumer)) == "consumer");
    CHECK(std::string(DrawKindName(DrawKind::Ignore)) == "ignore");
}
} // namespace

int main()
{
    TestKinds();
    TestContext();
    TestPrepassRules();
    TestGbufferRules();
    TestFinisherRules();
    TestConsumerRules();
    TestExhaustive();
    TestFinisherCheck();
    TestFinisherVerdict();
    TestPassState();
    TestSecondRenderInOneFrame();
    TestPassStartObservedSignatures();
    TestFrameCounterDoesNotCutARender();
    TestFrameCounterBetweenGbufferDrawsKnownLimit();
    TestGeometryFollowsPass();
    TestShapeOnlyRules();
    TestPassSurvivesShapeOnlyDraw();
    TestEmptyTableClassifiesNothing();
    TestNames();
    return TEST_RESULT("chara_decision");
}
