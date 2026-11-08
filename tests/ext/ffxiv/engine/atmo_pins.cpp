// Atmosphere: the wiring pins, read as text from the sources (no device), the depth of field's own pins' shape.
//
// What a wrong wiring would break, and what is pinned here:
//   the chain position - the stage sits between the light bounce and depth of field: the light bounce asks the atmosphere before depth of field (the
//   dispatchers in ChainHooks.h), the stage composes depth of field on its own answer, the dispatch order is shafts, puddles, march, light bounce,
//   atmosphere, depth of field, and a build that links the bounce without the atmosphere still reaches depth of field;
//   the fail-open contract - every exit where the stage has no answer (off, already answered, declined, threw) hands depth of field the texture it was
//   given or the owner turn; the owner role returns true only when it drew and draws exactly once on every path; a missing sun is NEVER a decline;
//   the key off is a byte-exact passthrough (the pass answers a copy and no draw) and the shader returns the scene's own texel for the sky, for a surface
//   nearer than the start distance and for a medium that does not touch the ray;
//   the shader's expression pinned operation for operation: the sky exclusion comes before any reconstruction, the start distance gates the optical depth,
//   the transmittance is applied once in LINEAR light, the lobe's sign, and the constants the rules and the shader share cannot drift;
//   the stamping contract - the stage's sources never read an earlier stage's arming or hooks, its profile stamp has its own tag, its answered-tracking
//   is its own counter (atmoFrame);
//   (L2b) the lobe's direction - the sky's sun sprite draw (cSunParam) is copied by this stage's own SunUvReader, read each tone map, held with its frame, and the
//   lobe is centred on the visible disc only through ChooseSunDirection (fresh, usable, agreeing with the light); the ambient term is neutralised by the key in
//   the shader exactly as the rules do; the switch is ON by default in the config and the rules;
//   (FOGFIX) the fog the stage lives on - the stage reads the engine's fog through the SAME reader the god rays use (godrays::FogReader, one profile check for both
//   stages: it names no check of its own and never reads the mix scale), takes a fog state only from `fogRead.usable`, keeps the last good one when a frame's is
//   refused, computes the engine's density at the camera from the profile alone (a clear-weather zero profile gives 0 and the engine fog scale 1), feeds the
//   shader the engine's own fog colour (cFogParam row 0) as the ambient term, and writes one line when the engine's fog changes between none and fogged;
//   (CAMFIX) the camera does not depend on the sun-shaped light draw - the light draws' camera copy (a second FrameReader, the same decode and verification) is taken at the frame's first light
//   draw that binds a camera table, before the sun hunt's exits (a taken sun draw, a rejected lamp, a draw of another shape), once a frame, under a budget that stands for both copies; the
//   readback folds the two reads through the one rule (the sun draw's copy first, each copy by its reader's own verification); a lamp's light never becomes the sun (the light draws' copy's light
//   is never read, the sun hold is fed inside the sun draw's branch); the sun and its colour are used only while the sun draw's copy is young (SunStands); the new reader and flag reset where the
//   sun draw's do; the staleness lines read the camera as the hold stands on it; a change of source writes one line through its own gate;
//   the registrations - feature bit 23, both light-capture lists, the pixel-shader seam, the five arming lists, the shader-creation stamp, the keys in
//   config, binder, ini and schema, the menu card's slot between the light bounce's and depth of field's.
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: atmo_pins <repo>\n");
        return 2;
    }
    const std::string repo = argv[1];
    const std::string rules = Code(repo, "ffxiv/engine/AtmoRules.h");
    const std::string draw = Code(repo, "ffxiv/engine/AtmoDraw.cpp");
    const std::string pass = Code(repo, "ffxiv/engine/AtmoPass.cpp");
    const std::string module = Code(repo, "ffxiv/engine/Atmo.h");
    const std::string menu = Code(repo, "ffxiv/engine/AtmoMenu.cpp");
    const std::string shader = Code(repo, "ffxiv/engine/shaders/Atmo.hlsl");
    const std::string props = Raw(repo, "ffxiv/engine/atmo.shaders.props");
    const std::string hooks = Code(repo, "ffxiv/engine/ChainHooks.h");
    const std::string marchDraw = Code(repo, "ffxiv/engine/GodraysDraw.cpp");
    const std::string giDraw = Code(repo, "ffxiv/engine/GiDraw.cpp");
    const std::string dofDraw = Code(repo, "ffxiv/engine/DofDraw.cpp");
    const std::string puddleDraw = Code(repo, "ffxiv/engine/PuddleToneDraw.cpp");
    const std::string shaftsDraw = Code(repo, "ffxiv/engine/ShaftsDraw.cpp");
    const std::string enginePass = Code(repo, "ffxiv/engine/EnginePass.cpp");
    const std::string seam = Code(repo, "ffxiv/lighting/FfxivLightingCapture.h");
    const std::string config = Code(repo, "ffxiv/FfxivConfig.h");
    const std::string binder = Code(repo, "core/ExtConfig.cpp");
    const std::string features = Code(repo, "ExtFeatures.h");
    const std::string layout = Code(repo, "core/MenuLayout.h");
    const std::string ini = Raw(repo, "ini/FFXIV.ini");
    const std::string schema = ReadFile(repo + "/tools/settings-schema.json");
    const std::string stageTable = Code(repo, "ffxiv/diag/StageDumpFormat.h");
    const std::string godraysRules = Code(repo, "ffxiv/engine/GodraysRules.h"); // the sky depth epsilon is the march's own constant, which the rules header reads
    const std::string godraysStamp = marchDraw; // the march's own file, read only to prove this stage's tag is not its tag

    EXPECT(!rules.empty() && !draw.empty() && !pass.empty() && !shader.empty() && !hooks.empty() && !giDraw.empty() && !enginePass.empty(),
           "every file the pins read is found\n");

    // ---- the chain position ----------------------------------------------------------------------------------------------------------
    {
        EXPECT(Has(hooks, "inlineChainSceneFnchainAtmoScene=nullptr;") && Has(hooks, "inlineToneOwnerFntoneMapAtmoOwner=nullptr;"),
               "the stage's two hooks have the chain's own types and are null until the stage registers\n");
        EXPECT(Has(hooks, "inlineChainSceneFnchainDofScene=nullptr;") && Has(hooks, "inlineToneOwnerFntoneMapDofOwner=nullptr;") &&
                   Has(hooks, "inlineChainSceneFnchainGiScene=nullptr;") && Has(hooks, "inlineToneOwnerFntoneMapGiOwner=nullptr;"),
               "depth of field's and the light bounce's hooks are untouched, beside the stage's two\n");
        EXPECT(Has(draw, "chainAtmoScene=&ext::ffxiv::engine::atmo::ChainScene;") && Has(draw, "toneMapAtmoOwner=&ext::ffxiv::engine::atmo::ToneMapOwner;"),
               "the stage registers both hooks at start-up\n");
        // The dispatchers: behind the night grade (INT1: the light bounce's dispatchers ask the night grade first, then fall to the night grade's own pair) the
        // atmosphere first, depth of field when the atmosphere is not linked, nothing otherwise.
        const std::string scene = Body(hooks, "inlineID3D11ShaderResourceView*chainAfterNightScene(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene)");
        EXPECT(Has(scene, "if(chainAtmoScene!=nullptr)returnchainAtmoScene(context,facts,scene);if(chainDofScene!=nullptr)returnchainDofScene(context,facts,scene);returnnullptr;"),
               "the texture dispatcher behind the night grade asks the atmosphere, then depth of field, then nothing (a build that links the night grade alone reaches depth of field)\n");
        EXPECT(Has(hooks, "inlinebooltoneMapAfterNightOwner(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene,ChainDrawFndraw,void*user)"
                          "{if(toneMapAtmoOwner!=nullptr)returntoneMapAtmoOwner(context,facts,scene,draw,user);if(toneMapDofOwner!=nullptr)returntoneMapDofOwner(context,facts,scene,draw,user);returnfalse;}"),
               "the owner dispatcher behind the night grade asks the atmosphere's owner turn, then depth of field's, else false (the caller draws the game's call once)\n");
        const std::string sceneGi = Body(hooks, "inlineID3D11ShaderResourceView*chainAfterGiScene(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene)");
        EXPECT(Has(sceneGi, "returnchainAfterNightScene(context,facts,scene);"),
               "and the light bounce's texture dispatcher falls to that one when the night grade is not linked (the atmosphere is still reached)\n");
        // The light bounce asks them through the dispatchers only (its file names neither stage's hooks).
        EXPECT(Count(giDraw, "chainAtmoScene") == 0 && Count(giDraw, "toneMapAtmoOwner") == 0 && Count(giDraw, "chainDofScene") == 0 && Count(giDraw, "toneMapDofOwner") == 0,
               "the light bounce's file names neither the atmosphere's hooks nor depth of field's: it asks the dispatchers\n");
        EXPECT(Count(giDraw, "chainAfterGiScene(") == 2 && Count(giDraw, "toneMapAfterGiOwner(") == 3, "and asks the dispatchers in exactly five places\n");
        // The earlier members' files do not name the stage.
        EXPECT(Count(marchDraw, "chainAtmoScene") == 0 && Count(marchDraw, "toneMapAtmoOwner") == 0 && Count(puddleDraw, "chainAtmoScene") == 0 &&
                   Count(shaftsDraw, "chainAtmoScene") == 0 && Count(dofDraw, "chainAtmoScene") == 0 && Count(dofDraw, "toneMapAtmoOwner") == 0,
               "no other stage's file names the atmosphere's hooks: the light bounce hands it the chain, and depth of field is handed the chain by it\n");
        // The stage composes depth of field on its own answer, in both roles, before any bind; every exit without an answer hands it the scene.
        const std::string chain = Body(draw, "ID3D11ShaderResourceView*ChainSceneInternal(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene)");
        EXPECT(!chain.empty(), "the stage's chained entry is found\n");
        EXPECT(Has(chain, "if(context==nullptr||scene==nullptr||!Armed())returnAskDof(context,facts,scene);"), "off: depth of field gets the texture the stage was handed\n");
        EXPECT(Has(chain, "if(state.haveAtmoFrame&&state.atmoFrame==state.frames)returnAskDof(context,facts,scene);"), "already answered this frame: the same\n");
        EXPECT(Has(chain, "if(ours!=nullptr){") && Has(chain, "ID3D11ShaderResourceView*composed=AskDof(context,facts,ours);returncomposed!=nullptr?composed:ours;}returnAskDof(context,facts,scene);}"),
               "answered: depth of field composes on the stage's answer and a null from it is the stage's answer unchanged; declined or threw: depth of field gets the scene\n");
        EXPECT(Ordered(chain, "StageAtToneMap(", "AskDof(context,facts,ours)"), "the stage runs first, depth of field composes on its answer (not the other way round)\n");
        EXPECT(Count(chain, "chainDof") == 0 && Count(draw, "chainDofScene(context,facts,scene)") == 1,
               "depth of field is asked through one helper (AskDof), whose one call site is the helper's own\n");
        EXPECT(Has(draw, "ID3D11ShaderResourceView*AskDof(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene){if(scene==nullptr||ext::ffxiv::engine::chainDofScene==nullptr)returnnullptr;"
                         "returnext::ffxiv::engine::chainDofScene(context,facts,scene);}"),
               "the helper asks depth of field's texture hook and answers null when it is not linked or has nothing\n");
        // The dispatch order.
        EXPECT(Has(enginePass, "if(ext::ffxiv::engine::gi::OnSeamDraw(context,anchors,draw,user))return;if(ext::ffxiv::engine::atmo::OnSeamDraw(context,anchors,draw,user))return;"
                               "if(ext::ffxiv::dof::OnSeamDraw(context,anchors,draw,user))return;") ||
                   (Ordered(enginePass, "ext::ffxiv::engine::gi::OnSeamDraw(", "ext::ffxiv::engine::atmo::OnSeamDraw(") &&
                    Ordered(enginePass, "ext::ffxiv::engine::atmo::OnSeamDraw(", "ext::ffxiv::dof::OnSeamDraw(")),
               "the dispatch order is shafts, puddles, march, light bounce, night grade, atmosphere, depth of field (the night grade's own pin and look_chain_order_pins hold the full order)\n");
        EXPECT(Count(enginePass, "ext::ffxiv::engine::atmo::OnSeamDraw(") == 1, "the stage's seam entry is called from exactly one place\n");
    }

    // ---- the fail-open contract: the owner role draws exactly once ----------------------------------------------------------------------------
    {
        const std::string runOwner = Body(draw, "boolRunToneMapAsOwner(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene,DrawFndraw,void*user)");
        EXPECT(!runOwner.empty(), "the owner role is found\n");
        EXPECT(Has(runOwner, "}catch(...){LOG_INFO(\"{}\",FormatDecline(Decline::Exception,Outcome::EngineOwn));if(!drew)draw(user);returntrue;}returnAskDofOwner(context,facts,scene,draw,user);}"),
               "a throw draws the game's call once when the stage has not drawn; a decline asks depth of field's owner turn AFTER the try block, true only if it drew, else false\n");
        EXPECT(Ordered(runOwner, "catch(...)", "returnAskDofOwner("), "so a throw after depth of field drew can never reach the stage's catch, which would draw the game's call a second time\n");
        EXPECT(Has(runOwner, "ID3D11ShaderResourceView*composed=AskDof(context,facts,ours);DrawWithBound(context,composed!=nullptr?composed:ours,draw,user);drew=true;returntrue;"),
               "the stage composes depth of field on its answer, then binds the result for the game's one draw\n");
        EXPECT(Count(draw, "draw(user);") == 3, "the game's call is drawn in exactly three places: the bind helper, and the two catches\n");
        EXPECT(Has(draw, "voidDrawWithBound(ID3D11DeviceContext*context,ID3D11ShaderResourceView*bind,DrawFndraw,void*user){") &&
                   Has(draw, "~Restore(){context->PSSetShaderResources(0,1,&view);if(view!=nullptr)view->Release();}"),
               "the owner's one bind puts the game's own view back afterwards, also when the draw throws\n");
        const std::string ownerInternal = Body(draw, "boolToneMapOwnerInternal(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene,DrawFndraw,void*user)");
        EXPECT(Has(ownerInternal, "if(context==nullptr||scene==nullptr||draw==nullptr||!Armed())returnAskDofOwner(context,facts,scene,draw,user);") &&
                   Has(ownerInternal, "if(state.haveAtmoFrame&&state.atmoFrame==state.frames)returnAskDofOwner(context,facts,scene,draw,user);"),
               "the owner entry's two early exits hand depth of field the owner turn\n");
        EXPECT(Has(draw, "boolAskDofOwner(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene,DrawFndraw,void*user){"
                         "if(ext::ffxiv::engine::toneMapDofOwner!=nullptr)returnext::ffxiv::engine::toneMapDofOwner(context,facts,scene,draw,user);returnfalse;}"),
               "depth of field's owner turn is asked on exactly the contract this entry carries (true only when it drew)\n");
        // A missing sun is never a decline, the stage runs on the ambient term (the god rays' gates are not inherited).
        EXPECT(!Has(rules, "NoSun") && !Has(draw, "NoSun") && !Has(draw, "LightGate") && !Has(draw, "JudgeLight") && !Has(draw, "NearestRayCosine") && !Has(draw, "LightAside"),
               "no decline names a sun, a shadow cascade or a beam gate\n");
        EXPECT(Has(draw, "if(!CameraStands(state.hold.stamps,state.frames)){NoteSkip(state,SkipReason::NoCamera);returnnullptr;}"
                         "if(!state.hold.haveFog){NoteSkip(state,SkipReason::NoFog);returnnullptr;}"),
               "the only things a frame needs are a camera and a fog state (both skips, neither a failure); the camera's age is the one rule's (CameraStands)\n");
        EXPECT(Has(draw, "boolhaveSun=false;if(sunStands&&hold.haveSun&&hold.haveView&&shafts::ViewDirectionOf(hold.view,hold.sunWorld,inputs.sunView))haveSun=true;") &&
                   Has(draw, "inputs.sunGain=SunGain(haveSun,haveLight,hold.luminance,elevation);"),
               "the sun is optional: without one the gain is 0 and the pass runs on the ambient term\n");
        EXPECT(Has(draw, "state.hold.engineDensity"), "the engine's own fog density reaches the pass's density (the engine-fog scale)\n");
        EXPECT(Has(draw, "inputs.densityPerMetre=EffectiveDensityPerMetre(state.params,hold.engineDensity,hold.luminance,hold.haveLuminance);") &&
                   Has(draw, "constfloatdayScale=NightDensityScale(state.params.nightDensity,hold.luminance,hold.haveLuminance);"),
               "the density is the key times the engine-fog scale times the night scale, all three through the one rule, and the night scale is on the held luminance (the time of day, not the zone: it does not age with the sun)\n");
        // The same shape refusals as the chain's other stages; the scene HDR decline; the replay refusal; the stand-down.
        EXPECT(Has(draw, "shafts::SceneHdrInactive(state.enabled,ext::hdr::FfxivSceneHdr::Requested())") && Has(draw, "Decline::SceneHdr") && Has(draw, "Decline::CharaReplay"),
               "the scene HDR decline and the character-preview replay refusal\n");
        EXPECT(Has(draw, "if(CountsTowardsStandDown(reason)&&state.streak.NoteDecline()&&!state.loggedOff)") && Has(draw, "ext::SetActive(ext::F::Atmosphere,false);"),
               "sixty-four counted refusals in a row switch the stage off and take its feature bit back\n");
        EXPECT(Has(draw, "state.streak.NoteRun();"), "a run starts the streak over\n");
    }

    // ---- the key off is a byte-exact passthrough, the pass's own contract -------------------------------------------------------------------------
    {
        EXPECT(Has(pass, "if(!Wanted(true,inputs.params)||!(inputs.densityPerMetre>0.0f)){context->CopyResource(_scene.Get(),sceneResource.Get());return_sceneSrv.Get();}"),
               "a stage with no medium, no light or no density answers the byte-exact copy it made and draws nothing\n");
        EXPECT(Has(pass, "D3D11_ASYNC_GETDATA_DONOTFLUSH") && !Has(pass, "D3D11_MAP_READ"), "the pass's GPU timing is read without flushing and never waits (no map)\n");
        EXPECT(Has(pass, "ext::core::explicit_state::GuardstateGuard(context,context1.Get(),device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1,kSpec);stateGuard.Neutralise();"),
               "the game's own state is saved and restored by the explicit-state guard around the draw\n");
        EXPECT(Has(pass, "constexprext::core::explicit_state::SpeckSpec{2,0,1};") && Has(pass, "constexpruint32_tkSlotDepth=0;") && Has(pass, "constexpruint32_tkSlotScene=1;"),
               "the explicit-state spec declares the pass's two pixel-shader slots and one constant buffer\n");
        EXPECT(Count(pass, "context->Draw(3,0);") == 1 && Has(pass, "context->CopyResource(_scene.Get(),sceneResource.Get());"), "one draw, over a copy of the scene\n");
        EXPECT(Has(pass, "ParamsBuffer)==12*16") && Has(shader, "float4SunTint;"), "the constant buffer is twelve rows on both sides\n");
        EXPECT(Has(draw, "if(!Wanted(state.enabled,state.params)||!state.streak.Armed())returnfalse;"), "the stage's own arming is the key, the medium and the streak\n");
    }

    // ---- the shader, operation for operation --------------------------------------------------------------------------------------------------
    {
        const std::string ps = Body(shader, "float4PS_Atmo(float4position:SV_Position):SV_Target0");
        EXPECT(!ps.empty(), "the pixel shader is found\n");
        // The sky exclusion comes before any reconstruction (mutant: sky exclusion dropped).
        EXPECT(Has(ps, "if(!(depth==depth)||!(depth>kSkyDepthEpsilon))returnscene;"), "the far plane and a NaN depth return the scene's own texel\n");
        EXPECT(Ordered(ps, "!(depth>kSkyDepthEpsilon)", "dot(InvProjRow0,clip)"), "and they do so before the position is reconstructed\n");
        // The start distance gates the optical depth (mutant: start ignored): nothing nearer is touched, the depth integral begins at it.
        EXPECT(Has(ps, "if(!(distance==distance)||!(distance>Medium.z)||!(distance>0.0))returnscene;") && Has(ps, "OpticalDepth(Medium.x,Medium.y,dot(direction,UpView.xyz),Medium.z,distance)"),
               "a surface nearer than the start distance returns the scene's own texel and the optical depth runs from the start\n");
        EXPECT(Ordered(ps, "!(distance>Medium.z)", "OpticalDepth("), "the start distance is tested before the optical depth is computed\n");
        // The transmittance is applied once, in linear light (mutants: T twice; the mix in stored space).
        EXPECT(Count(ps, "transmittance") == 4 && Has(ps, "constfloattransmittance=exp(-min(tau,kMaxOpticalDepth));") && Has(ps, "if(!(transmittance<1.0))returnscene;"),
               "one transmittance, computed once from the optical depth, and a medium that does not touch the ray returns the texel as it was\n");
        EXPECT(Has(ps, "constfloat3mixed=stored*stored*transmittance+max(air,float3(0.0,0.0,0.0))*(1.0-transmittance);returnfloat4(sqrt(max(mixed,float3(0.0,0.0,0.0))),scene.a);"),
               "out = scene^2 * T + air * (1 - T) in linear light, stored back as its root, alpha untouched\n");
        EXPECT(Has(ps, "constfloat3ambient=NeutralAmbient(FogColour.rgb,Light.z);constfloat3air=Light.x*ambient+Light.y*SunView.w*phase*SunTint.rgb;"),
               "(L2b) the air is the neutralised ambient term plus the sun's lobe scaled by the sun's gain (0 without a sun)\n");
        // The neutral blend is the rules' arithmetic: negative channels are no light, the grey is the same Rec. 709 luminance (the tree's constants), the share is clamped.
        EXPECT(Has(shader, "float3NeutralAmbient(float3fogColour,floatneutral){constfloat3colour=max(fogColour,float3(0.0,0.0,0.0));constfloatgrey=dot(colour,float3(0.2126390059,0.7151686788,0.0721923154));"
                           "constfloatshare=clamp(neutral,0.0,1.0);returncolour*(1.0-share)+grey*share;}") &&
                   Has(rules, "constfloatgrey=shafts::Luminance(colour);") && Has(rules, "out[i]=colour[i]*(1.0f-share)+grey*share;") && Has(godraysRules + Code(repo, "ffxiv/engine/ShaftsRules.h"), "kLumaR=0.2126390059f;") &&
                   Has(Code(repo, "ffxiv/engine/ShaftsRules.h"), "kLumaG=0.7151686788f;") && Has(Code(repo, "ffxiv/engine/ShaftsRules.h"), "kLumaB=0.0721923154f;"),
               "(L2b) the shader's neutral blend and the rules' are the same arithmetic on the same luminance weights\n");
        EXPECT(Has(pass, "Set(params.light,inputs.params.skyStrength,inputs.params.sunStrength*inputs.whiteLinear,inputs.params.neutral,0.0f);") &&
                   Has(Raw(repo, "ffxiv/engine/shaders/Atmo.hlsl"), "z = the neutral share"),
               "(L2b) the neutral key travels in Light.z (the layout stays twelve rows)\n");
        // The lobe's sign (mutant: g flipped): the denominator is (1-g)^2 + 2 g (1 - c), exactly the rules'.
        EXPECT(Has(shader, "constfloatdenominator=oneMinus*oneMinus+2.0*gg*(1.0-c);") && Has(rules, "constfloatdenominator=oneMinus*oneMinus+2.0f*gg*(1.0f-c);"),
               "the lobe's denominator is the same on both sides and its sign is the forward lobe's\n");
        EXPECT(Has(shader, "returnoneMinus*oneMinus*oneMinus/pow(max(denominator,1.0e-6),1.5);"), "normalised to 1 at the light\n");
        // The constants the two files share.
        for (const char* name : { "kSkyDepthEpsilon", "kMaxDensityGain", "kMaxFallExponent", "kMaxOpticalDepth" })
        {
            const double inShader = ConstantOf(shader, std::string("staticconstfloat") + name);
            const double inRules = ConstantOf(std::string(name) == "kSkyDepthEpsilon" ? godraysRules : rules, std::string("inlineconstexprfloat") + name);
            EXPECT(inShader == inShader && inRules == inRules && std::fabs(inShader - inRules) <= 1.0e-12 * std::fabs(inRules) + 1.0e-30,
                   "the shader's %s equals the rules' (%g vs %g)\n", name, inShader, inRules);
        }
        EXPECT(Has(shader, "constfloatf0=clamp(-a*start,-kMaxFallExponent,kMaxDensityGain);") && Has(rules, "constfloatf0=ClampTo(-a*start,-kMaxFallExponent,kMaxDensityGain,0.0f);") &&
                   Has(shader, "constfloatx=clamp(a*len,-kMaxDensityGain,kMaxFallExponent);") && Has(rules, "constfloatx=ClampTo(a*len,-kMaxDensityGain,kMaxFallExponent,0.0f);"),
               "the optical depth's exponents are clamped the same way on both sides\n");
        EXPECT(Has(shader, "constfloattau=rho0*exp(f0)*len*mean;") && Has(rules, "constfloattau=rho0*std::exp(f0)*len*mean;"), "and the integral is the same product\n");
        EXPECT(!Has(shader, "#include") && !Has(shader, "Sample(") && !Has(shader, "SamplerState"), "the shader includes nothing and samples nothing: every read is a Load\n");
        EXPECT(Has(props, "Atmo_Vs_cso") && Has(props, "Atmo_Ps_cso") && Has(props, "PS_Atmo") && Has(props, "shaders\\Atmo.hlsl"), "the build recipe compiles the two entry points\n");
    }

    // ---- the stamping contract and the stamp ----------------------------------------------------------------------------------------------------
    {
        for (const char* forbidden : { "godrays::Armed", "gi::Armed", "dof::Armed", "chainGiScene", "toneMapGiOwner", "chainMarchScene", "toneMapMarchOwner", "toneMapGiOwner" })
            EXPECT(!Has(draw, forbidden), "the stage's sources never read an earlier stage's arming or hooks (%s)\n", forbidden);
        EXPECT(Has(draw, "uint64_tatmoFrame=0;") && Has(draw, "boolhaveAtmoFrame=false;"), "the stage's answered-tracking is its own rendered-frame counter\n");
        EXPECT(Has(draw, "if(state.step.OpenAtComposite(state.frames)){state.attempts=0;state.sunDrawTaken=false;state.cameraTaken=false;}") && Count(draw, "OpenAtComposite(") == 1 && !Has(draw, "AdvanceFrame("),
               "the counter opens at the stage's own composite capture, once, and is never advanced by the stage itself\n");
        // The profile stamp's own tag: not the march's.
        EXPECT(Has(draw, "constexprGUIDkProfileStampTag{0x6d2f8a14,0x9e37,0x4c51,"), "the stage's profile stamp has its own tag\n");
        EXPECT(!Has(draw, "0x3b9a41d7") && Has(godraysStamp, "constexprGUIDkProfileStampTag{0x3b9a41d7,"), "and it is not the march's (the march's file is read only, to prove it)\n");
        EXPECT(Has(draw, "packed=godrays::PackProfileSlot(slot);") && Has(draw, "shader->SetPrivateData(kProfileStampTag,sizeof packed,&packed);") ||
                   Has(draw, "shader->SetPrivateData(kProfileStampTag,sizeofpacked,&packed);"),
               "the stamp carries the slot the shader's own reflection names cExpHeightFogParam at\n");
        EXPECT(Has(enginePass, "ext::ffxiv::engine::atmo::NoteShader(shader,code,size);") && Count(enginePass, "atmo::NoteShader(") == 1,
               "the stamp is written at shader creation, once, whatever the key says\n");
        EXPECT(Has(draw, "constuint32_tstamped=BoundProfileSlot(context);constuint32_tprofileSlot=godrays::ProfileSlotFor(stamped,boundBytes);"),
               "the fog post's profile is the buffer bound at the stamped slot\n");
        EXPECT(Has(draw, "godrays::FrameReaderframe;") && Has(draw, "godrays::FogReaderfog;") && Has(draw, "shafts::ToneReadertone;") && Has(draw, "godrays::StolenViewsviews;"),
               "the stage's readers are its own instances (the march's types, never its state)\n");
        EXPECT(Has(draw, "state.views.KeepDepth(") && !Has(draw, "SecureSky(") && !Has(draw, "SecureLut(") && !Has(draw, "SecureAtlas("),
               "only the depth is kept: the sky texture and the fog LUT are not used (the engine's fog colour is the ambient term)\n");
    }

    // ---- (L2b) the lobe follows the sky's visible sun disc, through the one rule -------------------------------------------------------------------
    {
        EXPECT(Has(draw, "shafts::SunUvReadersunUv;") && Has(draw, "boolhaveSunUv=false;") && Has(draw, "floatsunUvRow[4]{};") && Has(draw, "uint64_tsunUvFrame=0;"),
               "the stage has its own sun-UV reader and holds the row it read with the frame it was written at\n");
        EXPECT(Has(draw, "if(IsShaftsSunParamAnchors(anchors)){CaptureSunParam(context,anchors,state);returnfalse;}"),
               "the sky's own sun draw (cSunParam) is copied and passed on untouched, in this stage's own seam entry (whether or not the shafts are on)\n");
        EXPECT(Has(draw, "state.sunUv.Copy(context,params.Get(),state.frames);") && Has(draw, "if(desc.ByteWidth<shafts::kSunParamBytes||desc.ByteWidth>0x100)return;"),
               "the copy is the row's 16 bytes from a buffer of the engine's size\n");
        EXPECT(Has(draw, "constshafts::SunUvReaduv=state.sunUv.Read(context,state.frames);if(uv.landed&&shafts::SunParamUvUsable(uv.uv)){"),
               "each tone map reads the newest copy without waiting and keeps it only when the row is this build's layout\n");
        EXPECT(Has(draw, "shafts::SunUvFresh(hold.haveSunUv,hold.sunUvFrame,state.frames)"), "the disc is used only while its row is fresh (the shafts' own freshness rule)\n");
        EXPECT(Has(draw, "ChooseSunDirection(inputs.sunView,haveSun,discView,discFresh,inputs.upView)") && Has(draw, "SunDirectionFromUv(hold.inverseProjection,hold.sunUvRow[2],hold.sunUvRow[3],discView)"),
               "the lobe's direction goes through the rules' one choice, the disc made by the rules' one projection from the held camera\n");
        EXPECT(Has(draw, "FormatSunDisc(") && Has(draw, "run.lobeOnDisc=state.lastLobeOnDisc;"), "one line says the lobe follows the disc, and the run line says which the lobe is on\n");
        EXPECT(Count(draw, "state.sunUv.Reset();") >= 1 && Count(draw, "haveSunUv=false") >= 1, "the session's teardown forgets the reader's copies and the held row\n");
    }

    // ---- the registrations ----------------------------------------------------------------------------------------------------------------------
    {
        EXPECT(Has(features, "Atmosphere=1ull<<23,"), "the feature bit is 23 (22 is the lens flare's)\n");
        EXPECT(Count(features, "ActiveAny(") == 1, "ExtFeatures.h holds the ONE variadic ActiveAny: a consumer appends its flag and never adds an overload\n");
        EXPECT(Count(seam, "ext::F::Atmosphere") == 2, "both light-capture lists carry the stage's bit (the stage's light draws reach it)\n");
        EXPECT(Has(enginePass, "if(!ext::ActiveAny(ext::F::EnginePassOverrides,ext::F::LightShafts,ext::F::GodraysVolumetric,ext::F::Gi,ext::F::DepthOfField,ext::F::LensFlare,ext::F::Atmosphere,ext::F::NightGrade))"),
               "the pixel-shader seam tracks the anchors while the stage's key is on (the night grade's bit joined behind it)\n");
        EXPECT(Has(enginePass, "if(ext::Active(ext::F::Atmosphere))ext::ffxiv::engine::atmo::SampleSceneHdr();"), "the stage samples the scene HDR state on a pixel-shader bind\n");
        EXPECT(Count(enginePass, "atmo::Armed()") == 5, "the draw path's five arming lists carry the stage's own arming\n");
        EXPECT(Has(enginePass, "ext::ffxiv::engine::atmo::Release();"), "the engine's teardown releases the stage's device objects\n");
        EXPECT(Has(draw, "ext::SetActive(ext::F::Atmosphere,wanted);") && Has(draw, "ext::FfxivConfig::atmosphereOnLoaded=&ext::ffxiv::engine::atmo::OnConfigLoaded;"),
               "the stage publishes its own bit and registers its config hook\n");
        EXPECT(Has(draw, "ext::ffxiv::diag::StageRegister(ext::ffxiv::diag::StageId::Atmosphere,"), "and its stage-dump probe\n");
        EXPECT(Has(stageTable, "Atmosphere,") && Has(stageTable, "{\"atmosphere\",StageGroup::Chain,true},") && Ordered(stageTable, "Flare,", "Atmosphere,") && Ordered(stageTable, "Atmosphere,", "Count"),
               "the stage's dump id is appended after the flare card, so 10-dof and 11-flare keep their numbers\n");
        // The keys, in config, binder, ini and schema, with the rules' defaults.
        const struct { const char* member; const char* key; const char* rulesDefault; const char* configDefault; } keys[] = {
            { "atmosphereDensity", "AtmosphereDensity", "kDefaultDensity=1.5f", "atmosphereDensity{1.5f}" },
            { "atmosphereHeightFalloff", "AtmosphereHeightFalloff", "kDefaultFalloff=0.01f", "atmosphereHeightFalloff{0.01f}" },
            { "atmosphereStart", "AtmosphereStart", "kDefaultStart=50.0f", "atmosphereStart{50.0f}" },
            { "atmospherePhaseG", "AtmospherePhaseG", "kDefaultPhaseG=0.7f", "atmospherePhaseG{0.7f}" },
            { "atmosphereSunStrength", "AtmosphereSunStrength", "kDefaultSunStrength=0.3f", "atmosphereSunStrength{0.3f}" },
            { "atmosphereSkyStrength", "AtmosphereSkyStrength", "kDefaultSkyStrength=1.0f", "atmosphereSkyStrength{1.0f}" },
            { "atmosphereNightDensity", "AtmosphereNightDensity", "kDefaultNightDensity=0.1f", "atmosphereNightDensity{0.1f}" },
            { "atmosphereNeutral", "AtmosphereNeutral", "kDefaultNeutral=0.6f", "atmosphereNeutral{0.6f}" },
        };
        EXPECT(Has(config, "CustomOptional<bool>atmosphere{true};") && Has(binder, "b.Add(\"FFXIV\",\"Atmosphere\",atmosphere);") && Has(rules, "constexprboolkDefaultEnabled=true;"),
               "(L2b) the switch is ON by default (config and rules say so) and bound\n");
        EXPECT(Has(schema, "\"key\": \"[FFXIV] Atmosphere\"") && Has(ini, "\nAtmosphere=auto"), "and in the schema and the ini fragment\n");
        for (const auto& key : keys)
        {
            EXPECT(Has(config, std::string("CustomOptional<float>") + key.configDefault), "config default of %s equals the rules'\n", key.key);
            EXPECT(Has(rules, key.rulesDefault), "the rules carry the default of %s\n", key.key);
            EXPECT(Has(binder, std::string("b.Add(\"FFXIV\",\"") + key.key + "\"," + key.member + ");"), "binder %s\n", key.key);
            EXPECT(Has(schema, std::string("\"key\": \"[FFXIV] ") + key.key + "\""), "schema %s\n", key.key);
            EXPECT(Has(ini, std::string("\n") + key.key + "=auto"), "ini %s\n", key.key);
            EXPECT(Has(draw, std::string("cfg.") + key.member + ".value_or_default()"), "the stage reads %s\n", key.key);
            EXPECT(Has(menu, std::string("cfg.") + key.member), "the card controls %s\n", key.key);
        }
        EXPECT(Has(config, "static inline BoolHook atmosphereOnLoaded=nullptr;") || Has(config, "staticinlineBoolHookatmosphereOnLoaded=nullptr;"), "the config hook defaults to null\n");
        EXPECT(Has(config, "if(atmosphereOnLoaded!=nullptr)atmosphereOnLoaded(atmosphere.value_or_default());"), "and is called at every config load\n");
        EXPECT(Has(menu, "Atmosphere(FFXIV)") && Has(menu, "ffxiv_engine_passes") && Has(menu, "FormatCard"), "the card's title, group and state line\n");
        EXPECT(Has(layout, "{\"ffxiv_atmosphere\",\"FFXIV\",\"ffxiv_engine_light\"},") && Has(layout, "{\"ffxiv_dof\",\"FFXIV\",\"ffxiv_atmosphere\"},"),
               "the card sits between the night grade's (which follows the light bounce's) and depth of field's (the chain's order)\n");
        EXPECT(Has(Code(repo, "core/MenuManifest.h"), "\"ffxiv_atmosphere\""), "and is FFXIV-only\n");
        EXPECT(Has(module, "boolOnSeamDraw(ID3D11DeviceContext*context,Anchorsanchors,DrawFndraw,void*user);") && Has(module, "voidNoteShader(ID3D11DeviceChild*shader,constvoid*code,size_tsize);"),
               "the module's interface carries the seam entry and the stamp\n");
    }

    // ---- FOGFIX: the one shared fog reader, the colour, the zero profile ------------------------------------------------------------
    {
        const std::string readback = Body(draw, "voidReadback(ID3D11DeviceContext*context,State&state)");
        EXPECT(!readback.empty(), "the read-back is found\n");
        EXPECT(Has(draw, "godrays::FogReaderfog;") && Has(draw, "state.fog.Copy(context,fogParam.Get(),profile,viewportParam,state.frames);"),
               "the stage's fog ring is the god rays' own reader class, copied at the fog post\n");
        EXPECT(Has(readback, "constgodrays::FogReadfogRead=state.fog.Read(context,state.frames);"), "and read through its Read, whose profile check is the god rays'\n");
        for (const char* forbidden : { "ProfileRowUsable", "ProfileFailedField", "mixScale", "kMaxProfileDensity", "FogStateUsable(" })
            EXPECT(!Has(draw, forbidden), "the stage has no fog check of its own: %s is not named in its file\n", forbidden);
        EXPECT(Has(readback, "if(fogRead.usable){hold.fog=fogRead.fog;hold.haveFog=true;}"), "a fog state is taken only from a usable read\n");
        EXPECT(Has(readback, "elseif(!state.loggedFogUnusable)"), "and an unusable read leaves the last good state held (one line says so)\n");
        // The engine's density at the camera comes from the profile and nothing else: no gate on the mix scale, none on the weather, so a zero profile is 0.
        EXPECT(Has(readback, "if(hold.haveFog){hold.engineDensity=godrays::FogDensityAt(hold.fog,hold.cameraWorldY,1.0f);"), "the engine's density at the camera is the profile's density at the camera's height\n");
        EXPECT(Has(draw, "constfloatengineScale=EngineFogScale(hold.engineDensity);"), "and the engine fog scale is the rule's, of that density\n");
        // The colour: the ambient term's source is the engine's fog colour (cFogParam row 0), held with the state, handed to the pass as it is.
        EXPECT(Has(draw, "inputs.fogColour[i]=hold.fog.fogColor[i];"), "the pass is handed the engine's own fog colour\n");
        EXPECT(Has(rules, "NeutralAmbient(fogColour,params.neutral,ambient);") && Has(rules, "params.skyStrength*ambient[i]"), "and the in-scatter's ambient term is that colour, neutralised by the key\n");
        // The change line: the engine's fog going between none and fogged is written once per change (its own gate), from the rules' format.
        EXPECT(Has(readback, "godrays::EngineHasNoHeightFog(hold.fog)") && Has(readback, "FormatEngineFog("), "a change of the engine's fog between none and fogged writes the rules' line\n");
        EXPECT(Has(draw, "LogGatefogLog;") && Has(readback, "state.fogLog.Take("), "through its own gate, so the run and skip lines do not share its budget\n");
    }

    // ---- CAMFIX: the camera does not depend on the sun-shaped light draw ----------------------------------------------------------------
    {
        const std::string readback = Body(draw, "voidReadback(ID3D11DeviceContext*context,State&state)");
        const std::string capture = Body(draw, "voidCaptureLightDraw(ID3D11DeviceContext*context,Anchorsanchors,State&state)");
        const std::string fold = Body(rules, "constexprCameraFoldFoldCameraSources(constHoldStamps&held,CameraCopysunDraw,CameraCopylightDraw,uint64_tframes)noexcept");
        EXPECT(!readback.empty() && !capture.empty() && !fold.empty(), "the readback, the light-draw capture and the fold rule are found\n");
        // The rule: the sun draw's copy first, the light draws' copy second, both only when verified; only the sun draw's copy refreshes the sun's stamp.
        EXPECT(Has(fold, "if(sunDraw.landed&&sunDraw.verified){fold.source=CameraSource::SunDraw;") && Has(fold, "elseif(lightDraw.landed&&lightDraw.verified){fold.source=CameraSource::LightDraw;") &&
                   Ordered(fold, "sunDraw.landed&&sunDraw.verified", "lightDraw.landed&&lightDraw.verified"),
               "the sun draw's copy is the first choice and the light draws' one the second, each only when it landed with a camera this build reads\n");
        EXPECT(Count(fold, "sunDrawFrame=frames;") == 1 && Count(fold, "cameraFrame=frames;") == 2, "the sun's stamp is refreshed by the sun draw's copy alone; both copies refresh the camera's\n");
        EXPECT(ConstantOf(rules, "kMaxCameraAge") == 180.0 && !Has(draw, "kMaxCameraAge="), "the age a camera and a sun stand for is the rules' one constant (180 frames)\n");
        // The readback folds the two reads through the rule, from the verified flags of the two readers.
        EXPECT(Has(readback, "constCameraFoldfold=FoldCameraSources(hold.stamps,CameraCopy{frameRead.landed,frameRead.landed&&frameRead.camera},"
                             "CameraCopy{lightDrawRead.landed,lightDrawRead.landed&&lightDrawRead.camera},state.frames);hold.stamps=fold.stamps;"),
               "the frame's camera copy is chosen by the one rule, each copy by its reader's own verification (ProjectionOk and ViewProjectionOk inside FrameReader)\n");
        EXPECT(Has(readback, "if(fold.source==CameraSource::LightDraw){std::memcpy(hold.inverseProjection,lightDrawRead.inverseProjection,") &&
                   Has(readback, "if(fold.source==CameraSource::SunDraw){std::memcpy(hold.inverseProjection,frameRead.inverseProjection,"),
               "each source folds its own copy's camera rows into the hold\n");
        // A lamp's light must never become the sun: nothing of the light draws' copy's light is read, and the sun hold is fed inside the sun draw's branch only.
        EXPECT(!Has(draw, "lightDrawRead.facts") && !Has(draw, "lightDraws.LastLight"), "the light draws' copy's light is never read (a lamp never becomes the sun)\n");
        EXPECT(Ordered(readback, "if(fold.source==CameraSource::SunDraw){", "ClassifyLight(frameRead.facts") && Ordered(readback, "ClassifyLight(frameRead.facts", "hold.sunWorld,world"),
               "the sun hold is fed from the sun draw's read, inside the sun draw's branch\n");
        // The capture: the light draws' copy is taken before the sun hunt's exits (a taken sun draw, a rejected lamp, a draw of another shape), once a frame, and the budget stands for both.
        EXPECT(Has(capture, "if((state.sunDrawTaken&&state.cameraTaken)||state.attempts>=kMaxCaptureAttempts)return;++state.attempts;"), "the capture budget and the early exit stand for both copies\n");
        EXPECT(Has(capture, "if(!state.cameraTaken){") &&
                   Has(capture, "if(lightDesc.ByteWidth>=shafts::kLightBytes&&state.lightDraws.Copy(context,light.Get(),camera.Get(),state.frames))state.cameraTaken=true;"),
               "the light draws' camera copy is taken once a frame, at the first light draw that binds a camera table\n");
        EXPECT(Has(capture, "lightDesc.ByteWidth>=shafts::kLightBytes"), "and only from a light buffer the reader's light window fits in\n");
        EXPECT(Ordered(capture, "state.lightDraws.Copy(", "if(state.sunDrawTaken)return;") && Ordered(capture, "state.lightDraws.Copy(", "RejectedStill(") && Ordered(capture, "state.lightDraws.Copy(", "shafts::IsSunDraw("),
               "the light draws' copy comes before the taken-sun, rejected-lamp and sun-shape exits, so a lamp's draw and a frame with no sun-shaped draw still give a camera\n");
        // The sun stands for a bounded time after the sun draw's copy, and its stamp rides every use of it.
        EXPECT(Has(draw, "constboolsunStands=SunStands(hold.stamps,state.frames);constboolhaveLight=sunStands&&hold.haveLuminance;"), "the sun and the light's colour are used only while the sun draw's copy is young enough\n");
        // The resets: the new reader and its taken flag go wherever the sun draw's do.
        EXPECT(Count(draw, "state.lightDraws.Reset();") == 2 && Count(draw, "state.frame.Reset();") == 2, "the light draws' reader is reset with the sun draw's reader (disarm, release)\n");
        EXPECT(Count(draw, "state.cameraTaken=false;") == 2 && Count(draw, "state.sunDrawTaken=false;") >= 2, "its taken flag resets at the frame's composite and with the session\n");
        // The staleness lines read the camera as the hold stands on it, not one ring.
        EXPECT(Has(readback, "state.frame.Reads()==0&&state.lightDraws.Reads()==0&&(state.frame.NotReady()>=kReadbackStallFrames||state.lightDraws.NotReady()>=kReadbackStallFrames)") &&
                   Has(readback, "hold.stamps.haveCamera&&state.frames>=hold.stamps.cameraFrame&&state.frames-hold.stamps.cameraFrame>kMaxReadbackAge"),
               "'never started landing' needs neither ring to have read, 'stopped landing' is the hold's camera not refreshed by either copy\n");
        EXPECT(Has(draw, "LogGatesourceLog;") && Has(readback, "state.sourceLog.Take(uint32_t(fold.source))") && Has(readback, "FormatCameraSource(fold.source)"),
               "a change of the camera's source writes the rules' line through its own gate\n");
    }

    if (g_failures)
    {
        std::printf("atmo_pins: %d pin(s) failed\n", g_failures);
        return 1;
    }
    std::printf("atmo_pins: all pins hold\n");
    return 0;
}
