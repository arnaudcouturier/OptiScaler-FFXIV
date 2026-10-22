#include "check.h"

#include "ext/ffxiv/jitter/FfxivJitterSourceController.h"

#include <cmath>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace ext::ffxiv::jitter;
using namespace ext::ffxiv::jitter::source;

// The boundary half of the source fix against a fake parameter block, fake clock and fake log: when it writes, what it writes, what it never
// writes, how the calibrate-and-scale engine is told to stand down, the fall-back, the key switched off, the log lines.

namespace
{
bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

float Halton(std::uint64_t index, unsigned base)
{
    float factor = 1, result = 0;
    for (; index; index /= base)
    {
        factor /= base;
        result += factor * float(index % base);
    }
    return result - 0.5f;
}

struct Op
{
    bool set = false;
    Param param = Param::JitterX;
    float value = 0.0f;
};

struct FakeEnv
{
    bool key = false;
    HooksView hooks { true, true, true };
    std::map<Param, float> floats;
    std::map<Param, std::uint32_t> uints;
    std::uint64_t now = 100000;
    std::vector<std::string> logs;
    std::vector<Op> ops;
    int keyReads = 0, clockReads = 0, hookCalls = 0;
    bool failSetY = false;

    bool KeyOn()
    {
        ++keyReads;
        return key;
    }
    HooksView Hooks()
    {
        ++hookCalls;
        return hooks;
    }
    std::optional<float> GetFloat(Param p)
    {
        const auto it = floats.find(p);
        if (it == floats.end())
            return std::nullopt;
        ops.push_back({ false, p, it->second });
        return it->second;
    }
    std::uint32_t GetUInt(Param p)
    {
        const auto it = uints.find(p);
        return it == uints.end() ? 0u : it->second;
    }
    void SetFloat(Param p, float v)
    {
        if (failSetY && p == Param::JitterY)
            throw std::runtime_error("set failed");
        ops.push_back({ true, p, v });
        floats[p] = v;
    }
    std::uint64_t NowMs()
    {
        ++clockReads;
        return now;
    }
    void Log(const std::string& s) { logs.push_back(s); }

    bool Logged(const char* part) const
    {
        for (const auto& l : logs)
            if (Has(l, part))
                return true;
        return false;
    }
    int Count(const char* part) const
    {
        int n = 0;
        for (const auto& l : logs)
            if (Has(l, part))
                ++n;
        return n;
    }
    int Writes() const
    {
        int n = 0;
        for (const auto& o : ops)
            if (o.set)
                ++n;
        return n;
    }
};

struct Rig
{
    FakeEnv env;
    SharedState state;
    Controller<FakeEnv> controller;
    float strength = 0.6f;
    std::uint32_t renderW = 1920, renderH = 1080, outW = 3840, outH = 2160, phases = 32;
    unsigned frame = 0;
    float rawX = 0.0f, rawY = 0.0f, drawnX = 0.0f, drawnY = 0.0f;

    Rig()
    {
        env.key = true;
        SetSizes();
    }
    void SetSizes()
    {
        env.uints[Param::SubrectWidth] = renderW;
        env.uints[Param::SubrectHeight] = renderH;
        env.uints[Param::Width] = renderW;
        env.uints[Param::Height] = renderH;
        env.uints[Param::OutWidth] = outW;
        env.uints[Param::OutHeight] = outH;
    }

    // One game frame as the detours record it and the game hands it to NGX: the generator output divided by the strength, the camera sample, the
    // NGX raw pair in the block. `normalized` = the generator hook normalised this frame; `drawnScale` = what the game really multiplied by.
    void Frame(bool normalized = true, bool generated = true, bool observed = true, float drawnScale = 0.0f)
    {
        ++frame;
        env.now += 8;
        const float hx = Halton(frame % phases + 1, 2), hy = Halton(frame % phases + 1, 3);
        const float scale = drawnScale > 0.0f ? drawnScale : strength;
        float ox = hx, oy = hy;
        if (normalized)
        {
            ox = hx / strength;
            oy = hy / strength;
        }
        rawX = ox;
        rawY = oy;
        // the game multiplies the stored output by its strength: the drawn offset
        drawnX = scale * ox;
        drawnY = scale * oy;
        if (generated)
        {
            Generated g;
            g.outX = ox;
            g.outY = oy;
            g.expectedX = hx;
            g.expectedY = hy;
            g.strength = strength;
            g.phases = phases;
            g.renderW = renderW;
            g.renderH = renderH;
            g.outW = outW;
            g.outH = outH;
            g.normalized = normalized;
            g.timeMs = env.now;
            state.RecordGenerated(g, PlanWhy::Normalize);
        }
        if (observed)
        {
            Observed o;
            o.rawX = ox;
            o.rawY = oy;
            o.ndcX = 2.0f * scale * ox / static_cast<float>(renderW);
            o.ndcY = -(2.0f * scale * oy) / static_cast<float>(renderH);
            o.width = renderW;
            o.height = renderH;
            o.timeMs = env.now;
            state.RecordObserved(o);
        }
        env.floats[Param::JitterX] = ox;
        env.floats[Param::JitterY] = oy;
    }
    StepResult Step(bool correctionKey = true) { return controller.Step(env, state, correctionKey); }
};

void TestKeyOffCost()
{
    Rig r;
    r.env.key = false;
    r.Frame();
    const StepResult res = r.Step();
    CHECK(!res.active && !res.standDown);
    CHECK(r.env.keyReads == 1 && r.env.clockReads == 0 && r.env.hookCalls == 0 && r.env.ops.empty() && r.env.logs.empty());
    CHECK(!r.state.Enabled() && !r.controller.Engaged());
    for (int i = 0; i < 1000; ++i)
        r.Step();
    CHECK(r.env.clockReads == 0 && r.env.hookCalls == 0 && r.env.ops.empty() && r.env.logs.empty());
}

void TestActiveWritesDrawnOffset()
{
    Rig r;
    r.Frame();
    StepResult res = r.Step();
    CHECK(res.active && res.standDown);
    CHECK(r.state.Enabled() && r.controller.Engaged());
    // the pair of THIS evaluate is replaced by what was drawn; X then Y; both reads came first
    CHECK(r.env.Writes() == 2);
    const Op& wx = r.env.ops[r.env.ops.size() - 2];
    const Op& wy = r.env.ops[r.env.ops.size() - 1];
    CHECK(wx.set && wx.param == Param::JitterX && wy.set && wy.param == Param::JitterY);
    CHECK(std::fabs(wx.value - r.drawnX) < 1e-5f && std::fabs(wy.value - r.drawnY) < 1e-5f);
    bool writeSeen = false, readAfterWrite = false;
    for (const auto& o : r.env.ops)
    {
        if (o.set)
            writeSeen = true;
        else if (writeSeen && (o.param == Param::JitterX || o.param == Param::JitterY))
            readAfterWrite = true;
    }
    CHECK(!readAfterWrite);
    // the log: the active line, and the stand-down line because the correction key is on
    CHECK(r.env.Count("Jitter source: active (strength 0.600, phases 32 for render 1920x1080 output 3840x2160)") == 1);
    CHECK(r.env.Count("jitter correction stands down") == 1);
    // the probe's snapshot guard follows the generator value before the game's multiplier (0.5 / 0.6)
    CHECK(res.maxJitter > 0.83f && res.maxJitter < 0.9f);

    // many frames: every one of them told what was drawn, the active line stays at one
    for (int i = 0; i < 200; ++i)
    {
        r.Frame();
        res = r.Step();
        CHECK(res.active && res.standDown);
        CHECK(std::fabs(r.env.floats[Param::JitterX] - r.drawnX) < 1e-5f && std::fabs(r.env.floats[Param::JitterY] - r.drawnY) < 1e-5f);
    }
    CHECK(r.env.Count("Jitter source: active") == 1);
    CHECK(r.controller.Totals().checked >= 200 && r.controller.Totals().outside == 0);
    CHECK(r.env.Logged("Jitter source: verified"));
}

void TestNoStandDownLineWithoutCorrection()
{
    Rig r;
    r.Frame();
    r.Step(false);
    CHECK(r.env.Count("Jitter source: active") == 1 && r.env.Count("stands down") == 0);
}

// M1 (changed deliberately: the first version passed every miss through with the engine stood down). A miss while active no longer leaves the
// generator-divided raw value untold: it tells the generator record's own value when one exists for that raw pair; see TestMiss* below. The
// counting of the misses is unchanged.
void TestPairingMissIsCounted()
{
    Rig r;
    r.Frame();
    r.Step();
    // a frame whose camera sample was never recorded: counted as a miss (the generator record exists, so it is told, see TestMissTellsTheRecordedValue)
    r.Frame(true, true, false);
    StepResult res = r.Step();
    CHECK(res.active && res.standDown);
    CHECK(r.controller.Totals().unpaired == 1);
    // a stale sample (older than a second) is a miss too
    r.Frame();
    r.env.now += kMaxSampleAgeMs + 10;
    r.Frame(true, true, false);
    res = r.Step();
    CHECK(r.controller.Totals().unpaired == 2);
    // sizes that do not match the sample
    r.Frame();
    r.env.uints[Param::Width] = 1280;
    r.env.uints[Param::SubrectWidth] = 1280;
    r.Step();
    CHECK(r.controller.Totals().unpaired == 3);
    // the NGX pair absent
    r.env.uints[Param::Width] = 1920;
    r.env.uints[Param::SubrectWidth] = 1920;
    r.Frame();
    r.env.floats.erase(Param::JitterX);
    res = r.Step();
    CHECK(r.controller.Totals().unpaired == 4);
    CHECK(!res.standDown); // nothing to tell and nothing for the engine to scale: it is not stood down
}

void TestUnpairedRunFallsBack()
{
    Rig r;
    r.phases = 100000; // raw pairs that do not repeat within a second (a repeating pair would match an older sample, harmlessly)
    r.Frame();
    r.Step();
    for (unsigned i = 0; i < kUnpairedLimit + 5; ++i)
    {
        r.Frame(true, true, false);
        r.Step();
    }
    CHECK(r.controller.FellBack());
    CHECK(!r.state.Enabled());
    CHECK(r.env.Count("Jitter source: fell back to the old behaviour") == 1);
    CHECK(r.env.Logged("no camera sample matched"));
    // after the fall-back the generator is pass-through (frames are no longer normalised): the old behaviour owns the pair, no stand-down, no write
    r.Frame(false, true, true);
    const int before = r.env.Writes();
    const StepResult res = r.Step();
    CHECK(!res.active && !res.standDown && r.env.Writes() == before);
}

void TestDriftFallsBack()
{
    // the game multiplies by 0.3 although the generator hook believed 0.6: the drawn offset misses the Halton value
    Rig r;
    r.Frame(true, true, true, 0.3f);
    r.Step();
    for (unsigned i = 0; i < kOutsideToleranceLimit + 5; ++i)
    {
        r.Frame(true, true, true, 0.3f);
        r.Step();
    }
    CHECK(r.controller.FellBack() && !r.state.Enabled());
    CHECK(r.env.Count("Jitter source: fell back to the old behaviour") == 1 && r.env.Logged("differs from the generator's Halton value"));
    // the fall-back is latched: further frames do not re-enable it
    for (int i = 0; i < 20; ++i)
    {
        r.Frame();
        r.Step();
    }
    CHECK(!r.state.Enabled() && r.controller.FellBack());
    CHECK(r.env.Logged("inactive (fell back after the verification failed)"));
    // switching the key off and on again tries once more
    r.env.key = false;
    r.Step();
    CHECK(!r.controller.FellBack());
    r.env.key = true;
    r.Frame();
    const StepResult res = r.Step();
    CHECK(res.active && r.state.Enabled());
}

void TestNotMatchedAndGuards()
{
    {
        Rig r;
        r.env.hooks = { false, false, false };
        r.Frame();
        const StepResult res = r.Step();
        CHECK(!res.active && !res.standDown && r.env.Writes() == 0 && !r.state.Enabled());
        CHECK(r.env.Count("Jitter source: inactive (executable not matched)") == 1); // a permanent reason is logged at once
        for (int i = 0; i < 50; ++i)
            r.Step();
        CHECK(r.env.Count("Jitter source: inactive (executable not matched)") == 1);
    }
    {
        Rig r;
        r.env.hooks = { true, false, false };
        r.Frame();
        r.Step();
        CHECK(r.env.Count("inactive (instruction guard failed)") == 1 && r.env.Writes() == 0 && !r.state.Enabled());
    }
}

void TestHooksNotInstalledYet()
{
    Rig r;
    r.env.hooks = { true, true, false };
    r.Frame();
    r.Step();
    CHECK(r.env.logs.empty() && r.env.Writes() == 0 && !r.state.Enabled()); // a transitional reason is not logged at once
    r.env.now += Controller<FakeEnv>::kInactiveDelayMs;
    r.Frame();
    r.Step();
    CHECK(r.env.Count("inactive (hooks not installed)") == 1);
    // installed: the detours are switched on; the generator has not normalised yet, so it is still inactive
    r.env.hooks.installed = true;
    r.state.Clear();
    r.Frame(true, false, false);
    r.env.floats[Param::JitterX] = 0.4f;
    StepResult res = r.Step();
    CHECK(r.state.Enabled() && !res.active && !res.standDown);
    // the generator starts: active
    r.Frame();
    res = r.Step();
    CHECK(res.active && res.standDown && r.env.Count("Jitter source: active") == 1);
}

void TestGeneratorIdle()
{
    Rig r;
    r.Frame();
    CHECK(r.Step().active);
    // the generator stops normalising (strength invalid, another mode): after a second it is idle -> inactive -> the old behaviour
    r.env.now += kRecentMs + 50;
    r.state.NoteLeftAlone(PlanWhy::StrengthInvalid);
    const int before = r.env.Writes();
    StepResult res = r.Step();
    // changed deliberately (M2): the block still holds the pair the fix itself told, so the engine is kept out of it (it used to scale the told pair once)
    CHECK(!res.active && res.standDown && r.env.Writes() == before);
    CHECK(r.env.Count("Jitter source: inactive (the game's generator is not being normalised") == 1);
    // a block the game refreshed is the old behaviour's: the engine owns it
    r.Frame(false, true, true);
    res = r.Step();
    CHECK(!res.active && !res.standDown && r.env.Writes() == before);
    CHECK(r.state.Enabled()); // the detours stay in place, ready for the generator to come back
    // it comes back
    r.Frame();
    CHECK(r.Step().active);
    CHECK(r.env.Count("Jitter source: active") == 2);
}

void TestKeyOffDrainsInFlightFrames()
{
    Rig r;
    r.Frame();
    CHECK(r.Step().active);
    // the key goes off between the generator call of a frame and its boundary: the frame was drawn on the full lattice and is told so
    r.Frame(); // a normalised frame, recorded before the switch
    r.env.key = false;
    StepResult res = r.Step();
    CHECK(!r.state.Enabled() && !r.controller.Engaged());
    CHECK(!res.active && res.standDown);
    CHECK(std::fabs(r.env.floats[Param::JitterX] - r.drawnX) < 1e-5f && std::fabs(r.env.floats[Param::JitterY] - r.drawnY) < 1e-5f);
    CHECK(r.env.Count("Jitter source: inactive (key off)") == 1);
    // a frame drawn the old way afterwards (generator not normalising) is the old behaviour's: untouched
    r.Frame(false, true, true);
    const float rx = r.env.floats[Param::JitterX];
    const int writes = r.env.Writes();
    res = r.Step();
    CHECK(!res.standDown && r.env.Writes() == writes && r.env.floats[Param::JitterX] == rx);
    // a frame with no generator record at all
    r.Frame(true, false, true);
    res = r.Step();
    CHECK(!res.standDown);
    // after the drain window nothing is touched and the clock is no longer read
    r.env.now += kDrainMs + 10;
    r.Frame();
    res = r.Step();
    CHECK(!res.standDown);
    const int clocks = r.env.clockReads;
    r.Step();
    r.Step();
    CHECK(r.env.clockReads == clocks && r.env.hookCalls > 0);
    CHECK(r.env.Count("Jitter source: inactive (key off)") == 1);
}

void TestFallBackDrains()
{
    // a fall-back disables both detours at once; the frame already normalised is still told what was drawn
    Rig r;
    r.phases = 100000;
    r.Frame();
    r.Step();
    for (unsigned i = 0; i < kUnpairedLimit; ++i)
    {
        r.Frame(true, true, false);
        r.Step();
    }
    CHECK(r.controller.FellBack() && !r.state.Enabled());
    r.Frame(); // normalised and observed, recorded just before the fall-back took effect
    const StepResult res = r.Step();
    CHECK(!res.active && res.standDown);
    CHECK(std::fabs(r.env.floats[Param::JitterX] - r.drawnX) < 1e-5f);
}

void TestPhaseChangeIsLogged()
{
    Rig r;
    r.Frame();
    r.Step();
    r.renderW = 2560;
    r.renderH = 1440;
    r.phases = 18;
    r.SetSizes();
    r.Frame();
    const StepResult res = r.Step();
    CHECK(res.active);
    CHECK(r.env.Count("Jitter source: phases 32 -> 18 (strength 0.600, render 2560x1440 output 3840x2160)") == 1);
    // the same phase count again: no new line
    r.Frame();
    r.Step();
    CHECK(r.env.Count("phases 32 -> 18") == 1 && r.env.Count("Jitter source: phases") == 1);
}

void TestUnverifiedFrameIsStillTold()
{
    Rig r;
    r.Frame();
    r.Step();
    // the generator hook left this call alone (no record): the camera sample is still exact, the pair is told, the frame is not verified
    r.Frame(false, false, true);
    const StepResult res = r.Step();
    CHECK(res.active && res.standDown);
    CHECK(std::fabs(r.env.floats[Param::JitterX] - r.drawnX) < 1e-5f);
    CHECK(r.controller.Totals().unverified == 1);
}

void TestOutputSizeDisagreement()
{
    Rig r;
    r.env.uints[Param::OutWidth] = 2560;
    r.env.uints[Param::OutHeight] = 1440;
    r.Frame();
    CHECK(r.Step().active);
    CHECK(r.env.Logged("differs from NGX OutWidth/OutHeight (2560x1440)"));
}

void TestSetFailureRestoresX()
{
    Rig r;
    r.Frame();
    r.Step();
    r.Frame();
    const float original = r.env.floats[Param::JitterX];
    r.env.failSetY = true;
    bool threw = false;
    try
    {
        r.Step();
    }
    catch (const std::exception&)
    {
        threw = true;
    }
    CHECK(threw);
    CHECK(r.env.floats[Param::JitterX] == original); // X was put back
}

// A frame whose raw pair is small enough for the engine to scale (<= kMaxReportedPixels) whatever the Halton index.
void SmallRawFrame(Rig& r, bool generated, bool observed)
{
    do
    {
        r.Frame(true, generated, observed);
    } while (std::fabs(r.rawX) > 0.5f || std::fabs(r.rawY) > 0.5f);
}

// M1, the record path. Changed deliberately from TestNoGuessWritten, which pinned "a miss writes nothing": a pair that matches no camera
// sample is still never "corrected" by ARITHMETIC (raw * strength), but when the generator recorded the frame (matched by its raw pair) the
// recorded Halton value, which the verification proves equals the drawn offset, is what NGX is told.
void TestMissTellsTheRecordedValue()
{
    Rig r;
    r.Frame();
    r.Step();
    SmallRawFrame(r, true, false);
    const StepResult res = r.Step();
    CHECK(res.active && res.standDown);
    CHECK(std::fabs(r.env.floats[Param::JitterX] - r.drawnX) < 1e-5f && std::fabs(r.env.floats[Param::JitterY] - r.drawnY) < 1e-5f);
    // the value written is the generator's own expected value, not raw * strength computed here
    const std::optional<Generated> rec = r.state.FindGenerated(r.rawX, r.rawY);
    CHECK(rec.has_value());
    if (rec)
        CHECK(r.env.floats[Param::JitterX] == rec->expectedX && r.env.floats[Param::JitterY] == rec->expectedY);
    CHECK(r.controller.Totals().unpaired == 1 && r.controller.Totals().toldFromRecord == 1);
    // X then Y
    const Op& wx = r.env.ops[r.env.ops.size() - 2];
    const Op& wy = r.env.ops[r.env.ops.size() - 1];
    CHECK(wx.set && wx.param == Param::JitterX && wy.set && wy.param == Param::JitterY);
}

// M1, no record: nothing sensible can be told. A small raw value is the old behaviour's (a frame drawn before the fix was switched on, the
// character preview): not stood down, the engine scales it as today.
void TestMissWithoutRecordIsNotStoodDown()
{
    Rig r;
    r.Frame();
    r.Step();
    r.Frame(true, false, false);
    r.env.floats[Param::JitterX] = 0.4f;
    r.env.floats[Param::JitterY] = -0.2f;
    const int writes = r.env.Writes();
    const StepResult res = r.Step();
    CHECK(res.active && !res.standDown);
    CHECK(r.env.Writes() == writes && r.env.floats[Param::JitterX] == 0.4f && r.env.floats[Param::JitterY] == -0.2f);
    CHECK(r.controller.Totals().unpaired == 1 && r.controller.Totals().toldFromRecord == 0 && !r.controller.FellBack());
    // F3 (changed deliberately, round 3): a record older than a second is STILL a record. The same raw pair means the same phase index and the
    // same strength, so the same expected value; the pair comes round again every cycle. It is told (see TestMissTellsAStaleRecord).
    // a record that was not normalised (the generator left that call alone: the frame was drawn the old way) is not told either
    Rig n;
    n.Frame();
    n.Step();
    n.Frame(false, true, false);
    n.env.floats[Param::JitterX] = 0.4f;
    n.env.floats[Param::JitterY] = -0.2f;
    Generated g;
    g.outX = 0.4f;
    g.outY = -0.2f;
    g.expectedX = 0.123f;
    g.expectedY = 0.456f;
    g.normalized = false;
    g.timeMs = n.env.now;
    n.state.RecordGenerated(g, PlanWhy::Normalize);
    const int nWrites = n.env.Writes();
    const StepResult nres = n.Step();
    CHECK(nres.active && !nres.standDown && n.env.Writes() == nWrites);
}

// F3: a miss with a record is told whatever the age of the record. The same raw pair is the same phase and the same strength, so the same
// expected value: a record of the previous cycle (or of a render-thread hitch of more than a second) gives the identical offset.
void TestMissTellsAStaleRecord()
{
    Rig q;
    q.Frame();
    q.Step();
    SmallRawFrame(q, true, false);
    const std::optional<Generated> rec = q.state.FindGenerated(q.rawX, q.rawY);
    CHECK(rec.has_value());
    q.env.now += 10 * kMaxSampleAgeMs; // the record is ten seconds old
    {
        Generated g2; // the generator keeps running: a newer record of ANOTHER raw pair
        g2.outX = 0.111f;
        g2.outY = 0.222f;
        g2.expectedX = 0.0666f;
        g2.expectedY = 0.1332f;
        g2.strength = 0.6f;
        g2.phases = 32;
        g2.renderW = 1920;
        g2.renderH = 1080;
        g2.outW = 3840;
        g2.outH = 2160;
        g2.normalized = true;
        g2.timeMs = q.env.now;
        q.state.RecordGenerated(g2, PlanWhy::Normalize);
    }
    const StepResult res = q.Step();
    CHECK(res.active && res.standDown && !q.controller.FellBack());
    CHECK(rec && q.env.floats[Param::JitterX] == rec->expectedX && q.env.floats[Param::JitterY] == rec->expectedY);
    CHECK(q.controller.Totals().toldFromRecord == 1 && q.controller.Totals().untellable == 0);
    // a LARGE value (above the engine's limit) with a stale record is told too, and never counted as untellable
    Rig t;
    t.Frame();
    t.Step();
    do
    {
        t.Frame(true, true, false);
    } while (std::fabs(t.rawX) <= 0.6f && std::fabs(t.rawY) <= 0.6f);
    t.env.now += 5 * kMaxSampleAgeMs;
    {
        Generated g3; // the generator keeps running: a newer record of another raw pair keeps it "recent"
        g3.outX = 0.111f;
        g3.outY = 0.222f;
        g3.expectedX = 0.0666f;
        g3.expectedY = 0.1332f;
        g3.strength = 0.6f;
        g3.phases = 32;
        g3.renderW = 1920;
        g3.renderH = 1080;
        g3.outW = 3840;
        g3.outH = 2160;
        g3.normalized = true;
        g3.timeMs = t.env.now;
        t.state.RecordGenerated(g3, PlanWhy::Normalize);
    }
    const StepResult tres = t.Step();
    CHECK(tres.active && tres.standDown && !t.controller.FellBack() && t.controller.Totals().untellable == 0);
    CHECK(std::fabs(t.env.floats[Param::JitterX] - t.drawnX) < 1e-5f && std::fabs(t.env.floats[Param::JitterY] - t.drawnY) < 1e-5f);
}

// One evaluate whose block holds a generator-divided value above the engine's limit that no record explains.
StepResult UntellableEvaluate(Rig& r, float x = 0.8f, float y = 0.1f)
{
    r.Frame(true, true, false); // the generator keeps running (a record of ANOTHER raw pair); the block then holds a pair no record explains
    r.env.floats[Param::JitterX] = x;
    r.env.floats[Param::JitterY] = y;
    return r.Step();
}

// F3 (round 3; replaces TestUntellableFallsBackAtOnce, which pinned the one-shot latch): a generator-divided value above the engine's 0.55 px limit
// with no record at all is counted. One is left to the engine like the small-raw case (nothing written, the engine is not stood down), and only
// three within a second latch the fall-back for the session.
void TestUntellableNeedsThreeEvaluates()
{
    CHECK(kUntellableLimit == 3 && kUntellableWindowMs == 1000);
    Rig r;
    r.Frame();
    r.Step();
    const int writes = r.env.Writes();
    StepResult res = UntellableEvaluate(r);
    CHECK(!r.controller.FellBack() && r.state.Enabled());
    CHECK(res.active && !res.standDown && r.env.Writes() == writes); // left to the engine, nothing written
    CHECK(r.controller.Totals().untellable == 1 && r.controller.Totals().unpaired == 1);
    CHECK(r.env.Count("Jitter source: fell back") == 0);
    res = UntellableEvaluate(r);
    CHECK(!r.controller.FellBack() && r.state.Enabled() && !res.standDown && r.controller.Totals().untellable == 2);
    res = UntellableEvaluate(r);
    CHECK(r.controller.FellBack() && !r.state.Enabled());
    CHECK(!res.active && !res.standDown && r.env.Writes() == writes);
    CHECK(r.env.Count("Jitter source: fell back to the old behaviour") == 1 && r.env.Logged("cannot be told"));
    CHECK(r.controller.Totals().untellable == 3);

    // the Y axis alone counts too, and so does a negative value
    Rig q;
    q.Frame();
    q.Step();
    UntellableEvaluate(q, 0.1f, -0.7f);
    UntellableEvaluate(q, -0.9f, 0.0f);
    CHECK(!q.controller.FellBack() && q.controller.Totals().untellable == 2);
    UntellableEvaluate(q, 0.1f, 0.6f);
    CHECK(q.controller.FellBack());

    // a value the engine can scale (<= 0.55 px) is never untellable, however many
    Rig sm;
    sm.Frame();
    sm.Step();
    for (int i = 0; i < 20; ++i)
        UntellableEvaluate(sm, 0.5f, -0.5f);
    CHECK(!sm.controller.FellBack() && sm.controller.Totals().untellable == 0);

    // three, but spread over more than a second: the window restarts, no latch
    Rig w;
    w.Frame();
    w.Step();
    UntellableEvaluate(w);
    w.env.now += 600;
    UntellableEvaluate(w);
    w.env.now += 600; // 1200 ms after the first
    UntellableEvaluate(w);
    CHECK(!w.controller.FellBack() && w.controller.Totals().untellable == 3);
    // the third started a new window: two more quickly are 3 in it
    UntellableEvaluate(w);
    CHECK(!w.controller.FellBack());
    UntellableEvaluate(w);
    CHECK(w.controller.FellBack());

    // the counter resets after a good paired frame (a checked frame ...)
    Rig g;
    g.Frame();
    g.Step();
    UntellableEvaluate(g);
    UntellableEvaluate(g);
    g.Frame();
    CHECK(g.Step().standDown);
    UntellableEvaluate(g);
    UntellableEvaluate(g);
    CHECK(!g.controller.FellBack());
    UntellableEvaluate(g);
    CHECK(g.controller.FellBack());
    // ... and so does a paired frame the generator did not record (unverified)
    Rig u;
    u.Frame();
    u.Step();
    UntellableEvaluate(u);
    UntellableEvaluate(u);
    u.Frame(false, false, true);
    CHECK(u.Step().standDown && u.controller.Totals().unverified == 1);
    UntellableEvaluate(u);
    UntellableEvaluate(u);
    CHECK(!u.controller.FellBack());
    // a miss told from a record is neither untellable nor a reset: two untellable, a record-told miss, one more untellable latches
    Rig m;
    m.Frame();
    m.Step();
    UntellableEvaluate(m);
    UntellableEvaluate(m);
    do
    {
        m.Frame(true, true, false);
    } while (std::fabs(m.rawX) <= 0.6f && std::fabs(m.rawY) <= 0.6f);
    CHECK(m.Step().standDown && m.controller.Totals().toldFromRecord == 1 && !m.controller.FellBack());
    UntellableEvaluate(m);
    CHECK(m.controller.FellBack());
    // a large value WITH a record is simply told
    Rig t;
    t.Frame();
    t.Step();
    do
    {
        t.Frame(true, true, false);
    } while (std::fabs(t.rawX) <= 0.6f && std::fabs(t.rawY) <= 0.6f);
    const StepResult tres = t.Step();
    CHECK(tres.active && tres.standDown && !t.controller.FellBack());
    CHECK(std::fabs(t.env.floats[Param::JitterX] - t.drawnX) < 1e-5f);
}

// M2: a block that still holds the pair this controller itself wrote is already told: no pairing attempt, no miss counted, no write, and the
// engine stays out (it must not scale the told pair once).
void TestHeldPairIsNotAMiss()
{
    Rig r;
    r.Frame();
    r.Step();
    const int writes = r.env.Writes();
    const float tx = r.env.floats[Param::JitterX], ty = r.env.floats[Param::JitterY];
    // the game re-evaluates with a block it did not refresh, for far more evaluates than the unpaired limit
    for (unsigned i = 0; i < 4 * kUnpairedLimit; ++i)
    {
        const StepResult res = r.Step();
        CHECK(res.active && res.standDown);
    }
    CHECK(r.env.Writes() == writes && r.env.floats[Param::JitterX] == tx && r.env.floats[Param::JitterY] == ty);
    CHECK(r.controller.Totals().unpaired == 0 && !r.controller.FellBack());
    // a stall long enough for the generator to look idle: the fix is inactive, the block still holds the told pair, the engine is kept out
    r.env.now += kRecentMs + 50;
    r.state.NoteLeftAlone(PlanWhy::StrengthInvalid);
    StepResult res = r.Step();
    CHECK(!res.active && res.standDown && r.env.Writes() == writes);
    // the game refreshes the block with a pair of the old behaviour: the engine is back in charge
    r.Frame(false, true, true);
    res = r.Step();
    CHECK(!res.active && !res.standDown && r.env.Writes() == writes);
    res = r.Step();
    CHECK(!res.standDown); // the memory of the told pair is gone with the first different block
    // both axes must equal the told pair: one axis alone is a different block (a miss)
    Rig h;
    h.Frame();
    h.Step();
    h.env.floats[Param::JitterY] = 0.123f;
    h.Step();
    CHECK(h.controller.Totals().unpaired == 1);
    // the key goes off while the block holds the told pair: kept out for that evaluate, then free
    Rig k;
    k.Frame();
    k.Step();
    k.env.key = false;
    CHECK(k.Step().standDown);
    k.env.now += kDrainMs + 10;      // the drain is over, the block still holds the told pair: still kept out
    CHECK(k.Step().standDown && k.controller.Totals().unpaired == 0);
    CHECK(k.Step().standDown); // and on the evaluates after that (the early return of a key that is off must not skip a held block)
    k.Frame(false, true, true);
    CHECK(!k.Step().standDown);
    k.env.now += kDrainMs + 10;
    k.Step();
    const int clocks = k.env.clockReads;
    k.Step();
    k.Step();
    CHECK(k.env.clockReads == clocks); // once the block differs the key-off path is as cheap as before
}

// M0: the one diagnostic line about the objects the generator's call site is reached with. Written from the evaluate thread (never from a
// detour), after the first sighting has had time to settle, and again (budgeted) whenever a NEW object shows up, so a second camera that opens
// minutes later is not missed.
void TestSeenObjectsLine()
{
    Rig r;
    r.Frame();
    r.Step(); // the detours are enabled by the controller (switching on forgets a world object learned before)
    CHECK(r.env.Count("distinct jitter object") == 0);
    r.state.NoteCallSiteObject(0x1000);
    r.state.LearnWorldObject(0x1000);
    r.Frame();
    r.Step();
    CHECK(r.env.Count("distinct jitter object") == 0); // not yet: the sighting has to settle
    r.env.now += 5000;
    r.Frame();
    r.Step();
    CHECK(r.env.Count("reached with 1 distinct jitter object: 0x1000 (") == 1 && r.env.Logged("world camera"));
    for (int i = 0; i < 200; ++i)
    {
        r.state.NoteCallSiteObject(0x1000);
        r.Frame();
        r.Step();
    }
    CHECK(r.env.Count("distinct jitter object") == 1); // once
    // a second object, minutes later: a new line naming both
    r.env.now += 300000;
    r.state.NoteCallSiteObject(0x2000);
    r.Frame();
    r.Step();
    CHECK(r.env.Count("distinct jitter object") == 2 && r.env.Count("reached with 2 distinct jitter objects") == 1 && r.env.Logged("a second jittered object exists"));
    // a bounded number of lines, however many objects appear
    for (std::uintptr_t p = 0x3000; p < 0x3000 + 0x100; p += 0x10)
    {
        r.state.NoteCallSiteObject(p);
        r.Frame();
        r.Step();
    }
    CHECK(r.env.Count("distinct jitter object") <= 6);
    // no world camera known yet: nothing is said (there is nothing to mark), however long
    Rig q;
    q.Frame();
    q.Step();
    q.state.NoteCallSiteObject(0x1000);
    q.Frame();
    q.Step();
    q.env.now += 20000;
    q.Frame();
    q.Step();
    CHECK(q.env.Count("distinct jitter object") == 0);
    q.state.LearnWorldObject(0x1000); // once the world camera is known the settling time starts
    q.Frame();
    q.Step();
    CHECK(q.env.Count("distinct jitter object") == 0);
    q.env.now += 5000;
    q.Frame();
    q.Step();
    CHECK(q.env.Count("reached with 1 distinct jitter object") == 1);
}

// F1: the one in-game way the world tie can fail is a world pointer that is learned but never equals what the generator's call site sees. That
// is said once, with both pointers, after the settle time; and the idle text says which of the two causes it is.
void TestWorldLearnedButNeverSeenLine()
{
    const char* kLine = "Jitter source: the world camera pointer 0x1000 was learned from the camera getter but the generator's call site only saw 0x2000; "
                        "the source fix cannot act";
    Rig r;
    r.Frame(true, false, false);
    r.Step(); // enables the detours (forgets a world object learned before)
    r.state.NoteCallSiteObject(0x2000);
    r.state.LearnWorldObject(0x1000);
    r.Frame(true, false, false);
    r.Step();
    CHECK(r.env.Count("only saw") == 0); // the sighting has to settle first
    r.env.now += 5000;
    r.Frame(true, false, false);
    r.Step();
    CHECK(r.env.Count(kLine) == 1);
    for (int i = 0; i < 200; ++i)
    {
        r.state.NoteCallSiteObject(0x2000);
        r.Frame(true, false, false);
        r.Step();
    }
    CHECK(r.env.Count("only saw") == 1); // once
    // the world pointer changes (the camera was rebuilt) and is still not seen: a new line for the new pointer, once
    r.state.LearnWorldObject(0x4000);
    r.Frame(true, false, false);
    r.Step();
    r.Frame(true, false, false);
    r.Step();
    CHECK(r.env.Count("only saw") == 2 && r.env.Count("pointer 0x4000 was learned") == 1);

    // the world object IS among the seen ones: no such line
    Rig a;
    a.Frame(true, false, false);
    a.Step();
    a.state.NoteCallSiteObject(0x2000);
    a.state.NoteCallSiteObject(0x1000);
    a.state.LearnWorldObject(0x1000);
    a.Frame(true, false, false);
    a.Step();
    a.env.now += 20000;
    a.Frame(true, false, false);
    a.Step();
    CHECK(a.env.Count("only saw") == 0 && a.env.Count("reached with 2 distinct jitter objects") == 1);

    // nothing seen at the call site at all: the idle text says so, the F1 line needs a non-empty set
    Rig e;
    e.Frame(true, false, false);
    e.Step();
    e.state.LearnWorldObject(0x1000);
    e.Frame(true, false, false);
    e.Step();
    e.env.now += 20000;
    e.Frame(true, false, false);
    e.Step();
    CHECK(e.env.Count("only saw") == 0);

    // a world camera that is not learned: nothing to compare
    Rig u;
    u.Frame(true, false, false);
    u.Step();
    u.state.NoteCallSiteObject(0x2000);
    u.env.now += 20000;
    u.Frame(true, false, false);
    u.Step();
    CHECK(u.env.Count("only saw") == 0);

    // a full table cannot say "never seen": the object may be one that did not fit
    Rig o;
    o.Frame(true, false, false);
    o.Step();
    for (std::uintptr_t p = 0x5000; p < 0x5000 + 0x100; p += 0x10)
        o.state.NoteCallSiteObject(p);
    o.state.LearnWorldObject(0x1000);
    o.Frame(true, false, false);
    o.Step();
    o.env.now += 20000;
    o.Frame(true, false, false);
    o.Step();
    CHECK(o.env.Count("only saw") == 0);
}

// F1, the idle text: "not learned yet" and "learned but never seen at the generator call site" are two different statements.
void TestIdleReasonSaysWhichCause()
{
    // the world camera is not learned
    Rig n;
    n.Frame(true, false, false);
    n.Step();
    n.env.now += Controller<FakeEnv>::kInactiveDelayMs;
    n.Frame(true, false, false);
    n.Step();
    CHECK(n.env.Count("Jitter source: inactive (the game's generator is not being normalised: the world camera is not learned yet)") == 1);
    CHECK(!n.env.Logged("never seen at the generator call site"));

    // learned, but the call site only saw another object
    Rig s;
    s.Frame(true, false, false);
    s.Step();
    s.state.NoteCallSiteObject(0x2000);
    s.state.LearnWorldObject(0x1000);
    s.env.now += Controller<FakeEnv>::kInactiveDelayMs;
    s.Frame(true, false, false);
    s.Step();
    s.env.now += Controller<FakeEnv>::kInactiveDelayMs;
    s.Frame(true, false, false);
    s.Step();
    CHECK(s.env.Count("Jitter source: inactive (the game's generator is not being normalised: the world camera was learned but never seen at the generator call site)") == 1);
    CHECK(!s.env.Logged("is not learned yet"));

    // learned and seen, the generator is idle for another reason: the generic text, no claim about the world camera
    Rig g;
    g.Frame(true, false, false);
    g.Step();
    g.state.NoteCallSiteObject(0x1000);
    g.state.LearnWorldObject(0x1000);
    g.env.now += Controller<FakeEnv>::kInactiveDelayMs;
    g.Frame(true, false, false);
    g.Step();
    g.env.now += Controller<FakeEnv>::kInactiveDelayMs;
    g.Frame(true, false, false);
    g.Step();
    CHECK(g.env.Count("Jitter source: inactive (the game's generator is not being normalised (not in DLSS mode, native strength not valid, or not running))") == 1);
    CHECK(!g.env.Logged("world camera"));
}

// The verification line: the first after 120 checked frames, then every 30 s for five lines, then every five minutes (a session of hours stays
// readable, and the 20-line budget covers the first hour and a half).
void TestVerifyLineCadence()
{
    Rig r;
    for (int i = 0; i < 150; ++i)
    {
        r.Frame();
        r.Step();
    }
    CHECK(r.env.Count("Jitter source: verified") == 1);
    // one frame per second for ten minutes: 30 s apart for five lines, then every five minutes
    int lines = r.env.Count("Jitter source: verified");
    std::uint64_t lastAt = r.env.now;
    std::vector<std::uint64_t> gaps;
    for (int s = 0; s < 3600; ++s)
    {
        r.env.now += 1000;
        r.Frame();
        r.env.now -= 8; // Frame() adds 8 ms itself
        r.Step();
        const int now = r.env.Count("Jitter source: verified");
        if (now != lines)
        {
            gaps.push_back(r.env.now - lastAt);
            lastAt = r.env.now;
            lines = now;
        }
    }
    CHECK(gaps.size() >= 8);
    for (size_t i = 0; i < gaps.size() && i < 4; ++i)
        CHECK(gaps[i] >= 30000 && gaps[i] < 32000);
    for (size_t i = 5; i < gaps.size(); ++i)
        CHECK(gaps[i] >= 300000 && gaps[i] < 302000);
}

// M4 part 1: the verification line carries the told offsets' extremes and the number of distinct pairs since the previous line, so the log itself
// shows the lattice: at most the phase count (32 at Performance) however long the window, and the overflow marker for an unbounded pattern.
void TestVerifyLineShowsTheLattice()
{
    Rig r;
    bool switched = false;
    for (int i = 0; i < 150; ++i)
    {
        r.Frame();
        r.Step();
        if (!switched && r.env.Count("Jitter source: verified") == 1)
        {
            r.phases = 18; // a quality change right after the first line: the second window has its own pairs
            switched = true;
        }
    }
    CHECK(r.env.Count("Jitter source: verified") == 1);
    CHECK(r.env.Logged("32 distinct pairs") && r.env.Logged("phase count 32") && !r.env.Logged("more than"));
    // the window restarts after each line: the next one (30 s later) has its own frames and its own pairs: 18, not the 32 of the first window.
    for (int s = 0; s < 40; ++s)
    {
        r.env.now += 1000;
        r.Frame();
        r.env.now -= 8;
        r.Step();
    }
    CHECK(r.env.Count("Jitter source: verified") == 2);
    CHECK(r.env.Count("32 distinct pairs") == 1 && r.env.Count("18 distinct pairs") == 1);
    // an unbounded pattern (the game's own phase count) is visible as the overflow marker
    Rig u;
    u.phases = 100000;
    for (int i = 0; i < 150; ++i)
    {
        u.Frame();
        u.Step();
    }
    CHECK(u.env.Logged("120 distinct pairs") && !u.env.Logged("more than")); // the first line covers 120 frames: all different, none over the table
    for (int i = 0; i < 300; ++i)
    {
        u.Frame();
        u.Step();
    }
    u.env.now += 31000;
    u.Frame();
    u.Step();
    CHECK(u.env.Logged("more than 128 distinct pairs"));
    // pairs told from a record count too; a held block is not a new told frame
    Rig h;
    h.Frame();
    h.Step();
    for (int i = 0; i < 10; ++i)
        h.Step();
    CHECK(h.controller.Lattice().frames == 1);
}

void TestLogBudgets()
{
    Rig r;
    // flapping between active and idle cannot flood the log
    for (int i = 0; i < 200; ++i)
    {
        r.Frame();
        r.Step();
        r.env.now += kRecentMs + 50;
        r.state.NoteLeftAlone(PlanWhy::StrengthInvalid);
        r.Step();
        r.env.now += 2 * 2000;
        r.Step();
    }
    CHECK(r.env.logs.size() < 80);
}
} // namespace

int main()
{
    TestKeyOffCost();
    TestActiveWritesDrawnOffset();
    TestNoStandDownLineWithoutCorrection();
    TestPairingMissIsCounted();
    TestUnpairedRunFallsBack();
    TestDriftFallsBack();
    TestNotMatchedAndGuards();
    TestHooksNotInstalledYet();
    TestGeneratorIdle();
    TestKeyOffDrainsInFlightFrames();
    TestFallBackDrains();
    TestPhaseChangeIsLogged();
    TestUnverifiedFrameIsStillTold();
    TestOutputSizeDisagreement();
    TestSetFailureRestoresX();
    TestMissTellsTheRecordedValue();
    TestMissWithoutRecordIsNotStoodDown();
    TestMissTellsAStaleRecord();
    TestUntellableNeedsThreeEvaluates();
    TestHeldPairIsNotAMiss();
    TestSeenObjectsLine();
    TestWorldLearnedButNeverSeenLine();
    TestIdleReasonSaysWhichCause();
    TestVerifyLineCadence();
    TestVerifyLineShowsTheLattice();
    TestLogBudgets();
    return TEST_RESULT("jitter_source_controller");
}
