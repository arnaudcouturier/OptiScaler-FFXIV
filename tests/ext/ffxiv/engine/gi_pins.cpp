// Screen-space light bounce: the wiring pins, read as text from the sources (no device).
//
// What a wrong wiring would break, and what is pinned here:
//   GI answers with the march key off - the volumetric march is off by default and returns early in six places (its chained entry twice, its
//   owner entry twice, its seam entry twice); a stage placed after those returns would never run on a default install, so each exit hands the
//   stage the texture it was given (or, for the owner entries, the game's scene), and the seam's two exits fall through to the dispatch's next
//   member;
//   the game's tone-map draw runs exactly once on every path - an owner hook returns true only when it drew, the callers draw once otherwise,
//   a throw cannot make the march draw a second time after the stage drew, and the chained entries never draw;
//   the stamping contract - the stage's sources cannot reach the puddle stage's capture marker, and its answered-tracking is ONE counter of
//   its own, set before the attempt and shared by its three entries;
//   the arming - the feature bit is 20 (19 is the chara ambient's), the seam's feature tests carry it, and the arming lists of the engine
//   module's draw path carry the stage's own arming;
//   the flags - every staging Map carries D3D11_MAP_FLAG_DO_NOT_WAIT and every GetData D3D11_ASYNC_GETDATA_DONOTFLUSH in the flags argument;
//   the keys - every default is today's picture, in the config, the binder, the ini, the schema and the settings page;
//   the card - one card, placed after the god rays card, FFXIV-only;
//   the filter's kernel - the shader's two tables (int2 tap offsets, tap weights) read numerically against the rules header's, no fractional
//   tap position anywhere (the old table's 1.5 became 1 through int(), and every tap then had an even x+y: a checkerboard lattice), the centre
//   and the four direct neighbours in the table, the weights summing to 256 with a zero checkerboard response, the a-trous iteration reading
//   it at `step` times the offset with no clamp and no length;
//   the denoise - the clamp's and the filter's numbers (16, 0.1, 0.03, the steps 1, 2, 4, 8) equal in the shader and the rules; the guide and
//   the trace decode an origin through ONE function (so "traced" and "has a guide" cannot disagree); the filter reads no normal (the shading
//   normals are normal-mapped: every normal weight left streaks on cobbles) and skips a tap on its marker before it reads a colour; the pass
//   draws irradiance, guide, clamp, four iterations ping-ponging between two targets, apply, in that order, and the apply reads the final one;
//   the apply binds the normal view (it reads the material id from its alpha: the first version never bound it, so the special-row exclusion
//   never applied) and weighs the four texels around a pixel by whether they carry data;
//   the hit's radiance - the hit texel's own linear scene value (the stored texel squared, one argument, the clamp kMaxRadiance equal in the
//   shader and the rules), with no albedo read in the march loop, no albedo floor anywhere and the albedo still read by the apply;
//   the store's data marker - the alpha is the rays marched (found or not), written whenever any was marched, and the smooth tests nothing but
//   that alpha (it never looks at the colour, so a traced zero is averaged in);
//   the strength default - one number, GiRules.h's kDefaultStrength, read in every place that writes it (config, schema, settings page, ini
//   comment, menu tooltip, the features page), so one place drifting fails; and the pass's gain is the rules' kGain.
// No test here assumes a frame whose puddle stage ended in the game's own draw (the shape refusal, the dry skip, the no-answer case)
// reaches this stage: those three paths do not ask the chain's later members.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

// Comments removed (the house pins' own pattern), strings kept.
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

// Comments and whitespace removed, so a pin is about the code and not about its formatting.
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

// The body of the function whose (whitespace-free) signature text is `signature`, braces included; empty when it is not there. The signature
// must be the definition's, so a caller passes enough of it to be unique.
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

size_t At(const std::string& text, const std::string& needle) { return text.find(needle); }

// The numeric literals of a text, in order. The type names that carry a digit are dropped first, a minus sign belongs to the number after it,
// and a float suffix is skipped.
std::vector<double> Numbers(std::string text)
{
    for (const char* name : { "int2", "float2" })
    {
        const std::string drop = name;
        for (size_t at = text.find(drop); at != std::string::npos; at = text.find(drop))
            text.erase(at, drop.size());
    }
    std::vector<double> out;
    for (size_t i = 0; i < text.size();)
    {
        const char c = text[i];
        const bool digit = c >= '0' && c <= '9';
        const bool signedNumber = c == '-' && i + 1 < text.size() && ((text[i + 1] >= '0' && text[i + 1] <= '9') || text[i + 1] == '.');
        if (!digit && !signedNumber)
        {
            ++i;
            continue;
        }
        char* end = nullptr;
        out.push_back(std::strtod(text.c_str() + i, &end));
        i = size_t(end - text.c_str());
        if (i < text.size() && (text[i] == 'f' || text[i] == 'F'))
            ++i;
    }
    return out;
}

// The first number after `anchor` (skipping whatever is not a digit or a minus sign in between, at most 12 characters); NaN when the anchor or
// the number is not there.
double NumberAfter(const std::string& text, const std::string& anchor)
{
    const size_t at = text.find(anchor);
    if (at == std::string::npos)
        return std::nan("");
    size_t i = at + anchor.size();
    const size_t stop = std::min(text.size(), i + 12);
    while (i < stop && !((text[i] >= '0' && text[i] <= '9') || text[i] == '-'))
        ++i;
    if (i >= stop)
        return std::nan("");
    return std::strtod(text.c_str() + i, nullptr);
}

std::string Squash(const std::string& text)
{
    std::string out;
    for (char c : text)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            out += c;
    return out;
}

// The text between the first '{' after `anchor` and its matching '}' (braces included); empty when the anchor or the braces are not there.
std::string BraceBody(const std::string& text, const std::string& anchor)
{
    const size_t at = text.find(anchor);
    if (at == std::string::npos)
        return {};
    const size_t open = text.find('{', at + anchor.size());
    if (open == std::string::npos)
        return {};
    int depth = 0;
    for (size_t i = open; i < text.size(); ++i)
    {
        if (text[i] == '{')
            ++depth;
        else if (text[i] == '}' && --depth == 0)
            return text.substr(open, i - open + 1);
    }
    return {};
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: gi_pins <repo>\n");
        return 2;
    }
    const std::string repo = argv[1];
    const std::string march = Code(repo, "ffxiv/engine/GodraysDraw.cpp");
    const std::string giDraw = Code(repo, "ffxiv/engine/GiDraw.cpp");
    const std::string giPass = Code(repo, "ffxiv/engine/GiPass.cpp");
    const std::string giPassHeader = Code(repo, "ffxiv/engine/GiPass.h");
    const std::string giRules = Code(repo, "ffxiv/engine/GiRules.h");
    const std::string giHeader = Code(repo, "ffxiv/engine/Gi.h");
    const std::string giMenu = Code(repo, "ffxiv/engine/GiMenu.cpp");
    const std::string hooks = Code(repo, "ffxiv/engine/ChainHooks.h");
    const std::string shaftsDraw = Code(repo, "ffxiv/engine/ShaftsDraw.cpp");
    const std::string puddleDraw = Code(repo, "ffxiv/engine/PuddleToneDraw.cpp");
    const std::string enginePass = Code(repo, "ffxiv/engine/EnginePass.cpp");
    const std::string enginePassRules = Code(repo, "ffxiv/engine/EnginePassRules.h");
    const std::string seam = Code(repo, "ffxiv/lighting/FfxivLightingCapture.h");
    const std::string config = Code(repo, "ffxiv/FfxivConfig.h");
    const std::string binder = Code(repo, "core/ExtConfig.cpp");
    const std::string features = Code(repo, "ExtFeatures.h");
    const std::string layout = Code(repo, "core/MenuLayout.h");
    const std::string manifest = Code(repo, "core/MenuManifest.h");
    std::string ini = Raw(repo, "ini/FFXIV.ini");
    ini.erase(std::remove(ini.begin(), ini.end(), ''), ini.end()); // the fragment is stored with CRLF line ends
    const std::string rawHooks = Raw(repo, "ffxiv/engine/ChainHooks.h");
    const std::string shaderGi = Comments(Raw(repo, "ffxiv/engine/shaders/Gi.hlsl"));
    const std::string shaderMarch = Comments(Raw(repo, "ffxiv/engine/shaders/GiMarch.hlsl"));
    const std::string settingsPage = ReadFile(repo + "/docs/SETTINGS.md");
    const std::string schema = ReadFile(repo + "/tools/settings-schema.json");
    const std::string featuresPage = ReadFile(repo + "/docs/FEATURES.md");
    const std::string rawGiMenu = Comments(Raw(repo, "ffxiv/engine/GiMenu.cpp"));

    // ---- the chain's hooks: names and shapes DoF composes on ----------------------------------------------------------------------
    {
        EXPECT(Has(hooks, "inlineChainSceneFnchainGiScene=nullptr;"), "the stage's texture hook has the march's hook type and is null until the stage registers\n");
        EXPECT(Has(hooks, "inlineToneOwnerFntoneMapGiOwner=nullptr;"), "the stage's owner hook has the march's owner type and is null until the stage registers\n");
        EXPECT(Has(rawHooks, "True only if it drew") || Has(rawHooks, "true only if it drew") || Has(rawHooks, "True only when it drew") ||
                   Has(rawHooks, "false obliges the caller"),
               "the owner-return rule is written at the hooks' declarations\n");
        EXPECT(Has(giDraw, "chainGiScene=&ext::ffxiv::engine::gi::ChainScene;") && Has(giDraw, "toneMapGiOwner=&ext::ffxiv::engine::gi::ToneMapOwner;"),
               "the stage registers both hooks at start-up\n");
        EXPECT(Has(giDraw, "ext::FfxivConfig::giOnLoaded=&ext::ffxiv::engine::gi::OnConfigLoaded;"), "and its config hook\n");
        EXPECT(Count(puddleDraw, "chainGiScene") == 0 && Count(puddleDraw, "toneMapGiOwner") == 0,
               "the puddle stage's file is not touched by the stage: it hangs inside the march's own entries\n");
        EXPECT(Count(shaftsDraw, "chainGiScene") == 0 && Count(shaftsDraw, "toneMapGiOwner") == 0,
               "and neither is the shafts' file: the shafts reach the stage only through the march's entries\n");
    }

    // ---- GI answers with the march key off -----------------------------------------------------------------------------------------
    {
        const std::string chain = Body(march, "ID3D11ShaderResourceView*ChainScene(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene)");
        EXPECT(!chain.empty(), "the march's chained entry is found\n");
        EXPECT(Has(chain, "if(context==nullptr||scene==nullptr)returnnullptr;"), "the chained entry's only early return is the missing context or scene\n");
        EXPECT(!Has(chain, "||!Armed()") && !Has(chain, "||!MarchArmed()"), "the march being off is not an early return of the chained entry any more\n");
        // MarchArmed() is the march's own test; Armed() is the seam's (the march or the fog sun's night-curve copy) and never guards the march.
        EXPECT(Count(chain, "Armed()") == 1 && Has(chain, "if(MarchArmed()&&!(state.haveMarchFrame&&state.marchFrame==state.frames)){"),
               "the march's own armed test and its own one-answer gate guard only the march's attempt\n");
        const size_t attempt = At(chain, "if(MarchArmed()&&!(state.haveMarchFrame&&state.marchFrame==state.frames)){");
        const size_t answered = At(chain, "if(ours!=nullptr)returnours;");
        const size_t hand = At(chain, "if(ext::ffxiv::engine::chainGiScene!=nullptr)returnext::ffxiv::engine::chainGiScene(context,facts,scene);returnnullptr;}");
        EXPECT(attempt != std::string::npos && answered != std::string::npos && hand != std::string::npos && attempt < answered && answered < hand,
               "the march's attempt, then its answer is final, then every exit without an answer (off, already answered, declined, threw) hands "
               "the stage the texture the call was given\n");
        EXPECT(Count(chain, "chainGiScene(") == 1, "the stage is asked once per call of the chained entry\n");

        const std::string compose = Body(march, "voidComposeChainNext(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*&ours)");
        EXPECT(Has(compose, "if(ext::ffxiv::engine::chainGiScene==nullptr||ours==nullptr)return;") &&
                   Has(compose, "if(ID3D11ShaderResourceView*composed=ext::ffxiv::engine::chainGiScene(context,facts,ours))ours=composed;"),
               "when the march answered, the stage composes on the march's answer and a null from it is the march's answer unchanged\n");
        const std::string marchAtToneMap = Body(march, "ID3D11ShaderResourceView*MarchAtToneMap(State&state,ID3D11DeviceContext*context,");
        EXPECT(Count(marchAtToneMap, "ComposeChainNext(context,facts,ours);") == 1, "the march composes the stage once, in both roles\n");
        EXPECT(Has(marchAtToneMap, "state.session.NoteRun(Knob::Godrays);") && Has(marchAtToneMap, "FormatRun(state.params,"),
               "the march keeps its own run counter (the streak reset) and its run line\n");
        EXPECT(At(marchAtToneMap, "state.session.NoteRun(Knob::Godrays);") < At(marchAtToneMap, "ComposeChainNext(context,facts,ours);"),
               "and the march's counters finish before the stage is asked\n");

        const std::string owner = Body(march, "boolToneMapOwner(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene,DrawFndraw,void*user)");
        EXPECT(!owner.empty(), "the march's owner entry is found\n");
        EXPECT(Has(owner, "if(context==nullptr||scene==nullptr||draw==nullptr||!MarchArmed()){if(ext::ffxiv::engine::toneMapGiOwner!=nullptr)returnext::ffxiv::engine::toneMapGiOwner(context,facts,scene,draw,user);returnfalse;}"),
               "the owner entry's first exit (the march is off) hands the stage the game's scene\n");
        EXPECT(Has(owner, "if(state.haveMarchFrame&&state.marchFrame==state.frames){if(ext::ffxiv::engine::toneMapGiOwner!=nullptr)returnext::ffxiv::engine::toneMapGiOwner(context,facts,scene,draw,user);returnfalse;}"),
               "its second exit (this frame's tone map already answered) does the same, the stage's own gate deciding\n");
        EXPECT(Count(owner, "toneMapGiOwner(") == 2, "and there are exactly two\n");

        // The seam's two exits (the march off, the tone map already answered) return false and the dispatch's next member is the stage.
        const std::string seamDraw = Body(march, "boolSeamDraw(ID3D11DeviceContext*context,engine::Anchorsanchors,DrawFndraw,void*user)");
        EXPECT(!seamDraw.empty() && Count(seamDraw, "chainGiScene") == 0 && Count(seamDraw, "toneMapGiOwner") == 0,
               "the march's seam entry does not call the stage itself: its exits fall through to the dispatch, where the stage is the next member\n");
        EXPECT(Has(seamDraw, "if(!Armed()||context==nullptr)returnfalse;") &&
                   Has(seamDraw, "if(state.haveMarchFrame&&state.marchFrame==state.frames)returnfalse;"),
               "and both of those exits are plain falses\n");
        EXPECT(Count(enginePass, "ext::ffxiv::engine::gi::OnSeamDraw(") == 1, "the stage's seam entry is called from exactly one place\n");
        EXPECT(Has(enginePass, "if(ext::ffxiv::godrays::OnSeamDraw(context,anchors,draw,user))return;"
                               "if(ext::ffxiv::engine::gi::OnSeamDraw(context,anchors,draw,user))return;"),
               "the stage's seam entry is right behind the march's, so a false from the march always reaches it\n");
        EXPECT(Has(enginePass, "if(ext::ffxiv::shafts::OnEngineDraw(context,anchors,draw,user))return;") &&
                   Has(enginePass, "if(gloss::OnToneStageDraw(context,anchors,draw,user))return;"),
               "after the shafts' and the puddle stage's (the chain's order: shafts, puddles, march, light bounce)\n");

        // The stage's own entries start from the stage's own tests, never from the march's.
        EXPECT(Has(giDraw, "if(context==nullptr||scene==nullptr)returnnullptr;")&&Has(giDraw, "if(ArmedInternal(state)&&!(state.haveGiFrame&&state.giFrame==state.frames)){"), "the stage's chained entry applies the stage's own armed test to its own attempt only (depth of field is handed the texture on the exits without an answer)\n");
        EXPECT(Has(giDraw, "if(context==nullptr||scene==nullptr||draw==nullptr||!ArmedInternal(state))returnext::ffxiv::engine::toneMapAfterGiOwner(context,facts,scene,draw,user);"), "and so does its owner entry, whose exit hands the stage after it the game's scene\n");
        EXPECT(!Has(giDraw, "godrays::Armed"), "and none of them reads the march's arming\n");
    }

    // ---- the stage's inputs do not depend on the shafts or the march being armed ---------------------------------------------------
    {
        // The three recognitions the stage uses (the composite, the fog post, the tone map) are bits of the shader's own layout, computed when the
        // shader is created and stored whatever any key says, so a stage switched on from the menu after the shaders were created finds them.
        const std::string anchors = Body(enginePassRules, "constexprAnchorsDrawAnchors(constPassLayout&layout)noexcept");
        EXPECT(!anchors.empty() && Has(anchors, "kShaftsCompositeBit") && Has(anchors, "kShaftsToneMapBit") && Has(anchors, "kFogPostBit"),
               "the composite, tone map and fog post bits are set by the layout rule\n");
        for (const char* gate : { "Active(", "Armed(", "godrays", "shafts::", "gi::", "Cfg()" })
            EXPECT(!Has(anchors, gate), "and nothing in that rule reads a key or a module's arming (%s)\n", gate);
        // The composite stamp: written at shader creation, before anything that asks a key (the `logging` read is a log line only).
        const std::string created = Body(enginePass, "voidOnShaderCreated(ID3D11PixelShader*shader,constvoid*code,size_tsize,uint32_tcrc)");
        const size_t stamp = At(created, "ext::ffxiv::engine::gi::NoteShader(shader,code,size);");
        EXPECT(stamp != std::string::npos && !Has(created.substr(0, stamp), "Active(") && !Has(created.substr(0, stamp), "Armed("),
               "the composite stamp is written at shader creation under no key\n");
        EXPECT(stamp < At(created, "constboollogging="), "and before the one key the creation path reads\n");
    }

    // ---- the game's tone-map draw runs exactly once on every path ------------------------------------------------------------------
    {
        // The shafts' caller: draws once when the owner chain did not.
        const std::string withChain = Body(shaftsDraw, "boolDrawToneMapWithChain(ID3D11DeviceContext*context,constToneMapFacts&facts,ID3D11ShaderResourceView*scene,DrawFndraw,void*user,bool&drew)");
        EXPECT(!withChain.empty() && Count(withChain, "draw(user);") == 1, "the shafts' fallback chain has exactly one draw of its own\n");
        EXPECT(Has(withChain, "if(ext::ffxiv::engine::toneMapMarchOwner(context,facts,scene,draw,user))returntrue;}draw(user);returntrue;"),
               "the owner chain's true is the only way past the one draw (a false obliges the caller to draw)\n");

        // The march's owner role: its own catch draws once when the march threw before drawing, and the stage's turn is outside that try.
        const std::string runOwner = Body(march, "boolRunToneMapAsOwner(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene,DrawFndraw,void*user)");
        EXPECT(!runOwner.empty() && Count(runOwner, "draw(user);") == 1 && Has(runOwner, "if(!drew)draw(user);returntrue;}"),
               "the march's owner role draws in its catch only when it has not drawn\n");
        EXPECT(Has(runOwner, "}if(drew)returntrue;if(ext::ffxiv::engine::toneMapGiOwner!=nullptr)returnext::ffxiv::engine::toneMapGiOwner(context,facts,scene,draw,user);returnfalse;}"),
               "a march that declined hands the stage the owner turn AFTER its try block, true only if the stage drew, else a plain false\n");
        EXPECT(At(runOwner, "catch(...)") < At(runOwner, "toneMapGiOwner("),
               "so a throw after the stage drew can never reach the march's catch, which would draw the game's call a second time\n");
        EXPECT(Count(march, "draw(user);") == Count(Body(march, "ID3D11ShaderResourceView*MarchAtToneMap(State&state,ID3D11DeviceContext*context,"), "draw(user);") + 1,
               "the march's draws: its owner bind, and the catch above (nothing else draws the game's call)\n");

        // The stage's owner role.
        const std::string stage = Body(giDraw, "ID3D11ShaderResourceView*StageAtToneMap(State&state,ID3D11DeviceContext*context,");
        EXPECT(!stage.empty() && Count(stage, "draw(user);") == 1 && Has(stage, "draw(user);drew=true;returnours;}"),
               "the stage's one draw is the owner's bind, and `drew` is set only after it\n");
        EXPECT(Has(stage, "if(!owner)returnours;"), "the chained role returns its texture before any bind or draw\n");
        EXPECT(Count(giDraw, "PSSetShaderResources(0,1,") == 2, "only the owner path binds t0, once and restored\n");
        const std::string runStage = Body(giDraw, "boolRunToneMapAsOwner(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene,DrawFndraw,void*user)");
        EXPECT(Has(runStage, "StageAtToneMap(state,context,facts,scene,true,draw,user,drew);}catch(...)") && Has(runStage, "if(!drew)draw(user);returntrue;}if(drew)returntrue;returnext::ffxiv::engine::toneMapAfterGiOwner(context,facts,scene,draw,user);}") &&
                   Count(runStage, "draw(user);") == 1,
               "the stage's owner returns exactly what it drew, and its catch draws once only if the draw has not happened\n");
        const std::string ownerEntry = Body(giDraw, "boolToneMapOwnerInternal(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene,DrawFndraw,void*user)");
        EXPECT(Count(ownerEntry, "returnfalse;") == 0 && Count(ownerEntry, "returnext::ffxiv::engine::toneMapAfterGiOwner(") == 2 && Has(ownerEntry, "returnRunToneMapAsOwner(context,facts,scene,draw,user);") && Count(ownerEntry, "return") == 3,
               "the stage's owner entry returns false or the owner role's answer, nothing else\n");
        const std::string seamEntry = Body(giDraw, "boolOnSeamDrawInternal(ID3D11DeviceContext*context,engine::Anchorsanchors,DrawFndraw,void*user)");
        EXPECT(Has(seamEntry, "returnRunToneMapAsOwner(context,facts,scene.Get(),draw,user);") && Count(seamEntry, "returntrue") == 0,
               "the stage's seam entry returns true only through the owner role, which draws once\n");
        EXPECT(Has(giDraw, "boolToneMapOwner(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene,DrawFndraw,void*user){try{returnToneMapOwnerInternal(context,facts,scene,draw,user);}catch(...){returnfalse;}}"),
               "the exported owner hook never throws: an exception before any draw is a false, and the caller draws once\n");
        EXPECT(Has(giDraw, "ChainScene(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene){try{returnChainSceneInternal(context,facts,scene);}catch(...){returnnullptr;}}"),
               "and the exported chained hook never throws either: the chain's hold stands\n");
        EXPECT(Has(giDraw, "StageAtToneMap(state,context,facts,scene,false,nullptr,nullptr,drew)"), "the chained role is given no draw to call\n");

        // The seam's dispatch ends in the pass-through draw exactly once, so a false from the stage's seam entry still draws the game's call once.
        const std::string onGame = Body(enginePass, "voidOnGameDraw(ID3D11DeviceContext*context,DrawFndraw,void*user)");
        EXPECT(!onGame.empty() && Count(onGame, "ext::ffxiv::engine::gi::OnSeamDraw(") == 1,
               "the dispatch is found, and a false from the stage's seam entry falls through to the seam's own pass-through draw\n");
    }

    // ---- the stamping contract and the one-answer gate -----------------------------------------------------------------------------
    {
        for (const std::string* source : { &giDraw, &giPass, &giPassHeader, &giRules, &giHeader, &giMenu })
        {
            EXPECT(!Has(*source, "usedCapture"), "the stage's sources never reference the shared stamp (usedCapture)\n");
            EXPECT(!Has(*source, "capture.id"), "and never reference the capture id it is compared with\n");
        }
        EXPECT(Count(giDraw, "uint64_tgiFrame=0;") == 1 && Count(giDraw, "boolhaveGiFrame=false;") == 1,
               "the stage's answered-tracking is ONE counter of its own, with the flag that separates no answer yet from a counter value of 0\n");
        // Set BEFORE the attempt in each of the three entries, so a decline is never retried inside one frame and counts once.
        const std::string chained = Body(giDraw, "ID3D11ShaderResourceView*ChainSceneInternal(ID3D11DeviceContext*context,");
        const std::string ownerEntry = Body(giDraw, "boolToneMapOwnerInternal(ID3D11DeviceContext*context,");
        const std::string seamEntry = Body(giDraw, "boolOnSeamDrawInternal(ID3D11DeviceContext*context,");
        EXPECT(At(chained, "state.giFrame=state.frames;") < At(chained, "StageAtToneMap("), "the chained entry sets the counter before the attempt\n");
        EXPECT(At(ownerEntry, "state.giFrame=state.frames;") < At(ownerEntry, "RunToneMapAsOwner("), "the owner entry sets the counter before the attempt\n");
        EXPECT(At(seamEntry, "state.giFrame=state.frames;") < At(seamEntry, "RunToneMapAsOwner("), "the seam entry sets the counter before the attempt\n");
        EXPECT(Has(chained, "if(ArmedInternal(state)&&!(state.haveGiFrame&&state.giFrame==state.frames)){") &&
                   Has(ownerEntry, "if(state.haveGiFrame&&state.giFrame==state.frames){returnext::ffxiv::engine::toneMapAfterGiOwner(context,facts,scene,draw,user);}") &&
                   Has(seamEntry, "if(state.haveGiFrame&&state.giFrame==state.frames)returnfalse;"),
               "each entry declines a tone map the stage has already had its turn at in this rendered frame\n");
        EXPECT(Has(giDraw, "state.step.OpenAtComposite(state.frames);") && Count(giDraw, "OpenAtComposite(") == 1 && !Has(giDraw, "AdvanceFrame("),
               "the rendered-frame counter opens at the stage's own composite capture, once a frame (the game draws the composite twice a frame)\n");
        // The refusals the family shares.
        EXPECT(Has(giDraw, "Decline::SceneHdr") && Has(giDraw, "shafts::SceneHdrInactive(state.enabled,SceneHdrRequested())"), "the scene-HDR decline\n");
        EXPECT(Has(giDraw, "Decline::CharaReplay") && Has(giDraw, "if(facts.replay)"), "the character-preview replay refusal\n");
        EXPECT(Has(giDraw, "state.session.NoteDecline(Knob::Gi)") && Has(giDraw, "state.session.NoteRun(Knob::Gi)") && Has(giDraw, "kConsecutiveDeclineLimit"),
               "the 64-refusal stand-down on the stage's own knob\n");
        EXPECT(Has(giDraw, "ext::ffxiv::chara::ReplayScopeown;ext::ffxiv::engine::OwnWorkownWork;"), "the pass runs as the fork's own work, not as a game draw\n");
        // Every staging Map and every GetData carries its flag in the flags argument.
        EXPECT(Has(giDraw, "context->Map(staging,0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped)"), "the camera ring's Map carries DO_NOT_WAIT\n");
        EXPECT(Count(giPass, "GetData(") == 3 && Count(giPass, "D3D11_ASYNC_GETDATA_DONOTFLUSH)") == 3, "every GetData carries DONOTFLUSH in its flags argument\n");
        // The pass's pipeline state: captured and put back explicitly, never through the device-context state swap.
        EXPECT(Has(giPass, R"x(ext::core::explicit_state::GuardstateGuard(context,context1.Get(),device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1,kSpec);)x") &&
                   Has(giPass, R"x(stateGuard.Neutralise();)x"),
               "the game's state is captured, the private state's defaults (no HS, DS, GS, stream-out or predicate) are set explicitly and a scope guard puts it back\n");
        EXPECT(!Has(giPass, "SwapDeviceContextState") && !Has(giPass, "CreateDeviceContextState"),
               "no device-context state swap remains: it does nothing when the draw hook is nested\n");
        EXPECT(Has(giPass, R"x(explicit_state::SpeckSpec{8,0,1};)x") && Has(giPass, "static_assert(kSpec.psSrvCount==kSlots") &&
                   Has(giPass, "constexpruint32_tkSlotGuide=7;") && Has(giPass, "constexpruint32_tkSlots=8;"),
               "the pass declares exactly the state it touches: pixel-shader resource slots 0-7 (kSlots, the guide at 7), no sampler, constant buffer 0\n");
        {
            // The guard sits exactly where the swap sat: after the scene copy, the parameter buffer's Map and Unmap and the timing collection. The
            // copy is made under the game's own state (a game predicate skips it, as it did under the swap), so a guard above it changes the output.
            const size_t mapAt = giPass.find("context->Map(_params.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped)");
            const size_t copyAt = mapAt == std::string::npos ? mapAt : giPass.rfind("context->CopyResource(_scene.Get(),sceneResource.Get());", mapAt);
            const size_t unmapAt = giPass.find("context->Unmap(_params.Get(),0);", mapAt);
            const size_t timingAt = giPass.find("CollectGpuTiming(context);", unmapAt);
            const size_t guardAt = giPass.find("ext::core::explicit_state::GuardstateGuard(");
            EXPECT(copyAt != std::string::npos && mapAt != std::string::npos && unmapAt != std::string::npos && timingAt != std::string::npos &&
                       guardAt != std::string::npos && Count(giPass, "ext::core::explicit_state::GuardstateGuard(") == 1,
                   "the scene copy, the parameter Map and Unmap, the timing collection and the one guard are all found in Run\n");
            EXPECT(copyAt < mapAt && mapAt < unmapAt && unmapAt < timingAt && timingAt < guardAt,
                   "the guard comes after the scene copy, the parameter buffer's Map/Unmap and the timing collection (the copy runs under the game's state)\n");
        }
        EXPECT(!Has(giDraw, "D3D11_MAP_WRITE") && !Has(giDraw, "UpdateSubresource(") && !Has(giDraw, "CopyResource(sceneResource"), "the stage writes no game resource from its draw file\n");
    }

    // ---- the arming: the bit, the seam, the draw path's lists ------------------------------------------------------------------------
    {
        EXPECT(Has(features, "Gi=1ull<<20,"), "the feature bit is 20\n");
        EXPECT(Has(features, "CharaAmbient=1ull<<19,") && Count(features, "1ull<<19") == 1, "19 is the chara ambient's alone\n");
        EXPECT(Count(features, "boolActiveAny(") == 1 && Has(features, "template<class...Fs>inlineboolActiveAny(Ffirst,Fs...rest)"),
               "ActiveAny is the one variadic function: the stage appended its flag, it added no overload\n");
        EXPECT(Has(seam, "ext::ActiveAny(ext::F::ContactShadows,ext::F::EnginePassOverrides,ext::F::LightShafts,ext::F::GodraysVolumetric,ext::F::CharaAmbient,ext::F::Gi"),
               "the draw seam's outer test carries the stage's bit\n");
        EXPECT(Has(seam, "if(!ext::ActiveAny(ext::F::EnginePassOverrides,ext::F::LightShafts,ext::F::GodraysVolumetric,ext::F::Gi"),
               "and so does the engine patch's inner test\n");
        EXPECT(Count(enginePass, "ext::ffxiv::engine::gi::Armed()") == 5, "the five arming sites of the engine module's draw path carry the stage's arming\n");
        EXPECT(Has(enginePass, "ext::ActiveAny(ext::F::EnginePassOverrides,ext::F::LightShafts,ext::F::GodraysVolumetric,ext::F::Gi"), "the pixel-shader bind test carries it\n");
        EXPECT(Has(enginePass, "if(ext::Active(ext::F::Gi))ext::ffxiv::engine::gi::SampleSceneHdr();"), "the stage samples the scene HDR state on a bind\n");
        EXPECT(Has(enginePass, "ext::ffxiv::engine::gi::NoteShader(shader,code,size);") && Has(enginePass, "ext::ffxiv::engine::gi::Release();"),
               "the composite stamp at shader creation and the teardown release\n");
        EXPECT(Has(giDraw, "ext::SetActive(ext::F::Gi,wanted);") && Has(giDraw, "ext::SetActive(ext::F::Gi,false);"), "the stage publishes its own bit and takes it back at the stand-down\n");
    }

    // ---- the keys: every default is today's picture ----------------------------------------------------------------------------------
    {
        EXPECT(Has(config, "CustomOptional<bool>gi{false};") && Has(config, "CustomOptional<int32_t>giRays{4};") && Has(config, "CustomOptional<int32_t>giSteps{16};") &&
                   Has(config, "CustomOptional<float>giRange{5.0f};") && Has(config, "CustomOptional<float>giStrength{") &&
                   Has(config, "CustomOptional<std::string>giResolution{\"half\"};"),
               "the six keys and their defaults in the config\n");
        for (const char* key : { "Gi", "GiRays", "GiSteps", "GiRange", "GiStrength", "GiResolution" })
        {
            const std::string name = key;
            EXPECT(Has(binder, "b.Add(\"FFXIV\",\"" + name + "\","), "the binder carries %s\n", key);
            EXPECT(Has(schema, "\"key\": \"[FFXIV] " + name + "\""), "the schema carries %s\n", key);
            EXPECT(Has(settingsPage, "`" + name + "`") || Has(settingsPage, "| " + name + " ") || Has(settingsPage, name), "the settings page carries %s\n", key);
        }
        EXPECT(Has(ini, "\nGi=false\n"), "the master switch is off in the ini fragment\n");
        for (const char* key : { "GiRays=auto", "GiSteps=auto", "GiRange=auto", "GiStrength=auto", "GiResolution=auto" })
            EXPECT(Has(ini, std::string("\n") + key + "\n"), "the ini carries %s (auto = the default)\n", key);
        EXPECT(Has(config, "staticinlineBoolHookgiOnLoaded=nullptr;") &&
                   Has(config, "if(giOnLoaded!=nullptr)giOnLoaded(gi.value_or_default());"),
               "the config publishes the master switch to the stage at the load\n");
    }

    // ---- the card and the shaders ----------------------------------------------------------------------------------------------------
    {
        EXPECT(Has(giMenu, "\"ffxiv_gi\",\"FFXIV\",\"Lightbounce(FFXIV)\""), "one card, in the FFXIV tab\n");
        EXPECT(Has(layout, "{\"ffxiv_gi\",\"FFXIV\",\"ffxiv_godrays\"},") && Has(layout, "{\"ffxiv_engine_light\",\"FFXIV\",\"ffxiv_gi\"},") && Has(layout, "{\"ffxiv_atmosphere\",\"FFXIV\",\"ffxiv_engine_light\"},") && Has(layout, "{\"ffxiv_dof\",\"FFXIV\",\"ffxiv_atmosphere\"},"), "placed after the god rays card (the night grade card follows it, then the atmosphere's, then the depth-of-field card, then the jitter card)\n");
        EXPECT(Has(manifest, "\"ffxiv_gi\""), "and FFXIV-only\n");
        EXPECT(Has(shaderGi, "#include \"GiMarch.hlsl\""), "the stage marches through its own file\n");
        EXPECT(!Has(shaderGi, "SceneMarch") && !Has(shaderMarch, "SceneMarch") && !Has(shaderGi, "Godrays.hlsl"), "and shares no source with the fix line's march or the god rays\n");
    }

    // ---- the hit's radiance, the data marker and the gain -------------------------------------------------------------------------------
    {
        const size_t radianceAt = shaderGi.find("float GiBounceRadiance(");
        const size_t irradianceAt = shaderGi.find("float3 GiIrradianceOf(");
        const size_t psGiAt = shaderGi.find("float4 PS_Gi(");
        const size_t guideAt = shaderGi.find("float4 PS_Guide(");
        const size_t clampAt = shaderGi.find("float4 PS_Clamp(");
        const size_t atrousAt = shaderGi.find("float4 GiAtrous(");
        const size_t atrousEntriesAt = shaderGi.find("float4 PS_Atrous0(");
        const size_t applyAt = shaderGi.find("float4 PS_Apply(");
        const size_t smoothAt = guideAt; // the slice after PS_Gi (the guide, the clamp and the filter are all after it)
        const bool ordered = radianceAt != std::string::npos && irradianceAt != std::string::npos && psGiAt != std::string::npos &&
                             guideAt != std::string::npos && clampAt != std::string::npos && atrousAt != std::string::npos &&
                             atrousEntriesAt != std::string::npos && applyAt != std::string::npos && radianceAt < irradianceAt && irradianceAt < psGiAt &&
                             psGiAt < guideAt && guideAt < clampAt && clampAt < atrousAt && atrousAt < atrousEntriesAt && atrousEntriesAt < applyAt;
        EXPECT(ordered, "the shader's functions are found in the order the pins slice them: radiance, irradiance, PS_Gi, PS_Guide, PS_Clamp, GiAtrous, its four entries, PS_Apply\n");
        if (ordered)
        {
            const std::string radianceFn = Squash(shaderGi.substr(radianceAt, irradianceAt - radianceAt));
            const std::string irradianceFn = Squash(shaderGi.substr(irradianceAt, psGiAt - irradianceAt));
            const std::string psGi = Squash(shaderGi.substr(psGiAt, smoothAt - psGiAt));
            const std::string guideFn = Squash(shaderGi.substr(guideAt, clampAt - guideAt));
            const std::string clampFn = Squash(shaderGi.substr(clampAt, atrousAt - clampAt));
            const std::string smoothFn = Squash(shaderGi.substr(atrousAt, atrousEntriesAt - atrousAt)); // the a-trous iteration
            const std::string atrousEntries = Squash(shaderGi.substr(atrousEntriesAt, applyAt - atrousEntriesAt));
            const std::string applyFn = Squash(shaderGi.substr(applyAt));
            // The hit's radiance: the hit texel's own linear scene value, one argument, the stored texel squared, refused over the clamp.
            EXPECT(Has(radianceFn, "floatGiBounceRadiance(floatsceneStored)"), "the hit radiance takes the scene's stored texel alone (no albedo argument)\n");
            EXPECT(Has(radianceFn, "constfloatradiance=sceneStored*sceneStored;"), "and is that stored value squared (the linear domain), nothing divided\n");
            EXPECT(Has(radianceFn, "radiance>kMaxRadiance"), "and a value over the clamp is refused\n");
            EXPECT(!Has(radianceFn, "lbedo") && !Has(irradianceFn, "lbedo"), "no albedo is read or named in the radiance or in the march loop\n");
            EXPECT(!Has(irradianceFn, "Albedo.Load") && !Has(irradianceFn, "albedoHit"), "the march loop does not load the albedo target\n");
            EXPECT(Has(irradianceFn, "radiance.x=GiBounceRadiance(sceneHit.x);") && Has(irradianceFn, "radiance.y=GiBounceRadiance(sceneHit.y);") &&
                       Has(irradianceFn, "radiance.z=GiBounceRadiance(sceneHit.z);"),
                   "each channel of the hit's scene texel goes through the radiance on its own\n");
            EXPECT(!Has(shaderGi, "kAlbedoFloor") && !Has(giRules, "kAlbedoFloor"), "the albedo floor is gone from the shader and from the rules\n");
            EXPECT(Has(applyFn, "Albedo.Load(int3(at,0))"), "the apply still reads the origin's own albedo (the Albedo slot stays bound)\n");
            const double hlslBound = NumberAfter(Squash(shaderGi), "kMaxRadiance=");
            const double rulesBound = NumberAfter(giRules, "kMaxRadiance=");
            EXPECT(hlslBound == rulesBound && rulesBound == 8.0, "the radiance clamp is one number in the shader and the rules (%g, %g)\n", hlslBound, rulesBound);

            // The data marker: the rays marched, written whenever any was, found or not.
            EXPECT(Has(irradianceFn, "pairs=0.0;"), "an origin that is not traced leaves the marker at 0\n");
            const size_t countAt = irradianceFn.find("tracedCount+=1.0;");
            const size_t marchAt = irradianceFn.find("GiMarchFrom(");
            EXPECT(countAt != std::string::npos && marchAt != std::string::npos && countAt < marchAt && Count(irradianceFn, "tracedCount+=1.0;") == 1,
                   "a ray is counted once, at the point it is marched, before its hit is known\n");
            const size_t noneAt = irradianceFn.find("if(!(tracedCount>0.0))returnfloat3(0.0,0.0,0.0);");
            EXPECT(!Has(irradianceFn, "pairsCount") && !Has(irradianceFn, "if(!(sum>0.0))") && noneAt != std::string::npos &&
                       noneAt < irradianceFn.find("pairs=tracedCount;"),
                   "no count of hits remains; only an origin that marched no ray returns before the marker, and a zero sum is not 'no data'\n");
            EXPECT(Has(irradianceFn, "pairs=tracedCount;") && Has(irradianceFn, "returnsum*(3.14159265358979323846/float(max(rays,1)))*viewFade*fade;"),
                   "the marker is the traced count and the estimate is the sum over the rays asked for, not over the hits\n");
            EXPECT(!Has(irradianceFn, "if(!(viewFade") && !Has(irradianceFn, "if(viewFade") && !Has(irradianceFn, "viewFade>0") &&
                       Has(irradianceFn, "constfloatviewFade=saturate(dot(n,-normalize(originView)));"),
                   "the pixel's own view fade only scales the estimate: a grazing surface (fade 0) is a traced zero, never an early return that drops the marker\n");
            EXPECT(Has(irradianceFn, "if(!(fade>0.0))returnfloat3(0.0,0.0,0.0);"),
                   "the held state's fade at 0 (the frame's, not the pixel's) leaves every origin untraced: no data\n");
            EXPECT(Has(psGi, "returnfloat4(irradiance,pairs);"), "PS_Gi stores the estimate with the marker in the alpha\n");
            EXPECT(Has(psGi, "if(SkyOf(depth)>0.5)returnfloat4(0.0,0.0,0.0,0.0);"), "the sky stays out of the data (alpha 0)\n");
            // The filter tests the marker before it looks at a colour, so a traced zero is averaged in and a hole's colour is never read.
            EXPECT(Has(smoothFn, "if(!(value.a>0.0)||value.a!=value.a)continue;") && !Has(smoothFn, "dot(value.rgb") && !Has(smoothFn, "value.r>") &&
                       !Has(smoothFn, "value.rgb>") && At(smoothFn, "if(!(value.a>0.0)||value.a!=value.a)continue;") < At(smoothFn, "sum+=value.rgb*w;"),
                   "the filter skips a tap on its alpha before it reads the colour, and never tests the colour\n");
            EXPECT(Has(smoothFn, "if(!(centre.a>0.0)||centre.a!=centre.a||!(centreInverse>0.0))returnfloat4(0.0,0.0,0.0,0.0);") && Has(smoothFn, "returnfloat4(sum/weight,1.0);"),
                   "a texel with no data (or no guide) has no answer of its own, and one with data answers with alpha 1\n");
        // The denoise: the numbers equal in the shader and the rules, one decode for the trace and the guide, no normal in the filter, the draws' order.
        {
            const std::string squashedShader = Squash(shaderGi);
            EXPECT(NumberAfter(squashedShader, "kClampRatio=") == 16.0 && NumberAfter(giRules, "kClampRatio=") == 16.0, "the clamp ratio is 16 in the shader and the rules\n");
            EXPECT(NumberAfter(squashedShader, "kClampFloor=") == 0.1 && NumberAfter(giRules, "kClampFloor=") == 0.1, "the clamp floor is 0.1 in the shader and the rules\n");
            EXPECT(NumberAfter(squashedShader, "kDepthSigma=") == 0.03 && NumberAfter(giRules, "kDepthSigma=") == 0.03, "the depth sigma is 0.03 in the shader and the rules\n");
            // The four iterations' steps: the entries call the filter with 1, 2, 4, 8 in order, the rules' table says the same.
            std::vector<double> entrySteps;
            for (size_t at = atrousEntries.find("GiAtrous(position.xy,"); at != std::string::npos; at = atrousEntries.find("GiAtrous(position.xy,", at + 1))
                entrySteps.push_back(std::strtod(atrousEntries.c_str() + at + std::string("GiAtrous(position.xy,").size(), nullptr));
            const std::vector<double> rulesSteps = Numbers(BraceBody(giRules, "kFilterSteps[kFilterIterations]"));
            EXPECT(entrySteps == std::vector<double>({ 1.0, 2.0, 4.0, 8.0 }) && rulesSteps == entrySteps, "the four entries' steps (1, 2, 4, 8) equal the rules' table\n");
            EXPECT(Count(atrousEntries, "GiAtrous(position.xy,") == 4, "and there are exactly four iterations\n");
            // The trace and the guide decode an origin through one function.
            EXPECT(Count(squashedShader, "GiSurfaceAt(") == 3 && Has(guideFn, "if(!GiSurfaceAt(at,originView,n))returnfloat4(0.0,0.0,0.0,0.0);") &&
                       Has(irradianceFn, "if(!GiSurfaceAt(at,originView,n))returnfloat3(0.0,0.0,0.0);"),
                   "the trace (GiIrradianceOf) and the guide (PS_Guide) decode the origin through the one GiSurfaceAt (its definition and the two callers)\n");
            EXPECT(Has(guideFn, "if(SkyOf(depth)>0.5)returnfloat4(0.0,0.0,0.0,0.0);") && Has(guideFn, "returnfloat4(0.0,0.0,0.0,-originView.z);"),
                   "the guide keeps the sky out like the trace does and carries the depth distance in its alpha\n");
            // No normal in the filter: not in the clamp, not in the iteration, not in the guide's output.
            for (const std::string* body : { &clampFn, &smoothFn, &guideFn })
                EXPECT(!Has(*body, "Normal.Load") && !Has(*body, "centreNormal") && !Has(*body, "normalWeight"), "the clamp, the filter and the guide use no normal as an edge stop\n");
            EXPECT(!Has(squashedShader, "kNormalPower") && !Has(squashedShader, "kPlaneSigma") && !Has(squashedShader, "GiGuidePosition"), "and none of the normal or plane-through-the-normal constants is left\n");
            // The clamp: the texel is not in its own reference, the cap is the ratio times the mean plus the floor, the scale is the colour as a whole.
            EXPECT(Has(clampFn, "if(offset.x==0&&offset.y==0)continue;") && Has(clampFn, "constfloatcap=kClampRatio*(neighbourSum/neighbours)+kClampFloor;") &&
                       Has(clampFn, "returnfloat4(centre.rgb*(cap/luminance),centre.a);") && Has(clampFn, "if(!(neighbours>0.0))returncentre;"),
                   "the clamp leaves the texel out of its own mean, caps at ratio x mean + floor, scales the whole colour, passes the marker, and keeps a texel with no neighbour\n");
            EXPECT(Has(clampFn, "if(!(value.a>0.0)||value.a!=value.a)continue;"), "the clamp reads only the neighbours that carry data\n");
            // The filter's depth weight: the slope-predicted inverse depth, sigma of the centre's inverse depth.
            EXPECT(Has(smoothFn, "constfloatpredicted=centreInverse+dot(slope,float2(offset));") &&
                       Has(smoothFn, "constfloatdepthWeight=exp(-abs(tapInverse-predicted)/(kDepthSigma*centreInverse));") &&
                       Has(smoothFn, "constfloatw=kSmoothWeights[tap]*depthWeight;"),
                   "a tap weighs by the kernel and by how far its inverse depth is from the centre's depth plane\n");
            EXPECT(Has(squashedShader, "returnabs(stepPlus)<=abs(stepMinus)?stepPlus:stepMinus;"), "the slope along an axis is the smaller one-sided step where both sides exist\n");
            // The pass: the draws in order, the two filter targets ping-ponged, the final one read by the apply.
            const size_t guideDraw = giPass.find("PSSetShader(_guideShader.Get(),nullptr,0);");
            const size_t clampDraw = giPass.find("PSSetShader(_clampShader.Get(),nullptr,0);");
            const size_t atrousDraw = giPass.find("PSSetShader(_atrousShader[iteration].Get(),nullptr,0);");
            const size_t applyDraw = giPass.find("PSSetShader(_applyShader.Get(),nullptr,0);");
            const size_t giDrawAt = giPass.find("PSSetShader(_giShader.Get(),nullptr,0);");
            EXPECT(giDrawAt != std::string::npos && guideDraw != std::string::npos && clampDraw != std::string::npos && atrousDraw != std::string::npos &&
                       applyDraw != std::string::npos && giDrawAt < guideDraw && guideDraw < clampDraw && clampDraw < atrousDraw && atrousDraw < applyDraw,
                   "the pass draws the irradiance, the guide, the clamp, the filter iterations and the apply, in that order\n");
            EXPECT(Has(giPass, "for(uint32_titeration=0;iteration<4;++iteration)") && Has(giPass, "constboolfromA=(iteration&1u)==0;") &&
                       Has(giPass, "ID3D11RenderTargetView*target=fromA?filterBRtv:filterARtv;") &&
                       Has(giPass, "filterViews[kSlotIrradiance]=fromA?_filterA.srv.Get():_filterB.srv.Get();"),
                   "the four iterations ping-pong A to B, B to A, A to B, B to A\n");
            EXPECT(Has(giPass, "filterARtv=_filterA.rtv.Get();") && Has(giPass, "filterViews[kSlotIrradiance]=_irradiance.srv.Get();") &&
                       Has(giPass, "PSSetShader(_clampShader.Get(),nullptr,0);"),
                   "the clamp reads the raw irradiance and writes the first filter target\n");
            EXPECT(Has(giPass, "applyViews[kSlotIrradiance]=_filterA.srv.Get();"),
                   "the apply reads the filter's final target (A)\n");
            EXPECT(Has(giPassHeader, "SmoothView()constnoexcept{return_filterA.srv.Get();}") && Has(giPassHeader, "IrradianceView()constnoexcept{return_irradiance.srv.Get();}") &&
                       Has(giPassHeader, "GuideView()constnoexcept{return_guide.srv.Get();}"),
                   "SmoothView is the final filtered target and IrradianceView the raw one (the lab and the tests read them)\n");
            // The apply: the normal view is bound (the id byte), the guide slot is bound, the data-weighted read.
            EXPECT(Has(giPass, "applyViews[kSlotNormal]=inputs.normal;") && Has(giPass, "applyViews[kSlotGuide]=_guide.srv.Get();"),
                   "the apply binds the normal view (its alpha is the material id the special-row exclusion reads) and the guide\n");
            EXPECT(Has(applyFn, "float4(topLeft.a>0.0?1.0:0.0,topRight.a>0.0?1.0:0.0,bottomLeft.a>0.0?1.0:0.0,bottomRight.a>0.0?1.0:0.0)") && Has(applyFn, "if(!(shareSum>0.0))returnscene;"),
                   "the apply weighs the four texels by their bilinear share and by whether they carry data, and keeps the scene where none does\n");
            EXPECT(NumberAfter(giRules, "kDefaultStrength=") == 0.55, "the default strength is 0.55 (the rain square's speckle bar; 2026-11-07)\n");
        }
        }
        // The capture frame's two numbers (the stage line and the log): the pass reads its own targets back only when the draw says it is the capture
        // frame, with one blocking staging read of the raw and the final target, and the numbers come from the rules' own function.
        {
            EXPECT(Has(giPassHeader, "boolcaptureStats=false;"), "the pass's inputs carry the capture flag, off by default (the lab and every ordinary frame never read back)\n");
            EXPECT(Count(giPass, "context->Map(") == 2 && Count(giPass, "D3D11_USAGE_STAGING") == 1 && Has(giPass, "context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)"),
                   "the pass maps its own parameter buffer and, in one helper, a staging copy with a blocking read (the capture frame's)\n");
            const size_t statsAt = giPass.find("if(inputs.captureStats){");
            const size_t guardAt = giPass.find("ext::core::explicit_state::GuardstateGuard(");
            const size_t drawsEnd = giPass.find("context->PSSetShaderResources(0,kSlots,none);");
            EXPECT(statsAt != std::string::npos && drawsEnd != std::string::npos && guardAt < drawsEnd && drawsEnd < statsAt &&
                       Has(giPass, "if(inputs.captureStats){std::vector<float>raw,filtered;if(ReadTarget(context,_irradiance,raw)&&ReadTarget(context,_filterA,filtered)){"),
                   "the readback sits after the draws, under the guard, behind the capture test, over the raw and the final target\n");
            EXPECT(Has(giPass, "ComputeCaptureStats(raw.data(),filtered.data(),_irradianceWidth,_irradianceHeight,ClampStrength(inputs.params.strength)*kGain)"),
                   "the numbers come from the rules' function, over the raw and the final target, with the strength times the gain as the light scale\n");
            EXPECT(Has(giDraw, "inputs.captureStats=ext::ffxiv::diag::StageOpen(ext::ffxiv::diag::StageId::Gi);"), "the draw sets the flag from the stage dump's open state: the capture frame only\n");
            EXPECT(Has(giDraw, "FormatCaptureStats(state.pass.LastCaptureStats())") && Has(giDraw, "FormatCaptureLine(state.pass.LastCaptureStats())"),
                   "the stage line's words and the log line come from the rules' formatters\n");
            const std::string values = Body(giDraw, "std::stringStageValues()");
            EXPECT(!values.empty() && !Has(values, "captureText"),
                   "the stage's values text does not carry the capture frame's words (it is read before the stage runs: they would be the previous capture's)\n");
            EXPECT(Has(giDraw, "state.captureText=FormatCaptureStats(state.pass.LastCaptureStats());"), "the words are kept for the frame-time call that puts them on the line\n");
        }
        // The rules carry the same marker and radiance.
        EXPECT(Has(giRules, "constexprfloatDataMarker(uint32_traysTraced)noexcept") && Has(giRules, "returnraysTraced>0?float(raysTraced):0.0f;"),
               "the rules' data marker is the rays traced, 0 for none\n");
        EXPECT(Has(giRules, "constexprboolHasData(floatmarker)noexcept") && Has(giRules, "returnmarker>0.0f&&marker==marker;"), "and data is a marker above 0\n");
        EXPECT(Has(giRules, "constexprfloatBounceRadiance(floatsceneStored)noexcept") && Has(giRules, "constfloatradiance=sceneStored*sceneStored;"),
               "the rules' hit radiance is the stored scene value squared, one argument\n");
        // The pass's gain is the rules' kGain, applied to the strength once, on the shader's fourth tuning component.
        EXPECT(Has(giPass, "ClampStrength(inputs.params.strength)*kGain);"), "the pass hands the shader the strength times the rules' gain\n");
        EXPECT(Has(Squash(Raw(repo, "ffxiv/engine/shaders/Gi.hlsl")), "w=thestrengthkeytimesthepass'sowngain"),
               "and the shader names that component as the strength times the pass's own gain\n");
    }

    // ---- the strength default: one number, every place that writes it ---------------------------------------------------------------
    {
        const double rulesDefault = NumberAfter(giRules, "kDefaultStrength=");
        EXPECT(rulesDefault == rulesDefault && rulesDefault > 0.0 && rulesDefault <= 1.0, "the rules' default strength reads as a number in (0, 1] (%g)\n", rulesDefault);
        EXPECT(NumberAfter(config, "giStrength{") == rulesDefault, "the config's default strength equals the rules' (%g against %g)\n", NumberAfter(config, "giStrength{"), rulesDefault);
        const size_t schemaAt = schema.find("\"name\": \"GiStrength\"");
        const size_t schemaEnd = schemaAt == std::string::npos ? schemaAt : schema.find("\"flags\"", schemaAt);
        const std::string schemaEntry = schemaEnd == std::string::npos ? std::string() : schema.substr(schemaAt, schemaEnd - schemaAt);
        EXPECT(!schemaEntry.empty(), "the schema entry of GiStrength is found\n");
        EXPECT(NumberAfter(schemaEntry, "\"defaultMeans\":") == rulesDefault, "the schema's defaultMeans equals the rules' (%g against %g)\n", NumberAfter(schemaEntry, "\"defaultMeans\":"), rulesDefault);
        EXPECT(NumberAfter(schemaEntry, "the default ") == rulesDefault, "the schema's description names the same default (%g)\n", NumberAfter(schemaEntry, "the default "));
        const size_t rowAt = settingsPage.find("| `GiStrength` |");
        const size_t rowEnd = rowAt == std::string::npos ? rowAt : settingsPage.find('\n', rowAt);
        const std::string row = rowEnd == std::string::npos ? std::string() : settingsPage.substr(rowAt, rowEnd - rowAt);
        EXPECT(!row.empty(), "the settings page row of GiStrength is found\n");
        EXPECT(NumberAfter(row, "| number 0..1 | `") == rulesDefault, "the settings page's default column equals the rules' (%g)\n", NumberAfter(row, "| number 0..1 | `"));
        EXPECT(NumberAfter(row, "the default ") == rulesDefault, "the settings page's description names the same default (%g)\n", NumberAfter(row, "the default "));
        const size_t iniAt = ini.find("\nGiStrength=auto\n");
        const size_t iniStart = iniAt == std::string::npos ? iniAt : (ini.rfind("\n\n", iniAt) == std::string::npos ? 0 : ini.rfind("\n\n", iniAt));
        const std::string iniBlock = iniAt == std::string::npos ? std::string() : ini.substr(iniStart, iniAt - iniStart);
        EXPECT(!iniBlock.empty(), "the ini comment of GiStrength is found\n");
        EXPECT(NumberAfter(iniBlock, "Default (auto) is ") == rulesDefault, "the ini's default line equals the rules' (%g)\n", NumberAfter(iniBlock, "Default (auto) is "));
        EXPECT(NumberAfter(iniBlock, "the default ") == rulesDefault, "the ini's description names the same default (%g)\n", NumberAfter(iniBlock, "the default "));
        EXPECT(NumberAfter(rawGiMenu, "the default ") == rulesDefault, "the menu's tooltip names the same default (%g)\n", NumberAfter(rawGiMenu, "the default "));
        EXPECT(NumberAfter(featuresPage, "`GiStrength` (") == rulesDefault, "the features page's config line names the same default (%g)\n", NumberAfter(featuresPage, "`GiStrength` ("));
        // The slider keeps its whole range, and a strength of 0 stays the identity.
        EXPECT(Has(giRules, "kMinStrength=0.0f;") && Has(giRules, "kMaxStrength=1.0f;"), "the slider's range is still 0 to 1\n");
    }

    // ---- the docs: the light bounce page says what the pass does now ---------------------------------------------------------------
    {
        const size_t pageAt = featuresPage.find("## Light bounce (FFXIV, experimental)");
        const size_t pageEnd = pageAt == std::string::npos ? pageAt : featuresPage.find("\n## ", pageAt + 10);
        const std::string page = pageEnd == std::string::npos ? std::string() : featuresPage.substr(pageAt, pageEnd - pageAt);
        EXPECT(!page.empty(), "the light bounce section of the features page is found\n");
        EXPECT(!Has(page, "radial box"), "the page no longer says the estimate is smoothed by a radial box\n");
        EXPECT(Has(page, "5x5"), "and names the dense 5x5 stencil\n");
        EXPECT(!Has(page, "divided by the albedo carrier") && !Has(page, "cancels the surface's own colour") && !Has(page, "without the surface's own colour"),
               "and no longer says the surface's own colour cancels out of a hit\n");
        EXPECT(Has(page, "its own linear scene value") || Has(page, "own linear scene value"), "it says a hit sends back its own linear scene value\n");
        EXPECT(Has(page, "traced") && Has(page, "valid zero"), "and that a traced texel with no hit is a valid zero in the average\n");
    }

    // ---- the smooth's stencil: the shader's tables against the rules header's, and what the tables must be --------------------------------
    {
        const std::string offsetsText = BraceBody(shaderGi, "kSmoothOffsets[kSmoothTaps]");
        const std::string weightsText = BraceBody(shaderGi, "kSmoothWeights[kSmoothTaps]");
        const std::vector<double> hlslOffsets = Numbers(offsetsText);
        const std::vector<double> hlslWeights = Numbers(weightsText);
        const std::vector<double> rulesOffsets = Numbers(BraceBody(giRules, "kSmoothOffsets[kSmoothTaps][2]"));
        const std::vector<double> rulesWeights = Numbers(BraceBody(giRules, "kSmoothWeights[kSmoothTaps]"));
        EXPECT(Has(shaderGi, "static const int kSmoothTaps = 25;") && Has(giRules, "kSmoothTaps=25;"), "both files name the same 25 taps\n");
        EXPECT(Has(shaderGi, "static const int2 kSmoothOffsets[kSmoothTaps]"), "the shader's tap offsets are typed int2: a tap position is an integer by type\n");
        EXPECT(!offsetsText.empty() && offsetsText.find('.') == std::string::npos, "and no tap position in the table carries a fractional part\n");
        EXPECT(Has(giRules, "int32_tkSmoothOffsets[kSmoothTaps][2]"), "the rules header's tap offsets are integers too\n");
        EXPECT(hlslOffsets.size() == 50 && hlslWeights.size() == 25, "the shader's tables hold 25 offset pairs and 25 weights (%zu numbers, %zu weights)\n", hlslOffsets.size(),
               hlslWeights.size());
        EXPECT(hlslOffsets == rulesOffsets, "the shader's tap offsets equal the rules header's, element by element\n");
        EXPECT(hlslWeights == rulesWeights, "the shader's tap weights equal the rules header's, element by element\n");
        // What the table must be, read from the shader's own numbers: every position of the 5x5 square once (so the centre and the four direct
        // neighbours are in), each weight the outer product of 1-4-6-4-1 at its position, the total 256, and a zero response at the
        // checkerboard frequency - as much weight on the odd x+y taps as on the even ones.
        if (hlslOffsets.size() == 50 && hlslWeights.size() == 25)
        {
            const double row[3] { 6.0, 4.0, 1.0 };
            bool seen[5][5] {};
            double total = 0.0, checker = 0.0;
            bool weightsFollowPositions = true;
            for (size_t tap = 0; tap < 25; ++tap)
            {
                const double dx = hlslOffsets[tap * 2], dy = hlslOffsets[tap * 2 + 1];
                if (dx != std::floor(dx) || dy != std::floor(dy) || std::fabs(dx) > 2.0 || std::fabs(dy) > 2.0)
                {
                    EXPECT(false, "tap %zu sits at (%g, %g), not on an integer inside the 5x5 reach\n", tap, dx, dy);
                    continue;
                }
                const int ix = int(dx) + 2, iy = int(dy) + 2;
                EXPECT(!seen[iy][ix], "tap position (%d, %d) appears once\n", int(dx), int(dy));
                seen[iy][ix] = true;
                weightsFollowPositions = weightsFollowPositions && hlslWeights[tap] == row[std::abs(int(dx))] * row[std::abs(int(dy))];
                total += hlslWeights[tap];
                checker += hlslWeights[tap] * (((int(dx) + int(dy)) & 1) == 0 ? 1.0 : -1.0);
            }
            bool full = true;
            for (int iy = 0; iy < 5; ++iy)
                for (int ix = 0; ix < 5; ++ix)
                    full = full && seen[iy][ix];
            EXPECT(full && seen[2][2] && seen[2][1] && seen[2][3] && seen[1][2] && seen[3][2], "the table has the centre and the four direct neighbours, and every other position of the square\n");
            EXPECT(weightsFollowPositions, "each tap's weight is row[|dx|] * row[|dy|] with the row 1-4-6-4-1\n");
            EXPECT(total == 256.0 && checker == 0.0, "the weights sum to 256 and the response at the checkerboard frequency is zero (%g, %g)\n", total, checker);
        }
        // The stencil is read as the table says: the position added to the pixel's own, no cast, no clamp (an outside tap is skipped), the weight
        // from the table and the sum divided by the weights that were present.
        const size_t smoothAt = shaderGi.find("float4 GiAtrous(");
        const size_t applyAt = shaderGi.find("float4 PS_Atrous0(");
        const std::string smooth = smoothAt != std::string::npos && applyAt != std::string::npos && smoothAt < applyAt ? shaderGi.substr(smoothAt, applyAt - smoothAt) : std::string();
        EXPECT(!smooth.empty() && Has(smooth, "const int2 offset = kSmoothOffsets[tap] * stepSize;") && Has(smooth, "const int2 tapAt = at + offset;") && Has(smooth, "kSmoothWeights[tap]"), "the iteration reads the table's position (times the step) and weight for every tap\n");
        EXPECT(!Has(smooth, "clamp(") && !Has(smooth, "length("), "the iteration clamps nothing and takes no distance of its own\n");
        const std::string outside = "tapAt.x < 0 || tapAt.y < 0 || tapAt.x > last.x || tapAt.y > last.y";
        const size_t outsideAt = smooth.find(outside);
        EXPECT(outsideAt != std::string::npos && smooth.find("continue;", outsideAt) < outsideAt + outside.size() + 80,
               "a tap outside the extent is skipped (the test is followed by a continue)\n");
        EXPECT(Has(smooth, "sum / weight"), "and the sum is divided by the weights that were present\n");
    }

    // ---- METER sweep S1: the refused camera consults the hold before it declines -------------------------------------------------------------------------------
    {
        const std::string readback = Body(giDraw, "boolReadback(");
        EXPECT(!readback.empty(), "the readback is found\n");
        const size_t refusal = readback.find("if(read.landed&&!cameraUsable){");
        EXPECT(refusal != std::string::npos, "the refused read has its own branch\n");
        const std::string branch = refusal == std::string::npos ? std::string() : readback.substr(refusal, 700);
        EXPECT(branch.find("constboolholdStands=RefusedCameraUsesHold(state.hold.have,state.hold.Age(state.frames),state.hold.Weight(state.frames));") != std::string::npos,
               "the branch asks the rule with the hold's own age and weight\n");
        const size_t ask = branch.find("RefusedCameraUsesHold(");
        const size_t decline = branch.find("cameraRefused=true;");
        EXPECT(ask != std::string::npos && decline != std::string::npos && ask < decline && branch.find("if(!holdStands){cameraRefused=true;returnfalse;}") != std::string::npos,
               "the rule is asked before the refusal is counted, and only a frame with nothing to stand on is declined\n");
        EXPECT(branch.find("fade=state.hold.Weight(state.frames);returntrue;") != std::string::npos, "a frame that stands on the hold draws with the hold's own weight\n");
        EXPECT(branch.find("LOG_INFO(\"{}\",holdStands?kCameraRefusedHeld:kCameraRefused);") != std::string::npos, "the one line says which of the two it was\n");
        const size_t accept = readback.find("state.hold.Accept(read,state.frames)");
        EXPECT(accept != std::string::npos && refusal < accept, "a refused read is never accepted into the hold\n");
    }

    if (g_failures != 0)
    {
        std::printf("gi_pins: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("gi_pins: ok\n");
    return 0;
}
