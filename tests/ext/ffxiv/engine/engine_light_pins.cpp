// Engine light controls (ENG2): the wiring AS BUILT, read as text from the sources (no device). The behaviour is engine_light_rules / engine_light_lamps_warp /
// engine_light_pass_warp / engine_light_chain_warp; this half pins what a text can.
//
// What a wrong wiring would break, and what is pinned here:
//   the lamp wrapper is substituted for the game's draw function in the seam's draw path BEFORE any other module sees the draw (so the ambient occlusion / puddles /
//   reflection swaps compose with it): one place, after the bound anchors are read, before the first module entry, and the native-draw record is refreshed;
//   the lamp arm restores the game's blend state, factor and sample mask in a destructor, and arms that restore BEFORE the state call (so a half-failed set still restores);
//   an identity factor builds no wrapper (the early return is before any context call);
//   the feed request depends on "wanted", never on the exposure stage or the exposure mode (the starved cell), and the forced weight asks for nothing;
//   PublishCbFamily is null-checked and called from the one place that holds the applied weight, on a change of the weight or of the keys, and from Configure;
//   the exposure is read from the slot the composite's REFLECTION names (a stamp), never from a literal slot;
//   the stage keeps the old slot's wiring: chain hooks registered, the F9 stage 13 and the feature bit 24, the one seam entry, the release chain;
//   the retired luminance grade is gone: no code, no key, no shader, and its four keys are only in the schema's retired list;
//   every engine light config member has a binder, an INI line, a schema entry and a read in ParamsFromConfig; the card id is in the layout and the manifest.
#include <cctype>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

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

std::string Squeeze(const std::string& text)
{
    std::string out;
    for (char c : text)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            out += c;
    return out;
}

std::string Code(const std::string& repo, const char* relative) { return Squeeze(Comments(ReadFile(repo + "/OptiScaler/ext/" + relative))); }
std::string Raw(const std::string& repo, const char* relative) { return ReadFile(repo + "/" + relative); }

bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
size_t Count(const std::string& text, const std::string& needle)
{
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
        ++count;
    return count;
}
// `first` appears and `second` appears after it.
bool Ordered(const std::string& text, const std::string& first, const std::string& second)
{
    const size_t a = text.find(first);
    return a != std::string::npos && text.find(second, a + first.size()) != std::string::npos;
}

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
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: engine_light_pins <repo>\n");
        return 2;
    }
    const std::string repo = argv[1];
    const std::string enginePass = Code(repo, "ffxiv/engine/EnginePass.cpp");
    const std::string lamps = Code(repo, "ffxiv/enginelight/EngineLightLamps.cpp");
    const std::string lampsH = Code(repo, "ffxiv/enginelight/EngineLightLamps.h");
    const std::string draw = Code(repo, "ffxiv/enginelight/EngineLightDraw.cpp");
    const std::string rules = Code(repo, "ffxiv/enginelight/EngineLightRules.h");
    const std::string config = Code(repo, "ffxiv/enginelight/EngineLightConfig.cpp");
    const std::string menu = Code(repo, "ffxiv/enginelight/EngineLightMenu.cpp");
    const std::string ffxivConfig = Code(repo, "ffxiv/FfxivConfig.h");
    const std::string extConfig = Code(repo, "core/ExtConfig.cpp");

    // ---- the lamp wrapper in the seam's draw path ---------------------------------------------------------------------------------------------
    {
        const std::string onGameDraw = Body(enginePass, "voidOnGameDraw(ID3D11DeviceContext*context,DrawFndraw,void*user)");
        EXPECT(!onGameDraw.empty(), "OnGameDraw is found");
        EXPECT(Count(onGameDraw, "state.boundAnchors.load(std::memory_order_relaxed)") == 1, "the draw path reads the seam's anchors ONCE (the lamp arm shares that load)");
        EXPECT(Count(onGameDraw, "PrepareLampDraw(") == 1, "the lamp wrapper is substituted in exactly one place");
        EXPECT(Ordered(onGameDraw, "constAnchorsanchors=state.boundAnchors.load(std::memory_order_relaxed);", "PrepareLampDraw(context,anchors,draw,user,lampDraw)"),
               "... right after the anchors are read");
        EXPECT(Ordered(onGameDraw, "PrepareLampDraw(context,anchors,draw,user,lampDraw)", "ext::ffxiv::shafts::OnEngineDraw(context,anchors,draw,user)"),
               "... and before the first module entry sees the draw (the reader swaps, the stages and the pass-through all call the wrapped draw)");
        EXPECT(Ordered(onGameDraw, "PrepareLampDraw(context,anchors,draw,user,lampDraw)", "native.draw=draw;native.user=user;"),
               "... and the native-draw record follows the substitution");
        EXPECT(Has(onGameDraw, "if(!ext::ffxiv::chara::t_replay&&ext::ffxiv::enginelight::PrepareLampDraw(context,anchors,draw,user,lampDraw))"), "a character-preview replay never gets the lamp wrapper");
        EXPECT(Ordered(onGameDraw, "ext::ffxiv::enginelight::LampDrawlampDraw;", "structNativeDraw"), "the wrapper object lives for the whole call (the catch below draws through it too)");
        EXPECT(Ordered(onGameDraw, "ext::ffxiv::engine::gi::OnSeamDraw(", "ext::ffxiv::enginelight::OnSeamDraw(") && Ordered(onGameDraw, "ext::ffxiv::enginelight::OnSeamDraw(", "ext::ffxiv::engine::atmo::OnSeamDraw("),
               "the module keeps the night grade's place in the chain: behind the light bounce, ahead of the atmosphere");
        EXPECT(Count(enginePass, "ext::ffxiv::enginelight::Armed()") == 5, "the five arming lists carry the module");
        EXPECT(Count(enginePass, "ext::ffxiv::enginelight::NoteShader(shader,code,size);") == 1, "the composite's slot stamp is written at shader creation, once");
        EXPECT(Has(enginePass, "ext::ffxiv::enginelight::Release();"), "the release chain releases the module (hold pass, exposure ring, lamp blend states)");
    }

    // ---- the lamp arm ------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string prepare = Body(lamps, "boolPrepareLampDraw(ID3D11DeviceContext*context,engine::Anchorsanchors,DrawFn&draw,void*&user,LampDraw&out)");
        EXPECT(!prepare.empty(), "PrepareLampDraw is found");
        EXPECT(Ordered(prepare, "if(!engine::IsShaftsLightAnchors(anchors)", "if(NearOne(factor))returnfalse;") && Ordered(prepare, "if(NearOne(factor))returnfalse;", "draw=&LampDraw::Invoke;"),
               "an identity factor returns before the wrapper is built");
        EXPECT(!Has(prepare, "context->"), "PrepareLampDraw makes no context call at all");
        const std::string scope = Body(lamps, "classScope{");
        EXPECT(Has(lamps, "~Scope(){if(!_armed)return;try{_context->OMSetBlendState(_original.Get(),_originalFactor,_originalMask);}"), "the destructor puts the game's blend state, factor and sample mask back");
        EXPECT(Ordered(lamps, "_armed=true;", "context->OMSetBlendState(derived,factor,originalMask);"), "the restore is armed BEFORE the state call");
        EXPECT(Has(lamps, "context->OMSetBlendState(derived,factor,originalMask);"), "the derived state keeps the game's own sample mask");
        EXPECT(Has(lamps, "voidLampDraw::Invoke(void*opaque){LampDraw&self=*static_cast<LampDraw*>(opaque);Scopescope(self);self.draw(self.user);}"), "the wrapper sets, draws once, restores in the destructor");
        EXPECT(Has(lamps, "desc.RenderTarget[i].SrcBlend==D3D11_BLEND_ONE)desc.RenderTarget[i].SrcBlend=D3D11_BLEND_BLEND_FACTOR;"), "the derived state changes SrcBlend ONE to BLEND_FACTOR and nothing else");
        EXPECT(Has(lampsH, "kBlendOne=2") && Has(lampsH, "kBlendFactor=14") && Has(lampsH, "kBlendOpAdd=1") && Has(lampsH, "kFormatR11G11B10Float=26"), "the shape constants are D3D11's");
        EXPECT(Has(lamps, "StageOpen(ext::ffxiv::diag::StageId::NightGrade)"), "the capture-frame row is built only while the capture frame is open");
        (void)scope;
    }

    // ---- the module: feed, publish, exposure source, stage wiring ------------------------------------------------------------------------------
    {
        const std::string feed = Body(draw, "voidPublishFeed(State&state)");
        EXPECT(Has(feed, "nightFeed(WantedNow(state)&&!state.controller.params.forceNightWeight)"), "the feed request is 'the module is wanted' (and not the forced weight)");
        EXPECT(!Has(feed, "exposureMode") && !Has(feed, "StageWanted") && !Has(feed, "kExposure"), "... and never depends on the exposure mode or the stage (the starved cell)");
        EXPECT(Has(draw, "boolWantedNow(constState&state){returnstate.configured&&Wanted(state.controller.params);}"), "wanted = configured and Wanted(params): no stand-down, no stage state in it");
        const std::string outputs = Body(draw, "voidApplyOutputs(State&state,boolforce)");
        EXPECT(Has(outputs, "SetLampFactor(LampFactor(params,weight));"), "the lamp factor follows the applied weight and the keys");
        EXPECT(Has(outputs, "if(publishCbFamily!=nullptr&&(force||!state.publishedOnce||weight!=state.publishedWeight||state.paramsVersion!=state.publishedVersion))"),
               "PublishCbFamily is null-checked and called on a change of the weight or of the keys");
        EXPECT(Count(draw, "ApplyOutputs(state,") == 2, "ApplyOutputs runs from Configure (forced) and from the frame tick");
        EXPECT(Has(draw, "context1->PSGetConstantBuffers1(UINT(slot),1,&bound,&first,&count)") && Has(draw, "CommonSlotOfBoundShader(context)"), "the exposure buffer is taken at the slot the composite's stamp names");
        EXPECT(Has(draw, "constexprGUIDkCommonStampTag") && Has(draw, "ext::ffxiv::diag::BindKind::ConstantBuffer,\"g_AmbientParamArray\"") && Has(draw, "ext::ffxiv::diag::BindKind::ConstantBuffer,\"g_CommonParameter\""),
               "the stamp is written on a shader that declares the ambient table AND g_CommonParameter (the composite), from its reflection");
        EXPECT(Has(draw, "chainNightScene=&ext::ffxiv::enginelight::ChainScene;") && Has(draw, "toneMapNightOwner=&ext::ffxiv::enginelight::ToneMapOwner;"), "the chain hooks keep the slot");
        EXPECT(Has(draw, "StageRegister(ext::ffxiv::diag::StageId::NightGrade,&ext::ffxiv::enginelight::Armed,&ext::ffxiv::enginelight::StageValues)"), "the F9 stage 13 is registered");
        EXPECT(Has(draw, "ext::SetActive(ext::F::NightGrade,wanted);"), "the seam's feature bit follows 'wanted'");
        EXPECT(Has(draw, "ReleaseLamps();"), "the module's release frees the lamp arm's blend states");
        EXPECT(Has(draw, "ext::ffxiv::engine::chainAfterNightScene(context,facts,scene)"), "every null of the chained role hands the stage after the incoming scene");
        EXPECT(Count(draw, "state.session.Armed()") >= 3, "the exposure stage's own stand-down guards the stage entries (and only them)");
    }

    // ---- the keys: code, binder, INI, schema, card ---------------------------------------------------------------------------------------------
    {
        const std::string ini = Raw(repo, "OptiScaler/ext/ini/FFXIV.ini");
        const std::string schema = Raw(repo, "tools/settings-schema.json");
        const char* names[] { "EngineLight",          "EngineLightReferenceAB",   "EngineLightMoon",        "EngineLightMoonTintR",     "EngineLightMoonTintG",     "EngineLightMoonTintB",
                              "EngineLightAmbient",   "EngineLightAmbientTintR",  "EngineLightAmbientTintG", "EngineLightAmbientTintB", "EngineLightWaterMoon",     "EngineLightWaterAmbient",
                              "EngineLightWaterTintR", "EngineLightWaterTintG",   "EngineLightWaterTintB",  "EngineLightLamps",         "EngineLightExposure",      "EngineLightExposureManual",
                              "EngineLightForceNightWeight" };
        for (const char* name : names)
        {
            std::string member = name;
            member[0] = char(std::tolower(static_cast<unsigned char>(member[0])));
            EXPECT(Has(ffxivConfig, "CustomOptional") && Has(ffxivConfig, member + "{"), "FfxivConfig.h declares %s", member.c_str());
            EXPECT(Has(extConfig, std::string("b.Add(\"FFXIV\",\"") + name + "\"," + member + ");"), "the binder reads %s", name);
            EXPECT(Has(ini, std::string("\n") + name + "=auto"), "the INI fragment has %s", name);
            EXPECT(Has(schema, std::string("\"key\": \"[FFXIV] ") + name + "\""), "the schema has %s", name);
            if (std::string(name) != "EngineLight")
                EXPECT(Has(config, "cfg." + member + ".value_or_default()"), "ParamsFromConfig reads %s", name);
        }
        EXPECT(Has(config, "cfg.engineLight.value_or_default()"), "ParamsFromConfig reads the master");
        EXPECT(Has(ffxivConfig, "engineLightOnLoaded(engineLight.value_or_default())"), "the config load arms the module through the hook");
        // the retired luminance grade
        for (const char* gone : { "nightGrade", "nightDarkness", "nightKnee", "nightTint", "NightGradeRules", "nightgrade::" })
        {
            EXPECT(!Has(ffxivConfig, gone) && !Has(extConfig, gone) && !Has(enginePass, gone) && !Has(draw, gone), "the retired grade's %s is gone", gone);
        }
        for (const char* key : { "NightGrade", "NightDarkness", "NightKnee", "NightTint" })
        {
            EXPECT(!Has(ini, std::string("\n") + key + "="), "%s is no longer an INI key", key);
            EXPECT(Has(schema, std::string("\"key\": \"[FFXIV] ") + key + "\",\n      \"note\"") || Has(schema, std::string("\"key\": \"[FFXIV] ") + key + "\",\r\n      \"note\""),
                   "%s sits in the schema's retired list", key);
        }
        EXPECT(ReadFile(repo + "/OptiScaler/ext/ffxiv/engine/NightGradeDraw.cpp").empty() && ReadFile(repo + "/OptiScaler/ext/ffxiv/engine/shaders/NightGrade.hlsl").empty(),
               "the retired grade's module and shader files are gone");
        // the card
        const std::string layout = Code(repo, "core/MenuLayout.h");
        const std::string manifest = Code(repo, "core/MenuManifest.h");
        EXPECT(Has(layout, "{\"ffxiv_engine_light\",\"FFXIV\",\"ffxiv_gi\"},") && Has(layout, "{\"ffxiv_atmosphere\",\"FFXIV\",\"ffxiv_engine_light\"},"), "the card sits where the grade's card sat");
        EXPECT(Has(manifest, "\"ffxiv_engine_light\"") && Has(manifest, "\"[FFXIV]EngineLightForceNightWeight\""), "the manifest knows the card and the ini-only key");
        EXPECT(Has(menu, "{\"ffxiv_engine_light\",\"FFXIV\",\"Enginelight(FFXIV)\",Draw,\"ffxiv_engine_passes\",Visible}"), "the card registers under its id and title");
        EXPECT(Has(menu, "REPLACEStheMoonslider") || Has(menu, "REPLACES"), "the Water moon tooltip says it replaces the Moon slider (no stacking)");
        EXPECT(Has(menu, "Nightambientlift(Shadowmapping)isoverriddenwhileEnginelightison"), "the card says that Night ambient lift is overridden while Engine light is on");
        EXPECT(Ordered(menu, "if(changed)Publish();", "CurrentStatus()"), "a change publishes before the state line is built");
    }

    // ---- the rules and the contract agree --------------------------------------------------------------------------------------------------------
    {
        EXPECT(Has(rules, "#include\"ext/ffxiv/enginelight/EngineLightContract.h\""), "the rules build on the shared contract");
        EXPECT(Has(rules, "kMoonMin=0.05f,kMoonMax=1.0f") && Has(rules, "kLampMin=1.0f,kLampMax=5.0f") && Has(rules, "kManualMin=0.25f,kManualMax=4.0f") && Has(rules, "kTintMin=0.5f,kTintMax=1.5f"), "the ranges are the contract's");
        EXPECT(Has(rules, "constexprfloatHoldGain(floatsHold,floatsNow)noexcept") && Has(rules, "constfloatg=sHold/sNow;"), "the compensation is s_hold / s_now");
        EXPECT(!Has(rules, "sNow/sHold"), "... and nowhere the inverse");
        EXPECT(Has(rules, "kYFactor=2.04f"), "the fingerprint's factor is the captures'");
    }

    if (g_failures != 0)
    {
        std::printf("engine_light_pins: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("engine_light_pins: all pins hold\n");
    return 0;
}
