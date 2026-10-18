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
    TestNames();
    return TEST_RESULT("chara_decision");
}
