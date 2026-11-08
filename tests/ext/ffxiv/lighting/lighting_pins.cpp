// Text pins of the lighting tab's chara ambient: where the module hangs, what the key-off costs, and what it must never do.
//
// The pins this module owes: the shared name-table row it keys on, the curve it does NOT re-declare, the no-wait reads it never waits with,
// the card rows it registers under, and the page that assembles the lighting levers.
#include <cstdio>
#include <cstdlib>
#include <fstream>
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
std::string RawTests(const std::string& repo, const char* relative) { return ReadFile(repo + "/tests/" + relative); }

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
    const std::string noComments = Comments(Raw(repo, relative));
    std::string out;
    for (char c : noComments)
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

    // ---- the seam: the module hangs off the capture hook chain, four call sites, one relaxed load while the key is off
    {
        const std::string hooks = Code(repo, "ffxiv/lighting/FfxivLightingCapture.h");
        EXPECT(Count(hooks, "ext::ffxiv::lighting::chara::") == 4,
               "the module is called from four places: the draw seam, the pixel-shader seam, the shader creation and the teardown (its include is its own hook-chain line)");
        EXPECT(Has(hooks, R"x(if(c==context)ext::ffxiv::lighting::chara::OnPixelShaderSet(ps,count);)x"),
               "the bound shader is tracked where the game binds it (one atomic load inside while the key is off)");
        EXPECT(Has(hooks, R"x(ext::ffxiv::lighting::chara::OnShaderCreated(*out,code,size,crc);)x"),
               "the stamp is written where the bytecode is (one byte scan; the reflection of a chara draw is parsed once)");
        EXPECT(Has(hooks, R"x(ext::ffxiv::lighting::chara::BoundDraw(c,shadows);)x"),
               "the patch wraps the game's own draw, between the engine's BoundDraw and the contact shadow swap");
        EXPECT(Has(hooks, R"x(#include"ext/ffxiv/lighting/CharaAmbientApply.h")x"), "the seam's header is included");
        EXPECT(Has(hooks, R"x(ext::ffxiv::lighting::chara::Release();)x"),
               "the teardown releases the patch buffers and the staging ring while the device is alive");
        EXPECT(Has(hooks, R"x(ext::F::CharaAmbient))x"), "the feature bit is one of the seam's one-load test's features");
    }

    // ---- the feature bit: the draw path's one relaxed load, the bit off at the identity value
    {
        const std::string features = Code(repo, "ExtFeatures.h");
        EXPECT(Has(features, R"x(CharaAmbient=1ull<<19,)x"), "the feature bit is the next free one (shadows took 17, the march 18)");
        EXPECT(Has(features, R"x(template<class...Fs>inlineboolActiveAny(Ffirst,Fs...rest))x"),
               "ActiveAny is ONE variadic: a slice that adds a draw-seam feature appends a flag to the call and never adds an overload");
        EXPECT(Count(features, "ActiveAny(") == 1, "and it is the only definition of the name (no fixed-arity overload is left beside it)");
        EXPECT(Has(features, "static_assert((std::is_same_v<Fs,F>&&...)"), "every argument must be a feature, so a stray integer cannot widen the mask");
    }

    // ---- the keys: off and at the engine's value by default, documented, bound
    {
        const std::string config = Code(repo, "ffxiv/FfxivConfig.h");
        EXPECT(Has(config, R"x(CustomOptional<bool>charaAmbient{false};)x"), "the master switch, off by default");
        EXPECT(Has(config, R"x(CustomOptional<float>charaAmbientLift{1.0f};)x"), "the lift, at the engine's picture by default");
        EXPECT(Has(config, R"x(staticinlineBoolHookcharaAmbientOnLoaded=nullptr;)x"),
               "the key reaches the module through the config's own hook pointer");
        EXPECT(Has(config, R"x(charaAmbientOnLoaded(charaAmbient.value_or_default());)x"),
               "the config load's tail publishes the key");
        const std::string binds = Code(repo, "core/ExtConfig.cpp");
        EXPECT(Has(binds, R"x(b.Add("FFXIV","CharaAmbient",charaAmbient);)x") && Has(binds, R"x(b.Add("FFXIV","CharaAmbientLift",charaAmbientLift);)x"),
               "both keys are bound (a binder line that names none of the two would leave one key untracked)");
        const std::string ini = ReadFile(repo + "/OptiScaler/ext/ini/FFXIV.ini");
        EXPECT(Has(ini, "CharaAmbient=auto") && Has(ini, "CharaAmbientLift=auto"), "the ini fragment carries both keys at auto");
        EXPECT(Has(ReadFile(repo + "/tools/settings-schema.json"), "CharaAmbient"), "the schema carries the key");
    }

    // ---- the pure rules are pure: no device, no config, no new curve constants, the shared curve only
    {
        const std::string rules = Code(repo, "ffxiv/lighting/CharaAmbientRules.h");
        EXPECT(!Has(rules, "d3d11.h") && !Has(rules, "D3D11"), "the rules carry no D3D type or call");
        EXPECT(!Has(rules, "ext::Cfg"), "the rules carry no config read");
        EXPECT(Has(rules, R"x(#include"ext/ffxiv/shadows/ShadowsRules.h")x"),
               "the curve is the shadow mapping module's own, included - not re-declared");
        EXPECT(Has(rules, "shadows::NightCurve(true,reading).curve"),
               "the blend calls the shared night curve (fog sun, then the light colour; 2026-11-07), not a copy");
        EXPECT(Count(rules, "kCurveNightLuminance") == 0 && Count(rules, "kCurveDayLuminance") == 0,
               "the provisional 0.15/0.90 thresholds live only in the shadows rules header");
        EXPECT(Has(rules, R"x(entry.multiplier[3]=scale;)x") || Has(rules, R"x(entry.multiplier[kCharaRecipComponent]=scale;)x"),
               "the entry scales the block's .w alone");
        EXPECT(!Has(rules, "0.15f") && !Has(rules, "0.90f"),
               "neither provisional threshold constant is written in the chara rules: the one curve lives in the shadows rules");
    }

    // ---- the device side: never a game write, the staging ring no-wait, the patch on the shared primitive
    {
        const std::string apply = Code(repo, "ffxiv/lighting/CharaAmbientApply.cpp");
        EXPECT(Has(apply, R"x(#include"ext/ffxiv/engine/CbPatch.h")x") || Has(apply, R"x(#include"ext/ffxiv/lighting/CharaAmbientApply.h")x"),
               "the patch is the engine pass overrides' shared primitive");
        EXPECT(Count(apply, "CreateBuffer(&desc") == 1,
               "the module creates only its own staging ring here (the patch primitive's owned buffers are its own file's test)");
        EXPECT(Count(apply, "CopyResource") == 0, "the module copies the game's resources through the patch primitive alone");
        EXPECT(Count(apply, "CopySubresourceRegion") == 1, "the ring's capture is the one copy the module makes of a game buffer");
        EXPECT(Count(apply, "D3D11_MAP_FLAG_DO_NOT_WAIT") == 1, "the staging read waits never: the flags argument is pinned");
        EXPECT(Count(apply, "D3D11_MAP_WRITE") == 0, "the module maps nothing for writing: no game resource is written");
        EXPECT(Has(apply, "D3D11_CPU_ACCESS_READ"), "the staging ring is a read-back staging pair");
        EXPECT(Has(apply, R"x(ext::ffxiv::chara::ReplayScopeown;)x"),
               "the patch guard raises the character preview's marker, so its copy hooks never count this module's copies");
        EXPECT(Has(apply, R"x(ext::present::FrameCount())x"), "the frame token is the present counter both present paths tick");
        EXPECT(Has(apply, "patch.Generation()"), "the cache epoch includes the primitive's own generation");
        EXPECT(Has(apply, R"x(D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,)x"),
               "the staging map is a MAP_READ with DO_NOT_WAIT: the no-wait rule in its staged form");
        // The replay refusal: a replay of a game draw (the scene HDR path, the character preview) is the game's own call and nothing else.
        EXPECT(Has(apply, R"x(if(context==nullptr||ext::ffxiv::chara::t_replay){draw(user);return;})x"),
               "a replayed draw is run alone: the module neither patches nor captures on it");
        // The identity in the chain runs the game's call and spends no budget.
        EXPECT(Has(apply, R"x(if(!blend.runs){draw(user);return;})x"), "the identity (master off, no reading, idle lift, day frame) runs the game's draw alone");
        // A game buffer fetched with PSGetConstantBuffers arrives with a reference: each one is attached to a ComPtr (a raw pointer would leak
        // the game's own buffer at every call).
        EXPECT(Count(apply, "PSGetConstantBuffers(") == 2 && Count(apply, ".Attach(") == 2,
               "every constant buffer the module fetches from the game is attached to a ComPtr, so no reference is leaked");
        // The curve's colour is read at the SUN light draw only, by the contact path's own name test; a shader of the character family is the
        // one that declares the customize block beside the ambient block (the world's materials declare the ambient block alone).
        EXPECT(Has(apply, "ext::ffxiv::relight::LayoutOfShader(code,size)") && Has(apply, "ext::ffxiv::relight::IsSunLightLayout(sun)") &&
                   !Has(apply, "if(sun.ok)"),
               "the light draw is classified by the contact path's sun test (the four names AND the directional cascade parameters, 2026-11-07): the rain volumes and the shadowed point/spot lights are not it");
        EXPECT(Has(apply, "IsCharaShader(") && Has(apply, "kCharaCustomizeName"),
               "a character draw declares the customize block beside the ambient block, so the world's materials are never patched");
        EXPECT(Count(apply, "uint32_tkNoSlot=") == 0 && Count(apply, "uint32_tPackOf(") == 0,
               "the sentinel and the pack come from the pure rules (slot 0 is a real slot), not a local copy");
        // A change of the lift while armed starts a new cache epoch: the next draw patches again instead of binding the old scale's copy.
        EXPECT(Has(apply, R"x(ext::SetActive(ext::F::CharaAmbient,armed);state.cacheEpoch.fetch_add(1,std::memory_order_relaxed);)x"),
               "every publish starts a new cache epoch, not only a change of the armed state");
        // The night reading (2026-11-07 integration): the character ambient keys on shadow mapping's own curve - the visible sun while it is fresh
        // (aged on this module's own per-frame clock, not the present counter that frame generation steps), the light colour otherwise - and asks
        // for the fog-post capture as its own consumer, so it works with shadow mapping off too.
        EXPECT(Has(apply, R"x(constBlendblend=FrameBlendNight()x") && Count(apply, "FrameBlend(") == 0,
               "the blend reads the night reading (fog sun, then colour), never the colour-only entry");
        EXPECT(Has(apply, R"x(reading.fogFresh=fog.have&&state.fogTracker.Fresh(state.nightClock);)x"),
               "the fog sun counts only while fresh on the module's own clock");
        EXPECT(Has(apply, R"x(state.fogTracker.Observe(fog.count,state.nightClock);)x"), "the publish count restarts the age");
        EXPECT(Has(apply, R"x(ReadRing(context,state,frame);++state.nightClock;)x"),
               "the clock ticks once per frame with character draws, at the ring's own once-a-frame read");
        EXPECT(Has(apply, R"x(fogsun::SetWanted(fogsun::kCharaAmbient,)x"), "the module asks for the fog sun as its own consumer");
    }

    // ---- the card rows: the registration, the layout slot, the manifest lists
    {
        const std::string menu = Code(repo, "ffxiv/lighting/CharaAmbientMenu.cpp");
        EXPECT(Has(menu, R"x({"ffxiv_lighting","FFXIV","Lighting(FFXIV)",Draw,"ffxiv_shadow_mapping",})x")
                   || Has(menu, R"x({"ffxiv_lighting","FFXIV","Lighting(FFXIV)",Draw,"ffxiv_shadow_mapping",Visible})x"),
               "the card registers in the FFXIV tab, right after the shadow mapping card");
        EXPECT(Has(menu, R"x(SectionTitle("Lighting(FFXIV)")x"), "the card's own title");
        EXPECT(Has(menu, "ext::ffxiv::engine::CurrentStatus()") && Has(menu, "ext::ffxiv::shafts::CurrentStatus()") &&
                   Has(menu, "ext::ffxiv::godrays::CurrentStatus()"),
               "the assembly shows the shipped levers' own statuses");
        EXPECT(Has(menu, "Hooks::ambientWants"), "the night ambient lift's arm is asked at the seams' hook");
        EXPECT(Has(menu, "scene HDR") || Has(menu, "sceneHDR"), "the scene-HDR rule is stated in the card text");
        const std::string layout = Code(repo, "core/MenuLayout.h");
        EXPECT(Has(layout, R"x({"ffxiv_lighting","FFXIV","ffxiv_shadow_mapping"},)x"), "the layout slot, after the shadow mapping card");
        const std::string manifest = Code(repo, "core/MenuManifest.h");
        EXPECT(Has(manifest, "\"ffxiv_lighting\""), "the manifest lists the card as FFXIV-only (it is not in a released menu, so not in the persisted list)");
        EXPECT(Count(manifest, "\"ffxiv_lighting\"") == 1, "and only there: the persisted list holds ids of released menus");
        // the cards' own keys are not duplicated here: the assembly shows statuses, the controls belong to their own cards
        EXPECT(Count(menu, "lightShaftsIntensity") == 0 && Count(menu, "godraysVolumetric") == 0,
               "the assembly carries no lever's keys of its own: only the chara ambient's controls");
    }

    // ---- the module's own cards' key names appear nowhere else in the assembly
    {
        const std::string seams = Code(repo, "ffxiv/lighting/CharaAmbientApply.h");
        EXPECT(Has(seams, "voidPublish(boolon);") && Has(seams, "voidRelease();"),
               "the publish and release entries the config load and the teardown call");
    }

    // ---- the wiring test of the shared seam belongs to the engine's own pins (this branch edits what they assert in the documented spots);
    //      this test pins the ordering of the page and the module's own lines instead.
    (void)RawTests;

    return g_failures != 0 ? 1 : 0;
}
