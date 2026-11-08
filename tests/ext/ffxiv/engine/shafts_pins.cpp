// Text pins of the light shaft module: where it hangs, what it must never do, and that its keys default to today's picture.
//
// The feature is off by default and must stay free while it is off: the seam already loads one feature word per draw, and the module's entry
// returns at one test of the stored key and the intensity. The pass reads the engine's scene and the engine's depth, copies the scene into its
// own texture, binds that copy for exactly one draw and puts the game's binding back - also when the draw throws. Nothing of the game is
// written and no address or shader CRC identifies a draw.
#include <cstdio>
#include <cstdlib>
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
    std::string text = Comments(Raw(repo, relative));
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
        std::printf("usage: shafts_pins <repo>\n");
        return 2;
    }
    const std::string repo = argv[1];
    const std::string draw = Code(repo, "ffxiv/engine/ShaftsDraw.cpp");
    const std::string pass = Code(repo, "ffxiv/engine/ShaftsPass.cpp");
    const std::string passHeader = Code(repo, "ffxiv/engine/ShaftsPass.h");
    const std::string rules = Code(repo, "ffxiv/engine/ShaftsRules.h");
    const std::string sun = Code(repo, "ffxiv/engine/ShaftsSun.cpp");
    const std::string sunHeader = Code(repo, "ffxiv/engine/ShaftsSun.h");
    const std::string menu = Code(repo, "ffxiv/engine/ShaftsMenu.cpp");
    const std::string shader = Raw(repo, "ffxiv/engine/shaders/LightShafts.hlsl");
    const std::string enginePass = Code(repo, "ffxiv/engine/EnginePass.cpp");
    const std::string enginePassRules = Code(repo, "ffxiv/engine/EnginePassRules.h");
    const std::string config = Raw(repo, "ffxiv/FfxivConfig.h");
    const std::string binder = Code(repo, "core/ExtConfig.cpp");
    const std::string ini = Raw(repo, "ini/FFXIV.ini");
    const std::string layout = Code(repo, "core/MenuLayout.h");
    const std::string features = Code(repo, "ExtFeatures.h");
    const std::string seam = Code(repo, "ffxiv/lighting/FfxivLightingCapture.h");

    // ---- the one call in the draw path: the module sees a light draw, the composite and the tone map, and the two knobs' anchors check stays
    //      the engine's own
    {
        EXPECT(Count(enginePass, "ext::ffxiv::shafts::OnEngineDraw(") == 1, "the module is called from exactly one place");
        EXPECT(Has(enginePass, R"x(if(ext::ffxiv::shafts::OnEngineDraw(context,anchors,draw,user))return;)x"),
               "and that call decides whether it drew the game's call itself");
        EXPECT(Has(enginePass, R"x(#include"ext/ffxiv/engine/Shafts.h")x"), "the module's header is included by the draw path");
        EXPECT(Has(enginePass, R"x(if(!ext::ActiveAny(ext::F::EnginePassOverrides,ext::F::LightShafts,ext::F::GodraysVolumetric,ext::F::Gi,ext::F::DepthOfField,ext::F::LensFlare,ext::F::Atmosphere,ext::F::NightGrade))x"),
               "the pixel-shader seam tracks the anchors while either key is on (the light bounce's key joined the list), so the shafts' draws are classified too");
        EXPECT(Has(features, R"x(LightShafts=1ull<<16,)x"), "the feature bit");
        // The lighting tab's chara ambient and the light bounce joined the same one-load test (the fifth and sixth draw-path features), and the lens flare's key the seventh, so the needle names seven.
        EXPECT(Has(seam, R"x(ext::ActiveAny(ext::F::ContactShadows,ext::F::EnginePassOverrides,ext::F::LightShafts,ext::F::GodraysVolumetric,ext::F::CharaAmbient,ext::F::Gi,ext::F::DepthOfField,ext::F::LensFlare,ext::F::ShadowMapping,ext::F::Atmosphere,ext::F::NightGrade))x"),
               "the seam's one feature test covers the shafts, so their key alone opens the draw path");
        EXPECT(Has(enginePass, R"x(if(context==nullptr||!DrawPathWanted(state.session.Armed(),ao::Armed(),gloss::Armed(),ext::ffxiv::shafts::Armed(),ssr::Armed(),ShadowsAmbientAsks(),ext::ffxiv::godrays::Armed(),ext::ffxiv::engine::gi::Armed(),ext::ffxiv::enginelight::Armed(),ext::ffxiv::engine::atmo::Armed(),ext::ffxiv::dof::Armed(),flare::Armed()))x"),
               "the draw path opens for the shafts' own arming too: the engine's streak stood the two knobs down, not the shafts");
        EXPECT(Has(enginePassRules, R"x(template<typename...Features>constexprboolDrawPathWanted(boolsessionArmed,Features...featuresArmed)noexcept{returnsessionArmed||AnyFeatureArmed(featuresArmed...);})x"),
               "and that gate is one pure rule over every feature the seam carries, so what opens the draw path is testable without a device");
        EXPECT(Has(enginePass, R"x(constKnobIntentintent=WithShadowsAmbient(IntentOfSession(state.params,anchors,state.session.Armed(),state.keyOn.load(std::memory_order_relaxed)),anchors,ShadowsAmbientAsks());)x"),
               "a session that stood down, or a master key that is off, patches no knob: the shafts are the only thing the draw path still does for it");
        EXPECT(Has(enginePass, R"x(if(!state.session.Armed())SetArmed(AnyFeatureArmed(ao::Armed(),gloss::Armed(),ext::ffxiv::shafts::Armed(),ssr::Armed(),ShadowsAmbientAsks(),ext::ffxiv::godrays::Armed(),ext::ffxiv::engine::gi::Armed(),ext::ffxiv::enginelight::Armed(),ext::ffxiv::engine::atmo::Armed(),ext::ffxiv::dof::Armed(),flare::Armed()));)x"),
               "this module's feature bit goes off only when every feature has stood down: while the shafts want draws it stays on");
        EXPECT(Has(enginePass, R"x(ext::ffxiv::shafts::Release();)x"),
               "the engine's teardown releases the shafts' textures and read-back ring while the device is still valid");
        EXPECT(Has(draw, R"x(state.pass.Reset();state.sun.Reset();state.tone.Reset();state.sunUv.Reset();state.sunDraw=SunDraw{};state.sceneResource=nullptr;)x"),
               "that release drops the pass's textures, all three read-back rings and their ages, the frame's sun draw and the scene it pointed at");
    }

    // ---- the recognisers: three draws, all by name plus shape, and no CRC or address anywhere
    {
        EXPECT(Has(enginePassRules, R"x({"g_LightParam",NameKind::ConstantBuffer},)x"), "the light draw's own name");
        EXPECT(Has(enginePassRules, R"x(inlineconstexprAnchorskShaftsLightBit=1ull<<41;)x") &&
                   Has(enginePassRules, R"x(inlineconstexprAnchorskShaftsCompositeBit=1ull<<42;)x") &&
                   Has(enginePassRules, R"x(inlineconstexprAnchorskShaftsToneMapBit=1ull<<43;)x"),
               "the three recognitions' bits are the shared map's");
        EXPECT(Has(enginePassRules, R"x(inlineconstexpruint32_tkShaftsSlotShift=44;)x") &&
                   Has(enginePassRules, R"x(inlineconstexprAnchorskShaftsSlotMask=0xFFull<<44;)x"),
               "and the light parameter's slot field, above the shipped knobs' bits");
        EXPECT(!Has(enginePassRules, "kLightDrawBit") && !Has(rules, "kLightDrawBit"),
               "the module keeps no private copy of the map's bits: a renumber is one edit in the map");
        EXPECT(Has(rules, R"x(static_assert(uint32_t(engine::Knob::LightShafts)==2,)x"),
               "the refusal knob this module claims is asserted where a renumber is one line");
        EXPECT(Has(enginePassRules, R"x(if(IsLightDraw(layout))anchors|=kShaftsLightBit|((Anchors(layout.SlotOf(6))<<kShaftsSlotShift)&kShaftsSlotMask);)x"),
               "the light draw's anchors are packed where the draw path reads them");
        EXPECT(Has(enginePassRules, R"x(if(IsAmbientDraw(layout))anchors|=kShaftsCompositeBit;)x") &&
                   Has(enginePassRules, R"x(if(IsToneMapDraw(layout))anchors|=kShaftsToneMapBit;)x"),
               "and the composite's and the tone map's own bits");
        EXPECT(!Has(draw, "0x85E777EF") && !Has(draw, "0x1257B24B") && !Has(draw, "crc"),
               "no shader CRC and no captured CRC identifies a draw: the names and the shape do");
        EXPECT(!Has(pass, "0x85E777EF"), "nor does the pass");
        EXPECT(Has(draw, R"x(constDeclineshape=ClassifyToneMap(facts);)x"), "the tone-map shape rule decides before anything runs");
        EXPECT(Has(draw, R"x(if(ext::ffxiv::engine::IsShaftsLightAnchors(anchors)))x") &&
                   Has(draw, R"x(if(ext::ffxiv::engine::IsShaftsCompositeAnchors(anchors)))x") &&
                   Has(draw, R"x(if(ext::ffxiv::engine::IsShaftsToneMapAnchors(anchors)))x"),
               "the three draws the module hangs on, read through the map's accessors");
        EXPECT(Has(draw, R"x(boolOnEngineDraw(ID3D11DeviceContext*context,engine::Anchorsanchors,DrawFndraw,void*user))x"),
               "the draw path hands the whole 64-bit word over, so a bit above the low word still classifies the draw");
        EXPECT(Has(draw, R"x(constuint32_tlightSlot=ext::ffxiv::engine::ShaftsSlotOf(anchors);)x"),
               "the light's slot comes from the reflection, never from a constant");
    }

    // ---- the sun's draw: a positive shape (the light-buffer pair, written without blending) plus "no position" on the light itself, and the
    //      first match of a frame - never a table of candidates, and never the wetness volumes that also declare g_LightParam
    {
        EXPECT(Has(rules, R"x(constexprboolIsSunDraw(constSunDrawFacts&facts)noexcept)x") &&
                   Has(rules, R"x(facts.targets!=kLightTargets||!facts.sameTargets)x") &&
                   Has(rules, R"x(if(facts.blended)returnfalse;)x"),
               "the sun draw is recognised by its own shape: the light-buffer pair, not blended into");
        EXPECT(Has(rules, R"x(constexprboolIsSunLight(constLightFacts&facts)noexcept{returnClassifyLight(facts)==LightVerdict::Sun;})x") &&
                   Has(rules, R"x(constexprLightVerdictClassifyLight(constLightFacts&facts,floatelevation)noexcept)x") &&
                   Has(rules, R"x(if(!IsZeroPosition(facts.position))returnLightVerdict::Lamp;)x") &&
                   Has(rules, R"x(if(!IsUnitVector(facts.direction))returnLightVerdict::NoDirection;)x") &&
                   Has(rules, R"x(if(!(elevation>kMinSunElevation))returnLightVerdict::BelowHorizon;)x") &&
                   Has(rules, R"x(inlineconstexprfloatkMinSunElevation=-0.02f;)x") &&
                   Has(rules, R"x(constexprfloatWorldElevationOf(constfloatview[kViewRows],constfloatdirection[3])noexcept)x") &&
                   Has(rules, R"x(returnview[1]*direction[0]+view[5]*direction[1]+view[9]*direction[2];)x"),
               "and the light's own parameters must say a directional light: no world position, a unit direction, and a WORLD elevation above the "
               "horizon - the direction is view space, so the elevation is the view's second row dotted with it, and the bound is a tolerance about "
               "a degree below the line, because a sun 1 to 3 degrees up at dawn or dusk is a sun the player can see");
        EXPECT(!Has(rules, "BetterSun"), "the old \"first candidate wins\" rule is gone");
        EXPECT(Has(draw, R"x(if(state.sunDraw.taken&&SameRenderedFrame(state.sunDraw.frames,state.frames))return;)x"),
               "only the first sun-shaped light draw of a rendered frame is taken, compared on the rendered frame and not on the present token");
        EXPECT(Has(draw, R"x(if(!IsSunDraw(MeasureLightDraw(context,state,depth!=nullptr,camera!=nullptr)))return;)x"),
               "and it is the draw's own shape that decides, before anything is copied");
        EXPECT(Has(draw, R"x(if(static_cast<constvoid*>(light.Get())==state.rejectedLight&&RejectedStill(state.rejectedLight!=nullptr,state.rejectedFrame,state.frames))x"),
               "a lamp the read-back found is not looked at again while its buffer is still the pool's");
        EXPECT(Has(draw, R"x(constLightVerdictverdict=ClassifyLight(state.sun.Facts(),elevation);)x") &&
                   Has(draw, R"x(if(verdict!=LightVerdict::Sun||shadow.result==ShadowResult::Refused))x") && Has(draw, R"x(FormatNotTheSun(state.sun.Facts()))x") &&
                   Has(draw, R"x(if(verdict==LightVerdict::BelowHorizon))x") && Has(draw, R"x(FormatBelowHorizon(state.sun.Facts()))x") &&
                   Has(draw, R"x(state.belowHorizonFrame=state.frames;state.haveBelowHorizonFrame=true;)x"),
               "a light at a world position is refused by name once a session, and a directional light pointing below the horizon gets its own "
               "line: it is the sun's mirror, and the next sun-shaped light draw of the frame is taken instead. The refusal is remembered for the "
               "frame and counted there, not here, so the card counts that frame once");
        EXPECT(Has(draw, R"x(state.rejectedLight=state.sun.LastLight();)x") &&
                   Has(draw, R"x(state.sunDraw.taken=false;)x"),
               "a refused light's buffer is held off for a few seconds and the frame's sun draw is dropped, so the search goes on to the next "
               "sun-shaped light draw of the frame");
        EXPECT(!Has(draw, "kMaxSunCandidates") && !Has(draw, "candidate."), "the candidate table is gone");
    }

    // ---- the light has to be the frame's sun by the frame's own directional shadow, not only by its own direction: the shape rule and the horizon
    //      cannot tell the sun from another directional light, and a session drew shafts from a light that was a 48.9 degree yaw of the sun
    {
        EXPECT(Has(rules, R"x(inlineconstexpruint32_tkShadowBytes=0x80;)x") &&
                   Has(rules, R"x(inlineconstexpruint32_tkShadowRowAt=0x20;)x") &&
                   Has(rules, R"x(inlineconstexprfloatkShadowAgreementDegrees=3.0f;)x") &&
                   Has(rules, R"x(constexprboolShadowMatrixShape(constfloatbytes[kShadowFloats])noexcept)x") &&
                   Has(rules, R"x(inlinefloatShadowDegrees(constfloatrow[3],constfloatdirection[3],boolnegated=true)noexcept)x") &&
                   Has(rules, R"x(inlineShadowJudgementJudgeShadow(constShadowCandidate*candidates,uint32_tcount,constfloatdirection[3])noexcept)x"),
               "the frame's directional shadow is the identity rule: a 128-byte buffer whose third row at +0x20 is minus the light's own axis, "
               "an angle within a few degrees of the light's direction, and a matrix shape checked first so a same-size neighbour is never judged");
        EXPECT(Has(draw, R"x(ID3D11Buffer*shadows[kShadowCandidates]{};)x") &&
                   Has(draw, R"x(if(desc.ByteWidth!=kShadowBytes)continue;)x") &&
                   Has(draw, R"x(if(shadowCount<kShadowCandidates)shadows[shadowCount++]=buffer.Get();)x") &&
                   Has(draw, R"x(state.sun.Copy(context,light.Get(),camera.Get(),state.frames,shadows,shadowCount))x"),
               "the light draw's 128-byte parameter buffers are copied with the light and the camera, by size and never by a slot number");
        EXPECT(Has(draw, R"x(shadow=JudgeShadow(state.sun.Shadows(),state.sun.ShadowCount(),state.sun.Facts().direction);)x") &&
                   Has(draw, R"x(if(verdict!=LightVerdict::Sun||shadow.result==ShadowResult::Refused))x") &&
                   Has(draw, R"x(LOG_INFO("{}",FormatShadowRefused(state.sun.Facts().direction,shadow.degrees));)x") &&
                   Has(draw, R"x(state.notSunFrame=state.frames;state.haveNotSunFrame=true;)x"),
               "a light whose direction is not the frame's shadow axis is refused with its own line, its own angle and its own count, on the "
               "same path a lamp and a mirror light are refused");
        EXPECT(Has(draw, R"x(LOG_INFO("{}",kShadowAnchorMissing);)x") && Has(draw, R"x(LOG_INFO("{}",kShadowUnnegated);)x"),
               "a frame whose light draw bound no such buffer (or whose copy has not arrived) draws with the light's own direction and says so "
               "once: the effect is never silently refused for the anchor's absence, and a build whose matrix stores the other sign is named");
        EXPECT(Has(draw, R"x(sunLight,state.sunShadow.degrees,state.whitePoint))x") && Has(rules, R"x(shadow{},white{:.3f},fade{:.2f})x"),
               "the run line prints the angle the frame's own shadow agreed at, so a session's own frame says which light placed the shafts");
    }

    // ---- the sky's own sun: the disk's screen UV places the shafts when the light's clamped direction does not point where the sun is drawn
    {
        EXPECT(Has(enginePassRules, R"x({"cSunParam",NameKind::ConstantBuffer},)x") &&
                   Has(enginePassRules, R"x(constexprboolIsSunParamDraw(constPassLayout&layout)noexcept)x") &&
                   Has(enginePassRules, R"x(inlineconstexprAnchorskShaftsSunParamBit=1ull<<62;)x") &&
                   Has(enginePassRules, R"x(if(IsSunParamDraw(layout)&&!IsLightDraw(layout))anchors|=kShaftsSunParamBit|((Anchors(layout.SlotOf(12))<<kShaftsSlotShift)&kShaftsSlotMask);)x"),
               "the sky's sun parameter is one more name row, one more bit of the shared map, and its buffer slot travels in the light pass's field: "
               "the draw declares no g_LightParam, so the two draws never claim the same anchors");
        EXPECT(Has(draw, R"x(if(ext::ffxiv::engine::IsShaftsSunParamAnchors(anchors)){CaptureSunParam(context,anchors,state);returnfalse;})x") &&
                   Has(draw, R"x(context->PSGetConstantBuffers(slot,1,&buffer);)x") &&
                   Has(draw, R"x(if(desc.ByteWidth<kSunParamBytes||desc.ByteWidth>0x100)return;)x"),
               "the draw is read at the slot its own reflection declared, and a buffer that is not the engine's 128-byte one is passed on untouched");
        EXPECT(Has(sunHeader, R"x(classSunUvReader)x") && Has(sun, R"x(SunUvReadSunUvReader::Read(ID3D11DeviceContext*context,uint64_tframe,boolblock))x") &&
                   Has(draw, R"x(constSunUvReaduv=state.sunUv.Read(context,state.frames);)x"),
               "the sun parameters are read from a ring of their own, a frame or more late, like the tone map's exposure and the light's bytes");
        EXPECT(Has(rules, R"x(inlineconstexpruint32_tkSunParamBytes=0x10;)x") &&
                   Has(rules, R"x(constexprboolSunParamUvUsable(constfloatrow0[4])noexcept)x") &&
                   Has(rules, R"x(constexprSunProjectionProjectSunFromUv(floatuvX,floatuvY,constWindow&window)noexcept)x") &&
                   Has(rules, R"x(constexprboolSunUvFresh(boolhaveUv,uint64_tuvFrame,uint64_tframe)noexcept)x"),
               "the row is judged, the UV is projected into the window and the read's age is a rule of its own: an all-zero, NaN or foreign row "
               "falls back to the light's own projection");
        EXPECT(Has(draw, R"x(constboolsunFromUv=SunUvFresh(state.haveSunUv,state.sunUvFrame,state.frames);)x") &&
                   Has(draw, R"x(constSunProjectionprojected=sunFromUv?ProjectSunFromUv(state.sunUvRow[2],state.sunUvRow[3],facts.window):ProjectSun(projection,sunDirection,facts.window);)x"),
               "the sky's UV places the shafts when it is this frame's, and the light's direction is what places them otherwise");
        EXPECT(Has(draw, R"x(state.haveSunUv=false;)x") && Has(rules, R"x(kMaxOffscreen=0.50f;)x"),
               "a camera cut drops the sky's sun with the held one, and the fallback's margin is half a window: the morning sun the clamped light "
               "had moved away from still casts into the frame");
    }

    // ---- the frame's readings: a ring of staging pairs, the copy this frame, the newest copy of the frames before it, and never a wait
    {
        EXPECT(Has(draw, R"x(state.sun.Copy(context,light.Get(),camera.Get(),state.frames,shadows,shadowCount))x"),
               "the light's and the camera's bytes are copied at the sun draw, keyed on the module's own frame count");
        EXPECT(Has(draw, R"x(constSunReadread=state.sun.Read(context,state.frames);)x"), "and read at the tone map of a later frame");
        // The frame's white point comes from the tone map's own parameters, copied at its draw and read a frame late - never waited for - and the
        // bound it feeds can never be a fixed number unless the frame's own exposure never arrives.
        EXPECT(Has(draw, R"x(state.tone.Copy(context,params.Get(),state.frames))x") &&
                   Has(draw, R"x(constToneReadread=state.tone.Read(context,state.frames);)x") &&
                   Has(draw, R"x(context->PSGetConstantBuffers(kToneMapSlot,1,&buffer);)x"),
               "the tone map's parameter buffer is copied at the tone map draw and read a frame later, from the slot the captured tone-map "
               "shader's reflection puts it at");
        EXPECT(Has(rules, R"x(inlineconstexprfloatkVerifiedWhitePoint=0.838f;)x") &&
                   Has(rules, R"x(inlineconstexprfloatkMaxWhitePoint=kVerifiedWhitePoint;)x") &&
                   Has(rules, R"x(inlineconstexprfloatkFallbackWhitePoint=kVerifiedWhitePoint;)x") &&
                   Has(rules, R"x(inlineconstexprfloatkMinUsableExposure=0.25f;)x") &&
                   Has(rules, R"x(inlineconstexprfloatkShaftGain=0.30f;)x") &&
                   Has(rules, R"x(constexprfloatWhitePointFromExposure(floatexposure)noexcept)x") &&
                   Has(rules, R"x(constexprfloatShaftAdded(floatscene,floatlight,floatwhitePoint)noexcept)x") &&
                   Has(rules, R"x(constexprfloatPeakWhiteShare(floatintensity)noexcept)x") &&
                   Has(rules, R"x(constexprboolWhitePointStale(boolhaveWhite,uint64_tlastFrame,uint64_tframe)noexcept)x") &&
                   Has(draw, R"x(constfloatwhitePoint=WhitePointFromExposure(read.exposure);)x") &&
                   Has(draw, R"x(state.whitePoint=whitePoint;)x") && Has(draw, R"x(inputs.whitePoint=state.whitePoint;)x") &&
                   Has(pass, R"x(params.white[0]=inputs.whitePoint;)x") &&
                   Has(draw, R"x(LOG_INFO("{}",kWhitePointFallback);)x") && Has(draw, R"x(if(WhitePointStale(state.haveWhitePoint,state.whiteFrame,state.frames))x"),
               "the added light is measured against the frame's white point, which is the reciprocal of the tone map's own exposure inside the band "
               "the captured measurement supports: never above 0.838, and that measurement itself - not the band's dark end - while that exposure has "
               "not landed or has stopped, with the line that used to be dead code saying so");
        // The held sun: the last accepted direction, drawn for a few frames with a weight that falls a fifth at a time and dropped by a camera cut.
        EXPECT(Has(rules, R"x(structSunHold)x") && Has(rules, R"x(constexprfloatkHoldWeights[kHoldFrames+1u])x") &&
                   Has(rules, R"x(constexprfloatHoldFade(uint32_tage)noexcept{returnage<uint32_t(kHoldFrames+1u)?kHoldWeights[age]:0.0f;})x") &&
                   Has(rules, R"x(constexprboolCameraCut(constfloatheld[kProjectionRows],constfloatnow[kProjectionRows])noexcept)x") &&
                   Has(draw, R"x(constfloatfade=sunLanded?1.0f:state.hold.Weight(state.frames);)x") &&
                   Has(draw, R"x(state.hold.Accept(state.sun.Facts(),world,state.frames);)x") &&
                   Has(draw, R"x(inputs.fade=fade;)x") && Has(pass, R"x(params.white[1]=inputs.fade;)x"),
               "a frame whose own read has not landed draws with the last sun the read-back accepted, fading over at most kHoldFrames frames, "
               "so a missed read costs a little light instead of a whole frame of the effect");
        EXPECT(Has(draw, R"x(if(state.haveLastCamera&&CameraCut(state.lastCamera,state.sun.ViewProjection()))x") &&
                   Has(draw, R"x(state.hold.Drop();)x") && Has(rules, R"x(inlineconstexprconstchar*kCameraCut=)x"),
               "the held sun is dropped the moment a landed camera shows a cut, so it never lights the new picture from the old one's sky; the "
               "camera is taken before the light's verdict, so a refused light still shows the cut");
        // The held direction is projected with the newest camera that landed, not the one it was measured with, so the shafts stay on the sun while
        // the view moves. The facts and the camera come from one copy, so a missed light read also missed the camera - the documented limit.
        EXPECT(Has(draw, R"x(constfloat*projection=sunLanded?state.sun.Projection():(state.haveLastCamera?state.lastProjection:nullptr);)x") &&
                   Has(draw, R"x(!ViewDirectionOf(state.lastView,state.hold.world,sunDirection))x"),
               "the held sun is kept as a world vector and turned into the newest camera's view before it is projected, so the shafts stay on the "
               "sun while the view moves instead of sliding with the view that measured them");
        // F2: a below-horizon light is this frame's, not the next light's - only a lamp is remembered by its buffer.
        EXPECT(Has(draw, R"x(if(verdict==LightVerdict::Lamp){state.rejectedLight=state.sun.LastLight();state.rejectedFrame=state.frames;})x") &&
                   Has(draw, R"x(if(verdict==LightVerdict::BelowHorizon)x"),
               "a directional light below the horizon does not send the frames after it to another light draw: it means this frame has no sun above "
               "it. Only a lamp is remembered by its buffer, for the few seconds the pool keeps it");
        // F1: the tint is a colour, so the light's own magnitude cannot decide the effect.
        EXPECT(Has(rules, R"x(tint.rgb[i]/=peak;)x"),
               "the light's diffuse is normalised to a colour of peak 1, so the key is what says how much light there is");
        // The run line carries the direction, the buffer and the held sun's fade: what settles a light that is not the sun.
        EXPECT(Has(rules, R"x(({:.4f},{:.4f},{:.4f})ofthelightat{:x},shadow{},white{:.3f},fade{:.2f})x"),
               "the run line prints the direction the light reported, the buffer it came from, the shadow's angle, the white point and the fade the "
               "sun was drawn with");
        // F5: the card's numbers reach the log about once a minute of rendered frames, on every frame the module classifies - a session that draws
        // nothing must still write it, which the version that logged inside the drawn branch never did.
        EXPECT(Has(rules, R"x(inlineconstexpruint64_tkCountsLogFrames=3600;)x") && Has(draw, R"x(LOG_INFO("{}",FormatCard(numbers));)x") &&
                   Has(draw, R"x(voidLogCountsIfDue(State&state))x") && Has(draw, R"x(if(state.frames<state.nextCountsLog)return;)x") &&
                   Has(draw, R"x(CloseFrame(state);LogCountsIfDue(state);)x") &&
                   Has(rules, R"x({}renderedframes,{}withtheshafts({}ofthemdrawnwithaheldsun))x"),
               "the card's counts are written to the log once a minute from the frame's close, on the drawn, skipped and declined paths alike, with "
               "the module's own frame count and the number drawn from a held sun, so a session's share is computable from the log alone");
        // F3: the term is read bilinearly and the march starts at a per-pixel fraction of its first step.
        EXPECT(Has(shader, "lerp(Term.Load(") && Has(shader, "Hash01(position.xy)") && Has(shader, "const float2 coordinate = (position.xy - Window.xy) * 0.5 - 0.5;"),
               "the composite reads the half-resolution term between the four texels that surround the pixel, and the blur's march is offset per "
               "pixel by a hash of its own coordinate: the 2x2 block and the shared lattice a session saw are gone, and the term is still a function "
               "of the frame alone");
        EXPECT(Has(shader, "const float3 light = shafts * Sun.z * tint * Tuning.w * White.y;") &&
                   Has(shader, "const float3 head = max(White.x - scene.rgb, 0.0);") &&
                   Has(shader, "const float3 add = head * raw / max(raw + head, 1.0e-6);"),
               "the composite's own bound is the pixel's room to the frame's white point, so the same keys add the same share of the white in a "
               "bright frame and a dark one and no key can push a channel past it");
        EXPECT(Has(sun, R"x(UINT(D3D11_MAP_FLAG_DO_NOT_WAIT))x") && Has(sun, "D3D11_MAP_READ"),
               "the read-back never waits for the GPU");
        EXPECT(!Has(draw, "->Map(") && !Has(draw, "D3D11_MAPPED_SUBRESOURCE") && !Has(draw, "CopySubresourceRegion("),
               "the draw path itself maps and copies nothing: ShaftsSun.cpp owns both");
        EXPECT(Has(rules, R"x(inlineconstexpruint32_tkCameraBytes=0x160;)x") && Has(rules, R"x(inlineconstexpruint32_tkViewProjectionAt=0x060;)x") &&
                   Has(rules, R"x(inlineconstexpruint32_tkViewAt=0x000;)x") && Has(sun, R"x(constD3D11_BOXcameraBox=ByteBox(0,kCameraBytes);)x") &&
                   Has(sun, R"x(std::memcpy(_viewProjection,camera+kViewProjectionAt,sizeof_viewProjection);)x") &&
                   Has(sun, R"x(std::memcpy(_projection,camera+kProjectionOffset,sizeof_projection);)x") &&
                   Has(sun, R"x(std::memcpy(_view,camera+kViewAt,sizeof_view);)x"),
               "the camera is copied whole (view, view-projection, projection): the light's direction is view space and goes through the projection, "
               "the view's second row is the world's up (the horizon test and the shadow's own space), and the view-projection is what a camera cut "
               "is measured on");
        EXPECT(Has(rules, R"x(constexprboolViewProjectionOk(constfloatviewProjection[kProjectionRows])noexcept)x") &&
                   Has(sun, R"x(_haveProjection=ProjectionOk(_projection)&&ViewProjectionOk(_viewProjection);)x") &&
                   Has(draw, R"x(ProjectSun(projection,sunDirection,facts.window))x"),
               "the sun is projected with the projection alone - the direction is already in its space - and a buffer that does not hold this "
               "build's camera is NoCamera instead of a sun in the wrong place");
        EXPECT(Has(draw, R"x(desc.ByteWidth<kCameraBytes)x"), "the camera buffer is recognised by the size the whole camera needs");
        EXPECT(Has(sun, R"x(if(!ReadCandidate(slot.written,slot.frame,frame,_haveAccepted,_lastAcceptedFrame))continue;)x"),
               "a read takes an older copy the ring still holds, newest first, and never one older than the last it took");
        EXPECT(Has(sun, R"x(if(!MadeOn(slot.light.Get(),device.Get())||!MadeOn(slot.camera.Get(),device.Get()))x") &&
                   Has(sun, R"x(returnowner&&owner.Get()==device;)x"),
               "a staging pair made on another device is rebuilt instead of reused: a cross-device copy is refused silently and the read would map "
               "bytes the old device wrote");
        EXPECT(Has(sun, R"x(_projectionRefused=false;result.notReady=true;)x"),
               "a camera map that has not landed clears an earlier refusal, so a GPU that is behind is never counted as NoCamera");
        EXPECT(Has(sun, R"x(Slot&slot=_ring[SlotForAge(frame,age)];)x") && Has(sun, R"x(Slot&slot=_ring[RingIndex(frame)];)x"),
               "the ring is indexed by frame on both sides");
        EXPECT(Has(sun, R"x(if(result.notReady)++_notReady;)x"), "a not-ready map is counted apart from a read");
        EXPECT(Has(sunHeader, R"x(boolHaveRead()constnoexcept{return_haveRead;})x"), "and the last good values stay until a newer read lands");
        EXPECT(Has(sunHeader, R"x(boolProjectionRefused()constnoexcept{return_projectionRefused;})x"),
               "a camera that has not landed is not a refused projection");
        EXPECT(Has(draw, R"x(if(ReadStale(state.sun.HaveRead(),state.sun.LastReadFrame(),state.frames))x"),
               "a read-back that stopped arriving skips the frames instead of compositing an old sun");
        EXPECT(Has(rules, R"x(constexprboolReadCandidate(boolwritten,uint64_twrittenFrame,uint64_tframe,boolhaveAccepted,uint64_tlastAcceptedFrame)noexcept)x") &&
                   Has(rules, R"x(constexprboolReadStale(boolhaveRead,uint64_tlastReadFrame,uint64_tframe)noexcept)x"),
               "both rules are pure and tested on the host");
        EXPECT(Has(draw, R"x(LOG_INFO("{}",FormatProjection(state.sun.Projection()));)x") &&
                   Has(draw, R"x(LOG_INFO("{}",FormatViewProjection(state.sun.ViewProjection()));)x") &&
                   Has(rules, R"x(inlinestd::stringFormatViewProjection(constfloatviewProjection[kProjectionRows]))x") &&
                   Has(rules, R"x(inlineconstexprconstchar*kBelowHorizonSkip=)x"),
               "the projection and the view-projection are printed together once per session: the projection alone cannot tell a wrong direction "
               "from a wrong matrix, and the frame a refused light left alone says so in its own line");
        EXPECT(Has(rules, R"x(constexprvoidAdvanceFrame(uint64_t&frames)noexcept{++frames;})x") &&
                   Has(rules, R"x(constexprboolSameRenderedFrame(uint64_twritten,uint64_tnow)noexcept{returnwritten==now;})x") &&
                   Has(draw, R"x(if(!state.frameClosed)return;state.frameClosed=false;AdvanceFrame(state.frames);state.attempts=0;)x") &&
                   Has(draw, R"x(voidCloseFrame(State&state){state.frameClosed=true;})x"),
               "the count of rendered frames moves when a frame closes at its tone map, never on the output-present token: frame generation "
               "presents an image inside one rendered frame, and a token comparison dropped every frame a present landed in");
        EXPECT(Has(draw, R"x(structClose{State&state;~Close(){CloseFrame(state);LogCountsIfDue(state);}}close{state};)x"),
               "the frame is closed by a scope guard at its tone map, so a throw cannot leave it open for the next frame's light draw");
        EXPECT(Count(draw, "ext::present::FrameCount()") == 1 && Has(draw, R"x(state.hdrSampleFrame==token)x"),
               "the present count is kept only for the once-a-frame scene HDR sample: no draw identity reads it");
        EXPECT(Count(draw, "sun.Read(") == 1 && !Has(draw, "Read(context,state.frames,true)"),
               "the product's one read passes no block argument: the waiting form is the tests' only");
        EXPECT(Has(draw, R"x(inlineconstexpruint32_tkMaxCaptureAttempts=24;)x"),
               "a frame measures a bounded number of light draws while it looks for the sun's");
        EXPECT(Has(draw, R"x(if(light==nullptr)return;)x") && Has(draw, R"x(if(camera==nullptr)return;)x") &&
                   Has(draw, R"x(if(depth==nullptr)return;)x"),
               "the sun draw needs the light's own buffer, the camera and the depth");
        EXPECT(Has(draw, R"x(state.sunDraw.depth=depth;)x") && Has(draw, R"x(state.sunDraw.taken=true;)x"),
               "and this frame's depth is the one the mask reads");
    }

    // ---- the pass: four draws between an explicit save and restore, the scene copied and never written, every binding restored
    {
        EXPECT(Has(pass, R"x(ext::core::explicit_state::GuardstateGuard(context,context1.Get(),device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1,kSpec);)x") &&
                   Has(pass, R"x(stateGuard.Neutralise();)x"),
               "the game's state is captured, the private state's defaults (no HS, DS, GS, stream-out or predicate) are set explicitly, and it is put back by a scope guard, so a throw cannot leave it");
        EXPECT(!Has(pass, "SwapDeviceContextState") && !Has(pass, "CreateDeviceContextState"),
               "no device-context state swap remains: it does nothing when the draw hook is nested, and the setters would land on the game's own state");
        EXPECT(Has(pass, R"x(context->CopyResource(_scene.Get(),sceneResource.Get());)x"),
               "the engine's scene is copied GPU side into this pass's own texture");
        EXPECT(!Has(pass, "UpdateSubresource") && !Has(pass, "Map(_scene"), "nothing of the engine's scene is written or mapped");
        EXPECT(Count(pass, "context->Draw(3,0);") == 5, "five full-screen draws: the mask, the blur, the low-pass, the sun's colour and the composite");
        EXPECT(Has(pass, R"x(context->OMSetRenderTargets(1,&termRtv,nullptr);)x") &&
                   Has(pass, R"x(context->OMSetRenderTargets(1,&sceneRtv,nullptr);)x"),
               "the render target is switched before its views are bound, or D3D11 drops the shader resource");
        EXPECT(Has(pass, R"x(_failure="thesceneisnotFP16";)x"), "an 8-bit scene is refused with a reason");
        EXPECT(Has(pass, R"x(_failure="thispass'scopyofthescenecouldnotbecreated";)x"), "so is a copy the device refuses");
        EXPECT(Has(passHeader, R"x(ID3D11ShaderResourceView*Run(ID3D11DeviceContext*context,constInputs&inputs);)x"),
               "the pass hands back the view to bind, or null to leave the game's draw alone");
        EXPECT(Has(pass, R"x(if(inputs.tint.fromSky))x"),
               "the sun's colour is only searched when the shafts take the sky's colour: the neutral shaft needs no colour from the frame");
        EXPECT(Has(pass, R"x(EnsureHalf(device.Get(),halfWidth,halfHeight))x") && Has(pass, R"x(EnsureSunColour(device.Get()))x"),
               "the half pair follows the render window, not the allocation; the sun's colour is one texel and created once");
    }

    // ---- the beams (shafts2): the low-pass draw, its slot and its place in the pass, inside the same explicit state
    {
        EXPECT(Has(pass, R"x(constexpruint32_tkSlotLow=5;)x") && Has(pass, R"x(constexpruint32_tkSlots=6;)x") &&
                   Has(pass, R"x(ext::core::explicit_state::SpeckSpec{kSlots,0,1};)x"),
               "the low-pass is the sixth pixel-shader resource slot (t5), and the Guard's spec counts all the slots, with no sampler and one buffer");
        const size_t blurAt = pass.find("context->OMSetRenderTargets(1,&termRtv,nullptr);");
        const size_t lowAt = pass.find("context->OMSetRenderTargets(1,&lowRtv,nullptr);");
        const size_t colourAt = pass.find("context->OMSetRenderTargets(1,&sunColourRtv,nullptr);");
        const size_t compositeAt = pass.find("context->OMSetRenderTargets(1,&sceneRtv,nullptr);");
        EXPECT(blurAt != std::string::npos && lowAt != std::string::npos && colourAt != std::string::npos && compositeAt != std::string::npos &&
                   blurAt < lowAt && lowAt < colourAt && colourAt < compositeAt,
               "the low-pass is drawn after the blur (it reads the term) and before the composite (which reads its target), each with its render "
               "target switched first");
        EXPECT(Has(pass, R"x(context->PSSetShader(_lowShader.Get(),nullptr,0);)x") && Has(pass, R"x(LightShafts_Low_cso)x") &&
                   Has(pass, R"x(compositeViews[kSlotLow]=_lowSrv.Get();)x"),
               "the low-pass has its own shader, and the composite reads its target at t5");
        EXPECT(Has(pass, R"x(LowExtent(width))x") && Has(pass, R"x(LowExtent(height))x"),
               "the low-pass target's size is the rules' extent of the half-resolution pair (a block per kBeamBlock texels, rounded up)");
        EXPECT(Has(passHeader, R"x(ID3D11ShaderResourceView*LowView()const)x"), "the pass exposes the low-pass target for the tests and the lab");
        const std::string props = Raw(repo, "ffxiv/engine/shafts.shaders.props");
        const std::string prepare = ReadFile(repo + "/tests/ext/ffxiv/engine/prepare_shafts.ps1");
        EXPECT(Has(props, "<Variable>LightShafts_Low_cso</Variable>") && Has(props, "<Entry>PS_Low</Entry>") && Has(props, "<Header>LightShafts_Low.h</Header>"),
               "the product builds the new entry point");
        EXPECT(Has(prepare, "'PS_Low', 'Low'"), "and the tests' and the lab's header recipe builds it too");
        EXPECT(Has(shader, "float PS_Low(float4 position : SV_Position) : SV_Target0") && Has(shader, "Texture2D<float> Low : register(t5);"),
               "the shader has the entry and the slot");
    }

    // ---- the beams' formula in the shader: the pinned shape, the constants equal to the rules', and the boost before the white bound
    {
        const auto number = [](const std::string& text, const std::string& name) {
            const size_t at = text.find(name + " = ");
            if (at == std::string::npos)
                return -1.0e30;
            return std::strtod(text.c_str() + at + name.size() + 3, nullptr);
        };
        const std::string rulesRaw = Raw(repo, "ffxiv/engine/ShaftsRules.h");
        for (const char* name : { "kBeamBoost", "kBeamKeep", "kBeamFalloffRadius", "kBeamTermMax" })
        {
            const double inShader = number(shader, std::string("static const float ") + name);
            const double inRules = number(rulesRaw, std::string("inline constexpr float ") + name);
            EXPECT(inShader > -1.0e29 && inRules > -1.0e29 && inShader == inRules, "%s: shader %g, rules %g", name, inShader, inRules);
        }
        EXPECT(number(shader, "static const int kBeamBlock") == number(rulesRaw, "inline constexpr uint32_t kBeamBlock") && number(shader, "static const int kBeamBlock") > 1.0,
               "the block size is the rules' (and more than one texel: a block of 1 is the term itself and removes every beam)");
        EXPECT(Has(shader, "const float shafts = min((max(level - low, 0.0) * kBeamBoost + kBeamKeep * level) * falloff, kBeamTermMax);"),
               "the shaped term: the term over its low-pass, boosted, plus the kept share of the level, times the falloff, bounded");
        EXPECT(Has(shader, "const float reach = length(position.xy - Sun.xy) / max(Window.w, 1.0) / kBeamFalloffRadius;") &&
                   Has(shader, "const float falloff = exp(-reach * reach);"),
               "the falloff is a Gaussian of the distance from the sun in WINDOW HEIGHTS (the window's height divides it), not pixels");
        EXPECT(Has(shader, "const float2 lowCoordinate = (coordinate + 0.5) / float(kBeamBlock) - 0.5;") &&
                   Has(shader, "Low.Load(int3(clamp(lowBase, int2(0, 0), lastLow), 0))") && Has(shader, "const float low = lerp(lowTop, lowBottom, lowFraction.y);"),
               "the low-pass is read bilinearly at the block centres (block j is centred at (j + 0.5) * kBeamBlock)");
        const size_t shapedAt = shader.find("const float shafts = min(");
        const size_t lightAt = shader.find("const float3 light = shafts * Sun.z");
        const size_t boundAt = shader.find("const float3 add = head * raw / max(raw + head, 1.0e-6);");
        EXPECT(shapedAt != std::string::npos && lightAt != std::string::npos && boundAt != std::string::npos && shapedAt < lightAt && lightAt < boundAt,
               "the boost is in the term that goes into the light, which goes into the white bound: it is applied before the bound, never after");
        EXPECT(Count(shader, "kBeamBoost") == 2, "and the boost appears in its declaration and the pinned expression, nowhere after the bound");
        EXPECT(Has(shader, "PS_Low") && Has(shader, "const int2 end = min(first + int2(kBeamBlock, kBeamBlock), int2(termWidth, termHeight));") &&
                   Has(shader, "return count > 0.0 ? sum / count : 0.0;"),
               "a block's mean is over the texels that exist (an edge block is not darkened by a divide by the block area)");
    }

    // ---- the stage's capture numbers: the white, fade, sun position and tint stay noted once, after the run's pictures; the constants of the
    //      formula ride in the stage's existing values string
    {
        const size_t ranAt = draw.find("ext::ffxiv::diag::StageRan(ext::ffxiv::diag::StageId::Shafts,");
        const size_t noteAt = draw.find("ext::ffxiv::diag::StageNoteFrame(ext::ffxiv::diag::StageId::Shafts,values);");
        EXPECT(ranAt != std::string::npos && noteAt != std::string::npos && ranAt < noteAt && Count(draw, "StageNoteFrame(") == 1,
               "the frame's white, fade, sun position and tint are noted once, after the stage's pictures were taken");
        EXPECT(Has(draw, "if(ext::ffxiv::diag::StageOpen(ext::ffxiv::diag::StageId::Shafts))"),
               "and only on the capture frame (one relaxed load otherwise)");
        EXPECT(Has(draw, "std::stringStageValues(){returnFormatStageValues(S().params);}"),
               "the stage line carries the formula's constants through the rules' formatter: the values string, no new key");
    }

    // ---- the composite: the game's t0 goes back on every path, the frame is drawn once, and the pass's own work is marked
    {
        EXPECT(Has(draw, R"x(ext::ffxiv::engine::gloss::OnShaftsSceneDraw(context,facts,copy,draw,user);)x"),
               "the copy is bound at t0 for one draw through the puddles' stage, whose own entry is the one place that binds and restores it");
        EXPECT(Has(draw, R"x(if(drew)returntrue;drew=true;if(context!=nullptr&&scene!=nullptr&&ext::ffxiv::engine::gloss::ToneArmedForChain())x"),
               "a frame the shafts leave alone runs the puddles' tone-map stage first: the draw path it owns would otherwise leave the pools out");
        EXPECT(Has(draw, R"x(ext::ffxiv::engine::gloss::OnShaftsSceneDraw(context,facts,scene,draw,user);)x"),
               "and the stage's own entry owns the one binding of t0, so every path that draws restores the game's view the same way");
        EXPECT(Has(draw, R"x(returnDrawToneMapWithChain(context,facts,scene,draw,user,drew);)x"),
               "every path that skips or refuses the shafts' own pass hands the frame to the chained draw, so the game's call runs exactly once");
        EXPECT(Has(draw, R"x(catch(...))x") && Has(draw, R"x(if(!drew)draw(user);returntrue;)x"),
               "a throw inside the pass draws the game's call once and returns true, so the seam cannot draw it again");
        EXPECT(Has(draw, R"x(if(!SunVisible(projected))x") &&
                   Has(draw, R"x(CountSkip(state.skips,SkipReason::OffScreen);state.lastSun=projected;state.lastWindow=facts.window;)x"),
               "a sun behind the camera or outside the viewport skips the frame, counts no refusal, and leaves the sun and the window it measured "
               "on the card: with nothing written there a session read \"the last sun at 0,0 of a 0x0 window\"");
        EXPECT(Has(draw, R"x(if(reason==Decline::NoDepth)CountSkip(state.skips,SkipReason::NoDepth);elseif(reason==Decline::NoSun)CountSkip(state.skips,SkipReason::NoSun);)x"),
               "the two refusal-free skip reasons are counted apart where the frame is skipped, through the one function that keeps `skipped` and "
               "the reason in step, so the card can say which one a session hit");
        EXPECT(Has(draw, R"x(state.session.NoteDecline(Knob::LightShafts))x") && Has(draw, R"x(ext::SetActive(ext::F::LightShafts,false);)x"),
               "the module keeps its own refusal streak and switches its own feature bit off when it stands down");
        EXPECT(Has(draw, R"x(state.session.NoteRun(Knob::LightShafts);)x"), "a run starts its streak over");
        EXPECT(Has(draw, R"x(if(!state.sunDraw.taken||!SameRenderedFrame(state.sunDraw.frames,state.frames)))x"),
               "a frame without the sun draw has no depth for this frame's mask, and is skipped without counting");
        EXPECT(Has(draw, R"x(if(sunLanded&&!state.sun.HaveProjection())x") && Has(draw, R"x(Decline::NoCamera)x") &&
                   Has(draw, R"x(if(!state.sun.ProjectionRefused())x"),
               "a camera buffer that arrived and is not a projection is the counted reason it says; one that has not landed is skipped");
        EXPECT(Has(draw, R"x(if(state.sun.Reads()==0&&state.sun.NotReady()>=kReadbackStallFrames&&!state.loggedStall)x"),
               "a read-back that has never landed gets its own line instead of the no-sun one");
        EXPECT(Has(draw, R"x(desc.IndependentBlendEnable)x") && Has(draw, R"x(shape.blended=desc.RenderTarget[0].BlendEnable;)x"),
               "the blend test reads the entry that applies to the pair: entry 0 unless independent blending is on");
        EXPECT(Has(draw, R"x(RejectedStill(state.rejectedLight!=nullptr,state.rejectedFrame,state.frames))x") &&
                   Has(rules, R"x(constexprboolRejectedStill(boolhaveRejected,uint64_trejectedFrame,uint64_tframe)noexcept)x"),
               "a light refused as a lamp is skipped for a few seconds, not for the session: the buffers are a pool reused within the frame");
        EXPECT(Has(draw, R"x(BetterDepthView(best.view!=nullptr,best.matchesScene,found.matchesScene))x") &&
                   Has(rules, R"x(constexprboolBetterDepthView(boolhaveBest,boolbestMatchesScene,boolcandidateMatchesScene)noexcept)x"),
               "a depth view at the scene's size wins over the first one, so a shadow map in a lower slot cannot be read as the frame's depth");
        EXPECT(Has(draw, R"x(ext::ffxiv::engine::OwnWorkownWork;)x"),
               "the pass's own draws are marked, so the draw seam and the shader tracker do not classify them");
        EXPECT(Has(draw, R"x(if(state.haveToneMapFrame&&state.toneMapFrames==state.frames)returnfalse;)x"),
               "one tone-map-shaped draw per frame wins, so the engine's post-tone copy is never a second run");
        EXPECT(Has(draw, R"x(facts.sceneIsScene=texture.Get()==state.sceneResource&&state.sceneFrames==state.frames;)x"),
               "the draw's t0 is this frame's scene only when it is the resource the composite wrote AND that composite was this frame's: a "
               "menu, a cut or a resize leaves the previous frame's resource behind, and compositing into it would light the old picture");
        EXPECT(Has(draw, R"x(constboolwanted=Wanted(state.enabled,state.params)&&state.session.Armed();)x"),
               "the feature bit is the key and the streak alone, not the scene HDR state: while the engine's HDR path is requested the pass does "
               "not draw, and switching HDR back off has to start it again without the shafts key moving");
        EXPECT(Has(draw, R"x(return!SceneHdrInactive(state.enabled,SceneHdrRequested());)x") &&
                   Has(draw, R"x(status.hdrInactive=SceneHdrInactive(state.enabled,SceneHdrRequested());)x"),
               "the scene HDR key is read where the module decides and on the card, so a live toggle neither leaves the pass armed and inert nor "
               "keeps it off after HDR is switched back off");
        EXPECT(Has(draw, R"x(if(state.hdr.Sample(state.enabled,SceneHdrRequested()))x") && Has(draw, R"x(kSceneHdrInactive)x"),
               "the inactive line is written on the sample that first finds the module inactive: once per transition, not once a frame");
        EXPECT(Has(enginePass, R"x(ext::ffxiv::shafts::SampleSceneHdr();)x"),
               "the engine's own per-frame seam samples that key while the HDR path takes every game draw before the draw seam");
        EXPECT(Has(draw, R"x(if(!Wanted(state.enabled,state.params)||!state.session.Armed())returnfalse;return!SceneHdrInactive(state.enabled,SceneHdrRequested());)x"),
               "Armed() short-circuits on the key and the streak before it reads the config, so a session with the key off pays nothing for it");
        EXPECT(!Has(rules, "Reentry") && !Has(draw, "Decline::Reentry"), "the unreachable re-entry reason is gone");
    }

    // ---- the rules stay pure, and the shaders read exact texels with no sampler, at the right pixel
    {
        EXPECT(!Has(rules, "d3d11") && !Has(rules, "ID3D11") && !Has(rules, "windows.h"), "the rules are device free");
        EXPECT(!Has(rules, "std::filesystem") && !Has(rules, "std::ofstream"), "the rules do no I/O");
        EXPECT(!Has(shader, "SamplerState") && !Has(shader, ".Sample("), "every read is a Load on an exact texel");
        EXPECT(Has(shader, "kSkyDepth") && Has(shader, "return (depth == depth && depth >= 0.0 && depth <= kSkyDepth) ? 1.0 : 0.0;"),
               "the reversed-Z sky test, and a NaN or a negative depth is a surface");
        EXPECT(Has(shader, R"x(if (sky > 0.5))x") && Has(shader, R"x(return scene;)x"),
               "a sky pixel keeps the scene's own value: the shafts never glow on the sky they come from");
        EXPECT(Has(shader, "const float3 light = shafts * Sun.z * tint * Tuning.w * White.y;"), "the composite is scene plus shafts times the colour and the key");
        EXPECT(Has(shader, R"x(const int2 at = int2(position.xy);)x"),
               "the scene is read at the target's own pixel: the viewport's half-pixel origin is not added to it");
        EXPECT(Has(shader, "const float2 coordinate = (position.xy - Window.xy) * 0.5 - 0.5;"),
               "and the half-resolution term takes the window's origin out, so a half-res texel covers the right two pixels");
        EXPECT(Has(shader, R"x(const int2 at = min(int2(Window.xy + position.xy * 2.0), last);)x"),
               "the mask maps a half texel to its two pixels, clamped onto the depth's own extent so an odd window's last column is not a bogus sky");
        EXPECT(Has(shader, "PS_SunColour") && Has(shader, R"x(if (SkyOf(Depth.Load(int3(at, 0))) < 0.5))x"),
               "the shafts' colour is sampled from sky texels only, inside a search around the sun");
        EXPECT(Has(shader, R"x(if (!(sampled.a > 0.5))x") && Has(shader, R"x(return scene; // no sky around the sun)x"),
               "and a frame with no sky around the sun adds nothing at all");
        EXPECT(!Has(shader, "SV_Target1"), "one render target per draw");
    }

    // ---- the keys: off or at today's picture by default, bound, documented, with a control and in the schema
    {
        struct Key
        {
            const char* name;
            const char* configType; // the C++ type the config's CustomOptional holds
            const char* schemaType; // the type the settings schema declares
        };
        const Key keys[] {
            { "LightShafts", "bool", "bool" },
            { "LightShaftsIntensity", "float", "float" },
            { "LightShaftsDecay", "float", "float" },
            { "LightShaftsSamples", "int32_t", "int" },
            { "LightShaftsDensity", "float", "float" },
            { "LightShaftsColor", "std::string", "enum" },
        };
        const std::string schema = ReadFile(repo + "/tools/settings-schema.json");
        const std::string settings = ReadFile(repo + "/docs/SETTINGS.md");
        const std::string notes = ReadFile(repo + "/docs/notes/light-shafts.md");
        EXPECT(!notes.empty(), "docs/notes/light-shafts.md documents the feature");
        for (const Key& key : keys)
        {
            const std::string optional = std::string("CustomOptional<") + key.configType + "> " +
                                          std::string(1, char(key.name[0] + 32)) + (key.name + 1) + " {";
            EXPECT(Has(config, optional), "%s is a config key", key.name);
            EXPECT(Has(binder, std::string("b.Add(\"FFXIV\",\"") + key.name + "\","), "%s is bound", key.name);
            EXPECT(Has(ini, std::string(key.name) + "=auto"), "%s has its ini line", key.name);
            EXPECT(Has(menu, std::string("\"") + key.name + "\"") || Has(menu, std::string("cfg.") + std::string(1, char(key.name[0] + 32)) +
                                                                                (key.name + 1) + ".value_or_default()"),
                   "%s has a control", key.name);
            EXPECT(Has(schema, std::string("\"[FFXIV] ") + key.name + "\""), "%s is in the settings schema", key.name);
            EXPECT(Has(schema, std::string("\"name\": \"") + key.name + "\"") , "%s has a schema row", key.name);
            EXPECT(Has(schema, std::string("\"type\": \"") + key.schemaType + "\""), "%s declares its type", key.name);
            EXPECT(Has(settings, std::string("`") + key.name + "` |"), "%s is in the generated settings page", key.name);
        }
        EXPECT(Has(menu, R"x(AutoRegisterregistration{{"ffxiv_light_shafts","FFXIV",)x"),
               "the card is registered by its own file");

        // The public text names the bound as the white the pass reads, not the frame's own: a wrong-high live value on a much darker frame still
        // over-lights it, and the text says so instead of promising the maximum key cannot burn anything.
        EXPECT(Has(ini, "as a share of the white the pass reads") && Has(ini, "still over-lights") && !Has(ini, "frame's own white") &&
                   Has(menu, "asashareofthewhitethepassreads") && Has(menu, "stillover-lightsit") && Has(menu, "thecapturedmeasurement,0.838"),
               "the intensity key's ini line and its tooltip are measured against the white the pass reads and name the residual hole a wrong-high "
               "live value leaves on a much darker frame");

        // The defaults themselves, not only the declarations: a fresh ini is today's picture (the pass off, one full intensity key) and the other four
        // knobs sit at the values the rules name. The checks above match the declaration's name and type, which a changed default leaves intact.
        const std::string configCode = Code(repo, "ffxiv/FfxivConfig.h");
        for (const char* line : { R"x(CustomOptional<bool>lightShafts{false};)x", R"x(CustomOptional<float>lightShaftsIntensity{1.0f};)x",
                                  R"x(CustomOptional<float>lightShaftsDecay{0.96f};)x", R"x(CustomOptional<int32_t>lightShaftsSamples{48};)x",
                                  R"x(CustomOptional<float>lightShaftsDensity{0.9f};)x", R"x(CustomOptional<std::string>lightShaftsColor{"auto"};)x" })
            EXPECT(Has(configCode, line), "the key's default is written out: %s", line);
        EXPECT(Has(menu, R"x(if(changed)Publish();)x"), "and publishes on change, so nothing needs a restart");
        EXPECT(Has(rules, R"x(inlinestd::stringFormatCard(constCardNumbers&numbers))x") &&
                   Has(rules, R"x({}withnosunto""draw(nosunamongtheframe'slightdraws:{},thelight'sbytesnotarrived:{},thesunoutofview:{},thelightbelowthe""horizon:{},thelightnottheframe'ssun:{});theread-back:{}reads,{}notready,{}refusedasnotthesun;thelastsunat)x"),
               "the card's state line is a rules function, so its format is checked on the host: the five reasons a frame had no sun to draw print "
               "apart instead of one number for three");
        EXPECT(Has(menu, R"x(numbers.reads=status.reads;)x") && Has(menu, R"x(numbers.notReady=status.notReady;)x") &&
                   Has(menu, R"x(numbers.noDepth=status.noDepth;)x") && Has(menu, R"x(numbers.noSun=status.noSun;)x") &&
                   Has(menu, R"x(numbers.offScreen=status.offScreen;)x") && Has(menu, R"x(numbers.belowHorizon=status.belowHorizon;)x") &&
                   Has(menu, R"x(numbers.notTheSun=status.notTheSun;)x") && Has(menu, R"x(state=FormatCard(numbers);)x"),
               "and every number on it comes from the module's own counter, read at the card");
        EXPECT(Has(rules, R"x(enumclassSkipReason:uint8_t)x") &&
                   Has(rules, R"x(constexprvoidCountSkip(CardCounts&counts,SkipReasonreason)noexcept)x") &&
                   Has(draw, R"x(CountSkip(state.skips,SkipReason::NoDepth);)x") && Has(draw, R"x(CountSkip(state.skips,SkipReason::NoSun);)x") &&
                   Has(draw, R"x(CountSkip(state.skips,SkipReason::OffScreen);)x") &&
                   Has(draw, R"x(CountSkip(state.skips,SkipReason::BelowHorizon);)x") &&
                   Has(draw, R"x(CountSkip(state.skips,SkipReason::NotTheSun);)x") &&
                   Has(draw, R"x(if(state.haveBelowHorizonFrame&&state.belowHorizonFrame==state.frames))x") &&
                   Has(draw, R"x(if(state.haveNotSunFrame&&state.notSunFrame==state.frames))x") &&
                   Has(draw, R"x(returnSkipRefusedAndDraw(context,facts,scene.Get(),draw,user,drew);)x") &&
                   Has(draw, R"x(returnSkipNotTheSunAndDraw(context,facts,scene.Get(),draw,user,drew);)x"),
               "every skipped frame is counted through the one function that moves `skipped` and exactly one of its five reasons, so the five "
               "numbers on the card are the whole of the count they are printed under, a refused light is not counted twice, and each of them "
               "draws through the chained entry so the puddles' stage still runs on the scene the frame was handed");
        EXPECT(Has(menu, R"x(elseif(status.intensity<=kIntensityEpsilon))x"),
               "and it says a deliberate intensity of 0 is intensity 0, not a session that stood the pass down");
        EXPECT(Has(layout, R"x({"ffxiv_light_shafts","FFXIV","ffxiv_ambient_occlusion"},)x"), "the card is placed in the FFXIV tab after the ambient occlusion card");
        EXPECT(Has(layout, R"x({"ffxiv_godrays","FFXIV","ffxiv_screen_space_reflections"},)x"), "and the card after it is the jitter card");
    }

    // ---- public wording: no private path, no workspace or ledger name, no package or review id in anything a player can read
    {
        // The banned words are assembled from pieces so this file never carries them itself (the publish scanner reads tests too).
        const std::string texts[] { std::string("ps-") + "ledger", std::string("phase3-") + "ledger", std::string("BRI") + "EF",
                                    std::string("STU") + "DY", std::string("TRAC") + "KER", std::string("C:\\") + "Users", std::string("C:\\\\") + "Users",
                                    std::string("SHAFTS") + "-1", std::string("Nic") + "kle", std::string("Luc") + "ent" };
        for (const char* relative : { "ffxiv/engine/Shafts.h", "ffxiv/engine/ShaftsRules.h", "ffxiv/engine/ShaftsPass.h",
                                      "ffxiv/engine/ShaftsPass.cpp", "ffxiv/engine/ShaftsDraw.cpp", "ffxiv/engine/ShaftsSun.h",
                                      "ffxiv/engine/ShaftsSun.cpp", "ffxiv/engine/ShaftsMenu.cpp",
                                      "ffxiv/engine/shaders/LightShafts.hlsl" })
        {
            const std::string text = Raw(repo, relative);
            EXPECT(!text.empty(), "%s exists", relative);
            for (const std::string& banned : texts)
                EXPECT(!Has(text, banned), "%s does not carry the private word %s", relative, banned.c_str());
        }
    }

    // ---- METER sweep S2: a refused projection consults the held sun before it declines ----------------------------------------------------------------------------
    {
        EXPECT(Has(draw, "boolsunLanded=freshness==Freshness::Sun;"), "the frame's sun is mutable: a refused projection can hand the frame to the held sun\n");
        const size_t refused = draw.find("returnDeclineAndDraw(context,facts,scene.Get(),draw,user,drew,Decline::NoCamera);");
        const size_t ask = draw.find("if(!RefusedProjectionUsesHold(state.hold.Weight(state.frames),state.haveLastCamera))");
        EXPECT(ask != std::string::npos && refused != std::string::npos && ask < refused, "the rule is asked before the refusal is counted\n");
        EXPECT(Has(draw, "if(!RefusedProjectionUsesHold(state.hold.Weight(state.frames),state.haveLastCamera))returnDeclineAndDraw(context,facts,scene.Get(),draw,user,drew,Decline::NoCamera);if(state.log.Take(0x4000000Bu))LOG_INFO(\"{}\",kCameraRefusedHeld);sunLanded=false;}"),
               "only a frame with nothing to stand on is declined; the other writes one line and takes the held sun\n");
        const size_t pending = draw.find("returnSkipAndDraw(context,facts,scene.Get(),draw,user,drew,Decline::None,0x40000007u);");
        EXPECT(pending != std::string::npos && pending < ask, "a camera that has not landed yet is still a skip, decided before the refusal\n");
    }

    if (g_failures != 0)
    {
        std::printf("shafts_pins: %d expectation(s) failed\n", g_failures);
        return 1;
    }
    std::printf("shafts_pins: all checks passed\n");
    return 0;
}
