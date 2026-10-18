#include "check.h"
#include "ext/ffxiv/chara/CharaNrRecipe.h"

#include <limits>
#include <string>
#include <vector>

using namespace ext::ffxiv::chara::nr;

namespace
{
// Tracks the state of every image and records the order of the operations. Any use of an image in the wrong state is a failure.
struct Fake
{
    State state[8];
    std::vector<std::string> ops;
    int errors = 0;
    bool failEvaluate = false;
    DlssNrConstants constants[4];
    int passes = 0;
    ModelCall call;
    bool evaluated = false;

    Fake()
    {
        for (int i = 0; i < 8; ++i)
            state[i] = RestState(static_cast<Tex>(i));
    }
    State& S(Tex t) { return state[static_cast<int>(t)]; }
    void Expect(Tex t, State s, const char* what)
    {
        if (t != Tex::None && S(t) != s)
        {
            ++errors;
            std::printf("state error: %s on image %d (state %d, wanted %d)\n", what, static_cast<int>(t), static_cast<int>(S(t)),
                        static_cast<int>(s));
        }
    }
    void TimerStart() { ops.push_back("timer+"); }
    void TimerEnd() { ops.push_back("timer-"); }
    void Barrier(Tex t, State from, State to)
    {
        Expect(t, from, "barrier from");
        S(t) = to;
        ops.push_back("barrier");
    }
    bool Pass(const DlssNrConstants& c, Tex source, Tex model, Tex original, Tex target, Tex keep)
    {
        Expect(source, State::Srv, "pass source");
        Expect(model, State::Srv, "pass model");
        Expect(original, State::Srv, "pass original");
        Expect(target, State::Uav, "pass target");
        Expect(keep, State::Uav, "pass keep");
        if (source == target || source == keep || model == target || original == target)
        {
            ++errors; // one image read and written in one dispatch
            std::printf("hazard: image read and written by one pass\n");
        }
        if (passes < 4)
            constants[passes] = c;
        ++passes;
        ops.push_back("pass:" + std::to_string(c.Mode));
        return true;
    }
    bool Evaluate(const ModelCall& m)
    {
        Expect(Tex::Proxy, State::Srv, "model proxy");
        Expect(Tex::Depth, State::Srv, "model depth");
        Expect(Tex::Motion, State::Srv, "model motion");
        Expect(Tex::ModelOut, State::Uav, "model answer");
        call = m;
        evaluated = true;
        ops.push_back("evaluate");
        return !failEvaluate;
    }
    bool AllAtRest() const
    {
        for (int i = 0; i < 7; ++i)
        {
            if (state[i] != RestState(static_cast<Tex>(i)))
                return false;
        }
        return true;
    }
    int IndexOf(const std::string& op) const
    {
        for (size_t i = 0; i < ops.size(); ++i)
        {
            if (ops[i] == op)
                return static_cast<int>(i);
        }
        return -1;
    }
};

FramePlan Plan(bool init)
{
    FramePlan p;
    p.width = 576;
    p.height = 960;
    p.initGuides = init;
    p.reset = init;
    return p;
}

void TestSequence()
{
    Fake f;
    CHECK(RecordRun(f, Plan(false)));
    CHECK(f.errors == 0);
    CHECK(f.AllAtRest()); // shared images back in COMMON, private ones in UAV
    CHECK(f.passes == 2 && f.evaluated);
    CHECK(f.ops.front() == "timer+" && f.ops.back() == "timer-");
    const int encode = f.IndexOf("pass:0"), model = f.IndexOf("evaluate"), resolve = f.IndexOf("pass:1");
    CHECK(encode >= 0 && encode < model && model < resolve);
    CHECK(f.IndexOf("pass:11") == -1); // no zero-motion pass after the first run

    Fake g;
    CHECK(RecordRun(g, Plan(true)));
    CHECK(g.errors == 0 && g.AllAtRest() && g.passes == 3);
    const int zero = g.IndexOf("pass:11");
    CHECK(zero >= 0 && zero < g.IndexOf("pass:0")); // the motion image is zeroed before the model reads it

    // the other result image
    Fake h;
    FramePlan p = Plan(false);
    p.result = Tex::Result;
    CHECK(RecordRun(h, p) && h.errors == 0);
}

void TestConstants()
{
    Fake f;
    FramePlan p = Plan(true);
    p.settings.whitePointScale = 1.5f;
    p.settings.transferStrength = 0.75f;
    p.settings.colourStrength = 0.5f;
    p.settings.maxRatio = 3.0f;
    p.settings.transfer = 0;
    p.settings.applyModel = false;
    RecordRun(f, p);
    const DlssNrConstants& zero = f.constants[0];
    const DlssNrConstants& encode = f.constants[1];
    const DlssNrConstants& resolve = f.constants[2];
    CHECK(zero.Mode == DlssNrMode_ZeroMotion && zero.Width == 576 && zero.Height == 960);
    CHECK(encode.Mode == DlssNrMode_Encode && encode.Passthrough == 1 && encode.WhitePoint == 1.5f);
    CHECK(encode.Width == 576 && encode.Height == 960 && encode.UseGameExposure == 0);
    CHECK(resolve.Mode == DlssNrMode_Resolve && resolve.Passthrough == 1);
    CHECK(resolve.TransferStrength == 0.75f && resolve.ColourStrength == 0.5f && resolve.MaxRatio == 3.0f);
    CHECK(resolve.Transfer == 0 && resolve.ApplyModel == 0 && resolve.WhitePoint == 1.5f);
    // never a diagnostic overlay
    CHECK(resolve.DebugView == 0 && resolve.CompareMode == 0 && resolve.ShowSkinMask == 0);
    CHECK(resolve.UseGameExposure == 0 && resolve.SpatialResidual == 0 && resolve.ResidualRejection == 0);

    // strengths of the skin and environment controls are clamped to 0..1, a non-finite value means 1
    Settings s;
    s.skinDetail = 2.0f;
    s.skinColour = -1.0f;
    s.environmentDetail = std::numeric_limits<float>::quiet_NaN();
    s.environmentColour = 0.25f;
    const DlssNrConstants r = MakeResolve(8, 8, s);
    CHECK(r.SkinDetail == 1.0f && r.SkinColour == 0.0f && r.EnvironmentDetail == 1.0f && r.EnvironmentColour == 0.25f);
    s.skinToneEnabled = false;
    CHECK(MakeResolve(8, 8, s).SkinColour == 0.0f);
    s.skinProtection = true;
    CHECK(MakeResolve(8, 8, s).SkinProtection == 1u);
}

void TestModelCall()
{
    Fake f;
    FramePlan p = Plan(true);
    p.tuning.intensity = 0.8f;
    p.tuning.style = 2;
    p.tuning.structure = 0.6f;
    p.tuning.tone = 0.4f;
    p.tuning.skin = -1.0f;
    p.tuning.autoMask = false;
    RecordRun(f, p);
    const ModelCall& m = f.call;
    CHECK(m.width == 576 && m.height == 960);
    CHECK(m.guideWidth == 576 && m.guideHeight == 960 && m.motionWidth == 576 && m.motionHeight == 960);
    CHECK(m.depthBaseX == 0 && m.depthBaseY == 0 && m.motionBaseX == 0 && m.motionBaseY == 0);
    CHECK(m.depthInverted == 0 && m.reset == 1);
    CHECK(m.intensity == 0.8f && m.style == 2 && m.structure == 0.6f && m.tone == 0.4f && m.skin == -1.0f && m.autoMask == 0);
    CHECK(m.mvScaleX == 1.0f && m.mvScaleY == 1.0f);

    Fake g;
    RecordRun(g, Plan(false));
    CHECK(g.call.reset == 0 && g.call.autoMask == 1);
}

void TestFailure()
{
    // a failing model call stops the recording: no resolve, no timer end; the caller discards the list
    Fake f;
    f.failEvaluate = true;
    CHECK(!RecordRun(f, Plan(false)));
    CHECK(f.IndexOf("pass:1") == -1 && f.ops.back() != "timer-");
}
} // namespace

int main()
{
    TestSequence();
    TestConstants();
    TestModelCall();
    TestFailure();
    return TEST_RESULT("chara_nr_recipe");
}
