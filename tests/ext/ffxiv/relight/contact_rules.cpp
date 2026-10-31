// Contact shadows: the pure rules (ext/ffxiv/relight/ContactShadowsRules.h). No device, no shader, no file.
//
// The rules are what keeps this feature fail-open: which draw may be touched, which of its bindings may be the screen-space mask (the
// cascade atlas carries the same reflected name), which sizes must agree, when the module switches itself off, and what the log says.
#include "check.h"

#include "ext/ffxiv/relight/ContactShadowsRules.h"

#include <cstring>
#include <string>

using namespace ext::ffxiv::relight;

namespace
{
// A draw that passes every rule: the shape the capture measured (a 2560x1440 mask and depth, the view position, both buffers).
DrawFacts Good()
{
    DrawFacts facts;
    facts.hasMask = facts.hasDepth = facts.hasViewPosition = facts.hasLightParam = facts.hasCameraParam = true;
    facts.maskFormat = 87; // DXGI_FORMAT_B8G8R8A8_UNORM
    facts.maskWidth = facts.maskHeight = 0;
    facts.maskWidth = 2560;
    facts.maskHeight = 1440;
    facts.maskSamples = 1;
    facts.mask2D = true;
    facts.depthWidth = 2560;
    facts.depthHeight = 1440;
    facts.viewPositionWidth = 2560;
    facts.viewPositionHeight = 1440;
    facts.displayWidth = 2560;
    facts.displayHeight = 1440;
    facts.window = { 0.5f, 0.5f, 2560.0f, 1440.0f };
    return facts;
}
} // namespace

int main()
{
    // ---- the keys: today's behaviour by default, every value clamped into the range the design names
    {
        const Params defaults = BuildParams(kDefaultLength, kDefaultStrength, kDefaultSteps, ParseSunSpace("auto"), ParseSunSign("auto"));
        CHECK_EQ(defaults.length, 0.5f);
        CHECK_EQ(defaults.strength, 0.5f);
        CHECK_EQ(defaults.steps, 12u);
        CHECK(defaults.space == SunSpace::View); // the measured space
        CHECK(defaults.sign == SunSign::Toward);
        // the clamps
        CHECK_EQ(ClampLength(-1.0f), kMinLength);
        CHECK_EQ(ClampLength(99.0f), kMaxLength);
        CHECK_EQ(ClampLength(0.5f), 0.5f);
        CHECK_EQ(ClampStrength(-0.5f), 0.0f);
        CHECK_EQ(ClampStrength(2.0f), 1.0f);
        CHECK_EQ(ClampSteps(0), 4u);
        CHECK_EQ(ClampSteps(1000), 24u);
        CHECK_EQ(ClampSteps(12), 12u);
        // a key out of range never reaches the shader: the clamp is applied where the parameters are built
        const Params wild = BuildParams(100.0f, -3.0f, -1, SunSpace::View, SunSign::From);
        CHECK_EQ(wild.length, kMaxLength);
        CHECK_EQ(wild.strength, 0.0f);
        CHECK_EQ(wild.steps, uint32_t(kMinSteps));
        CHECK(wild.space == SunSpace::View);
        CHECK(wild.sign == SunSign::From);
    }

    // ---- the two keys that describe the game's sun vector: anything unknown reads as the measured reading
    {
        CHECK(ParseSunSpace("world") == SunSpace::World);
        CHECK(ParseSunSpace("World") == SunSpace::World);
        CHECK(ParseSunSpace("view") == SunSpace::View);
        CHECK(ParseSunSpace("auto") == SunSpace::View); // the measured reading
        CHECK(ParseSunSpace("") == SunSpace::View);
        CHECK(ParseSunSpace("WORLDLY") == SunSpace::View);
        CHECK(ParseSunSign("from") == SunSign::From);
        CHECK(ParseSunSign("From") == SunSign::From);
        CHECK(ParseSunSign("toward") == SunSign::Toward);
        CHECK(ParseSunSign("auto") == SunSign::Toward);
        CHECK(ParseSunSign("face") == SunSign::Toward);
    }

    // ---- the unit-length guard the first sample of a session passes through (the value the capture measured is exactly 1.0)
    {
        CHECK(IsUnitDirection(0.0f, 0.8f, 0.6f));
        CHECK(IsUnitDirection(-0.267f, 0.834f, 0.4829f));
        CHECK(!IsUnitDirection(0.0f, 0.0f, 0.0f));
        CHECK(!IsUnitDirection(0.0f, 0.08f, 0.06f)); // a member that turned out not to be normalised
        CHECK(IsUnitDirection(0.0f, 0.8008f, 0.5998f)); // 0.2 % off: still the sun
        CHECK(!IsUnitDirection(0.0f, 0.84f, 0.63f));    // 5 % off: not the sun
        CHECK(!IsUnitDirection(1.0f, 1.0f, 1.0f));
    }

    // ---- the "moves faster than the sun" guard
    {
        CHECK(MovesTooFast(0.98f, 0.5));      // 11 degrees in half a second: impossible for the sun
        CHECK(!MovesTooFast(0.98f, 60.0));    // the same change over a minute is the game's time of day
        CHECK(!MovesTooFast(0.9999f, 0.5));   // a fraction of a degree in half a second is fine
        CHECK(!MovesTooFast(0.5f, 0.0));      // no baseline: nothing to compare with yet
    }

    // ---- the drop list: the draw the capture measured runs
    {
        CHECK(Classify(Good()) == Decline::None);
        DrawFacts facts = Good();
        facts.hasMask = false;
        CHECK(Classify(facts) == Decline::NoMask);
        facts = Good();
        facts.hasDepth = false;
        CHECK(Classify(facts) == Decline::NoDepth);
        // A view-position binding is not required: the light draw has none, so the pass reconstructs the position from the depth.
        facts = Good();
        facts.hasViewPosition = false;
        CHECK(Classify(facts) == Decline::None);
        facts = Good();
        facts.hasLightParam = false;
        CHECK(Classify(facts) == Decline::NoLightParam);
        facts = Good();
        facts.hasCameraParam = false;
        CHECK(Classify(facts) == Decline::NoCameraParam);
        facts = Good();
        facts.mask2D = false;
        CHECK(Classify(facts) == Decline::MaskArrayOr3D);
        facts = Good();
        facts.maskSamples = 4;
        CHECK(Classify(facts) == Decline::MaskMsaa);
        facts = Good();
        facts.maskFormat = 61; // DXGI_FORMAT_R8_UNORM: the cascade atlas
        CHECK(Classify(facts) == Decline::MaskFormat);
        facts = Good();
        facts.maskWidth = 2048;
        facts.maskHeight = 10240; // the atlas' size, whatever its format says
        CHECK(Classify(facts) == Decline::MaskSize);
        facts = Good();
        facts.displayWidth = 1280; // the depth binding (which gives the allocation's resolution) of another size
        facts.displayHeight = 720;
        CHECK(Classify(facts) == Decline::MaskSize);
        facts = Good();
        facts.window = { 0.0f, 0.0f, 0.0f, 1440.0f };
        CHECK(Classify(facts) == Decline::WindowDegenerate);
        facts = Good();
        facts.window = { 100.0f, 0.0f, 2560.0f, 1440.0f }; // runs off the right edge of the mask
        CHECK(Classify(facts) == Decline::WindowNotInMask);
        facts = Good();
        facts.window = { 0.0f, 0.0f, 1505.0f, 847.0f }; // a smaller render window inside the allocation is normal (the benchmark)
        CHECK(Classify(facts) == Decline::None);
    }

    // ---- the reasons and the two lines that name them: one per state, readable, and never a path or a name
    {
        CHECK_EQ(std::string(DeclineName(Decline::None)), std::string("run"));
        CHECK(std::string(DeclineName(Decline::NoMask)).find("g_SamplerShadow") != std::string::npos);
        CHECK(std::string(DeclineName(Decline::MaskFormat)).find("format") != std::string::npos);
        const std::string line = FormatDecline(Decline::MaskSize);
        CHECK(line.find("Contact shadows:") == 0);
        CHECK(line.find("left alone") != std::string::npos);
        CHECK(line.find('\\') == std::string::npos); // no path
        const Params params = BuildParams(0.5f, 0.5f, 12, SunSpace::View, SunSign::Toward);
        const std::string run = FormatRun(params, 2560, 1440, 2560.0f, 1440.0f, true, 0.0412);
        CHECK(run.find("2560x1440") != std::string::npos);
        CHECK(run.find("0.50 m") != std::string::npos);
        CHECK(run.find("view space") != std::string::npos);
        CHECK(run.find("its two draws 0.0412 ms on the GPU") != std::string::npos); // the pass's own cost, separate from the hook's
        CHECK(FormatRun(params, 2560, 1440, 2560.0f, 1440.0f, false, 0.0).find("not measured yet") != std::string::npos);
        CHECK(run.find("toward the sun") != std::string::npos);
        const std::string classification = FormatClassification(87, 2560, 1440, 0.0f, 0.8f, 0.6f, SunSpace::View);
        CHECK(classification.find("0.800") != std::string::npos);
        CHECK(classification.find("view space") != std::string::npos);
        CHECK(std::string(FormatSwitchedOff()).find("off for this session") != std::string::npos);
    }

    // ---- the log budget: one line per change of state, 32 a session at most
    {
        LogGate gate;
        CHECK(gate.Take(1));
        CHECK(!gate.Take(1)); // the same state again says nothing
        CHECK(gate.Take(2));
        CHECK(gate.Take(1)); // a state that comes back is a change again
        for (unsigned i = 0; i < 100; ++i)
            gate.Take(1000 + i);
        CHECK_EQ(gate.lines, kMaxLogLines); // the budget, counted
    }

    // ---- the switch-off: sixteen refusals in a row, and one accepted draw resets the count
    {
        Session session;
        CHECK(!session.off);
        for (uint32_t i = 0; i + 1 < kDeclineLimit; ++i)
            CHECK(!session.NoteDecline());
        CHECK(!session.off);
        session.NoteRun(); // a light draw ran again: the count starts over
        CHECK_EQ(session.consecutiveDeclines, 0u);
        for (uint32_t i = 0; i + 1 < kDeclineLimit; ++i)
            CHECK(!session.NoteDecline());
        CHECK(session.Armed());
        CHECK(session.NoteDecline()); // the sixteenth in a row switches the module off for the session
        CHECK(session.off);
        CHECK(!session.Armed());
        CHECK(!session.NoteDecline()); // and it stays off
    }

    // ---- a refusal about the draw's shape says "another draw", not "this build is broken": the atlas writer declares the same names as the
    // sun light draw and is refused on every shadow pass, so those refusals must not feed the switch-off
    {
        CHECK(IsShapeDecline(Decline::MaskFormat));
        CHECK(IsShapeDecline(Decline::MaskSize));
        CHECK(IsShapeDecline(Decline::MaskNotA2D));
        CHECK(IsShapeDecline(Decline::MaskMsaa));
        CHECK(IsShapeDecline(Decline::MaskArrayOr3D));
        CHECK(!IsShapeDecline(Decline::None));
        CHECK(!IsShapeDecline(Decline::WindowNotInMask)); // the draw's own resolution: the design has not seen it, so it counts
        CHECK(!IsShapeDecline(Decline::WindowDegenerate));
        CHECK(!IsShapeDecline(Decline::SunNotUnit));
        CHECK(!IsShapeDecline(Decline::SunMoving));
        CHECK(!IsShapeDecline(Decline::TextureFailed));
        CHECK(!IsShapeDecline(Decline::PassFailed));
        CHECK(!IsShapeDecline(Decline::Exception));
        CHECK(!IsShapeDecline(Decline::NoMask));
        CHECK(!IsShapeDecline(Decline::NoDepth));
    }

    // ---- the half-resolution target of the march: half in each axis, never zero
    {
        CHECK_EQ(HalfExtent(2560.0f), 1280u);
        CHECK_EQ(HalfExtent(1440.0f), 720u);
        CHECK_EQ(HalfExtent(1.0f), 1u);
        CHECK_EQ(HalfExtent(3.0f), 1u);
        CHECK_EQ(HalfExtent(4.0f), 2u);
        CHECK_EQ(HalfExtent(1505.0f), 752u);
    }

    // ---- the five reflected names are the ones the capture recorded
    {
        CHECK_EQ(std::string(kMaskSampler), std::string("g_SamplerShadow"));   // there is no g_SamplerShadowMask in this build
        CHECK_EQ(std::string(kDepthSampler), std::string("g_SamplerDepth"));
        CHECK_EQ(std::string(kViewPositionSampler), std::string("g_SamplerViewPosition"));
        CHECK_EQ(std::string(kLightParamBuffer), std::string("g_LightParam"));
        CHECK_EQ(std::string(kCameraParamBuffer), std::string("g_CameraParameter"));
        CHECK(IsMaskFormat(87));
        CHECK(!IsMaskFormat(28)); // R8G8B8A8_UNORM is the colour, not the mask
        CHECK(!IsMaskFormat(61)); // R8_UNORM is the cascade atlas
    }
    // ---- the name scan that keeps the index filled while the key is off: only a shader that names the mask is parsed
    {
        const char* withMask = "...g_SamplerShadow.T...g_SamplerDepth.T...";
        const char* without = "...g_Texture.T...g_MaterialParameter...";
        CHECK(MentionsMaskName(withMask, std::strlen(withMask)));
        CHECK(!MentionsMaskName(without, std::strlen(without)));
        CHECK(!MentionsMaskName(nullptr, 100));
        CHECK(!MentionsMaskName("g_SamplerShadow", 5)); // shorter than the name
    }

    // ---- the camera layout: the products the reflected layout promises, and the refusal of anything else
    {
        float rows[kCameraCheckFloats] {};
        // The identity in view, view-projection, inverse view-projection, inverse projection and projection: every check holds.
        const uint32_t identityAt[] { 0, 6, 10, 14, 18 };
        for (const uint32_t base : identityAt)
            for (uint32_t i = 0; i < 4; ++i)
                rows[(base + i) * 4 + i] = 1.0f;
        CHECK(CameraLayoutOk(rows, kCameraCheckFloats));
        // The view-projection and its inverse must not both be the identity: a product that is not the identity is refused.
        rows[6 * 4 + 0] = 2.0f;
        CHECK(!CameraLayoutOk(rows, kCameraCheckFloats));
        rows[6 * 4 + 0] = 1.0f;
        CHECK(CameraLayoutOk(rows, kCameraCheckFloats));
        // A view basis that is not orthonormal is refused (a patch that moves a matrix).
        rows[0 * 4 + 0] = 2.0f;
        CHECK(!CameraLayoutOk(rows, kCameraCheckFloats));
        rows[0 * 4 + 0] = 1.0f;
        CHECK(CameraLayoutOk(rows, kCameraCheckFloats));
        CHECK(!CameraLayoutOk(rows, 8)); // too few rows to be a camera buffer
        CHECK(!CameraLayoutOk(nullptr, kCameraCheckFloats));
    }

    // ---- a remembered shape refusal expires, so a re-created mask is looked at again; a camera layout refusal is a shape refusal
    {
        CHECK(ShapeRefusalActive(1000, 1000, 1000));          // just refused
        CHECK(ShapeRefusalActive(1000, 3999, 1000));          // 2.999 s old
        CHECK(!ShapeRefusalActive(1000, 4000, 1000));         // 3 s old: inspected again
        CHECK(!ShapeRefusalActive(0, 4000, 1000));            // never refused
        CHECK(!ShapeRefusalActive(1000, 1000, 0));            // no clock
        CHECK(IsShapeDecline(Decline::CameraLayout));         // judged again after kCameraCheckSeconds, not a session-long refusal
        CHECK(IsShapeDecline(Decline::MaskSize));
        CHECK(!IsShapeDecline(Decline::PassFailed));
    }

    // ---- the on-cost meter: one sample in 262144 draws, then the average of what it saw
    {
        CostMeter meter;
        for (uint32_t i = 0; i < kCostEveryDraws; ++i)
        {
            CHECK(!meter.Sampling());
            meter.Skip();
        }
        CHECK(meter.Sampling());
        CHECK(!meter.Done());
        for (uint32_t i = 0; i < kCostSampleDraws; ++i)
        {
            CHECK(meter.Sampling());
            meter.Sample(2); // 2 ticks of a 1 MHz clock: 0.002 ms a draw
        }
        CHECK(meter.Done());
        CHECK(!meter.Sampling());
        CHECK(meter.MillisecondsPerDraw(1000000) > 0.0019 && meter.MillisecondsPerDraw(1000000) < 0.0021);
        const std::string line = FormatCost(meter, 300000, BindCostMeter {}, 0, 1000000);
        CHECK(line.find("0.00200 ms per draw") != std::string::npos);
        CHECK(line.find("64 sampled") != std::string::npos);
        CHECK(line.find("0.0000 ms a frame at that rate") != std::string::npos); // no binds seen yet: no second half to report
    }

    // ---- the seam's meter: the same rule at the shader binds' own, lower rate, and a line that reports both halves
    {
        BindCostMeter binds;
        for (uint32_t i = 0; i < kBindCostEvery; ++i)
        {
            CHECK(!binds.Sampling());
            binds.Skip();
        }
        CHECK(binds.Sampling());
        for (uint32_t i = 0; i < kCostSampleDraws; ++i)
            binds.Sample(1); // 1 tick of a 1 MHz clock: 0.001 ms a bind
        CHECK(!binds.Sampling());
        CHECK(binds.MillisecondsPerBind(1000000) > 0.0009 && binds.MillisecondsPerBind(1000000) < 0.0011);
        CostMeter draws;
        for (uint32_t i = 0; i < kCostEveryDraws; ++i)
            draws.Skip();
        for (uint32_t i = 0; i < kCostSampleDraws; ++i)
            draws.Sample(0); // a lookup the clock cannot even see: the pointer compare is that cheap
        const std::string line = FormatCost(draws, 925000, binds, 210000, 1000000000);
        CHECK(line.find("the draw's lookup 0.00000 ms per draw") != std::string::npos);
        CHECK(line.find("210000 pixel-shader binds seen") != std::string::npos);
        CHECK(line.find("0.00000 ms per bind") != std::string::npos); // 1 tick of a 1 GHz clock: below the printed resolution
        // The frame cost of the seam follows the bind rate the session measured, not a constant: 210000 binds over 925000 draws is
        // 2100 a frame at 9250 draws.
        CHECK(line.find("2100.0000 ms a frame") == std::string::npos);
        CHECK(line.find("ms a frame at that rate") != std::string::npos);
    }

    // ---- the dump's one-frame decision: off unless the key asks, once per session, never before the delay
    {
        CHECK(!DumpDue(0, false, 1000, 999999999, 1000));    // 0 = off
        CHECK(!DumpDue(10, false, 0, 999999999, 1000));      // no session start
        CHECK(!DumpDue(10, false, 1000, 10999, 1000));       // 9.999 s: not yet
        CHECK(DumpDue(10, false, 1000, 11000, 1000));        // 10 s: due
        CHECK(DumpDue(10, false, 1000, 2000000, 1000));      // late is still due
        CHECK(!DumpDue(10, true, 1000, 2000000, 1000));      // written once, never again
        CHECK(!DumpDue(10, false, 1000, 2000000, 0));        // no clock
        CHECK(!DumpDue(-5, false, 1000, 2000000, 1000));     // negative is off
        CHECK(!DumpDue(kMaxDumpDelaySeconds + 1, false, 1000, 2000000, 1000));
    }

    // ---- the draw hook's body runs the game's draw exactly once per call: the pass on the first call of a draw, the plain draw on a replay
    // of it (the scene HDR path and the lighting scan run a draw twice). Never twice in one call, never not at all.
    {
        bool first = true;
        int bound = 0, plain = 0;
        const auto boundPath = [&] { ++bound; };
        const auto plainPath = [&] { ++plain; };
        RunDrawOnce(first, false, boundPath, plainPath); // the frame's own draw: the pass runs with it, the draw itself is inside it
        CHECK_EQ(bound, 1);
        CHECK_EQ(plain, 0);
        CHECK(!first);
        RunDrawOnce(first, false, boundPath, plainPath); // a replay of the same draw: the draw alone, no second pass
        CHECK_EQ(bound, 1);
        CHECK_EQ(plain, 1);
        CHECK_EQ(bound + plain, 2); // two calls of the hook body, two draws: neither doubled nor dropped

        // The scene HDR path asks for the draw itself before the first call: the pass is skipped and the draw still runs once.
        bool fresh = true;
        int hdrBound = 0, hdrPlain = 0;
        RunDrawOnce(fresh, true, [&] { ++hdrBound; }, [&] { ++hdrPlain; });
        RunDrawOnce(fresh, true, [&] { ++hdrBound; }, [&] { ++hdrPlain; });
        CHECK_EQ(hdrBound, 0);
        CHECK_EQ(hdrPlain, 2);
        CHECK(fresh); // no pass ran, so the flag is still free for a draw that is not a replay
    }

    return Finish("contact_shadows_rules");
}
