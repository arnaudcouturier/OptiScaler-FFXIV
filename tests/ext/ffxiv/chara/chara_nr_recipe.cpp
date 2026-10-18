#include "check.h"
#include "ext/ffxiv/chara/CharaNrRecipe.h"

#include <limits>
#include <string>
#include <tuple>
#include <vector>

using namespace ext::ffxiv::chara::nr;

namespace
{
// Tracks the state of every image and records the order of the operations. Any use of an image in the wrong state is a failure.
struct Fake
{
    State state[kTexCount];
    std::vector<std::string> ops;
    std::vector<std::tuple<Tex, State, State>> barriers; // every barrier in order
    bool touched[kTexCount] = {};                         // images any barrier or pass named
    int errors = 0;
    bool failEvaluate = false;
    DlssNrConstants constants[4];
    int passes = 0;
    ModelCall call;
    bool evaluated = false;
    bool quiet = false; // a deliberately wrong sequence: no message

    Fake()
    {
        for (size_t i = 0; i < kTexCount; ++i)
            state[i] = RestState(static_cast<Tex>(i));
    }
    State& S(Tex t) { return state[static_cast<int>(t)]; }
    void Expect(Tex t, State s, const char* what)
    {
        if (t != Tex::None && S(t) != s)
        {
            ++errors;
            if (!quiet)
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
        touched[static_cast<int>(t)] = true;
        barriers.emplace_back(t, from, to);
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
        for (size_t i = 0; i < static_cast<size_t>(Tex::None); ++i)
        {
            if (state[i] != RestState(static_cast<Tex>(i)))
                return false;
        }
        return true;
    }
    // the barriers of one image, in order
    std::vector<std::pair<State, State>> BarriersOf(Tex t) const
    {
        std::vector<std::pair<State, State>> out;
        for (const auto& b : barriers)
        {
            if (std::get<0>(b) == t)
                out.emplace_back(std::get<1>(b), std::get<2>(b));
        }
        return out;
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

}

// Two runs of one stream, as the module issues them: the plan names the result image (and in the late variant the colour image) a run
// uses, and the recording touches exactly those. Before, both results were the same name here, so a run that wrote the wrong image could
// not be told from one that wrote the right one.
void TestTwoBuffers()
{
    Fake f; // one set of images shared by the two runs, as in a stream
    FramePlan a = Plan(true);
    a.result = Tex::Result0;
    a.colour = Tex::Colour0;
    CHECK(RecordRun(f, a) && f.errors == 0 && f.AllAtRest());
    CHECK(f.touched[static_cast<int>(Tex::Result0)] && !f.touched[static_cast<int>(Tex::Result1)]);
    CHECK(f.touched[static_cast<int>(Tex::Colour0)] && !f.touched[static_cast<int>(Tex::Colour1)]);

    FramePlan b = Plan(false);
    b.result = Tex::Result1;
    b.colour = Tex::Colour1;
    CHECK(RecordRun(f, b) && f.errors == 0 && f.AllAtRest());
    CHECK(f.touched[static_cast<int>(Tex::Result1)] && f.touched[static_cast<int>(Tex::Colour1)]);

    // run b alone touches only its own pair, in the order the first run used for its own
    Fake g;
    CHECK(RecordRun(g, b) && g.errors == 0);
    CHECK(!g.touched[static_cast<int>(Tex::Result0)] && !g.touched[static_cast<int>(Tex::Colour0)]);
    Fake h;
    FramePlan a2 = Plan(false);
    CHECK(RecordRun(h, a2));
    const auto r0 = h.BarriersOf(Tex::Result0);
    const auto r1 = g.BarriersOf(Tex::Result1);
    CHECK(r0 == r1 && r0.size() == 2); // Common -> Uav before the resolve, Uav -> Common after it
    CHECK(h.BarriersOf(Tex::Colour0) == g.BarriersOf(Tex::Colour1));
    CHECK(h.BarriersOf(Tex::Colour0).size() == 2);

    // the shared images rest in COMMON, so alternating never leaves an image in a state the next run does not expect
    CHECK(IsShared(Tex::Colour1) && IsShared(Tex::Result1) && RestState(Tex::Result1) == State::Common && RestState(Tex::Colour1) == State::Common);

    // a recording that uses the plan's other result image is caught by the state tracker: its resolve writes an image it never
    // transitioned (a run for buffer 1 that names Result0 in the pass but Result1 in the barrier would fail the same way)
    Fake bad;
    FramePlan mixed = Plan(false);
    mixed.result = Tex::Result1;
    bad.quiet = true;
    bad.S(Tex::Result1) = State::Uav; // the other run left it in the wrong state
    RecordRun(bad, mixed);
    CHECK(bad.errors > 0);
}

// The state model of the Fake is the recipe's own; this is an independent oracle: the transitions the main NR pass makes for the same
// images on its SDR path, read from DlssNr_Dx12.cpp (colorCopy = the proxy, hdrCopy = the kept original, output = the model's answer):
//   encode writes colorCopy and hdrCopy in UNORDERED_ACCESS, then UAV -> NON_PIXEL_SHADER_RESOURCE for both (lines 2419, 2425);
//   MakeModelWritable(output) keeps/puts the answer in UAV before the evaluate (2678), MakeModelReadable(finalAnswer) UAV -> NPSR after it
//   (2700), and after the resolve MakeModelWritable(output) NPSR -> UAV (2962/3001); colorCopy and hdrCopy end NPSR -> UAV (3005, 3070).
// What this cannot prove, and only a GPU run with the D3D12 debug layer can: COMMON <-> UAV / NPSR transitions of the shared images
// (opened from a D3D11 shared handle), the model's own use of its inputs inside the evaluate, and that the encode pass does not need
// the colour image in another state.
void TestMatchesTheMainPass()
{
    Fake f;
    CHECK(RecordRun(f, Plan(false)));
    using P = std::pair<State, State>;
    const std::vector<P> privateUse = { P { State::Uav, State::Srv }, P { State::Srv, State::Uav } };
    CHECK(f.BarriersOf(Tex::Proxy) == privateUse);    // colorCopy
    CHECK(f.BarriersOf(Tex::Original) == privateUse); // hdrCopy
    CHECK(f.BarriersOf(Tex::ModelOut) == privateUse); // output: readable after the evaluate, writable again after the resolve
    // the main pass starts every frame with these three in UAV and ends with them there
    CHECK(RestState(Tex::Proxy) == State::Uav && RestState(Tex::Original) == State::Uav && RestState(Tex::ModelOut) == State::Uav);
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
    TestTwoBuffers();
    TestMatchesTheMainPass();
    TestConstants();
    TestModelCall();
    TestFailure();
    return TEST_RESULT("chara_nr_recipe");
}
