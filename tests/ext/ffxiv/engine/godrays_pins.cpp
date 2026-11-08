// God rays: the volumetric march's wiring pins, read as text from the sources (no device).
//
// What a wrong wiring would break, and what is pinned here:
//   the chain's order - the march is the last member, so a march placed before the puddle's stage would composite beams into a scene the
//   pools have not shaped yet, and one that bound t0 itself while chained would give the frame two owners;
//   the stamping contract - a march decline that unstamps the composite's capture marker would let a later chain member run twice on the
//   same frame, so the march's sources must not be able to reach the stamp at all;
//   the hold semantics - the chained decline's line must say the hold stands, and the owner decline's must say the frame is the engine's
//   own, so a session can tell the two apart;
//   the timing flags - a staging Map without D3D11_MAP_FLAG_DO_NOT_WAIT waits for the GPU on the render thread, and a GetData without
//   D3D11_ASYNC_GETDATA_DONOTFLUSH flushes the pipe; both are the FRESH wave's bug class, pinned in the flags argument;
//   the anchors allocation - bit 63 is the fog post's, its slot rides the light shafts' field under the cannot-coexist guard, and the
//   atlas readers take no bit of their own (they ride the light bit the assembly draws already carry);
//   the keys - every default is today's picture, in the config, the binder, the ini, the schema and the settings page.
#include <cctype>
#include <cstdio>
#include <fstream>
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
    if (!file)
        return {};
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// Comments and whitespace removed, so a pin is about the code and not about its formatting (the house pins' own pattern).
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

std::string Raw(const std::string& repo, const char* relative)
{
    return ReadFile(repo + "/OptiScaler/ext/" + relative);
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
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
        ++count;
    return count;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: godrays_pins <repo>\n");
        return 2;
    }
    const std::string repo = argv[1];
    const std::string rules = Code(repo, "ffxiv/engine/GodraysRules.h");
    const std::string draw = Code(repo, "ffxiv/engine/GodraysDraw.cpp");
    const std::string sun = Code(repo, "ffxiv/engine/GodraysSun.cpp");
    const std::string sunHeader = Code(repo, "ffxiv/engine/GodraysSun.h");
    const std::string pass = Code(repo, "ffxiv/engine/GodraysPass.cpp");
    const std::string passHeader = Code(repo, "ffxiv/engine/GodraysPass.h");
    const std::string hooks = Code(repo, "ffxiv/engine/ChainHooks.h");
    const std::string module = Code(repo, "ffxiv/engine/Godrays.h");
    const std::string menu = Code(repo, "ffxiv/engine/GodraysMenu.cpp");
    const std::string shader = Raw(repo, "ffxiv/engine/shaders/Godrays.hlsl");
    const std::string shaderCode = Code(repo, "ffxiv/engine/shaders/Godrays.hlsl");
    const std::string shadowsDispatch = Code(repo, "ffxiv/shadows/ShadowsDispatch.cpp");
    const std::string shadowsRulesCode = Code(repo, "ffxiv/shadows/ShadowsRules.h");
    const std::string fogSun = Code(repo, "ffxiv/engine/FogSun.h");
    const std::string shaftsDraw = Code(repo, "ffxiv/engine/ShaftsDraw.cpp");
    const std::string puddleDraw = Code(repo, "ffxiv/engine/PuddleToneDraw.cpp");
    const std::string enginePass = Code(repo, "ffxiv/engine/EnginePass.cpp");
    const std::string enginePassRules = Code(repo, "ffxiv/engine/EnginePassRules.h");
    const std::string seam = Code(repo, "ffxiv/lighting/FfxivLightingCapture.h");
    const std::string config = Code(repo, "ffxiv/FfxivConfig.h");
    const std::string binder = Code(repo, "core/ExtConfig.cpp");
    const std::string features = Code(repo, "ExtFeatures.h");
    const std::string ini = Raw(repo, "ini/FFXIV.ini");
    // Raw (comments kept) copies, for pins on wording that lives in comments or string literals with spaces.
    const std::string rawRules = ReadFile(repo + "/OptiScaler/ext/ffxiv/engine/GodraysRules.h");
    const std::string rawEnginePassRules = ReadFile(repo + "/OptiScaler/ext/ffxiv/engine/EnginePassRules.h");

    // ---- the chain: the march is the last member, through hook pointers the earlier stages call ------------------------------------
    {
        // The hook pointers live in their own header, so the puddle's and the shafts' tests link without the march and the pointers are
        // null there - the march being off is the same answer the product gives with the key off.
        EXPECT(Has(hooks, "chainMarchScene") && Has(hooks, "toneMapMarchOwner"), "the two chain hooks the march registers at start-up\n");
        EXPECT(Has(hooks, "=nullptr"), "and they default to null: a build without the march's object file runs the chain unchanged\n");
        // The puddle's chained entry: the march runs on the chain's current output after the stage answered (or answered nothing), before
        // t0 is bound - and the pinned bind statement is untouched.
        EXPECT(Has(puddleDraw, "ext::ffxiv::engine::chainMarchScene"), "the puddle's chained entry calls the march's hook\n");
        EXPECT(Has(puddleDraw, "context->PSSetShaderResources(0,1,&bind);"), "the chained entry still binds its own `bind` at t0\n");
        // The count is the merged file's own: the fix line's wave funnelled two skip paths through its hold (7 became 6 there, and its own pin
        // says 6); the march's hook adds no draw, so it stays 6 on the integration line.
        EXPECT(Count(puddleDraw, "draw(user);") == 6, "the puddle's own draw count is untouched by the march's hook\n");
        // The puddle's answer tail: the march chains on the stage's own answer before that path binds t0.
        EXPECT(Has(puddleDraw, "chainMarchScene(context,ChainFactsOf(facts),ours)"),
               "the puddle's answer tail chains the march on its own output\n");
        // The order inside the stage's own answer: the pass's run or the hold has resolved `ours` first, the march chains on it, then t0 is bound,
        // and the composite is stamped last - so a decline of the march can neither unstamp the composite nor bind anything but the chain's hold.
        {
            const size_t hold = puddleDraw.find("ours=state.pass.OutputView();state.held++;");
            const size_t march = puddleDraw.find("chainMarchScene(context,ChainFactsOf(facts),ours)");
            const size_t bindAt = puddleDraw.find("context->PSSetShaderResources(0,1,&ours);");
            const size_t stamp = puddleDraw.find("state.usedCapture=state.capture.id;");
            EXPECT(hold != std::string::npos && march != std::string::npos && bindAt != std::string::npos && stamp != std::string::npos &&
                       hold < march && march < bindAt && bindAt < stamp,
                   "the fallback's order: the hold resolves the answer, the march chains on it, t0 is bound, the composite is stamped\n");
            const size_t chainHold = puddleDraw.rfind("ours=state.pass.OutputView();state.held++;");
            const size_t chainStamp = puddleDraw.rfind("state.usedCapture=state.capture.id;");
            const size_t chainMarch = puddleDraw.find("chainMarchScene(context,facts,bind)");
            const size_t chainBind = puddleDraw.find("context->PSSetShaderResources(0,1,&bind);");
            EXPECT(chainHold != std::string::npos && chainMarch != std::string::npos && chainStamp != std::string::npos &&
                       chainBind != std::string::npos && chainHold < chainStamp && chainStamp < chainMarch && chainMarch < chainBind,
                   "the chain's order: the stage stamps its answer (run or hold) inside OnShaftsScene, then the march chains on `bind`, then t0 is bound\n");
            EXPECT(Has(puddleDraw, "ID3D11ShaderResourceView*bind=pooled!=nullptr?pooled:scene;"),
                   "`bind` is the stage's answer or the scene the caller handed over (this frame's own), never anything older\n");
        }
        // The shafts' fallback owner branch: when neither earlier stage answered, the march owns the draw.
        EXPECT(Has(shaftsDraw, "toneMapMarchOwner"), "the shafts' chain hands the tone map to the march when the puddle is not armed\n");
        EXPECT(Has(shaftsDraw, "ext::ffxiv::engine::gloss::OnShaftsSceneDraw(context,facts,scene,draw,user);"),
               "the shafts' existing chained calls are untouched\n");
        EXPECT(Count(shaftsDraw, "OnShaftsSceneDraw(") == 2, "and still exactly two of them\n");
        // The owner-return rule of the chain: a fallback-owner hook returns true only when it drew the game's call, and a false obliges the
        // caller to draw. The hook is registered whether or not the march is on, and its default (key off, tone map already answered, a decline)
        // is false - a caller that ignored the result would leave the tone map undrawn on a frame the shafts own.
        EXPECT(Has(shaftsDraw, "if(ext::ffxiv::engine::toneMapMarchOwner(context,facts,scene,draw,user))returntrue;"),
               "the shafts' chain draws the game's call itself when the march's owner hook did not (the hook's false is a promise nothing was drawn)\n");
        EXPECT(!Has(shaftsDraw, "toneMapMarchOwner(context,facts,scene,draw,user);"),
               "and no call of the hook ignores its result\n");
        // The march's seam entry: called from exactly one place, after the shafts' and the puddle's entries.
        EXPECT(Count(enginePass, "ext::ffxiv::godrays::OnSeamDraw(") == 1, "the march's seam entry is called from exactly one place\n");
        EXPECT(Has(enginePass, "if(ext::ffxiv::shafts::OnEngineDraw(context,anchors,draw,user))return;") &&
                   Has(enginePass, "if(gloss::OnToneStageDraw(context,anchors,draw,user))return;") &&
                   Has(enginePass, "if(ext::ffxiv::godrays::OnSeamDraw(context,anchors,draw,user))return;"),
               "the chain's dispatch order: the shafts, the puddle's stage, then the march\n");
        // The march's arming reaches every gate the seam's features share.
        EXPECT(Count(enginePass, "godrays::Armed()") >= 3, "the draw path's arming lists carry the march's own arming\n");
        EXPECT(Has(enginePass, "ext::ffxiv::godrays::Release();"), "the engine's teardown releases the march's device objects\n");
        EXPECT(Has(seam, "ext::F::GodraysVolumetric"), "the draw seam's feature test opens for the march's key alone\n");
        EXPECT(Has(enginePass, "ext::ffxiv::godrays::SampleSceneHdr();"), "the march samples the scene HDR state on a pixel-shader bind\n");
    }

    // ---- the stamping contract: the march's sources cannot reach the shared stamp, and its own tracking is its own ------------------
    {
        // The composite's capture marker (`usedCapture` / `capture.id`) is the puddle stage's file-local state. The march's sources must
        // not mention either name: a decline that wrote the stamp would let a later chain member run twice on the same frame, and one
        // that cleared it would let the stage answer a composite twice.
        for (const std::string* source : { &rules, &draw, &sun, &sunHeader, &pass, &passHeader, &hooks, &module, &menu })
        {
            EXPECT(!Has(*source, "usedCapture"), "the march's sources never reference the shared stamp (usedCapture)\n");
            EXPECT(!Has(*source, "capture.id"), "and never reference the capture id it is compared with\n");
        }
        // The march's own answered-tracking is its own rendered-frame counter, never the shared stamp.
        EXPECT(Has(draw, "marchFrame"), "the march tracks the frame it answered with its own counter\n");
        EXPECT(Has(draw, "haveMarchFrame"), "and the flag that separates no answer yet from a counter value of 0\n");
        // The decline paths bind nothing and draw nothing: the chain hook returns a texture or null, and the caller does the binding.
        // The draw path's own t0 binds are the owner's exactly two (the set and the restore's set-back); the chained entry adds none.
        EXPECT(Count(draw, "PSSetShaderResources(0,1,") == 2,
               "only the owner path binds t0, once and restored; the chained entry binds nothing\n");
        EXPECT(Has(draw, "chainMarchScene=&ext::ffxiv::godrays::ChainScene"), "the chained entry is the hook the earlier stages call\n");
        EXPECT(Has(hooks, "ID3D11ShaderResourceView*"), "the chain hook's answer is a texture the caller binds\n");
    }

    // ---- the hold semantics: the two decline wordings exist, and the march's outcome enum names them --------------------------------
    {
        // The wordings live in the log-line format strings, so they are pinned against the raw file (Code strips the spaces).
        EXPECT(Has(rawRules, "the chain's hold stands"), "the chained decline's wording\n");
        EXPECT(Has(rawRules, "the frame is the engine's own"), "the owner decline's wording\n");
        EXPECT(Has(rules, "Outcome::HoldStands") && Has(rules, "Outcome::EngineOwn") && Has(rules, "Outcome::Answered") &&
                   Has(rules, "Outcome::Skipped"),
               "the four outcomes the chain's bookkeeping distinguishes\n");
        // The march's own scene copy exists in the pass (the owner's own copy, the chained one too - the pass always copies what it is
        // handed and writes only its own texture).
        EXPECT(Has(pass, "CopyResource"), "the pass copies the scene it is handed into its own texture\n");
        EXPECT(Has(passHeader, "TermView"), "the march's term is its own texture, for the test that pins it\n");
    }

    // ---- the march is its own file, with no SceneMarch contact and the copied hash named --------------------------------------------
    {
        EXPECT(!Has(shader, "#include"), "Godrays.hlsl has no #include at all: fully self-contained, no SceneMarch contact\n");
        EXPECT(!Has(pass, "SceneMarch"), "and the pass includes none of its headers\n");
        EXPECT(!Has(draw, "SceneMarch"), "and neither does the draw path\n");
        EXPECT(Has(shader, "Hash01"), "the march carries the per-pixel hash\n");
        EXPECT(Has(shader, "LightShafts.hlsl 93-103"), "and names the file and lines it was copied from\n");
        EXPECT(Has(shader, "PS_March") && Has(shader, "PS_Composite"), "the march's own entry points\n");
        EXPECT(Has(shader, "PS_SunColour"), "and the one-texel colour pass the auto colour needs\n");
    }

    // ---- the timing flags: every staging Map carries DO_NOT_WAIT, every GetData carries DONOTFLUSH, in the flags argument -----------
    {
        // The FRESH wave's bug class: a read that waits for the GPU on the render thread. The pin greps the flags argument, not a
        // literal: every Map call site in the march's device files must carry the no-wait flag (a write-discard map is the one
        // exception, and it is pinned as such), and every GetData must carry the no-flush flag.
        EXPECT(Count(sun, "D3D11_MAP_FLAG_DO_NOT_WAIT") >= 1, "the staging reads never wait for the GPU\n");
        EXPECT(Count(sun, "D3D11_MAP_READ") == Count(sun, "D3D11_MAP_FLAG_DO_NOT_WAIT"),
               "every read map in the march's readers carries the no-wait flag in its flags argument\n");
        EXPECT(Count(pass, "D3D11_MAP_WRITE_DISCARD") >= 1, "the parameter buffer's write is a discard map\n");
        EXPECT(Count(pass, "D3D11_MAP_READ") == 0, "the pass maps nothing for reading\n");
        EXPECT(Count(pass, "D3D11_ASYNC_GETDATA_DONOTFLUSH") >= 1, "the timing query's GetData never flushes the pipe\n");
        EXPECT(Count(pass, "GetData(") == Count(pass, "D3D11_ASYNC_GETDATA_DONOTFLUSH"),
               "every GetData call site carries the no-flush flag in its flags argument\n");
    }

    // ---- the anchors allocation: bit 63 is the fog post's, its slot rides the shafts' field, the atlas readers take no bit ----------
    {
        EXPECT(Has(enginePassRules, "kFogPostBit=1ull<<63"), "the fog post's bit is the map's last free flag bit\n");
        EXPECT(Has(enginePassRules, "constexprboolIsFogPostDraw(constPassLayout&layout)noexcept"),
               "the fog post's recognition, by name plus negative anchors\n");
        EXPECT(Has(enginePassRules, "layout.Has(15)&&!layout.Has(17)&&!IsLightDraw(layout)&&!IsSunParamDraw(layout)&&!IsToneMapDraw(layout)"),
               "cFogParam present, the material family's fog buffer absent, and none of the field's other users\n");
        EXPECT(Has(enginePassRules, "if(IsFogPostDraw(layout))"), "and DrawAnchors packs it\n");
        EXPECT(Has(enginePassRules, "constexpruint32_tFogPostSlotOf(Anchorsanchors)noexcept"),
               "the fog post's cFogParam slot rides the light shafts' slot field\n");
        EXPECT(Has(enginePassRules, "{\"g_SamplerShadow\",NameKind::Texture},") &&
                   Has(enginePassRules, "{\"g_DirectionalShadowParameter\",NameKind::ConstantBuffer},") &&
                   Has(enginePassRules, "{\"cFogParam\",NameKind::ConstantBuffer},") &&
                   Has(enginePassRules, "{\"cExpHeightFogParam\",NameKind::ConstantBuffer},") &&
                   Has(enginePassRules, "{\"g_FogParameter\",NameKind::ConstantBuffer},"),
               "the five name rows, appended once\n");
        EXPECT(Has(enginePassRules, "kFogPostBit,"), "the anchors map's disjointness check covers the new bit\n");
        // The atlas readers take no bit: the assembly draws declare g_LightParam, so they carry the light shafts' bit already.
        EXPECT(!Has(enginePassRules, "kAtlasReaderBit"), "no atlas-reader bit exists on this base\n");
        EXPECT(Has(rawEnginePassRules, "the assembly draws declare g_LightParam"), "and the map says why\n");
    }

    // ---- the feature bit: 18, beside the shadows slice's 17 ------------------------------------------------------------------------
    {
        EXPECT(Has(features, "GodraysVolumetric=1ull<<18"), "the march's feature bit is 18\n");
        EXPECT(Has(features, "ShadowMapping=1ull<<17"), "bit 17 is the shadows slice's claim, landed beside this slice on the integration line\n");
    }

    // ---- the keys: ten of them, off or at the engine's own value by default, everywhere a key lives ---------------------------------
    {
        struct Key
        {
            const char* name;
            const char* iniDefault;
        };
        const Key keys[] {
            { "GodraysVolumetric", "false" },   { "GodraysSteps", "48" },       { "GodraysResolution", "quarter" },
            { "GodraysFogBoost", "1.0" },       { "GodraysHeightLift", "1.0" }, { "GodraysIntensity", "1.0" },
            { "GodraysColor", "auto" },         { "GodraysShadowSoftness", "0" }, { "GodraysJitter", "true" },
            { "GodraysPhaseBoost", "1.0" },     { "GodraysDensity", "20.0" },   { "GodraysAnisotropy", "0.8" },
        };
        for (const Key& key : keys)
        {
            std::string member = std::string(key.name);
            member[0] = char(std::tolower(static_cast<unsigned char>(member[0])));
            const std::string label = std::string(key.name);
            EXPECT(Has(config, member), (label + ": the config member\n").c_str());
            EXPECT(Has(binder, member), (label + ": the binder row\n").c_str());
            EXPECT(Has(ini, key.name), (label + ": the ini fragment\n").c_str());
        }
        EXPECT(Has(ini, "GodraysVolumetric=false"), "the master switch is off by default\n");
        EXPECT(Has(ini, "GodraysFogBoost=auto"), "the engine's own density is the default (auto = 1.0)\n");
        EXPECT(Has(ini, "GodraysJitter=auto"), "the jitter is on by default\n");
        EXPECT(Has(config, "godraysOnLoaded"), "the config hook arms the seam's feature bit\n");
        EXPECT(Has(rules, "kDefaultSteps=48") && Has(rules, "kDefaultIntensity=1.0f") && Has(rules, "kDefaultSoftness=0"),
               "the rules carry the defaults the card and the schema state\n");
        EXPECT(Has(rules, "kMarchGain=0.70f"), "the composite gain of the in-scatter integral (the first march's 0.30 was the gain of an average)\n");
        // The menu card exists, in the engine-pass group, with the state line the skip decomposition feeds.
        EXPECT(Has(menu, "Godrays:volumetric(FFXIV)"), "the card's title\n");
        EXPECT(Has(menu, "ffxiv_engine_passes"), "the card sits in the FFXIV tab's engine-pass group\n");
        EXPECT(Has(menu, "FormatCard"), "the card's state line is the log's own decomposition\n");
    }

    // ---- the composite adds the beams, not the haze (Brief R3): the shader's constants are the rules', and the shaping sits before the bound ----------
    {
        EXPECT(Has(rules, "kBeamBoost=0.75f") && Has(shaderCode, "staticconstfloatkBeamBoost=0.75;"), "the boost: the rules' and the shader's number\n");
        EXPECT(Has(rules, "kBeamKeep=0.10f") && Has(shaderCode, "staticconstfloatkBeamKeep=0.10;"), "the kept share: the rules' and the shader's number\n");
        EXPECT(Has(rules, "kBeamTapsPerSide=4") && Has(shaderCode, "staticconstintkBeamTapsPerSide=4;"), "the grid's taps per side: the rules' and the shader's number\n");
        EXPECT(Has(rules, "kBeamReachPixels=48.0f") && Has(shaderCode, "staticconstfloatkBeamReachPixels=48.0;"), "the grid's reach: the rules' and the shader's number\n");
        EXPECT(Has(shaderCode, "returnmin(structure*kBeamBoost+kBeamKeep*term,kMaxTerm);") &&
                   Has(rules, "constfloatshaped=structure*kBeamBoost+kBeamKeep*term;returnshaped>kMaxTerm?kMaxTerm:shaped;"),
               "the shaped term is min(structure * boost + keep * term, kMaxTerm) in both, the boost before the white bound\n");
        EXPECT(Has(shaderCode, "(lowpass==lowpass&&term>lowpass)?term-max(lowpass,0.0):0.0;"), "the level is subtracted and clamped at 0 (no negative light)\n");
        EXPECT(Has(shaderCode, "sum/float((2*kBeamTapsPerSide+1)*(2*kBeamTapsPerSide+1))"), "the low-pass is the mean of the whole grid\n");
        EXPECT(Has(shaderCode, "centre+int2(dx,dy)*stride"), "the grid's taps are spaced by the stride\n");
        EXPECT(Has(shaderCode, "for(intdy=-kBeamTapsPerSide;dy<=kBeamTapsPerSide;++dy)for(intdx=-kBeamTapsPerSide;dx<=kBeamTapsPerSide;++dx)"),
               "the grid is square and symmetric around its centre (both loops run -taps..+taps inclusive)\n");
        EXPECT(Has(shaderCode, "int2(floor(coordinate+0.5))"), "the grid's centre is the nearest march texel (the rules' BeamCentreTexel)\n");
        EXPECT(Has(shaderCode, "int(kBeamReachPixels/Tuning.w/float(kBeamTapsPerSide)+0.5)"), "the stride follows the march's resolution (the rules' BeamTapStride)\n");
        EXPECT(Has(shaderCode, "constfloatterm=BeamTerm(upsampled,BeamLowpass(position.xy));"), "the composite shapes the upsampled term\n");
        const size_t shaped = shaderCode.find("constfloatterm=BeamTerm(upsampled,");
        const size_t bound = shaderCode.find("constfloat3head=max(Bound.x-scene.rgb,0.0);");
        EXPECT(shaped != std::string::npos && bound != std::string::npos && shaped < bound, "the shaping comes before the room-to-white bound\n");
        EXPECT(Has(shaderCode, "constfloat3light=term*Bound.z*tint*Bound.y;"), "the light is the shaped term times the key, the tint, the gain and the fade\n");
    }

    // ---- the fail-open contract: the scene-HDR decline, the replay refusal, the stand-down, the private state -----------------------
    {
        EXPECT(Has(rules, "kSceneHdrInactive"), "the scene-HDR decline's line exists\n");
        EXPECT(Has(draw, "SceneHdrRequested"), "the key is read live, so a session that switches HDR off starts the march again\n");
        EXPECT(Has(rules, "CharaReplay"), "the chara replay refusal is a decline of its own\n");
        EXPECT(Has(rules, "Knob::Godrays") || Has(rules, "Godrays)==3"), "the march's refusal streak is its own knob\n");
        EXPECT(Has(pass, R"x(ext::core::explicit_state::GuardstateGuard(context,context1.Get(),device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1,kSpec);)x") &&
                   Has(pass, R"x(stateGuard.Neutralise();)x"),
               "the game's state is captured, the private state's defaults (no HS, DS, GS, stream-out or predicate) are set explicitly and a scope guard puts it back\n");
        EXPECT(!Has(pass, "SwapDeviceContextState") && !Has(pass, "CreateDeviceContextState"),
               "no device-context state swap remains: it does nothing when the draw hook is nested\n");
        EXPECT(Has(pass, R"x(explicit_state::SpeckSpec{8,0,1};)x") && Has(pass, "static_assert(kSpec.psSrvCount==kSlots"),
               "the pass declares exactly the state it touches: pixel-shader resource slots 0-7 (kSlots), no sampler, constant buffer 0\n");
        {
            // The guard sits exactly where the swap sat: after the scene copy, the parameter buffer's Map and Unmap and the timing collection. The
            // copy is made under the game's own state (a game predicate skips it, as it did under the swap), so a guard above it changes the output.
            const size_t mapAt = pass.find("context->Map(_params.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped)");
            const size_t copyAt = mapAt == std::string::npos ? mapAt : pass.rfind("context->CopyResource(_scene.Get(),sceneResource.Get());", mapAt);
            const size_t unmapAt = pass.find("context->Unmap(_params.Get(),0);", mapAt);
            const size_t timingAt = pass.find("CollectGpuTiming(context);", unmapAt);
            const size_t guardAt = pass.find("ext::core::explicit_state::GuardstateGuard(");
            EXPECT(copyAt != std::string::npos && mapAt != std::string::npos && unmapAt != std::string::npos && timingAt != std::string::npos &&
                       guardAt != std::string::npos && Count(pass, "ext::core::explicit_state::GuardstateGuard(") == 1,
                   "the scene copy, the parameter Map and Unmap, the timing collection and the one guard are all found in Run\n");
            EXPECT(copyAt < mapAt && mapAt < unmapAt && unmapAt < timingAt && timingAt < guardAt,
                   "the guard comes after the scene copy, the parameter buffer's Map/Unmap and the timing collection (the copy runs under the game's state)\n");
            // Draw 2, the sun's colour, runs only when the colour key asks for the sky's colour: without the condition it would run every frame.
            EXPECT(Has(pass, "if(inputs.params.colour==Colour::Auto&&inputs.tintFromSky){ID3D11ShaderResourceView*colourViews[kSlots]{};") &&
                       Count(pass, "context->Draw(3,0);") == 3,
                   "the sun colour's draw is behind colour == Auto && tintFromSky, and the pass makes exactly three draws\n");
        }
        EXPECT(Has(pass, "IsSceneFormat"), "the scene is refused when it is not FP16\n");
        EXPECT(Has(pass, "IsDepthFormat"), "and so is a depth the pass cannot read\n");
        // The march's captures never wait for the GPU and never write a game resource.
        EXPECT(Has(sun, "CopySubresourceRegion"), "the captures copy into their own staging buffers\n");
        EXPECT(!Has(sun, "CopyResource(game"), "nothing of the game is written\n");
        // The log lines are the rules' own, so the host tests can pin them.
        EXPECT(Has(rules, "FormatDecline") && Has(rules, "FormatSkip") && Has(rules, "FormatCard") && Has(rules, "FormatRun"),
               "the log and card lines are pure rules\n");
        EXPECT(Has(rules, "CountsTowardsStandDown"), "and the stand-down's counting rule is one the host can test\n");
    }

    // ---- the beams that follow the engine's own shadows: the second march's wiring ---------------------------------------------------------
    {
        // The sky's ceiling comes from the scene's own sky pixels; the engine's sSky texture is never read (its median is 0 and it does not correlate).
        EXPECT(!Has(shaderCode, "sSky") && !Has(pass, "sSky") && !Has(draw, "sSky") && !Has(sun, "sSky") && !Has(sunHeader, "sSky"),
               "no god rays source reads the engine's sSky texture\n");
        // The march: every cascade, each only inside its own slice, the minimum; the first-cascade-wins pick and the cos^16 lobe are gone.
        EXPECT(!Has(shaderCode, "found>=0.0") && !Has(shaderCode, "pow(cosTheta,FogC.z)"),
               "neither the first-cascade pick nor the pow(cos, exponent) lobe is left in the shader\n");
        EXPECT(Has(shaderCode, "InSlice(") && Has(shaderCode, "HenyeyGreenstein(") && Has(shaderCode, "exp(-tau)") && Has(shaderCode, "AtlasParam.w"),
               "the shader has the slice test, the Henyey-Greenstein phase, the extinction and the air bias\n");
        EXPECT(Has(shaderCode, "visibility=min(visibility,"), "and the cascades' visibilities are blended with a minimum\n");
        // The stats pass: one compute entry, one dispatch with the pass's own compute-state save and restore, and a stats texture the reader copies.
        EXPECT(Has(shaderCode, "CS_Stats") && Has(shaderCode, "groupshared") && Has(shaderCode, "[numthreads("), "the stats compute entry exists\n");
        // The product build compiles the compute entry into the header the pass includes (the test recipe has its own list, so only this pin sees the product's).
        const std::string shaderProps = Raw(repo, "ffxiv/engine/godrays.shaders.props");
        EXPECT(Has(shaderProps, "<Header>Godrays_Stats.h</Header>") && Has(shaderProps, "<Profile>cs_5_0</Profile>") && Has(shaderProps, "<Entry>CS_Stats</Entry>") &&
                   Has(pass, "#include<Godrays_Stats.h>"),
               "the product's shader list compiles CS_Stats into the header the pass includes\n");
        EXPECT(Has(pass, "ComputeStateRestore") && Count(pass, "context->Dispatch(") == 1,
               "the pass makes exactly one dispatch, under its own compute-state save and restore (the explicit-state Guard does not cover the compute stage)\n");
        EXPECT(Has(passHeader, "StatsTexture()") && Has(passHeader, "floatlightGain=1.0f;") && Has(passHeader, "boolskyCeiling=true;") &&
                   Has(passHeader, "boolbilateral=true;"),
               "the pass hands its stats texture to the reader, and the new inputs default to the full effect (the lab driver sets none of them)\n");
        EXPECT(Has(sunHeader, "classStatsReader") && Has(sun, "StatsReader::Read"), "the stats read-back is a reader of the sun file, with the DO_NOT_WAIT maps counted above\n");
        // The fog sun: read from the 128-byte copy without the profile, published through the one accessor, and the sun-only capture reuses the reader.
        EXPECT(Has(sunHeader, "sunLanded") && Has(sun, "FogSunOf("), "the fog read decodes the sun row on its own\n");
        EXPECT(Has(draw, "fogsun::Publish(") && Has(draw, "fogsun::SetWantedChangedHook("), "the god rays module publishes the sun and registers for the consumer's flag\n");
        EXPECT(!Has(fogSun, "Godrays") && !Has(fogSun, "Shadows"), "the accessor names neither module\n");
        // Arming: the seam is wanted by the march OR the fog sun; every march branch is behind the march's own test.
        EXPECT(Has(module, "boolMarchArmed();") && Has(module, "boolArmed();"), "the interface has the march's own test and the seam's\n");
        EXPECT(Has(draw, "boolArmed(){returnMarchArmed()||") && Has(draw, "fogsun::Wanted()"), "Armed is the march or the fog sun's flag\n");
        EXPECT(Has(draw, "if(!MarchArmed()&&!IsFogPostAnchors(anchors))") || Has(draw, "if(!Armed()||context==nullptr)"), "the seam entry exits at its own test\n");
        EXPECT(Has(draw, "if(ext::ffxiv::engine::IsShaftsLightAnchors(anchors)&&MarchArmed())") &&
                   Has(draw, "if(ext::ffxiv::engine::IsShaftsCompositeAnchors(anchors)&&MarchArmed())") &&
                   Has(draw, "if(ext::ffxiv::engine::IsShaftsToneMapAnchors(anchors)&&MarchArmed())"),
               "the light, composite and tone map branches are the march's only: with the fog sun alone the tone map is the game's own\n");
        EXPECT(Has(draw, "if(MarchArmed()&&!(state.haveMarchFrame&&state.marchFrame==state.frames))") && Has(draw, "!MarchArmed()"),
               "the chain's entries answer only for the march\n");
        EXPECT(Has(draw, "CaptureFogSunOnly(") && Has(draw, "state.fog.Copy(context,fogParam.Get(),nullptr,nullptr,"),
               "the sun-only capture copies the fog constants through the same reader, without a profile\n");
        // The light: the cascade's axis is the march's direction and the verdict is one rule; the term peak in the run line is measured.
        EXPECT(Has(draw, "JudgeLight(") && Has(draw, "CascadeAxisOf(") && !Has(draw, "marchState.sunDirection[0]=state.hold.facts.direction[0];"),
               "the march's direction is the finest cascade's axis, judged against the light draw's direction\n");
        EXPECT(!Has(draw, "state.whitePoint,0.0f))"), "the run line's term peak is no longer a literal 0\n");
        EXPECT(Has(draw, "FormatStageValues(") && Has(draw, "cfg.godraysDensity.value_or_default()") && Has(draw, "cfg.godraysAnisotropy.value_or_default()"),
               "the stage values string and the two new keys reach the module\n");
        // The shadow mapping side: it reads the accessor, never the module.
        EXPECT(!Has(shadowsDispatch, "godrays::") && !Has(shadowsDispatch, "GodraysDraw") && Has(shadowsDispatch, "fogsun::Read()") &&
                   Has(shadowsDispatch, "fogsun::SetWanted("),
               "the night curve reads the fog sun through the accessor and asks for it with the flag\n");
        EXPECT(Has(shadowsDispatch, "NightCurve(") && Has(shadowsDispatch, "FrameBlendCurve(") && Has(shadowsDispatch, "FormatNightSource("),
               "both consumers go through the one curve and the source change is logged\n");
        EXPECT(Has(shadowsRulesCode, "kFogSunMaxAgeFrames") && Has(shadowsRulesCode, "structFogSunTracker"), "the fog sun's freshness is a rule\n");
        // The default stays free: the flag is false until shadow mapping wants the curve.
        EXPECT(Has(fogSun, "boolWanted()") && Has(fogSun, "std::atomic<uint32_t>g_wantedBy{0}"),
               "the accessor's flag (one bit per consumer, 2026-11-07) is down by default\n");
    }
    std::printf(g_failures ? "godrays_pins: %d pin(s) failed\n" : "godrays_pins: all pins hold\n", g_failures);
    return g_failures != 0;
}
