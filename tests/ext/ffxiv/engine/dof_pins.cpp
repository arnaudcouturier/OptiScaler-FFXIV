// Depth of field, Route B: the wiring pins, read as text from the sources (no device), the march's own pins' shape.
//
// What a wrong wiring would break, and what is pinned here:
//   DoF answers with the march AND GI off - the march and the light bounce are off by default and return early in several places each; the
//   stage hangs inside the light bounce's entries (one link further than the bounce hangs inside the march's), so every exit of those
//   entries that has no answer hands the stage the texture it was given (or, for the owner entries, the game's scene), and the seam's
//   early exits fall through to the dispatch's next member, which is the stage;
//   the game's tone-map draw runs exactly once on every path - an owner hook returns true only when it drew, the callers draw once otherwise,
//   a declining owner never draws, a throw cannot make an earlier stage draw a second time after the stage drew, and the chained entries
//   never draw;
//   the hold semantics - the chained decline's own wording leaves the chain's hold standing, the owner's leaves the game's draw to the
//   caller;
//   the stamping contract - the stage's sources never reference the shared stamp's names, and the stage's own
//   answered-tracking is its own rendered-frame counter (dofFrame), never the chain's shared state;
//   the timing flags - a staging Map without D3D11_MAP_FLAG_DO_NOT_WAIT waits for the GPU on the render thread, and a
//   GetData without D3D11_ASYNC_GETDATA_DONOTFLUSH flushes the pipe; both are pinned in the flags argument;
//   the dispatch order - the shafts, the puddle's stage, the march's entry, then this stage's entry;
//   the feature bit 21 (19 is the chara ambient's, 20 the light bounce's), the six keys in config, binder, ini and schema, and
//   the chain hooks defaulting to null;
// No test here assumes a frame whose puddle stage ended in the game's own draw (the shape refusal, the dry skip, the no-answer case)
// reaches this stage: those three paths do not ask the chain's later members.
//   the fail-open contract: the scene-HDR decline, the replay refusal, the stand-down, the explicit state save and restore.
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

size_t At(const std::string& text, const std::string& needle) { return text.find(needle); }

// The body of the function whose (whitespace-free) signature text is `signature`, braces included; empty when it is not there.
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

bool Before(const std::string& text, const std::string& first, const std::string& second)
{
    const size_t firstAt = text.find(first);
    const size_t secondAt = text.find(second, firstAt + first.size());
    return firstAt != std::string::npos && secondAt != std::string::npos;
}

// The number a whitespace-free code text assigns to the float constant `name` ("...floatkDeadZone=0.12f;" or "...floatkDeadZone=0.12;"),
// or NaN when it is not defined there exactly once.
double ConstantOf(const std::string& code, const std::string& name)
{
    const std::string needle = "float" + name + "=";
    const size_t at = code.find(needle);
    if (at == std::string::npos || code.find(needle, at + needle.size()) != std::string::npos)
        return std::nan("");
    return std::strtod(code.c_str() + at + needle.size(), nullptr); // stops at the 'f' or the ';'
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: dof_pins <repo>\n");
        return 2;
    }
    const std::string repo = argv[1];
    const std::string rules = Code(repo, "ffxiv/engine/DofRules.h");
    const std::string draw = Code(repo, "ffxiv/engine/DofDraw.cpp");
    const std::string camera = Code(repo, "ffxiv/engine/DofCamera.cpp");
    const std::string cameraHeader = Code(repo, "ffxiv/engine/DofCamera.h");
    const std::string pass = Code(repo, "ffxiv/engine/DofPass.cpp");
    const std::string passHeader = Code(repo, "ffxiv/engine/DofPass.h");
    const std::string hooks = Code(repo, "ffxiv/engine/ChainHooks.h");
    const std::string module = Code(repo, "ffxiv/engine/Dof.h");
    const std::string menu = Code(repo, "ffxiv/engine/DofMenu.cpp");
    const std::string shader = ReadFile(repo + "/OptiScaler/ext/ffxiv/engine/shaders/Dof.hlsl");
    const std::string shaderCode = Code(repo, "ffxiv/engine/shaders/Dof.hlsl");
    const std::string marchDraw = Code(repo, "ffxiv/engine/GodraysDraw.cpp");
    const std::string giDraw = Code(repo, "ffxiv/engine/GiDraw.cpp");
    const std::string puddleDraw = Code(repo, "ffxiv/engine/PuddleToneDraw.cpp");
    const std::string rawHooks = Raw(repo, "ffxiv/engine/ChainHooks.h");
    const std::string shaftsDraw = Code(repo, "ffxiv/engine/ShaftsDraw.cpp");
    const std::string enginePass = Code(repo, "ffxiv/engine/EnginePass.cpp");
    const std::string enginePassRules = Code(repo, "ffxiv/engine/EnginePassRules.h");
    const std::string seam = Code(repo, "ffxiv/lighting/FfxivLightingCapture.h");
    const std::string config = Code(repo, "ffxiv/FfxivConfig.h");
    const std::string binder = Code(repo, "core/ExtConfig.cpp");
    const std::string features = Code(repo, "ExtFeatures.h");
    const std::string ini = Raw(repo, "ini/FFXIV.ini");
    const std::string layout = Code(repo, "core/MenuLayout.h");
    // Raw (comments kept) copies, for pins on wording that lives in comments or string literals with spaces.
    const std::string rawRules = ReadFile(repo + "/OptiScaler/ext/ffxiv/engine/DofRules.h");
    const std::string rawShader_RS = shader;

    // ---- the chain's hooks: names and shapes, and where the stage hangs ------------------------------------------------------------
    {
        EXPECT(Has(hooks, "inlineChainSceneFnchainDofScene=nullptr;"), "the stage's texture hook has the chain's hook type and is null until the stage registers\n");
        EXPECT(Has(hooks, "inlineToneOwnerFntoneMapDofOwner=nullptr;"), "the stage's owner hook has the chain's owner type and is null until the stage registers\n");
        EXPECT(Has(hooks, "inlineChainSceneFnchainGiScene=nullptr;") && Has(hooks, "inlineToneOwnerFntoneMapGiOwner=nullptr;"),
               "the light bounce's two hooks are untouched, beside the stage's two\n");
        EXPECT(Has(rawHooks, "false obliges the caller"), "the owner-return rule is written at the hooks' declarations\n");
        EXPECT(Has(draw, "chainDofScene=&ext::ffxiv::dof::ChainScene;") && Has(draw, "toneMapDofOwner=&ext::ffxiv::dof::ToneMapOwner;"),
               "the stage registers both hooks at start-up\n");
        // The stage hangs inside the light bounce's entries, one link further down the chain than the bounce hangs inside the march's: none of
        // the earlier members' files names it, except the bounce's own.
        EXPECT(Count(marchDraw, "chainDofScene") == 0 && Count(marchDraw, "toneMapDofOwner") == 0,
               "the march's file is not touched by the stage: the light bounce's entries hand it the chain\n");
        EXPECT(Count(puddleDraw, "chainDofScene") == 0 && Count(puddleDraw, "toneMapDofOwner") == 0 && Count(shaftsDraw, "chainDofScene") == 0 &&
                   Count(shaftsDraw, "toneMapDofOwner") == 0,
               "and neither are the puddle stage's and the shafts' files\n");
        EXPECT(Count(giDraw, "chainDofScene") == 0 && Count(giDraw, "toneMapDofOwner") == 0 && Count(giDraw, "chainAfterGiScene(") == 2 &&
                   Count(giDraw, "toneMapAfterGiOwner(") == 3,
               "the light bounce's file names neither of depth of field's hooks (it asks the atmosphere, then depth of field, through ChainHooks.h's two "
               "dispatchers) and asks them in exactly five places: the compose step and the chained entry's hand-over, and the owner entry's two exits plus "
               "the owner role's hand-over after its decline\n");
        EXPECT(Has(hooks, "inlineID3D11ShaderResourceView*chainAfterGiScene(ID3D11DeviceContext*context,") && Has(hooks, "inlinebooltoneMapAfterGiOwner(ID3D11DeviceContext*context,"),
               "the dispatchers live in the chain's own header\n");
        // The stage's own sources never read the earlier stages' arming or hooks: its entries start from its own tests.
        for (const std::string* source : { &draw, &camera, &pass })
        {
            EXPECT(!Has(*source, "godrays::Armed") && !Has(*source, "gi::Armed"), "the stage's sources never read an earlier stage's arming\n");
            EXPECT(!Has(*source, "chainGiScene") && !Has(*source, "toneMapGiOwner") && !Has(*source, "chainMarchScene") && !Has(*source, "toneMapMarchOwner"),
                   "and never call an earlier stage's hook: the chain only ever goes forward\n");
        }
    }

    // ---- DoF answers with the march AND GI off -------------------------------------------------------------------------------------
    {
        // The march is off by default and returns early in six places and the light bounce is off by default too; a stage placed after them is
        // reachable only when every exit that has no answer hands it the texture it was given (or, for the owner entries, the game's scene).
        const std::string chain = Body(giDraw, "ID3D11ShaderResourceView*ChainSceneInternal(ID3D11DeviceContext*context,");
        EXPECT(!chain.empty(), "the light bounce's chained entry is found\n");
        EXPECT(Has(chain, "if(context==nullptr||scene==nullptr)returnnullptr;"), "its only early return is the missing context or scene\n");
        EXPECT(Count(chain, "ArmedInternal(state)") == 1 && Has(chain, "if(ArmedInternal(state)&&!(state.haveGiFrame&&state.giFrame==state.frames)){"),
               "the bounce's own armed test and its own one-answer gate guard only the bounce's attempt\n");
        const size_t attempt = At(chain, "if(ArmedInternal(state)&&!(state.haveGiFrame&&state.giFrame==state.frames)){");
        const size_t answered = At(chain, "if(ours!=nullptr)returnours;");
        const size_t hand = At(chain, "returnext::ffxiv::engine::chainAfterGiScene(context,facts,scene);}");
        EXPECT(attempt != std::string::npos && answered != std::string::npos && hand != std::string::npos && attempt < answered && answered < hand,
               "the bounce's attempt, then its answer is final (depth of field already composed on it), then every exit without an answer (off, "
               "already answered, declined, threw) hands the stage the texture the call was given - the march's answer when the march answered, "
               "else what the march was handed\n");
        EXPECT(Count(chain, "chainAfterGiScene(") == 1, "the stage is asked once per call of the chained entry\n");

        const std::string compose = Body(giDraw, "voidComposeChainNext(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*&ours)");
        EXPECT(Has(compose, "if(ours==nullptr)return;") &&
                   Has(compose, "if(ID3D11ShaderResourceView*composed=ext::ffxiv::engine::chainAfterGiScene(context,facts,ours))ours=composed;"),
               "when the bounce answered, the stage composes on the bounce's answer and a null from it is the bounce's answer unchanged\n");
        const std::string bounceStage = Body(giDraw, "ID3D11ShaderResourceView*StageAtToneMap(State&state,ID3D11DeviceContext*context,");
        EXPECT(Count(bounceStage, "ComposeChainNext(context,facts,ours);") == 1, "the bounce composes the stage once, in both roles\n");
        EXPECT(At(bounceStage, "state.session.NoteRun(Knob::Gi);") < At(bounceStage, "ComposeChainNext(context,facts,ours);") &&
                   At(bounceStage, "ComposeChainNext(context,facts,ours);") < At(bounceStage, "if(!owner)returnours;"),
               "the bounce's counters finish before the stage is asked, and the stage's answer is what both roles bind or return\n");

        const std::string bounceOwner = Body(giDraw, "boolToneMapOwnerInternal(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene,DrawFndraw,void*user)");
        EXPECT(Has(bounceOwner, "if(context==nullptr||scene==nullptr||draw==nullptr||!ArmedInternal(state))returnext::ffxiv::engine::toneMapAfterGiOwner(context,facts,scene,draw,user);"),
               "the bounce's owner entry's first exit (the bounce is off) hands the stage the game's scene\n");
        EXPECT(Has(bounceOwner, "if(state.haveGiFrame&&state.giFrame==state.frames){returnext::ffxiv::engine::toneMapAfterGiOwner(context,facts,scene,draw,user);}"),
               "its second exit (this frame's tone map already answered) does the same, the stage's own gate deciding\n");
        EXPECT(Count(bounceOwner, "toneMapAfterGiOwner(") == 2, "and there are exactly two\n");
        // The bounce's seam entry stays a plain false at its early exits: the dispatch's next member is the stage.
        const std::string bounceSeam = Body(giDraw, "boolOnSeamDrawInternal(ID3D11DeviceContext*context,engine::Anchorsanchors,DrawFndraw,void*user)");
        EXPECT(!bounceSeam.empty() && Count(bounceSeam, "chainNightScene") == 0 && Count(bounceSeam, "toneMapNightOwner") == 0,
               "the bounce's seam entry does not call the stage itself: its exits fall through to the dispatch, where the stage is the next member\n");
        EXPECT(Has(bounceSeam, "if(!ArmedInternal(state)||context==nullptr)returnfalse;"), "and its off exit is a plain false\n");

        // The stage's own entries start from the stage's own tests, never from an earlier stage's.
        EXPECT(Has(draw, "if(context==nullptr||scene==nullptr||!Armed())returnnullptr;"), "the stage's chained entry applies the stage's own armed test\n");
        EXPECT(Has(draw, "if(context==nullptr||scene==nullptr||draw==nullptr||!Armed())returnfalse;"), "and so does its owner entry\n");
        EXPECT(Count(enginePass, "ext::ffxiv::dof::OnSeamDraw(") == 1, "the stage's seam entry is called from exactly one place\n");
        EXPECT(Has(enginePass, "if(ext::ffxiv::engine::gi::OnSeamDraw(context,anchors,draw,user))return;"
                               "if(ext::ffxiv::enginelight::OnSeamDraw(context,anchors,draw,user))return;"
                               "if(ext::ffxiv::engine::atmo::OnSeamDraw(context,anchors,draw,user))return;"
                               "if(ext::ffxiv::dof::OnSeamDraw(context,anchors,draw,user))return;"),
               "the stage's seam entry is right behind the atmosphere's, which is right behind the night grade's, which is right behind the light bounce's: the seam order as built is gi, night grade, atmosphere, depth of field, so a false from any of them always reaches it\n");
        EXPECT(Before(enginePass, "if(ext::ffxiv::shafts::OnEngineDraw(context,anchors,draw,user))return;",
                      "if(ext::ffxiv::godrays::OnSeamDraw(context,anchors,draw,user))return;") &&
                   Before(enginePass, "if(ext::ffxiv::godrays::OnSeamDraw(context,anchors,draw,user))return;",
                          "if(ext::ffxiv::engine::gi::OnSeamDraw(context,anchors,draw,user))return;") &&
                   Before(enginePass, "if(gloss::OnToneStageDraw(context,anchors,draw,user))return;", "if(ext::ffxiv::godrays::OnSeamDraw(context,anchors,draw,user))return;"),
               "the dispatch order is shafts, puddles, march, light bounce, atmosphere, then this stage\n");

        // The stage's inputs (the four recognitions it reads: the light draws, the composite, the fog post and the tone map) are bits of the
        // shader's own layout, stored whatever any key says, so a stage switched on from the menu with the march and the bounce off finds them.
        const std::string anchors = Body(enginePassRules, "constexprAnchorsDrawAnchors(constPassLayout&layout)noexcept");
        EXPECT(!anchors.empty() && Has(anchors, "kShaftsLightBit") && Has(anchors, "kShaftsCompositeBit") && Has(anchors, "kShaftsToneMapBit") &&
                   Has(anchors, "kFogPostBit"),
               "the light, composite, tone map and fog post bits are set by the layout rule\n");
        for (const char* gate : { "Active(", "Armed(", "godrays", "shafts::", "gi::", "dof::", "Cfg()" })
            EXPECT(!Has(anchors, gate), "and nothing in that rule reads a key or a module's arming (%s)\n", gate);
        // The frame boundary: the stage's one-answer gate compares against a rendered-frame counter the stage opens itself at the composite, once
        // a frame (the game draws the composite twice a frame; recog_pins pins the close at the tone map and the step line).
        EXPECT(Has(draw, "if(state.step.OpenAtComposite(state.frames)){state.attempts=0;state.cameraTaken=false;}"),
               "the rendered-frame counter opens at the stage's own composite capture\n");
        EXPECT(Count(draw, "OpenAtComposite(") == 1 && !Has(draw, "AdvanceFrame("), "and nowhere else, and never advanced by the stage itself\n");
        EXPECT(Has(seam, "ext::F::DepthOfField"), "the draw seam's feature test opens for the stage's key alone\n");
        EXPECT(Count(enginePass, "dof::Armed()") >= 5, "the draw path's arming lists carry the stage's own arming\n");
        EXPECT(Has(enginePass, "ext::ffxiv::dof::Release();"), "the engine's teardown releases the stage's device objects\n");
        EXPECT(Has(enginePass, "if(ext::Active(ext::F::DepthOfField))ext::ffxiv::dof::SampleSceneHdr();"),
               "the stage samples the scene HDR state on a pixel-shader bind\n");
        EXPECT(Has(enginePass, "if(!ext::ActiveAny(ext::F::EnginePassOverrides,ext::F::LightShafts,ext::F::GodraysVolumetric,ext::F::Gi,ext::F::DepthOfField,ext::F::LensFlare,ext::F::Atmosphere,ext::F::NightGrade))"),
               "the pixel-shader seam tracks the anchors while the stage's key is on, appended to the one variadic test (no overload)\n");
        EXPECT(Count(features, "ActiveAny(") == 1, "ExtFeatures.h holds the ONE variadic ActiveAny: a consumer appends its flag and never adds an overload\n");
    }

    // ---- the game's tone-map draw runs exactly once on every path ------------------------------------------------------------------
    {
        // The shafts' caller: draws once when the owner chain did not.
        const std::string withChain = Body(shaftsDraw, "boolDrawToneMapWithChain(ID3D11DeviceContext*context,constToneMapFacts&facts,ID3D11ShaderResourceView*scene,DrawFndraw,void*user,bool&drew)");
        EXPECT(!withChain.empty() && Count(withChain, "draw(user);") == 1, "the shafts' fallback chain has exactly one draw of its own\n");
        EXPECT(Has(withChain, "if(ext::ffxiv::engine::toneMapMarchOwner(context,facts,scene,draw,user))returntrue;}draw(user);returntrue;"),
               "the owner chain's true is the only way past the one draw (a false obliges the caller to draw)\n");

        // The light bounce's owner role hands the stage the owner turn AFTER its try block, true only if the stage drew, else a plain false.
        const std::string bounceRun = Body(giDraw, "boolRunToneMapAsOwner(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene,DrawFndraw,void*user)");
        EXPECT(!bounceRun.empty() && Count(bounceRun, "draw(user);") == 1 && Has(bounceRun, "if(!drew)draw(user);returntrue;}"),
               "the bounce's owner role draws in its catch only when it has not drawn\n");
        EXPECT(Has(bounceRun, "}if(drew)returntrue;returnext::ffxiv::engine::toneMapAfterGiOwner(context,facts,scene,draw,user);}"),
               "a bounce that declined hands the stage the owner turn AFTER its try block, true only if the stage drew, else a plain false\n");
        EXPECT(At(bounceRun, "catch(...)") < At(bounceRun, "toneMapAfterGiOwner("),
               "so a throw after the stage drew can never reach the bounce's catch, which would draw the game's call a second time\n");
        EXPECT(Count(giDraw, "draw(user);") == 2, "the bounce's draws: its owner bind, and the catch above (nothing else draws the game's call)\n");
        EXPECT(Count(giDraw, "PSSetShaderResources(0,1,") == 2, "and the bounce binds t0 only in the owner path, once and restored\n");

        // The stage's owner role: one bind, one draw, `drew` set only after it; declines and skips never draw.
        const std::string stage = Body(draw, "ID3D11ShaderResourceView*StageAtToneMap(State&state,ID3D11DeviceContext*context,");
        EXPECT(!stage.empty() && Count(stage, "draw(user);") == 1 && Has(stage, "draw(user);drew=true;returnours;}"),
               "the stage's one draw is the owner's bind, and `drew` is set only after it\n");
        EXPECT(Has(stage, "if(!owner)returnours;"), "the chained role returns its texture before any bind or draw\n");
        EXPECT(Count(draw, "draw(user);") == 2, "the stage's draws: the owner's bind and the owner role's catch (an owner decline or skip never draws)\n");
        EXPECT(Count(draw, "PSSetShaderResources(0,1,") == 2, "only the owner path binds t0, once and restored\n");
        EXPECT(Has(draw, "voidNoteDecline(State&state,Declinereason,Outcomeoutcome)") && Has(draw, "voidNoteSkip(State&state,SkipReasonreason)"),
               "the decline and skip bookkeeping takes no draw: a declining owner returns false and the caller draws once\n");
        const std::string run = Body(draw, "boolRunToneMapAsOwner(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene,DrawFndraw,void*user)");
        EXPECT(Has(run, "StageAtToneMap(state,context,facts,scene,true,draw,user,drew);returndrew;}") && Has(run, "if(!drew)draw(user);returntrue;}") &&
                   Count(run, "draw(user);") == 1,
               "the stage's owner returns exactly what it drew, and its catch draws once only if the draw has not happened\n");
        const std::string ownerEntry = Body(draw, "boolToneMapOwnerInternal(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene,DrawFndraw,void*user)");
        EXPECT(Count(ownerEntry, "returnfalse;") == 2 && Has(ownerEntry, "returnRunToneMapAsOwner(context,facts,scene,draw,user);") && Count(ownerEntry, "return") == 3,
               "the stage's owner entry returns false or the owner role's answer, nothing else\n");
        const std::string seamEntry = Body(draw, "boolSeamDraw(ID3D11DeviceContext*context,engine::Anchorsanchors,DrawFndraw,void*user)");
        EXPECT(Has(seamEntry, "returnRunToneMapAsOwner(context,facts,scene.Get(),draw,user);") && Count(seamEntry, "returntrue") == 0,
               "the stage's seam entry returns true only through the owner role, which draws once\n");
        EXPECT(Has(draw, "boolToneMapOwner(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene,DrawFndraw,void*user){try{returnToneMapOwnerInternal(context,facts,scene,draw,user);}catch(...){returnfalse;}}"),
               "the exported owner hook never throws: an exception before any draw is a false, and the caller draws once\n");
        EXPECT(Has(draw, "ChainScene(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene){try{returnChainSceneInternal(context,facts,scene);}catch(...){returnnullptr;}}"),
               "and the exported chained hook never throws either: the chain's hold stands\n");
        EXPECT(Has(draw, "StageAtToneMap(state,context,facts,scene,false,nullptr,nullptr,drew)"), "the chained role is given no draw to call\n");
        // The seam's dispatch ends in the pass-through draw exactly once, so a false from the stage's seam entry still draws the game's call once.
        const std::string onGame = Body(enginePass, "voidOnGameDraw(ID3D11DeviceContext*context,DrawFndraw,void*user)");
        EXPECT(!onGame.empty() && Count(onGame, "ext::ffxiv::dof::OnSeamDraw(") == 1,
               "the dispatch is found, and a false from the stage's seam entry falls through to the seam's own pass-through draw\n");
    }

    // ---- the stamping contract: the stage's sources cannot reach the shared stamp, and its own tracking is its own -----------
    {
        for (const std::string* source : { &rules, &draw, &camera, &cameraHeader, &pass, &passHeader, &hooks, &module, &menu })
        {
            EXPECT(!Has(*source, "usedCapture"), "the stage's sources never reference the shared stamp (usedCapture)\n");
            EXPECT(!Has(*source, "capture.id"), "and never reference the capture id it is compared with\n");
        }
        // The stage's answered-tracking is its own rendered-frame counter, never the chain's shared state.
        EXPECT(Has(draw, "dofFrame"), "the stage tracks the frame it answered with its own counter\n");
        EXPECT(Has(draw, "haveDofFrame"), "and the flag that separates no answer yet from a counter value of 0\n");
        // The chained role never binds and never draws: the chained hook returns a texture or null, and the caller binds.
        EXPECT(Has(hooks, "ID3D11ShaderResourceView*"), "the chain hook's answer is a texture the caller binds\n");
        // The stage's own binds of t0 are the owner role's: the set and the restore's set-back (the chained role binds nothing). The counts
        // of the draws and the binds are pinned in the section on the game's one tone-map draw.
        EXPECT(Count(draw, "PSSetShaderResources(0,1,") == 2, "the stage's own draw path binds t0 exactly twice\n");
    }

    // ---- the hold semantics: the two decline wordings exist, and the outcome enum names them ---------------------------------------
    {
        EXPECT(Has(rawRules, "the chain's hold stands"), "the chained decline's wording\n");
        EXPECT(Has(rawRules, "the frame is the engine's own"), "the owner decline's wording\n");
        EXPECT(Has(rules, "Outcome::HoldStands") && Has(rules, "Outcome::EngineOwn") && Has(rules, "Outcome::Answered") &&
                   Has(rules, "Outcome::Skipped"),
               "the four outcomes the chain's bookkeeping distinguishes\n");
        // The shafts' chain entry is unchanged by this stage.
        EXPECT(Has(shaftsDraw, "ext::ffxiv::engine::toneMapMarchOwner"), "the shafts' chain hands the tone map out, unchanged\n");
        // The stage's pass copies the scene it is handed and writes only its own texture.
        EXPECT(Has(pass, "CopyResource"), "the pass copies the scene it is handed into its own texture\n");
        EXPECT(Has(passHeader, "PackView"), "the pack is the stage's own texture, for the test that pins it\n");
        EXPECT(Has(passHeader, "NearView") && Has(passHeader, "FarView"), "and the near and far half are its own textures\n");
    }

    // ---- the stage is its own file, with no march or engine march shader contact -------------------------------------------------
    {
        EXPECT(!Has(shader, "#include"), "Dof.hlsl has no #include at all: fully self-contained, no march or SceneMarch contact\n");
        EXPECT(!Has(pass, "Godrays"), "and the pass includes none of the march's headers\n");
        EXPECT(!Has(draw, "GodraysPass"), "and neither does the draw path\n");
        EXPECT(Has(shader, "PS_Pack") && Has(shader, "PS_Blur") && Has(shader, "PS_Merge"), "the stage's own entry points\n");
        // The pack's own two habits, the game's own family's: the pack's cap and the CoC in the alpha (the captured weight shader's own).
        EXPECT(Has(shader, "min(scene.rgb, Blend.y)") || Has(shader, "min(scene.rgb,Blend.y)"),
               "the pack's colour cap is the pack's own min, the game's own Weight2 habit\n");
        EXPECT(Has(shader, "blurPromises") || Has(shader, "float4(own.rgb,") || Has(shader, "own.rgb"),
               ": the pack's CoC rides the alpha (the captured weight shader's own convention)\n");
    }
    (void) rawShader_RS;

    // ---- the shaped CoC: the shader's constants and expression are the rules' own, and nothing else shapes the pack's alpha -----------
    {
        const double ruleZone = ConstantOf(rules, "kDeadZone"), ruleCap = ConstantOf(rules, "kNearCap");
        const double shaderZone = ConstantOf(shaderCode, "kDeadZone"), shaderCap = ConstantOf(shaderCode, "kNearCap");
        EXPECT(ruleZone == ruleZone && ruleCap == ruleCap, "DofRules.h defines the focus band's half-width and the near cap, once each\n");
        EXPECT(shaderZone == shaderZone && shaderCap == shaderCap, "Dof.hlsl defines kDeadZone and kNearCap, once each\n");
        EXPECT(shaderZone == ruleZone, "the shader's dead zone is the rules' (%.9g against %.9g): neither file may drift\n", shaderZone, ruleZone);
        EXPECT(shaderCap == ruleCap, "the shader's near cap is the rules' (%.9g against %.9g)\n", shaderCap, ruleCap);
        EXPECT(ruleZone > 0.0 && ruleZone < 1.0 && ruleCap > 0.0 && ruleCap < 1.0, "both are fractions of the unit CoC\n");
        // The shader's CoC, operation for operation (the replay lab's measured shape): guards first, then the clamp, the dead zone with
        // its linear remap, and the near-only cap; the far side is the negated magnitude and nothing caps it.
        const std::string cocBody = Body(shaderCode, "floatCoCSigned(floatviewZ,floatfocus)");
        EXPECT(!cocBody.empty(), "the shader's CoCSigned is found\n");
        EXPECT(Has(cocBody, "if(!(viewZ>0.0)||!(focus>0.0))return0.0;") && Has(cocBody, "if(!(c==c))return0.0;"),
               "its two guards stay: a broken depth, focus or division is no blur\n");
        EXPECT(Has(cocBody, "constfloatraw=clamp(c,-1.0,1.0);"), "the shaping starts from the clamped CoC\n");
        EXPECT(Has(cocBody, "constfloatmagnitude=max(abs(raw)-kDeadZone,0.0)/(1.0-kDeadZone);"),
               "the dead zone is a linear remap (continuous at the band's edge), not a hard cut\n");
        EXPECT(Has(cocBody, "returnraw>0.0?min(magnitude,kNearCap):-magnitude;"), "only the near side (raw > 0) is capped; the far side is the negated magnitude\n");
        EXPECT(At(cocBody, "if(!(c==c))return0.0;") < At(cocBody, "constfloatraw="), "the NaN guard comes before the shaping\n");
        // The three readers of the pack's alpha read it as the shaped value: the constants are used in the one expression and nowhere else.
        EXPECT(Count(shaderCode, "kDeadZone") == 3 && Count(shaderCode, "kNearCap") == 2,
               "the dead zone and the cap appear in the definitions and the one shaping expression only: PS_Blur and PS_Merge read the alpha as it is\n");
        EXPECT(Has(shaderCode, "returnfloat4(min(scene.rgb,Blend.y),CoCSigned(viewZ,focus));"), "the pack writes the shaped value into the alpha, unchanged otherwise\n");
        EXPECT(Has(shaderCode, "returnmin(abs(tapCoc),1.0);") && Has(shaderCode, "if(!(radius<=abs(tapCoc)*reach))continue;"),
               "PS_Blur's tap weight and reach read the pack's alpha as it is (bounded by 1, so by the cap on the near side)\n");
        EXPECT(Has(shaderCode, "nearAmount=max(cocSigned,0.0)*strength;farAmount=max(-cocSigned,0.0)*strength;"),
               "PS_Merge's amounts are the alpha's halves times the strength key, unchanged\n");
        // The rules' own mirror: ShapeCoC guards NaN, clamps, and CoCSigned returns it.
        const std::string shape = Body(rules, "constexprfloatShapeCoC(floatraw)noexcept");
        EXPECT(!shape.empty() && Has(shape, "if(!(raw==raw))return0.0f;"), "the rules' shaping answers 0 to a NaN\n");
        EXPECT(Has(shape, "(1.0f-kDeadZone)") && Has(shape, "kDeadZone") && Has(shape, "kNearCap"), "and uses the same two constants and the same remap divisor\n");
        const std::string rulesCoc = Body(rules, "constexprfloatCoCSigned(floatviewZ,floatfocus)noexcept");
        EXPECT(!rulesCoc.empty() && Has(rulesCoc, "returnShapeCoC("), "the rules' CoCSigned is the shaped value, as the shader's is\n");
        // No new key, bind or state: the shaping is inside the pack's existing draw.
        EXPECT(Count(pass, "explicit_state::SpeckSpec{5,0,1};") == 1, "the explicit-state Spec is unchanged\n");
        EXPECT(!Has(config, "dofNearCap") && !Has(config, "dofDeadZone") && !Has(ini, "DofNearCap") && !Has(ini, "DofDeadZone"),
               "and no key carries the two constants\n");
    }

    // ---- the timing flags: every staging Map carries DO_NOT_WAIT, every GetData carries DONOTFLUSH, in the flags argument ----
    {
        // The FRESH wave's bug class: a read that waits for the GPU on the render thread. The pin greps the flags
        // argument, not a literal: every Map call site in the stage's device files must carry the no-wait flag (a
        // write-discard map is the one exception, and it is pinned as such), and every GetData must carry no-flush.
        EXPECT(Count(camera, "D3D11_MAP_FLAG_DO_NOT_WAIT") >= 1, "the staging reads never wait for the GPU\n");
        EXPECT(Count(camera, "D3D11_MAP_READ") == Count(camera, "D3D11_MAP_FLAG_DO_NOT_WAIT"),
               "every read map in the stage's readers carries the no-wait flag in its flags argument\n");
        EXPECT(Count(pass, "D3D11_MAP_WRITE_DISCARD") >= 1, "the parameter buffer's write is a discard map\n");
        EXPECT(Count(pass, "D3D11_MAP_READ") == 0, "the pass maps nothing for reading\n");
        EXPECT(Count(pass, "D3D11_ASYNC_GETDATA_DONOTFLUSH") >= 1, "the timing query's GetData never flushes the pipe\n");
        EXPECT(Count(pass, "GetData(") == Count(pass, "D3D11_ASYNC_GETDATA_DONOTFLUSH"),
               "every GetData call site carries the no-flush flag in its flags argument\n");
        // The stage's captures never wait for the GPU and never write a game resource.
        EXPECT(Has(camera, "CopySubresourceRegion"), "the captures copy into their own staging buffers\n");
        EXPECT(!Has(camera, "CopyResource(scene"), "nothing of the game is written\n");
    }

    // ---- the feature bit: 21 (19 is the chara ambient's, 20 the light bounce's) ---------------------------------------------------
    {
        EXPECT(Has(features, "DepthOfField=1ull<<21"), "the stage's feature bit is 21\n");
        EXPECT(Has(features, "CharaAmbient=1ull<<19") && Has(features, "Gi=1ull<<20"), "19 is the chara ambient's and 20 the light bounce's, below this stage's\n");
        EXPECT(Has(features, "GodraysVolumetric=1ull<<18"), "bit 18 is the march's\n");
        EXPECT(!Has(features, "DepthOfField=1ull<<19"), "and the stage no longer claims 19\n");
    }

    // ---- the keys: seven of them (DofMode joined), off by default, everywhere a key lives ------------------------------------
    {
        struct Key
        {
            const char* name;
            const char* iniDefault;
        };
        const Key keys[] {
            { "DepthOfField", "false" },  { "DofFocusMode", "auto" },  { "DofFocusDistance", "auto" },
            { "DofAperture", "auto" },    { "DofStrength", "auto" },   { "DofSteps", "auto" },
            { "DofMode", "auto" },
        };
        for (const Key& key : keys)
        {
            std::string member = std::string(key.name);
            member[0] = char(std::tolower(static_cast<unsigned char>(member[0])));
            EXPECT(Has(config, member), "%s: the config member\n", key.name);
            EXPECT(Has(binder, key.name), "%s: the binder row\n", key.name);
            EXPECT(Has(ini, key.name), "%s: the ini fragment\n", key.name);
        }
        EXPECT(Has(ini, "DepthOfField=auto"), "the master switch is off by default (the ini's own auto)\n");
        EXPECT(Has(config, "dofOnLoaded"), "the config hook arms the seam's feature bit\n");
        EXPECT(Has(rules, "kDefaultSteps=24") && Has(rules, "kDefaultStrength=0.8f") &&
                   Has(rules, "kDefaultAperture=0.05f") && Has(rules, "kDefaultFocusDistance=3.0f"),
               "the rules carry the defaults the card and the schema state\n");
        // The menu card exists, in the engine-pass group, with the state line the skip decomposition feeds; and its slot
        // sits after the god-rays card's (the chain's order: the stage is the last member).
        EXPECT(Has(menu, "Depthoffield(FFXIV)"), "the card's title\n");
        EXPECT(Has(menu, "ffxiv_engine_passes"), "the card sits in the FFXIV tab's engine-pass group\n");
        EXPECT(Has(menu, "FormatCard"), "the card's state line is the log's own decomposition\n");
        EXPECT(Has(layout, R"({"ffxiv_dof","FFXIV","ffxiv_atmosphere"},)"), "the card after the light bounce's, the chain's order\n");
        EXPECT(Has(Code(repo, "core/MenuManifest.h"), "\"ffxiv_dof\""), "and FFXIV-only\n");
        EXPECT(Has(layout, R"({"ffxiv_lens_flare","FFXIV","ffxiv_dof"},)") && Has(layout, R"({"ffxiv_jitter","FFXIV","ffxiv_lens_flare"},)"),
               "and the lens flare card after its, with the jitter card after the flare card\n");
    }

    // ---- the fail-open contract: the scene-HDR decline, the replay refusal, the stand-down, the private state ----------
    {
        EXPECT(Has(rules, "SceneHdr"), "the scene-HDR decline is a decline of its own\n");
        EXPECT(Has(draw, "SceneHdrRequested"), "the key is read live, so a session that switches HDR off starts the stage again\n");
        EXPECT(Has(rules, "CharaReplay"), "the chara replay refusal is a decline of its own\n");
        EXPECT(Has(enginePassRules, "DoF") && Has(enginePassRules, "dofDeclines"),
                   "the stage's refusal streak is its own knob (the engine module's shared Session)\n");
        EXPECT(Has(pass, R"x(ext::core::explicit_state::GuardstateGuard(context,context1.Get(),device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1,kSpec);)x") &&
                   Has(pass, R"x(stateGuard.Neutralise();)x"),
               "the game's state is captured, the private state's defaults (no HS, DS, GS, stream-out or predicate) are set explicitly and a scope guard puts it back\n");
        EXPECT(!Has(pass, "SwapDeviceContextState") && !Has(pass, "CreateDeviceContextState"),
               "no device-context state swap remains: it does nothing when the draw hook is nested\n");
        EXPECT(Has(pass, R"x(explicit_state::SpeckSpec{5,0,1};)x") && Has(pass, "static_assert(kSpec.psSrvCount==kSlots"),
               "the pass declares exactly the state it touches: pixel-shader resource slots 0-4 (kSlots), no sampler, constant buffer 0\n");
        {
            // The guard sits exactly where the swap sat: after the scene copy, the parameter buffer's Map and Unmap and the timing collection. The
            // copy is made under the game's own state (a game predicate skips it, as it did under the swap), so a guard above it changes the output.
            // Run has two scene copies: the strength-0 early exit's (before any guard) and the main one; the one that precedes the Map is pinned.
            const size_t mapAt = pass.find("context->Map(_params.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped)");
            const size_t copyAt = mapAt == std::string::npos ? mapAt : pass.rfind("context->CopyResource(_scene.Get(),sceneResource.Get());", mapAt);
            const size_t unmapAt = pass.find("context->Unmap(_params.Get(),0);", mapAt);
            const size_t timingAt = pass.find("CollectGpuTiming(context);", unmapAt);
            const size_t guardAt = pass.find("ext::core::explicit_state::GuardstateGuard(");
            EXPECT(copyAt != std::string::npos && mapAt != std::string::npos && unmapAt != std::string::npos && timingAt != std::string::npos &&
                       guardAt != std::string::npos && Count(pass, "ext::core::explicit_state::GuardstateGuard(") == 1 &&
                       Count(pass, "context->CopyResource(_scene.Get(),sceneResource.Get());") == 2,
                   "the scene copy (the main one and the early exit's), the parameter Map and Unmap, the timing collection and the one guard are all found in Run\n");
            EXPECT(copyAt < mapAt && mapAt < unmapAt && unmapAt < timingAt && timingAt < guardAt,
                   "the guard comes after the main scene copy, the parameter buffer's Map/Unmap and the timing collection (the copy runs under the game's state)\n");
        }
        EXPECT(Has(pass, "IsSceneFormat"), "the scene is refused when it is not FP16\n");
        EXPECT(Has(pass, "IsDepthFormat"), "and so is a depth the pass cannot read\n");
        // The log lines are the rules' own, so the host tests can pin them.
        EXPECT(Has(rules, "FormatDecline") && Has(rules, "FormatSkip") && Has(rules, "FormatCard") && Has(rules, "FormatRun"),
               "the log and card lines are pure rules\n");
        EXPECT(Has(rules, "CountsTowardsStandDown"), "and the stand-down's counting rule is one the host can test\n");
        // The stage's camera read side carries the contact pass's own camera guard (the band's own plane pair).
        EXPECT(Has(camera, "CameraDepthBand"), "the band is the camera's own format guard\n");
    }

    // ---- the two looks: the cinematic pack is the first release's code untouched, the distance look has its own entry points ---------------
    {
        // 1. The entry points: the first release's PS_Pack stays (the cinematic look; the lab's bit-exact comparison against rc6 rests on it),
        //    the distance look adds PS_PackDistance and the focus update PS_Focus; the headers come from prepare_dof.ps1 and the props file.
        EXPECT(Has(shaderCode, "float4PS_Pack(float4position:SV_Position):SV_Target0") && Has(shaderCode, "float4PS_PackDistance(float4position:SV_Position):SV_Target0") &&
                   Has(shaderCode, "float4PS_Focus(float4position:SV_Position):SV_Target0"),
               "the three pack-side entry points\n");
        const std::string packBody = Body(shaderCode, "float4PS_Pack(float4position:SV_Position):SV_Target0");
        EXPECT(!packBody.empty() && !Has(packBody, "FocusState") && !Has(packBody, "DistanceCoC") && !Has(packBody, "kStartFocusMultiple"),
               "the cinematic pack reads neither the focus state nor the distance CoC: it is the first release's code, the instantaneous centre texel\n");
        EXPECT(Has(packBody, "constfloatcentreDepth=Depth.Load(int3(centreTexel,0));") && Has(packBody, "returnfloat4(min(scene.rgb,Blend.y),CoCSigned(viewZ,focus));"),
               "and its centre-texel focus and its alpha are the first release's\n");
        const std::string prepare = ReadFile(repo + "/tests/ext/ffxiv/engine/prepare_dof.ps1");
        const std::string props = ReadFile(repo + "/OptiScaler/ext/ffxiv/engine/dof.shaders.props");
        for (const char* entry : { "PS_PackDistance", "PS_Focus" })
        {
            EXPECT(Has(prepare, entry), "%s: the lab and the tests build the header from the tree's own prepare_dof.ps1\n", entry);
            EXPECT(Has(props, entry), "%s: the product build compiles the entry point\n", entry);
        }
        EXPECT(Has(pass, "#include<Dof_PackDistance.h>") || Has(ReadFile(repo + "/OptiScaler/ext/ffxiv/engine/DofPass.cpp"), "#include <Dof_PackDistance.h>"),
               "the pass includes the distance pack's header\n");

        // 2. The numbers the shader mirrors, compared as numbers.
        for (const char* name : { "kStartFocusMultiple", "kStartFloorMetres", "kFarOverStart", "kFocusFarRatio", "kFocusTimeConstant", "kFocusFarTimeConstant",
                                  "kFocusStaleSeconds" })
        {
            const double inRules = ConstantOf(rules, name), inShader = ConstantOf(shaderCode, name);
            EXPECT(inRules == inRules && inShader == inShader, "%s: defined once in the rules and once in the shader\n", name);
            EXPECT(inRules == inShader, "%s: the shader's %.9g and the rules' %.9g are one number\n", name, inShader, inRules);
        }

        // 3. The distance CoC, operation for operation, and the pack that writes it.
        const std::string startBody = Body(shaderCode, "floatDistanceStart(floatfocus)");
        EXPECT(!startBody.empty() && Has(startBody, "returnmax(kStartFocusMultiple*focus,kStartFloorMetres);"),
               "the start is the larger of a multiple of the focus and the floor in metres\n");
        const std::string cocBody = Body(shaderCode, "floatDistanceCoC(floatviewZ,floatfocus)");
        EXPECT(!cocBody.empty() && Has(cocBody, "if(!(viewZ>0.0))return0.0;") && Has(cocBody, "if(!(viewZ>start))return0.0;") &&
                   Has(cocBody, "constfloatcoc=(1.0-start/viewZ)/(1.0-1.0/kFarOverStart);") && Has(cocBody, "returnsaturate(coc);"),
               "nothing up to the start, then the thin-lens difference anchored at the start over the span to the far limit, clamped to [0, 1]\n");
        const std::string distanceBody = Body(shaderCode, "float4PS_PackDistance(float4position:SV_Position):SV_Target0");
        EXPECT(!distanceBody.empty() && Has(distanceBody, "returnfloat4(min(scene.rgb,Blend.y),-DistanceCoC(viewZ,focus));"),
               "the distance pack writes the negated CoC (the far side only; the near half of the split stays empty) with the same colour cap\n");
        EXPECT(Has(distanceBody, "FocusState.Load(int3(0,0,0)).x") && Has(distanceBody, "Blend.z"),
               "its focus is the state's smoothed value (auto) or the key's metres (manual)\n");
        EXPECT(!Has(distanceBody, "CoCSigned(") && !Has(distanceBody, "kNearCap"), "and it never goes through the shaped cinematic CoC\n");

        // 4. The focus update: the smoothing's step and weight, operation for operation, and the state it writes.
        const std::string stepBody = Body(shaderCode, "floatFocusStep(floatprevious,floatseen,floatdt)");
        EXPECT(!stepBody.empty() && Has(stepBody, "constfloattau=seen>previous*kFocusFarRatio?kFocusFarTimeConstant:kFocusTimeConstant;") &&
                   Has(stepBody, "returnprevious+(seen-previous)*FocusAlpha(dt,tau);") && Has(stepBody, "if(!(seen>0.0)||!isfinite(seen))returnprevious>0.0?previous:0.0;"),
               "a missing sample holds the focus, a far outlier is approached with the slow constant, the step is previous + (sample - previous) x alpha\n");
        const std::string alphaBody = Body(shaderCode, "floatFocusAlpha(floatdt,floattau)");
        EXPECT(!alphaBody.empty() && Has(alphaBody, "if(!(dt>0.0))return0.0;") && Has(alphaBody, "if(dt>=kFocusStaleSeconds||!(tau>0.0))return1.0;") &&
                   Has(alphaBody, "return1.0-exp(-dt/tau);"),
               "the weight of a step is 1 - exp(-dt / tau), nothing for no time, everything for a stale gap\n");
        const std::string focusBody = Body(shaderCode, "float4PS_Focus(float4position:SV_Position):SV_Target0");
        EXPECT(!focusBody.empty() && Has(focusBody, "FocusState.Load(int3(0,0,0))") && Has(focusBody, "Centre.z") && Has(focusBody, "Centre.w") &&
                   Has(focusBody, "returnfloat4(focus,seen,0.0,0.0);"),
               "the update reads the previous state, the time since the last run (Centre.z) and the tap offset (Centre.w), and writes (focus, raw sample)\n");

        // 5. The pass: the state is read at an idle slot (the Spec is unchanged), one texel, initial zero, written by a draw of its own; only
        //    the distance look with an auto focus pays for it; the keys the look decides are applied on the CPU so PS_Blur and PS_Merge are unchanged.
        EXPECT(Has(pass, "constexpruint32_tkSlotFocus=3;") && Has(pass, "DXGI_FORMAT_R32G32_FLOAT") && Has(passHeader, "FocusView()"), "the focus state's slot, format and accessor\n");
        EXPECT(Has(pass, "inputs.params.mode==Mode::Distance&&inputs.params.focusMode==FocusMode::Auto"), "only the distance look with an auto focus runs the update\n");
        EXPECT(Has(pass, "ReachPixelsFor(inputs.params.mode,inputs.window.height,inputs.params.aperture)") &&
                   Has(pass, "EffectiveStrength(inputs.params.mode,inputs.params.strength)"),
               "the look decides the reach and the strength on the CPU\n");
        EXPECT(Has(pass, "Set(params.centre,centreX,centreY,deltaSeconds,") && Has(pass, "inputs.deltaSeconds") && Has(passHeader, "floatdeltaSeconds=0.0f;"),
               "the time since the pass last ran travels in Centre.z (0 for a caller that gives none: the frozen lab driver, which then sees no smoothing)\n");
        EXPECT(Count(pass, "D3D11_MAP_READ") == 0 && Count(pass, "GetData(") == Count(pass, "D3D11_ASYNC_GETDATA_DONOTFLUSH"), "no readback anywhere in the pass\n");
        EXPECT(Has(shaderCode, "nearOut.rgb=nearWeight>0.0?nearSum/nearWeight:own.rgb;") && Has(shaderCode, "farOut.rgb=farWeight>0.0?farSum/farWeight:own.rgb;"),
               "PS_Blur's energy normalisation is the first release's text: it is not edited\n");

        // 6. The key.
        EXPECT(Has(config, "dofMode{\"distance\"}") || Has(config, "dofMode{"), "the config member\n");
        EXPECT(Has(binder, "b.Add(\"FFXIV\",\"DofMode\",dofMode);"), "the binder row\n");
        EXPECT(Has(draw, "cfg.dofMode.value_or_default()"), "the module builds its parameters with the look\n");
        EXPECT(Has(menu, "cfg.dofMode"), "and the card sets it\n");
        EXPECT(Has(ReadFile(repo + "/OptiScaler/ext/ffxiv/engine/DofMenu.cpp"), "Cinematic") && Has(ReadFile(repo + "/OptiScaler/ext/ffxiv/engine/DofMenu.cpp"), "Distance"),
               "the card names both looks and says what each does\n");
    }

    // ---- the engine's own depth of field: the stamp, the bind, the draw, the decline -------------------------------------------------------
    {
        EXPECT(Has(enginePassRules, "{\"cWeightSamplingParam\",NameKind::ConstantBuffer},"), "the weight pass's parameter buffer is a name of the shader table (so the creation hook reaches the stage)\n");
        const std::string anchorsRule = Body(enginePassRules, "constexprAnchorsDrawAnchors(constPassLayout&layout)noexcept");
        EXPECT(!anchorsRule.empty() && !Has(anchorsRule, "Has(19)"), "no anchor bit is set for it: the anchors word is full and the weight pass is not stored in the engine's index\n");
        EXPECT(Count(enginePass, "ext::ffxiv::dof::NoteShader(shader,code,size);") == 1 &&
                   enginePass.find("ext::ffxiv::dof::NoteShader(shader,code,size);") < enginePass.find("constPassLayoutlayout=PassLayoutOfShader(code,size);"),
               "the creation hook stamps the family's shaders once, before the engine's own layout rule\n");
        EXPECT(Count(enginePass, "ext::ffxiv::dof::OnPixelShaderSet(shader,instances);") == 1 &&
                   Has(enginePass, "if(ext::Active(ext::F::DepthOfField))ext::ffxiv::dof::OnPixelShaderSet(shader,instances);"),
               "the bind seam reads the stamp while the stage's key is on\n");
        EXPECT(Has(draw, "voidNoteShader(void*shaderVoid,constvoid*code,size_tsize)") && Has(draw, "if(!IsEngineDofLayout(declaresParam,declaresInput))return;") && Has(draw, "BindPointOf("),
               "the stamp rule: the shader declares the parameter buffer and a picture input (the CoC LUT update declares the buffer alone)\n");
        const std::string bindBody = Body(draw, "voidOnPixelShaderSet(constvoid*shader,uint32_tinstances)");
        EXPECT(!bindBody.empty() && Has(bindBody, "if(instances!=0||shader==nullptr){state.boundWeightStamp.store(0,std::memory_order_relaxed);return;}"),
               "a class-instance override or a null shader answers no stamp until the next plain bind (the stamp is never read for them)\n");
        EXPECT(Has(draw, "if(state.boundWeightStamp.load(std::memory_order_relaxed)!=0)NoteEngineDofDraw(context,state);"),
               "the seam notes a stamped draw before anything else of the seam's entry\n");
        const std::string noteBody = Body(draw, "voidNoteEngineDofDraw(ID3D11DeviceContext*context,State&state)");
        EXPECT(!noteBody.empty() && Has(noteBody, "IsSceneSized(") && Has(noteBody, "state.engineDofFrame=state.frames;") && Has(noteBody, "state.haveEngineDofFrame=true;"),
               "a stamped draw counts only when it draws to a scene-sized target, and the flag is the stage's own frame counter\n");
        const std::string stage = Body(draw, "ID3D11ShaderResourceView*StageAtToneMap(State&state,ID3D11DeviceContext*context,");
        const size_t classify = At(stage, "shafts::ClassifyToneMap(facts)");
        const size_t decline = At(stage, "if(EngineDofRanThisFrame(state.haveEngineDofFrame,state.engineDofFrame,state.frames)){NoteDecline(state,Decline::EngineDof,declineOutcome);");
        const size_t run = At(stage, "RunStage(state,context,facts,scene)");
        EXPECT(classify != std::string::npos && decline != std::string::npos && run != std::string::npos && classify < decline && decline < run,
               "the stage stands down for the frame the engine's weight pass drew in: after the tone-map shape is known, before the pass runs\n");
        EXPECT(Has(rules, "caseDecline::EngineDof:") && Has(rules, "EngineDof,"), "a decline of its own, not counted toward the stand-down\n");
    }

    // ---- the stage line: the capture frame's numbers --------------------------------------------------------------------------------------
    {
        EXPECT(Has(draw, "ext::ffxiv::diag::StageProbe(ext::ffxiv::diag::StageId::Dof,context,") && Has(draw, "ext::ffxiv::diag::StageNoteText(ext::ffxiv::diag::StageId::Dof,") &&
                   Has(draw, "ext::ffxiv::diag::StageNoteFrame("),
               "the stage hands the capture frame its numbers through the dump's late-values hooks\n");
        const std::string valuesBody = Body(draw, "std::stringStageValues()");
        EXPECT(!valuesBody.empty() && Has(valuesBody, "mode={}") && Has(valuesBody, "ModeName(params.mode)"), "the snapshot of the keys names the look\n");
        EXPECT(!Has(valuesBody, "StageProbe") && !Has(valuesBody, "Map("), "and the snapshot reads nothing from the GPU\n");
        EXPECT(Count(draw, "QueryPerformanceCounter") + Count(draw, "Qpc()") >= 1 && Has(draw, "inputs.deltaSeconds="),
               "the time the smoothing sees is the time since the pass last ran (a QPC pair), not a frame count\n");
        EXPECT(Has(draw, "state.lastRunQpc"), "kept at every run\n");
    }

    std::printf(g_failures ? "dof_pins: %d pin(s) failed\n" : "dof_pins: all pins hold\n", g_failures);
    return g_failures != 0;
}
