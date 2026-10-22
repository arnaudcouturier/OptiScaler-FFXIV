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

void TestPairingMissPassesThrough()
{
    Rig r;
    r.Frame();
    r.Step();
    const int before = r.env.Writes();
    // a frame whose camera sample was never recorded: no write, the pair stays the game's own, the engine still stands down
    r.Frame(true, true, false);
    const float rx = r.env.floats[Param::JitterX], ry = r.env.floats[Param::JitterY];
    StepResult res = r.Step();
    CHECK(res.active && res.standDown);
    CHECK(r.env.Writes() == before);
    CHECK(r.env.floats[Param::JitterX] == rx && r.env.floats[Param::JitterY] == ry);
    CHECK(r.controller.Totals().unpaired == 1);
    // a stale sample (older than a second) is a miss too
    r.Frame();
    r.env.now += kMaxSampleAgeMs + 10;
    r.Frame(true, true, false);
    r.env.now -= 0;
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
    r.Step();
    CHECK(r.controller.Totals().unpaired == 4);
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
    const StepResult res = r.Step();
    CHECK(!res.active && !res.standDown && r.env.Writes() == before);
    CHECK(r.env.Count("Jitter source: inactive (the game's generator is not being normalised") == 1);
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

void TestNoGuessWritten()
{
    // a pair that matches nothing is never "corrected" by arithmetic: raw * strength is not written
    Rig r;
    r.Frame();
    r.Step();
    r.Frame(true, true, false);
    const float rx = r.env.floats[Param::JitterX];
    const int writes = r.env.Writes();
    r.Step();
    CHECK(r.env.Writes() == writes && r.env.floats[Param::JitterX] == rx);
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
    TestPairingMissPassesThrough();
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
    TestNoGuessWritten();
    TestVerifyLineCadence();
    TestLogBudgets();
    return TEST_RESULT("jitter_source_controller");
}
