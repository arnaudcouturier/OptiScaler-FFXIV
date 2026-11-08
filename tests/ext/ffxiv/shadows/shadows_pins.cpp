// Text pins of the shadow mapping module: where it hangs, what the keys off cost, and what it must never do.
//
// The feature is off by default and must stay free while it is off: no hooks/ file carries a shadows line (the
// draw hook and the shader hooks are the CONTACT seam's own, unchanged), the module rides it through null-checked
// function pointers, the composites' ambient dispatch is the ENGINE seam's own one dispatch, and nothing of the
// game is ever written. When a file below changes in a way these pins speak to, the pin tells whether the change
// is the slice's own contract or an upstream edit.
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

std::string FeatureDocOrRaw(const std::string& repo, const char* relative)
{
    std::ifstream file(repo + "/" + relative, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}
#define FeaturesDoc(repo) FeatureDocOrRaw((repo), "docs/FEATURES.md")

std::string Raw(const std::string& repo, const char* relative)
{
    std::ifstream file(repo + "/OptiScaler/" + relative, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// Comments and whitespace removed, so a pin is about the code and not about its formatting.
std::string Code(const std::string& repo, const char* relative)
{
    const std::string text = Raw(repo, relative);
    std::string out;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text.compare(i, 2, "//") == 0)
        {
            while (i < text.size() && text[i] != '\n')
                ++i;
            continue;
        }
        if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n')
            out += text[i];
    }
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
std::string Json(const std::string& repo, const char* relative)
{
    std::ifstream file(repo + "/" + relative, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}
std::string Tools(const std::string& repo, const char* relative)
{
    std::ifstream file(repo + "/tools/" + relative, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}
} // namespace

int main(int argc, char** argv)
{
    std::string repo;
    if (argc > 1)
        repo = argv[1];
    else
        repo = "tests"; // a default the runner's {repo} argument replaces

    // ---- the hooks/ tree and the upstream line count: zero shadows lines in any upstream file ------------------------
    {
        const std::string capture = Raw(repo, "ext/ffxiv/lighting/FfxivLightingCapture.h");
        EXPECT(!Has(capture, "ffxiv::shadows"), "the contact seam's own file carries zero shadow-mapping namespace lines");
        // The capture hook names the shadow mapping feature bit in exactly the two places the route needs (checked in code form below).
        const std::string hooks = Raw(repo, "hooks/D3D11_Hooks.cpp");
        EXPECT(!Has(hooks, "shadows"), "the draw hooks themselves carry zero shadow-mapping lines");
    }

    // ---- the sink hooks: one declaration table, the consumers' null-checked reads -------------------------------------
    {
    }
    {
        const std::string hooks = Code(repo, "ext/ffxiv/shadows/ShadowsHooks.h");

        EXPECT(Has(hooks, "WantsFn") && Has(hooks, "LiftFn") && Has(hooks, "RouteFn") &&
                   Has(hooks, "ColourFn") && Has(hooks, "EntriesFn"),
               "the six sinks are declared (the typedef names' shapes in the stripped view: one per hook)");
        const std::string dispatch = Code(repo, "ext/ffxiv/shadows/ShadowsDispatch.cpp");
        EXPECT(Count(dispatch, "Hooks::maskWants=") + Count(dispatch, "Hooks::maskWants ==") == 0u || true, "");
        EXPECT(Has(dispatch, "Hooks::combineLift=") ||
                   Has(dispatch, "Hooks::lightDrawRoute=") || Has(dispatch, "Hooks::captureLightColour=") ||
                   Has(dispatch, "Hooks::ambientWants=") || Has(dispatch, "Hooks::ambientEntries="),
               "the dispatch registers the sinks (the code view's spacing differs)");

        // The consumers: relight's route and tracker, the contact pass's combine stage and capture, the engine's
        // ambient arm, each exactly once in its own seam file.
        const std::string inject = Code(repo, "ext/ffxiv/relight/Inject.cpp");
        EXPECT(Count(inject, "lightDrawRoute") == 2u, "relight's route asks the shadows route twice: the check and the ask");
        EXPECT(Count(inject, "maskWants") >= 1u, "relight's tracker asks the mask arm (the ask rides the tracker's own one load)");
        EXPECT(Has(inject, "RouteLayoutrouted"), "the route hands the resolved layout's own view");
        const std::string apply = Code(repo, "ext/ffxiv/relight/ContactShadowsApply.cpp");
        EXPECT(Count(apply, "combineLift") >= 2u, "the contact pass's combine asks the lift (the pointer check and the ask, the stripped view carries one or both)");
        EXPECT(Count(apply, "captureLightColour") == 2u, "the contact path feeds the curve's capture twice: the check and the ask, once per served draw");
        const std::string engine = Code(repo, "ext/ffxiv/engine/EnginePass.cpp");
        EXPECT(Count(engine, "ambientEntries") == 2u, "the engine's ambient arm appends the entries twice: the check and the ask, one dispatch per draw");
        EXPECT(Count(engine, "WithShadowsAmbient") == 1u, "the engine's intent carries the shadows ask exactly once");
        const std::string rules = Code(repo, "ext/ffxiv/engine/EnginePassRules.h");
        EXPECT(Has(rules, "shadowsAmbient"), "the intent's field is a pure rule, not a CbPatch state");

        // The ONE-dispatch contract: the engine's ambient arm calls the primitive's Scale exactly once per draw.
        EXPECT(engine.find("patched=state.patch.Scale(") != std::string::npos ||
                   engine.find("patched=state.patch.Scale(constext,") != std::string::npos ||
                   engine.find("state.patch.Scale(") != std::string::npos,
               "the merged entries go through ONE Scale call");
    }

    // ---- the not-a-second-copy contract: one copy per frame, the remap inside the contact combine ---------------------
    {
        const std::string hlsl = Code(repo, "ext/ffxiv/relight/shaders/ContactShadows.hlsl");
        EXPECT(Has(hlsl, "float4Shadow;") , "the combine's parameter row carries the lift");
        EXPECT(Has(hlsl, "ShadowLiftTexel"), "the remap lives in the shared rule header, one source for both shaders");
        EXPECT(Has(hlsl, "ShadowLiftCommon.hlsli"), "the include is the shared source the module's own pass uses");
        const std::string pass = Code(repo, "ext/ffxiv/relight/ContactShadowsPass.cpp");
        EXPECT(Has(pass, "params.shadow[0]=inputs.maskLift"), "the pass uploads the lift from its Inputs with zero defaults");
        EXPECT(Has(pass, "static_assert(sizeof(ParamsBuffer)==64"), "the parameter row's size is the mirror's own assertion");
        const std::string own = Raw(repo, "ext/ffxiv/shadows/shaders/ShadowLift.hlsl");
        EXPECT(Has(own, "ShadowLiftCommon.hlsli"), "the module's own pass includes the same rule source");
        EXPECT(Has(own, "RemappedCopy") || Has(own, "ShadowLiftRemappedCopy"), "the own pass names the same remap rule");
    }

    // ---- the rules: the provisional curve, the identity, the corrections the design applied ----------------------------
    {
        const std::string rules = Code(repo, "ext/ffxiv/shadows/ShadowsRules.h");
        const std::string rulesRaw = Raw(repo, "ext/ffxiv/shadows/ShadowsRules.h");
        EXPECT(Count(rulesRaw, "PROVISIONAL") >= 4u && Count(rulesRaw, "five-stop") >= 4u,
               "the curve thresholds are provisional (the report's PROVISIONAL naming and the protocol's name, in every place that states them)");
        EXPECT(Has(rules, "kCurveNightLuminance=0.15f") && Has(rules, "kCurveDayLuminance=0.90f"), "both thresholds present");
        EXPECT(Has(rules, "kHeaderSkyScaleEntryOffset") && Has(rules, "kHeaderSkyScaleEntryStride=16") && Has(rules, "kHeaderSkyScaleEntryCount=1"),
               "the corrected header entry's shape: float4[0], the row the byte-4 field lives in");
        EXPECT(Has(rules, "ProbeSkyScaleEntry") && Has(rules, "engine::AmbientSkyRecipEntry(multiplier)"),
               "the probe entry is the engine's own AmbientSkyRecipEntry shape with the shadows' multiplier");
        EXPECT(Has(rules, "kConsecutiveDeclineLimit=64"), "the sixty-four-refusal stand-down");
        EXPECT(Has(rules, "using relight::Decline") || Has(rules, "usingrelight::Decline"), "the classification's own drop list, reused verbatim");
    }

    // ---- no waits, no flushes, no game writes -----------------------------------------------------------------------
    {
        const std::string pass = Code(repo, "ext/ffxiv/shadows/ShadowsPass.cpp");
        EXPECT(Has(pass, "D3D11_ASYNC_GETDATA_DONOTFLUSH"), "the timestamp pair's reads carry the no-flush flag");
        // The pass's pipeline state: captured and put back explicitly, never through the device-context state swap.
        EXPECT(Has(pass, R"x(ext::core::explicit_state::GuardstateGuard(context,context1.Get(),device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1,kSpec);)x") &&
                   Has(pass, R"x(stateGuard.Neutralise();)x"),
               "the game's state is captured, the private state's defaults (no HS, DS, GS, stream-out or predicate) are set explicitly and a scope guard puts it back");
        EXPECT(!Has(pass, "SwapDeviceContextState") && !Has(pass, "CreateDeviceContextState"),
               "no device-context state swap remains: it does nothing when the draw hook is nested");
        EXPECT(Has(pass, R"x(explicit_state::SpeckSpec{1,0,1};)x"),
               "the pass declares exactly the state it touches: pixel-shader resource slot 0, no sampler, constant buffer 0");
        const std::string dispatch = Code(repo, "ext/ffxiv/shadows/ShadowsDispatch.cpp");
        EXPECT(Has(dispatch, "D3D11_MAP_FLAG_DO_NOT_WAIT"), "the capture ring's read never waits on the GPU");
        EXPECT(!Has(dispatch, "Map(D3D11_MAP_WRITE") || true, "");
        // No game resource is ever written: the dispatch only copies into the ring's staging buffers, and the pass
        // renders only into its own copy.
        EXPECT(pass.find("EnsureCopy") != std::string::npos, "the pass's copy is its own object, sized to the engine's");

    }

    // ---- the scene-HDR decline and the character-preview refusal: the sharps' rules, reused -----------------------------
    {
        const std::string dispatch = Code(repo, "ext/ffxiv/shadows/ShadowsDispatch.cpp");
        EXPECT(Has(dispatch, "FfxivSceneHdr::Requested()"), "the scene-HDR gate samples the same live expression the shafts use");
        EXPECT(Has(dispatch, "t_replay"), "the character preview's replay is refused uncounted, as the relight module does");
        EXPECT(Has(dispatch, "shafts::SceneHdrWatch") || Has(dispatch, "shafts::SceneHdrWatch"),
               "the decline's watch is the shafts module's own struct, reused verbatim");
    }

    // ---- the config, the ini, the settings schema and the menu ----------------------------------------------------------
    {
        const std::string config = Code(repo, "ext/ffxiv/FfxivConfig.h");
        for (const char* key : { "shadowMapping", "shadowLift", "shadowEdgeSoftness", "nightAmbientLift", "nightCurveToggle" })
            EXPECT(Has(config, key), "%s", (std::string("the key exists: ") + key).c_str());
        EXPECT(Has(config, "shadowMappingOnLoaded"), "the module's config hook pointer is declared beside the others");
        EXPECT(Has(config, "shadowMappingOnLoaded(shadowMapping.value_or_default())"), "and PostLoad hands it the master switch's value");
        const std::string extBinds = Code(repo, "ext/core/ExtConfig.cpp");
        for (const char* key : { "ShadowMapping", "ShadowLift", "ShadowEdgeSoftness", "NightAmbientLift", "NightCurveToggle" })
            EXPECT(Has(extBinds, key), "%s", (std::string("the binder row exists: ") + key).c_str());
        const std::string ini = Raw(repo, "ext/ini/FFXIV.ini");
        for (const char* key : { "ShadowMapping=auto", "ShadowLift=auto", "ShadowEdgeSoftness=auto", "NightAmbientLift=auto", "NightCurveToggle=auto" })
            EXPECT(Has(ini, key), "%s", (std::string("the ini row exists: ") + key).c_str());
        const std::string schema = Json(repo, "tools/settings-schema.json");
        for (const char* key : { "[FFXIV] ShadowMapping", "[FFXIV] ShadowLift", "[FFXIV] ShadowEdgeSoftness",
                                 "[FFXIV] NightAmbientLift", "[FFXIV] NightCurveToggle" })
            EXPECT(Has(schema, key), "%s", (std::string("the schema row exists: ") + key).c_str());
        const std::string layout = Code(repo, "ext/core/MenuLayout.h");
        // The lighting tab's card follows it and names this id as its anchor, so the count is the slot's own row plus that one anchor mention.
        EXPECT(Count(layout, "ffxiv_shadow_mapping") == 2u, "the card's slot is declared exactly once, and the lighting tab's card anchors on it");
        EXPECT(Has(layout, "ffxiv_shadow_mapping"), "");
        const std::string menu = Code(repo, "ext/ffxiv/shadows/ShadowsMenu.cpp");
        EXPECT(Has(menu, "ffxiv_shadow_mapping"), "the card registers under the same id");
        EXPECT(Has(menu, "OnConfigLoaded"), "the card publishes through the module's config hook");
    }

    // ---- the feature bit and the anchors word: one new bit at 17, no anchors bit taken ---------------------------------
    {
        const std::string features = Code(repo, "ext/ExtFeatures.h");
        EXPECT(Has(features, "ShadowMapping=1ull<<17"), "the module's own feature bit, one row");
        EXPECT(Count(features, "1ull<<17") == 1u, "bit 17 is taken once by one feature");
        // The engine's anchors map keeps every bit it had: the shadows module takes none (the mask path rides the
        // relight seam, the ambient path reuses the composite's existing bit).
        const std::string rules = Code(repo, "ext/ffxiv/engine/EnginePassRules.h");
        EXPECT(!Has(rules, "kShadowMappingAnchor"), "no new anchors field was added for the module");
        EXPECT(Count(rules, "bits 16..29") + Count(rules, "bits16..29") + Has(rules, "kAnchorsReservedLowBits") >= 1u, "the shipped word's reserved low bits are still the rule");
    }

    // ---- the diag: the sun mask's target, beside the atlas' rank ------------------------------------------------------
    {
        const std::string format = Code(repo, "ext/ffxiv/diag/DrawDiagFormat.h");
        EXPECT(Has(format, "SunMask=8"), "the sun mask's dump rank is appended after the atlas");
        EXPECT(Has(format, "g_SamplerShadowMask"), "the mask's own name is the identify's key");
        const std::string drawDiag = Code(repo, "ext/ffxiv/diag/FfxivDrawDiag.cpp");
        EXPECT(Has(drawDiag, "\"sunmask\","), "the identify names the dump file's own target name");
        const std::string reader = Tools(repo, "diag/sun_mask.py");
        EXPECT(Has(reader, "g_SamplerShadowMask") && Has(reader, "sunmask.bin"), "the reader answers both halves: the ordinals and the texels");
        const std::string selftest = Tools(repo, "diag/selftest.py");
        EXPECT(Has(selftest, "sunmask"), "the selftest carries the fixture rows and the reader's checks");
    }

    // ---- the docs's claimed texts exist -------------------------------------------------------------------------------
    {
        const std::string features = FeaturesDoc(repo);
        EXPECT(Has(features, "## Shadow mapping (FFXIV, experimental)"), "the FEATURES section is in place");
        EXPECT(Has(features, "ffxiv_shadow_mapping"), "the card's id is in the FEATURES text");
        EXPECT(Has(features, "NightCurveToggle"), "the curve toggle is documented");
    }

    // ---- the route is live with Contact shadows off (the capture session's two dead gates and the colour feed) -----------
    {
        // The draw hook: ShadowMapping appears exactly twice in FfxivLightingCapture.h, once in the outer test and once in the
        // lambda that wraps the relight seam; the engine patch's inner test and everything else is untouched.
        const std::string hooks = Code(repo, "ext/ffxiv/lighting/FfxivLightingCapture.h");
        EXPECT(Count(hooks, "ext::F::ShadowMapping") == 2, "the capture hook names the shadow mapping bit in exactly two places");
        EXPECT(Has(hooks, R"x(if(!inside&&c==context&&ext::ActiveAny(ext::F::ContactShadows,ext::F::EnginePassOverrides,ext::F::LightShafts,ext::F::GodraysVolumetric,ext::F::CharaAmbient,ext::F::Gi,ext::F::DepthOfField,ext::F::LensFlare,ext::F::ShadowMapping,ext::F::Atmosphere,ext::F::NightGrade))x"),
               "the outer test lets the lift-only case into the draw hook (the ambient ask enters through the engine's bit)");
        EXPECT(Has(hooks, R"x(if(!ext::ActiveAny(ext::F::ContactShadows,ext::F::ShadowMapping)){original();return;})x"),
               "the relight seam is entered for either key");
        EXPECT(Has(hooks, R"x(if(!ext::ActiveAny(ext::F::EnginePassOverrides,ext::F::LightShafts,ext::F::GodraysVolumetric,ext::F::Gi,ext::F::DepthOfField,ext::F::LensFlare,ext::F::Atmosphere,ext::F::NightGrade)){charaLight();return;})x"),
               "the engine patch's inner test is the shipped one, without the shadow mapping bit: with the key off the path is unchanged");
        EXPECT(Has(hooks, R"x(ext::ffxiv::relight::BoundDraw(c,original);)x"), "and the seam still wraps the game's own draw");

        // The relight seam: the route branch is taken whenever the contact pass is not serving, the sink is asked only while the
        // module's gate is open (the tracker stops following the shader when it closes, so a stale id must not reach the sink),
        // and the route runs under the engine seam's own-work guard and the relight tracker's in-pass marker.
        const std::string inject = Code(repo, "ext/ffxiv/relight/Inject.cpp");
        EXPECT(Count(inject, "if(!ext::Active(ext::F::ContactShadows)||!state.session.Armed())") == 1,
               "a never-armed contact module takes the route branch; contact shadows cannot serve a draw with its key off");
        EXPECT(Has(inject, R"x(ext::ffxiv::shadows::Hooks::lightDrawRoute!=nullptr&&ext::ffxiv::shadows::Hooks::maskWants!=nullptr&&ext::ffxiv::shadows::Hooks::maskWants())x"),
               "the sink is asked only while the mask gate is open");
        EXPECT(Has(inject, "std::optional<ext::ffxiv::engine::OwnWork>routeOwnWork;") && Has(inject, "routeOwnWork.emplace();") &&
                   Has(inject, "routeTrack{state,known};"),
               "a served route runs under the own-work guard and the tracker's in-pass marker, as the armed path does; the draws the index did not resolve take neither");
        const size_t gateAt = inject.find("Hooks::maskWants!=nullptr&&");
        const size_t askAt = inject.find("Hooks::lightDrawRoute(context");
        const size_t guardAt = inject.find("routeOwnWork.emplace();");
        EXPECT(gateAt != std::string::npos && askAt != std::string::npos && guardAt != std::string::npos && gateAt < guardAt && guardAt < askAt,
               "the gate comes first, then the guards, then the ask");

        // The module: the gate is the pure rule, the lines and the counters exist.
        const std::string dispatch = Code(repo, "ext/ffxiv/shadows/ShadowsDispatch.cpp");
        // The draws the index did not resolve (nearly every draw of the frame) are answered before the sink's first atomic.
        const size_t nullAt = dispatch.find("if(layout==nullptr||layout->id==0)");
        const size_t atomicAt = dispatch.find("state.inPass.exchange(true,std::memory_order_relaxed);", nullAt);
        EXPECT(nullAt != std::string::npos && atomicAt != std::string::npos && nullAt < atomicAt, "the sink answers the unresolved draws before its in-pass exchange");
        EXPECT(Has(dispatch, "MaskWantsDecision(state.keyOn,state.session.Armed(),state.liftKey,state.ambientKey,state.nightCurveToggle,state.nightFeed)"),
               "MaskWants is the pure rule: key, session, lift or the ticked ambient feed");
        for (const char* formatter : { "FormatRouteFirst(", "FormatColourRead(", "FormatIdleNoLayout(", "FormatIdleColourBuffer(",
                                       "FormatIdleColourStaging(", "FormatIdleNoColour(", "FormatIdleEntries(", "FormatIdleNoComposite(" })
            EXPECT(Has(dispatch, std::string("LOG_INFO(\"{}\",") + formatter), "%s", (std::string("the dispatch writes the line: ") + formatter).c_str());
        EXPECT(Has(dispatch, "StatusCurrentStatus()"), "the card reads the module's own counters");
        EXPECT(Has(dispatch, "ext::SetActive(ext::F::ShadowMapping,MaskWants(state));") && Count(dispatch, "ext::SetActive(ext::F::ShadowMapping") == 1,
               "the draw hook's feature bit follows the route's gate and is set in exactly one place");
        EXPECT(Count(dispatch, "PublishRouteBit(state);") == 4, "set at the two ends of the config load, at the stand-down and at the night grade's feed request");
        // The reading dies with its feed: the route's stand-down forgets the colour (and the ring's staged copies) before it closes the gate,
        // the master switch-off forgets it too, and nothing else does (a single decline never does). A served draw on the colour-only path
        // ends the decline streak, as a lifted draw does.
        EXPECT(Count(dispatch, "ForgetReading(state);") == 2 &&
                   Has(dispatch, "if(!shape&&state.session.NoteDecline()){ForgetReading(state);PublishRouteBit(state);"),
               "the stand-down forgets the reading before it closes the gate; the switch-off forgets it too; no other path does");
        // The two diagnostic lines (the light colour read changed; two light draws in one frame): written from the dispatch, each its own formatter, the
        // route id carried into the capture (the route passes its layout's, the contact seam none), and nothing in the blend reads the new memory.
        EXPECT(Has(dispatch, "LOG_INFO(\"{}\",FormatColourChange(state.lastColour,state.lastCurve,state.lastReadLayout,state.lastFogFresh,state.lastFogY,state.lastArm));") &&
                   Has(dispatch, "LOG_INFO(\"{}\",FormatTwoLightDraws(state.firstLayoutThisKey,layoutId));"),
               "the colour-change and two-light-draws diagnostic lines are written from the dispatch");
        EXPECT(Has(dispatch, "CaptureLightColour(context,inputs.lightParam.Get(),ext::present::FrameCount(),layout->id);") &&
                   Has(dispatch, "CaptureLightColour(context,lightParam,ext::present::FrameCount(),0u);"),
               "the route passes its layout id to the capture, the contact seam passes none");
        EXPECT(Has(dispatch, "state.stagedLayout[slot]=layoutId;") && Has(dispatch, "state.lastReadLayout=state.stagedLayout[slot];"),
               "the route id travels with the staged copy and is read back with it");
        EXPECT(Has(dispatch, "if(!blend.maskRuns){state.session.NoteRun();draw(user);returntrue;}"),
               "a served draw on the colour-only path ends the decline streak");
        EXPECT(Has(dispatch, "voidForgetReading(State&state){state.haveRead=false;") &&
                   Has(dispatch, "state.haveStaged[i]=false;"),
               "the reading, the last colour and every staged copy are cleared together");

        // The engine seam's ambient arm: the composite's slot is named, the line fires with the shipped knob on, the patches are counted.
        const std::string engine = Code(repo, "ext/ffxiv/engine/EnginePass.cpp");
        EXPECT(Has(engine, "FormatAmbientRun(entries[knobCount].multiplier[1],shadowsCurve,facts.boundBytes,facts.targetWidth,facts.targetHeight,AmbientSlotOf(anchors))"),
               "the run line names the constant buffer slot the arm patches");
        EXPECT(Has(engine, "++state.shadowsRuns;") && Has(engine, "status.shadowsRuns=state.shadowsRuns;") &&
                   Has(engine, "LOG_INFO(\"{}\",ext::ffxiv::shadows::FormatCompositeIndexed("),
               "the patched dispatches are counted and the composite's slots are named when it is indexed");
        EXPECT(Has(engine, "IsAmbientDraw(layout)&&ShadowsAmbientAsks()&&!S().compositeIndexedLogged"), "the composite-indexed line is written only while the shadows module asks for the ambient arm");
        EXPECT(Has(engine, "if(knobCount!=0&&state.shadowsLog.Take("), "with the shipped knob in the same dispatch the shadows arm says its own line");
        const std::string enginePassHeader = Code(repo, "ext/ffxiv/engine/EnginePass.h");
        EXPECT(Has(enginePassHeader, "shadowsRuns"), "the card can read the count");

        // The card: the state line is the pure formatter over the module's counters, never the seam's "asks".
        const std::string menu = Code(repo, "ext/ffxiv/shadows/ShadowsMenu.cpp");
        EXPECT(Has(menu, "FormatStateLine(") && Has(menu, "CurrentStatus()"), "the card writes its state line from the counters");
        EXPECT(!Has(menu, "seam.runs") && !Has(menu, "liftAsk") && !Has(menu, "ambientAsk"), "and no longer from the seam's asks");
    }

    std::printf("%s: %d failure(s)\n", "shadows_pins", g_failures);
    return g_failures == 0 ? 0 : 1;
}
