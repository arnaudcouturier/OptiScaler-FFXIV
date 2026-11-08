// Atmosphere: the sun lobe's supply does not depend on the light shafts module (INT2), pinned as text and run on the rules.
//
// L2b centres the lobe on the sun disc the sky draws: the sky's sun draw (cSunParam, row 0 = the disc's UV) is copied at that draw and read a frame late. The light
// shafts module reads the same 16 bytes for its own purpose, and the question this file answers is whether the atmosphere shares any of that: it must not, because
// with `LightShafts = false` (the shafts module off) a shared copy would go stale or never arrive, and the lobe would hold an old disc position or none at all.
//
// What is pinned, and what is run:
//   the atmosphere OWNS its copy: its own reader instance (State::sunUv), its own held row and frame (Hold), its own capture function in its own seam entry
//     (reached through the sky's sun-draw anchors, which the engine pass derives from the shader's layout - not from any feature), and its own reset; the file includes
//     no header of the shafts MODULE (Shafts.h) and names none of that module's entry points or its key - only the readers and rules the module shares as types;
//   the capture is not gated: no feature test in the sun-draw branch, nothing in the engine pass's dispatch between the shafts' entry and the lens flare's tests
//     LightShafts, and no seam entry before the atmosphere's takes (answers true for) the sun draw - the light bounce, the night grade, the march, the puddles'
//     stage and the shafts' own entry are pinned to pass it on (the light bounce and the night grade are also driven at run time in look_chain_order_warp);
//   the read is where all three entries meet: Readback is called from StageAtToneMap only (the own seam entry, the chained hook - the night grade's hand-off - and the
//     owner hook all pass through it, atmo_counts_pins), it keeps a row only when it is this build's layout, and the lobe's direction is chosen from it only while it is
//     FRESH (SunUvFresh) and agrees with the light (ChooseSunDirection);
//   run on the rules: the choice, frame by frame, with a capture that lands and then stops (the shafts-off case: nothing refreshes the row) - the disc is used for
//     exactly as long as the freshness rule allows and from then on the lobe is the light's direction, bit for bit, never the old disc position; a capture that never
//     lands gives the light's direction on every frame; a stale disc with no light gives no lobe at all.
#include "ext/ffxiv/engine/AtmoRules.h"
#include "ext/ffxiv/engine/ShaftsRules.h"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace
{
int g_failures = 0;
#define CHECK(cond) EXPECT(cond, "\n")
#define EXPECT(cond, ...)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::printf("    FAIL line %d: %s  ", __LINE__, #cond);                                                    \
            std::printf(__VA_ARGS__);                                                                                   \
            std::printf("\n");                                                                                         \
        }                                                                                                              \
    } while (0)

std::string ReadFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string Comments(const std::string& text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text.compare(i, 2, "//") == 0)
        {
            while (i < text.size() && text[i] != '\n')
                ++i;
            continue;
        }
        if (text.compare(i, 2, "/*") == 0)
        {
            const size_t end = text.find("*/", i + 2);
            i = end == std::string::npos ? text.size() : end + 1;
            continue;
        }
        out += text[i];
    }
    return out;
}

std::string Raw(const std::string& repo, const char* relative) { return ReadFile(repo + "/OptiScaler/ext/" + relative); }

std::string Strip(const std::string& text)
{
    std::string out;
    for (char c : text)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            out += c;
    return out;
}

// Comments and whitespace removed, so a pin is about the code and not about its formatting.
std::string Code(const std::string& repo, const char* relative) { return Strip(Comments(Raw(repo, relative))); }

bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
size_t At(const std::string& text, const std::string& needle) { return text.find(needle); }

std::string Body(const std::string& code, const std::string& signature)
{
    const size_t at = code.find(signature);
    if (at == std::string::npos)
        return {};
    const size_t open = code.find('{', at + signature.size());
    if (open == std::string::npos)
        return {};
    int depth = 0;
    for (size_t i = open; i < code.size(); ++i)
    {
        if (code[i] == '{')
            ++depth;
        else if (code[i] == '}' && --depth == 0)
            return code.substr(open, i - open + 1);
    }
    return {};
}

size_t Count(const std::string& text, const std::string& needle)
{
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
        ++count;
    return count;
}

bool Ordered(const std::string& text, const std::string& first, const std::string& second)
{
    const size_t firstAt = text.find(first);
    const size_t secondAt = text.find(second);
    return firstAt != std::string::npos && secondAt != std::string::npos && firstAt < secondAt;
}

// The number a whitespace-free code text assigns to a constant `name` ("...kName=0.12f;" / "...kName=1.0e-6;"), or NaN when it is not defined once.
double ConstantOf(const std::string& code, const std::string& name)
{
    const std::string needle = name + "=";
    const size_t at = code.find(needle);
    if (at == std::string::npos || code.find(needle, at + needle.size()) != std::string::npos)
        return std::nan("");
    return std::strtod(code.c_str() + at + needle.size(), nullptr);
}
} // namespace

namespace
{
using namespace ext::ffxiv::engine::atmo;

// The capture's own inverse projection (reversed Z, infinite far, near 0.1), as atmo_rules uses it.
void Projection(float out[16])
{
    std::memset(out, 0, 16 * sizeof(float));
    out[0] = 0.7307643f;
    out[5] = 0.4110417f;
    out[11] = -1.0f;
    out[14] = 10.0f;
}

// The stage's per-frame decision, exactly as AtmoDraw.cpp RunStage takes it: the freshness of the held row, the disc through the inverse projection, the choice.
struct Held
{
    bool have = false;
    float row[4] { 1.78f, 1.0f, 0.0f, 0.0f };
    uint64_t frame = 0;
};

SunChoice Lobe(const Held& held, uint64_t frame, const float projection[16], const float light[3], bool haveLight, const float up[3])
{
    float disc[3] {};
    const bool fresh = ext::ffxiv::shafts::SunUvFresh(held.have, held.frame, frame) && SunDirectionFromUv(projection, held.row[2], held.row[3], disc);
    return ChooseSunDirection(light, haveLight, disc, fresh, up);
}

bool Same(const float a[3], const float b[3])
{
    return std::memcmp(a, b, 3 * sizeof(float)) == 0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: atmo_lobe_pins <repo>\n");
        return 2;
    }
    const std::string repo = argv[1];
    const std::string draw = Code(repo, "ffxiv/engine/AtmoDraw.cpp");
    const std::string enginePass = Code(repo, "ffxiv/engine/EnginePass.cpp");
    const std::string shaftsDraw = Code(repo, "ffxiv/engine/ShaftsDraw.cpp");
    const std::string shaftsHeader = Code(repo, "ffxiv/engine/Shafts.h");
    EXPECT(!draw.empty() && !enginePass.empty() && !shaftsDraw.empty() && !shaftsHeader.empty(), "every file the pins read is found\n");

    // ---- the atmosphere owns its copy -----------------------------------------------------------------------------------------------------
    {
        EXPECT(Has(draw, "shafts::SunUvReadersunUv;"), "the stage's state carries its own reader instance of the sky-sun copy\n");
        EXPECT(Has(draw, "boolhaveSunUv=false;floatsunUvRow[4]{};uint64_tsunUvFrame=0;"), "and its own held row, with the frame it was written at\n");
        EXPECT(Has(draw, "state.sunUv.Copy(context,params.Get(),state.frames);") && Count(draw, "sunUv.Copy(") == 1, "the capture copies into the stage's own reader, with the stage's own frame count\n");
        EXPECT(Has(draw, "constshafts::SunUvReaduv=state.sunUv.Read(context,state.frames);if(uv.landed&&shafts::SunParamUvUsable(uv.uv)){std::memcpy(hold.sunUvRow,uv.uv,sizeofhold.sunUvRow);hold.sunUvFrame=uv.writtenFrame;hold.haveSunUv=true;}"),
               "the read lands in the stage's own row, kept only when it is this build's layout, with its own written frame\n");
        // Nothing of the shafts MODULE: no include of its header, none of its entry points, not its key.
        EXPECT(!Has(draw, "#include\"ext/ffxiv/engine/Shafts.h\""), "the stage includes no header of the light shafts module\n");
        const char* entryPoints[] { "shafts::Armed(", "shafts::OnEngineDraw(", "shafts::SampleSceneHdr(", "shafts::CurrentStatus(", "shafts::Publish(", "shafts::OnConfigLoaded(", "shafts::Release(" };
        for (const char* name : entryPoints)
            EXPECT(!Has(draw, name), "the stage never calls the shafts module's %s\n", name);
        EXPECT(!Has(draw, "F::LightShafts") && !Has(draw, "lightShafts") && !Has(draw, "LightShafts"), "and never tests the shafts key\n");
        // The module's entry points really are the ones listed (a rename would let the list above rot).
        EXPECT(Has(shaftsHeader, "boolArmed();") && Has(shaftsHeader, "boolOnEngineDraw(") && Has(shaftsHeader, "voidSampleSceneHdr();") && Has(shaftsHeader, "StatusCurrentStatus();") &&
                   Has(shaftsHeader, "voidPublish();") && Has(shaftsHeader, "voidOnConfigLoaded(boolon);") && Has(shaftsHeader, "voidRelease();"),
               "the list of the shafts module's entry points is the module's header\n");
        // Its resets: the copy in flight and the held row go with the stage's own key and release.
        const std::string apply = Body(draw, "voidApply(boolon)");
        EXPECT(Count(apply, "state.sunUv.Reset();") == 2 && Count(apply, "state.hold=Hold{};") == 2, "the stage's own off / not-wanted branches drop the copy in flight and the held row\n");
        EXPECT(Has(Body(draw, "voidRelease()"), "state.sunUv.Reset();") && Has(Body(draw, "voidRelease()"), "state.hold=Hold{};"), "and so does Release\n");
    }

    // ---- the capture is not gated and nothing in front of the stage takes the draw ------------------------------------------------------------
    {
        const std::string seam = Body(draw, "boolSeamDraw(ID3D11DeviceContext*context,Anchorsanchors,DrawFndraw,void*user)");
        EXPECT(!seam.empty(), "the stage's seam entry is found\n");
        EXPECT(Has(seam, "if(IsShaftsSunParamAnchors(anchors)){CaptureSunParam(context,anchors,state);returnfalse;}"), "the sky's sun draw is captured in the stage's own seam entry and passed on (false)\n");
        EXPECT(Ordered(seam, "IsShaftsSunParamAnchors(anchors)", "IsShaftsCompositeAnchors(anchors)") && Ordered(seam, "IsShaftsSunParamAnchors(anchors)", "IsShaftsToneMapAnchors(anchors)"),
               "before the composite and the tone map branches\n");
        const std::string capture = Body(draw, "voidCaptureSunParam(ID3D11DeviceContext*context,Anchorsanchors,State&state)");
        EXPECT(!capture.empty() && !Has(capture, "Active(") && !Has(capture, "ActiveAny(") && !Has(capture, "Armed("), "and the capture itself tests no feature\n");
        // The dispatch between the shafts' entry and the lens flare's: no shafts key.
        const size_t first = At(enginePass, "ext::ffxiv::shafts::OnEngineDraw(context,anchors,draw,user)");
        const size_t last = At(enginePass, "flare::OnSeamDraw(context,draw,user)");
        EXPECT(first != std::string::npos && last != std::string::npos && first < last, "the seam's dispatch is found\n");
        if (first != std::string::npos && last != std::string::npos && first < last)
        {
            const std::string dispatch = enginePass.substr(first, last - first);
            EXPECT(!Has(dispatch, "LightShafts"), "nothing between the shafts' entry and the lens flare's tests the shafts key (the atmosphere's entry is not behind it)\n");
            EXPECT(Has(dispatch, "if(ext::ffxiv::engine::atmo::OnSeamDraw(context,anchors,draw,user))return;"), "the atmosphere's seam entry is called unconditionally in the dispatch\n");
        }
        // The entries in front of the stage pass the sun draw on: they never branch on its anchors, except the shafts' own, which captures and answers false.
        EXPECT(Has(shaftsDraw, "if(ext::ffxiv::engine::IsShaftsSunParamAnchors(anchors)){CaptureSunParam(context,anchors,state);returnfalse;"), "the shafts' own entry captures the sun draw and answers false\n");
        const char* before[] { "ffxiv/engine/GodraysDraw.cpp", "ffxiv/engine/GiDraw.cpp", "ffxiv/enginelight/EngineLightDraw.cpp", "ffxiv/engine/PuddleToneDraw.cpp" };
        for (const char* file : before)
        {
            const std::string text = Code(repo, file);
            EXPECT(!text.empty() && !Has(text, "IsShaftsSunParamAnchors") && !Has(text, "kShaftsSunParamBit"), "%s never branches on the sun draw's anchors, so it cannot take the draw\n", file);
        }
    }

    // ---- the read is where the entries meet; the choice is gated on freshness and agreement ---------------------------------------------------
    {
        const std::string stage = Body(draw, "ID3D11ShaderResourceView*StageAtToneMap(State&state,ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene,OutcomedeclineOutcome,Entryentry)");
        EXPECT(Count(draw, "Readback(context,state);") == 1 && Has(stage, "Readback(context,state);"), "Readback (the sun row's read) is called from StageAtToneMap only, which every entry passes through\n");
        EXPECT(Has(draw, "constbooldiscFresh=shafts::SunUvFresh(hold.haveSunUv,hold.sunUvFrame,state.frames)&&SunDirectionFromUv(hold.inverseProjection,hold.sunUvRow[2],hold.sunUvRow[3],discView);"
                         "constSunChoicechoice=ChooseSunDirection(inputs.sunView,haveSun,discView,discFresh,inputs.upView);"),
               "the disc is used only while the row is fresh (the freshness comes first) and the choice is the rules' own\n");
        EXPECT(Has(draw, "if(choice.disc){") && Has(draw, "for(uint32_ti=0;i<3;++i)inputs.sunView[i]=choice.dir[i];}state.lastLobeOnDisc=choice.disc;"), "the lobe's direction is replaced only when the choice says disc\n");
    }

    // ---- run on the rules: the shafts-off case ------------------------------------------------------------------------------------------------------
    {
        float projection[16];
        Projection(projection);
        const float up[3] { 0.0f, 1.0f, 0.0f };
        // The disc the row points at, and a light above it in the same azimuth (the engine clamps the light's elevation, never its azimuth).
        float disc[3] {};
        CHECK(SunDirectionFromUv(projection, 0.533f, 0.200f, disc));
        float light[3] { disc[0], disc[1] + 0.3f, disc[2] };
        const float length = std::sqrt(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);
        for (float& c : light)
            c /= length;
        CHECK(ChooseSunDirection(light, true, disc, true, up).disc); // the setup agrees: a fresh disc is taken

        // (A) the capture lands on frames 1..10 and then never again (the shafts module is off and nothing else refreshes the row).
        Held held;
        for (uint64_t frame = 1; frame <= 10; ++frame)
        {
            held.have = true;
            held.row[2] = 0.533f;
            held.row[3] = 0.200f;
            held.frame = frame;
            const SunChoice choice = Lobe(held, frame, projection, light, true, up);
            CHECK(choice.disc && Same(choice.dir, disc));
        }
        const uint64_t kHold = ext::ffxiv::shafts::kHoldFrames;
        for (uint64_t frame = 11; frame <= 10 + kHold; ++frame)
        {
            const SunChoice choice = Lobe(held, frame, projection, light, true, up);
            CHECK(choice.disc && Same(choice.dir, disc)); // inside the freshness window: the disc a few frames old is still the sun
        }
        for (uint64_t frame = 11 + kHold; frame <= 2000; ++frame)
        {
            const SunChoice choice = Lobe(held, frame, projection, light, true, up);
            CHECK(!choice.disc); // from the first stale frame on, never the old disc position
            CHECK(Same(choice.dir, light)); // the light's direction, bit for bit
            CHECK(!Same(choice.dir, disc));
        }

        // (B) the capture never lands: the light's direction on every frame.
        Held never;
        for (uint64_t frame = 1; frame <= 200; ++frame)
        {
            const SunChoice choice = Lobe(never, frame, projection, light, true, up);
            CHECK(!choice.disc && Same(choice.dir, light));
        }

        // (C) the disc comes back after a gap: used for the window, then the light again.
        held.have = true;
        held.frame = 3000;
        CHECK(Lobe(held, 3000, projection, light, true, up).disc);
        CHECK(Lobe(held, 3000 + kHold, projection, light, true, up).disc);
        CHECK(!Lobe(held, 3001 + kHold, projection, light, true, up).disc);

        // (D) no light at all and a stale disc: no lobe (the sun gain is 0 without a light), and the disc is not the lobe's direction either.
        const SunChoice noLight = Lobe(held, 9000, projection, light, false, up);
        CHECK(!noLight.disc);
        // (E) a fresh disc and no light: nothing for the disc to agree with, so the choice is not the disc.
        held.frame = 9000;
        CHECK(!Lobe(held, 9000, projection, light, false, up).disc);
        // (F) a row the clock never reached (a frame count that went back) is not fresh.
        held.frame = 5000;
        CHECK(!Lobe(held, 4999, projection, light, true, up).disc);
    }

    if (g_failures)
    {
        std::printf("atmo_lobe_pins: %d pin(s) failed\n", g_failures);
        return 1;
    }
    std::printf("atmo_lobe_pins: all pins hold\n");
    return 0;
}
