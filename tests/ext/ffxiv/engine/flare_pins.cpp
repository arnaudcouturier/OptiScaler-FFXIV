// Text pins of the lens flare's card: where it hangs, what the key off costs, and what it must never do.
//
// The card is off by default and must stay free while it is off: the flare's four stamps are built at shader creation whatever the key says,
// its own joins to the seam ride the engine module's hooks (no new hook of its own), every arming chain names the card, and the draw path
// costs one relaxed load when no part is off the engine's own picture.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>

namespace
{
int g_failures = 0;
#define EXPECT(cond, ...)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::printf("    FAIL line %d: %s  ", __LINE__, #cond);                                                    \
            std::printf(__VA_ARGS__);                                                                                  \
            std::printf("\n");                                                                                         \
        }                                                                                                              \
    } while (0)

std::string ReadFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::string Raw(const std::string& repo, const char* relative) { return ReadFile(repo + "/OptiScaler/ext/" + relative); }

std::string ReadJson(const std::string& repo, const char* relative) { return ReadFile(repo + "/tools/" + relative); }

// Comments and whitespace removed, so a pin is about the code and not about its formatting.
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

std::string Code(const std::string& repo, const char* relative)
{
    const std::string text = Comments(Raw(repo, relative));
    std::string out;
    for (char c : text)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            out += c;
    return out;
}

bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

size_t Count(const std::string& text, const std::string& needle)
{
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
        ++n;
    return n;
}
} // namespace

int main(int, char** argv)
{
    const std::string repo = argv[1];

    // ---- the seam: a card that rides the engine module's hooks, with no hook of its own and no extra call in the capture seam
    {
        const std::string hooks = Code(repo, "ffxiv/lighting/FfxivLightingCapture.h");
        EXPECT(Count(hooks, "ext::ffxiv::engine::") == 4, // LENS-1: the count stays; the card reaches the bind path through them
               "the flare adds no call of its own to the capture seam: it rides the engine module's four hooks");
        EXPECT(Count(hooks, R"xF(,ext::F::LensFlare)xF") == 2, // no closing paren: the outer test continues with the shadow mapping bit
               "both ActiveAny tests the draw seam makes name the flare's bit beside the four that were there (the outer one now ends with the shadow mapping bit after it)");
    }

    // ---- the card's own module: the card's stamps before the index decides, and the bind seam's one relaxed load
    {
        const std::string engine = Code(repo, "ffxiv/engine/EnginePass.cpp");
        EXPECT(Count(engine, "flare::NoteShader(shader,code,size);") == 1,
               "the flare stamps its shaders in the engine module's creation hook, once per shader object");
        EXPECT(engine.find("flare::NoteShader(shader,code,size);") < engine.find("constPassLayoutlayout=PassLayoutOfShader(code,size);"),
               "the flare's stamps are written before the engine module's index decides anything: a shader the index does not store still "
               "answers for its own reflected names");
        EXPECT(Count(engine, "flare::OnPixelShaderSet(shader,instances);") == 1,
               "the bind seam is one call inside the engine module's, and only while the card's feature bit is on");
        EXPECT(Count(engine, "if(ext::Active(ext::F::LensFlare))") == 1,
               "the bind seam is one atomics test, never a config read");
        EXPECT(Count(engine, "flare::OnSeamDraw(context,draw,user)") == 1 &&
                   Has(engine, R"x(if(flare::OnSeamDraw(context,draw,user))return;)x"),
               "the draw path hands the card every draw the seam sees, one relaxed load of the card's bind value for the frames that are "
               "not the family's");
        EXPECT(engine.find("flare::OnSeamDraw(context,draw,user)") > engine.find("godrays::OnSeamDraw(context,anchors,draw,user)"),
               "the card answers after the march in the seam's feature chain, before the anchors pass-through");
        EXPECT(Count(engine, "flare::Armed()") == 5,
               "the arming chains name the card five times: the draw path's gate, the feature bit's rule, the switch-off, the draw that "
               "found every feature stood down, and the config publish's own");
    }

    // ---- the feature bit is its own, and everything else of the anchors word keeps its bits
    {
        const std::string features = Code(repo, "ExtFeatures.h");
        EXPECT(Has(features, R"x(LensFlare=1ull<<22,)x"), "the feature bit is the stack's next free word: 22, after the chara ambient's 19, the light bounce's 20 and the depth of field's 21");
        EXPECT(Count(features, "inlineboolActiveAny(") == 1 && !Has(features, "ActiveAny(Fa,Fb"),
               "ActiveAny has ONE definition, the variadic: the card appends its flag to the seam's calls and adds no fixed-arity overload");
        EXPECT(Has(features, R"x(LightShafts=1ull<<16,)x") && Has(features, R"x(ShadowMapping=1ull<<17,)x") &&
                   Has(features, R"x(GodraysVolumetric=1ull<<18,)x") && Has(features, R"x(CharaAmbient=1ull<<19,)x") &&
                   Has(features, R"x(Gi=1ull<<20,)x") && Has(features, R"x(DepthOfField=1ull<<21,)x"),
               "the six earlier bits keep theirs: a feature added to the enum appends, never renumbers");
        // The card's keys leave the engine module's name table and the anchors word alone (the word had no free bit for a fourth flag,
        // which is why the card carries its own stamps).
        const std::string rules = Code(repo, "ffxiv/engine/EnginePassRules.h");
        EXPECT(!Has(rules, "cHaloParam") && !Has(rules, "cRadialBlurParam") && !Has(rules, "\"sToneMap\""),
               "the flare's names live in the card's own table, not in the engine module's");
        EXPECT(!Has(rules, "Flare"), "the engine module's rules do not name the card");
        EXPECT(!Has(rules, "1ull<<22"), "the anchors word has no bit claimed at 22: the card's stamps are its own table's business");
        // The feature bit changes in the card's own publish, and in no other place of the module.
        const std::string flareDraw = Code(repo, "ffxiv/engine/FlareDraw.cpp");
        EXPECT(Count(flareDraw, "ext::SetActive(ext::F::LensFlare,") == 1,
               "the feature bit changes in one place: the card's own publish");
    }

    // ---- the keys: every default today's picture, bound, with a control and in the schema, and the card row registered
    {
        const std::string config = Code(repo, "ffxiv/FfxivConfig.h");
        EXPECT(Has(config, R"x(CustomOptional<bool>flareCard{false};)x"), "the master ships off: a fresh ini is today's picture");
        EXPECT(Has(config, R"x(CustomOptional<int32_t>flareParts{7};)x"), "the parts ship all three on: today's picture");
        EXPECT(Has(config, R"x(CustomOptional<float>flareRingScale{1.0f};)x") &&
                   Has(config, R"x(CustomOptional<float>flareSunScale{1.0f};)x") &&
                   Has(config, R"x(CustomOptional<float>flareGhostScale{1.0f};)x"),
               "the three scale keys ship at the engine's own values");
        EXPECT(Has(config, R"x(staticinlineBoolHooklensFlareOnLoaded=nullptr;)x"), "the config's hook pointer goes through");
        EXPECT(Has(config, R"x(if(lensFlareOnLoaded!=nullptr))x"), "the config load publishes the card");

        const std::string binder = Code(repo, "core/ExtConfig.cpp");
        for (const char* line : { R"x(FlareCard",flareCard)x", R"x(FlareParts",flareParts)x", R"x(FlareRingScale",flareRingScale)x",
                                  R"x(FlareSunScale",flareSunScale)x", R"x(FlareGhostScale",flareGhostScale)x" })
            EXPECT(Count(binder, line) == 1, "the binder names %s once", line);

        const std::string schema = ReadJson(repo, "settings-schema.json");
        for (const char* key : { "\"[FFXIV] FlareCard\"", "\"[FFXIV] FlareParts\"", "\"[FFXIV] FlareRingScale\"",
                                 "\"[FFXIV] FlareSunScale\"", "\"[FFXIV] FlareGhostScale\"" })
            EXPECT(Has(schema, key), "the schema carries %s", key);
        EXPECT(Count(schema, "\"card\": \"ffxiv_lens_flare\"") == 5, "all five entries point at the card");

        const std::string menu = Code(repo, "ffxiv/engine/FlareMenu.cpp");
        EXPECT(Has(menu, R"x(cfg.flareCard=enabled;)x") && Has(menu, R"x(cfg.flareRingScale=flare::ClampScale(ring);)x"),
               "every player-facing key has a control that clamps it");
        EXPECT(Has(menu, R"x(flare::kPartRing)x") && Has(menu, R"x(flare::kPartSunOrbs)x") && Has(menu, R"x(flare::kPartVeil)x"),
               "the three parts' checkboxes are the bitfield's own bits");
        EXPECT(Has(menu, R"x(if(changed)Publish();)x"), "every control publishes its value, so a switch turned on in a running session arms the card");
        EXPECT(Has(menu, R"x({"ffxiv_lens_flare","FFXIV","Lensflare(FFXIV)",Draw,"ffxiv_engine_passes",Visible})x"),
               "the card is registered, anchored on the shafts' registration as the FFXIV tab's cards all are");

        // The card's visible text: no slider label carries stray punctuation, and the master tooltip says the game's buffers are only read.
        {
            const std::string rawMenu = Raw(repo, "ffxiv/engine/FlareMenu.cpp");
            EXPECT(Has(rawMenu, "SliderFloat(\"Ring\"") && Has(rawMenu, "SliderFloat(\"Sun + orbs\"") && Has(rawMenu, "SliderFloat(\"Ghost step\""),
                   "the three sliders are labelled by the part they scale");
            EXPECT(!Has(rawMenu, "\"---") && !Has(rawMenu, "SliderFloat(\"-"), "no control label starts with stray dashes");
            EXPECT(!Has(rawMenu, "written but the game's own buffers are read"),
                   "the tooltip does not say that nothing is written but the game's own buffers");
            EXPECT(Has(rawMenu, "Nothing of the game's own buffers is ever written"),
                   "the master tooltip states the rule the card holds: the game's buffers are read, never written");
            std::string fragmentRaw = Raw(repo, "ini/FFXIV.ini");
            fragmentRaw.erase(std::remove(fragmentRaw.begin(), fragmentRaw.end(), '\r'), fragmentRaw.end()); // a checkout may hold CRLF
            EXPECT(!Has(fragmentRaw, "the ring\n; the sun's colour goes to 0"), "the FlareParts comment reads as a sentence");
        }

        const std::string layout = Code(repo, "core/MenuLayout.h");
        EXPECT(Has(layout, R"x({"ffxiv_lens_flare","FFXIV","ffxiv_dof"},)x"), "the card sits after the depth of field card, the last render card of the stack, in the tab's table");
        EXPECT(Has(layout, R"x({"ffxiv_jitter","FFXIV","ffxiv_lens_flare"},)x") && Has(layout, R"x({"ffxiv_gi","FFXIV","ffxiv_godrays"},)x") && Has(layout, R"x({"ffxiv_dof","FFXIV","ffxiv_atmosphere"},)x"), "the stack's own cards keep their places and the jitter card follows the flare card");

        const std::string fragment = Raw(repo, "ini/FFXIV.ini");
        for (const char* line : { "FlareCard=auto", "FlareParts=auto", "FlareRingScale=auto", "FlareSunScale=auto", "FlareGhostScale=auto" })
            EXPECT(Count(fragment, line) == 1, "the fragment names %s once", line);
        std::string fragmentText = fragment;
        fragmentText.erase(std::remove(fragmentText.begin(), fragmentText.end(), '\r'), fragmentText.end());
        EXPECT(Has(fragmentText, "Default (auto) is false\nFlareCard=auto"), "the fragment's master says what its default is");
    }

    // ---- the timing flags: the card maps nothing of the game and reads nothing back, whatever it patches or skips
    {
        const std::string draw = Code(repo, "ffxiv/engine/FlareDraw.cpp");
        const std::string rules = Code(repo, "ffxiv/engine/FlareRules.h");
        // The card's own files make no staging map and no GetData: the flag rules of the fork live in the primitive that runs the patch.
        for (const std::string& text : { draw, rules })
        {
            EXPECT(Count(text, "D3D11_MAP_READ") == 0, "the card's code reads nothing back: the flags live with the primitive");
            EXPECT(Count(text, "D3D11_MAP_FLAG_DO_NOT_WAIT") == 0, "and holds no flag argument of its own to keep in one place");
            EXPECT(Count(text, "D3D11_ASYNC_GETDATA_DONOTFLUSH") == 0, "nor a GetData's flags");
        }
        EXPECT(Count(draw, "CopyResource") == 0,
               "the card copies nothing itself: the game's buffer goes into the patch primitive through its own CopyResource");
        EXPECT(Count(draw, "state.patch.Scale(context,bound.Get(),desc.ByteWidth,&entry,1)") == 1,
               "the patch runs through the primitive on the game's own buffer, and it is the only patch the draw path makes");
        // The veil is never patched, and its skip rides its own family's branch.
        const size_t veilAt = draw.find("if(family==Family::Veil)");
        const size_t patchAt = draw.find("constext::ffxiv::engine::ScaleEntryentry=EntryOf(family,verdict.scale);");
        EXPECT(veilAt != std::string::npos && patchAt != std::string::npos && veilAt < patchAt,
               "the veil's branch returns before any patch exists: it is skipped, never patched");
        EXPECT(draw.compare(veilAt, patchAt - veilAt == std::string::npos ? 0u : patchAt - veilAt, "") == 0 ||
                   draw.substr(veilAt, patchAt - veilAt).find("returntrue") != std::string::npos,
               "the veil's decision for a skip ends the draw there");
        // The recycled address answer: the stamp check belongs to the shader object's private data.
        EXPECT(Count(draw, "SetPrivateData(kFlareStampTag,sizeofid,&id)") == 1 && Count(draw, "GetPrivateData(kFlareStampTag") == 1,
               "the stamps are the shader object's own answer, so a recycled address cannot borrow a record");
    }

    // ---- the replay refusal and the one-game-draw-per-game-draw rule of the draw path
    {
        const std::string draw = Code(repo, "ffxiv/engine/FlareDraw.cpp");
        // The chara preview's replay of a game draw is the game's own call: the early-out sits before the veil's branch and before any patch.
        const size_t replayAt = draw.find("if(ext::ffxiv::chara::t_replay)returnfalse;");
        const size_t veilAt = draw.find("if(family==Family::Veil)");
        const size_t patchAt = draw.find("constext::ffxiv::engine::ScaleEntryentry=EntryOf(family,verdict.scale);");
        EXPECT(replayAt != std::string::npos && veilAt != std::string::npos && patchAt != std::string::npos && replayAt < veilAt &&
                   veilAt < patchAt,
               "a character-preview replay is refused (the game's own call) before the veil's skip and before any patch exists");
        // Every refusal leaves the game's draw to run exactly once: each counted decline of the draw path is followed by the draw and the
        // handled return, and the catch runs the draw only when it had not run.
        for (const char* path : { "NoteDecline(state,Decline::NoBuffer);draw(user);returntrue;",
                                  "NoteDecline(state,Decline::BufferSize);draw(user);returntrue;",
                                  "NoteDecline(state,Decline::Exception);if(!drew)draw(user);returntrue;" })
            EXPECT(Count(draw, path) == 1, "the refusal path %s runs the game's draw once and answers handled", path);
        EXPECT(Count(draw, "NoteDecline(state,Decline::PatchFailed);draw(user);returntrue;") == 3,
               "the three PatchFailed refusals (the entry's fit, the primitive's null answer, the slot swap) each run the game's draw once");
        EXPECT(Has(draw, "engine::BoundConstantBufferswap(context,slot,patched);") && Has(draw, "drew=true;draw(user);"),
               "the patched buffer is bound for the one draw by the scope guard, and the draw is marked run before it is made");
    }

    // ---- the card's own file carries no D3D call of a seam it does not own and no lock on the draw path
    {
        const std::string draw = Code(repo, "ffxiv/engine/FlareDraw.cpp");
        EXPECT(Count(draw, "lock_guard") == 1, "only the creation hook takes one lock (the stamps'); the draw path takes none");
        const size_t seamAt = draw.find("boolOnSeamDraw(");
        EXPECT(seamAt != std::string::npos && draw.find("lock_guard") < seamAt,
               "the draw path's own body takes no lock: the stamps' registry lock is all the creation hook holds");
        EXPECT(Count(draw, "D3D11_MAP_WRITE_DISCARD") == 0,
               "the card maps none of the primitive's own parameter blocks: that is the primitive's job");
    }

    std::printf(g_failures ? "%d pin(s) failed\n" : "flare pins: all pass\n", g_failures);
    return g_failures != 0 ? 1 : 0;
}
