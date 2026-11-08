// INT1: the tone-map chain's order AS BUILT, read as text from the sources (no device). The order, agreed in analysis\rc6-session\PLAN-LOOK-2026-11-07.md
// ("Advisor edits", darkening before in-scatter):
//     god-rays march -> light bounce (GI) -> night grade (L1) -> atmosphere (L2) -> depth of field -> t0
// The runtime half is look_chain_order_warp (the real dispatchers, the real night grade, the real light bounce's off exits, stand-ins for what cannot answer on a
// software adapter); this half pins what a text can: every place the order is written, and that no other place writes it.
//
// What a wrong wiring would break, and what is pinned here:
//   the order is written ONLY in ChainHooks.h - four hook pairs declared in chain order, and four dispatchers (after the light bounce: the night grade first, then
//   what it hands on; after the night grade: the atmosphere, then depth of field) whose bodies are pinned exactly; a stage names the dispatcher behind it and never
//   the stage two links down (the light bounce names none of night / atmosphere / depth of field; the night grade names neither the atmosphere nor depth of field; the
//   atmosphere names depth of field only; depth of field names none; the earlier stages name none of the three);
//   the seam's fallback-owner order in EnginePass.cpp is the same order, each stage's seam entry is called from exactly one place, and the five arming lists, the
//   pixel-shader seam's feature test and both light-capture feature tests carry both bits (the owner role of a stage that is not asked cannot draw the game's call);
//   the runtime harness's own seam model (look_chain_order_warp.cpp) calls its stages in the same order as EnginePass.cpp, so the model cannot drift from the product;
//   the stage-dump ids (atmosphere 12, night grade 13: appended, so the numbers the lab's bridge parses never move), the feature bits (distinct), the menu order.
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
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
std::string TestCode(const std::string& repo, const char* relative) { return Squeeze(Comments(ReadFile(repo + "/tests/ext/ffxiv/engine/" + relative))); }

bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
size_t At(const std::string& text, const std::string& needle) { return text.find(needle); }
size_t Count(const std::string& text, const std::string& needle)
{
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
        ++count;
    return count;
}
// `first` appears, and `second` appears after it.
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
        std::printf("usage: look_chain_order_pins <repo>\n");
        return 2;
    }
    const std::string repo = argv[1];
    const std::string hooks = Code(repo, "ffxiv/engine/ChainHooks.h");
    const std::string giDraw = Code(repo, "ffxiv/engine/GiDraw.cpp");
    const std::string nightDraw = Code(repo, "ffxiv/enginelight/EngineLightDraw.cpp");
    const std::string atmoDraw = Code(repo, "ffxiv/engine/AtmoDraw.cpp");
    const std::string dofDraw = Code(repo, "ffxiv/engine/DofDraw.cpp");
    const std::string marchDraw = Code(repo, "ffxiv/engine/GodraysDraw.cpp");
    const std::string puddleDraw = Code(repo, "ffxiv/engine/PuddleToneDraw.cpp");
    const std::string shaftsDraw = Code(repo, "ffxiv/engine/ShaftsDraw.cpp");
    const std::string enginePass = Code(repo, "ffxiv/engine/EnginePass.cpp");
    const std::string capture = Code(repo, "ffxiv/lighting/FfxivLightingCapture.h");
    const std::string features = Code(repo, "ExtFeatures.h");
    const std::string layout = Code(repo, "core/MenuLayout.h");
    const std::string stageTable = Code(repo, "ffxiv/diag/StageDumpFormat.h");
    const std::string warpTest = TestCode(repo, "look_chain_order_warp.cpp");

    for (const std::string* text : { &hooks, &giDraw, &nightDraw, &atmoDraw, &dofDraw, &marchDraw, &puddleDraw, &shaftsDraw, &enginePass, &capture, &features, &layout, &stageTable, &warpTest })
        EXPECT(!text->empty(), "every file the pins read is found\n");

    std::printf("-- pins: the order is written in ChainHooks.h only --\n");
    {
        // four pairs, declared in chain order
        const char* chain[] = { "chainGiScene=nullptr;", "chainNightScene=nullptr;", "chainAtmoScene=nullptr;", "chainDofScene=nullptr;" };
        const char* owner[] = { "toneMapGiOwner=nullptr;", "toneMapNightOwner=nullptr;", "toneMapAtmoOwner=nullptr;", "toneMapDofOwner=nullptr;" };
        for (int i = 0; i < 4; ++i)
        {
            EXPECT(Count(hooks, chain[i]) == 1 && Count(hooks, owner[i]) == 1, "each hook is declared once\n");
            if (i > 0)
                EXPECT(At(hooks, chain[i - 1]) < At(hooks, chain[i]) && At(hooks, owner[i - 1]) < At(hooks, owner[i]), "the hooks are declared in chain order\n");
        }
        // the dispatchers: the only code that names the next stage by hook
        const std::string afterGi = Body(hooks, "inlineID3D11ShaderResourceView*chainAfterGiScene(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene)");
        EXPECT(afterGi == "{if(chainNightScene!=nullptr)returnchainNightScene(context,facts,scene);returnchainAfterNightScene(context,facts,scene);}",
               "after the light bounce: the night grade when linked, else whatever the night grade would hand on (so the atmosphere is reached without it)\n");
        const std::string afterGiOwner = Body(hooks, "inlinebooltoneMapAfterGiOwner(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene,ChainDrawFndraw,void*user)");
        EXPECT(afterGiOwner == "{if(toneMapNightOwner!=nullptr)returntoneMapNightOwner(context,facts,scene,draw,user);returntoneMapAfterNightOwner(context,facts,scene,draw,user);}",
               "the owner turn after the light bounce: the same order\n");
        const std::string afterNight = Body(hooks, "inlineID3D11ShaderResourceView*chainAfterNightScene(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene)");
        EXPECT(afterNight == "{if(chainAtmoScene!=nullptr)returnchainAtmoScene(context,facts,scene);if(chainDofScene!=nullptr)returnchainDofScene(context,facts,scene);returnnullptr;}",
               "after the night grade: the atmosphere, then depth of field, then nothing (the hold stands)\n");
        const std::string afterNightOwner = Body(hooks, "inlinebooltoneMapAfterNightOwner(ID3D11DeviceContext*context,constshafts::ToneMapFacts&facts,ID3D11ShaderResourceView*scene,ChainDrawFndraw,void*user)");
        EXPECT(afterNightOwner == "{if(toneMapAtmoOwner!=nullptr)returntoneMapAtmoOwner(context,facts,scene,draw,user);if(toneMapDofOwner!=nullptr)returntoneMapDofOwner(context,facts,scene,draw,user);returnfalse;}",
               "the owner turn after the night grade: the atmosphere's, then depth of field's, then a plain false (the caller draws the game's call once)\n");
        // who names what
        const char* hookNames[] = { "chainNightScene", "toneMapNightOwner", "chainAtmoScene", "toneMapAtmoOwner", "chainDofScene", "toneMapDofOwner" };
        for (const char* name : hookNames)
            EXPECT(Count(giDraw, name) == 0, "the light bounce's file names no stage behind it (it asks the dispatchers)\n");
        EXPECT(Count(giDraw, "chainAfterGiScene(") == 2 && Count(giDraw, "toneMapAfterGiOwner(") == 3, "the light bounce asks its dispatchers in exactly five places\n");
        EXPECT(Count(giDraw, "chainAfterNight") == 0 && Count(giDraw, "toneMapAfterNight") == 0, "and never the night grade's own pair\n");
        for (const char* name : { "chainAtmoScene", "toneMapAtmoOwner", "chainDofScene", "toneMapDofOwner" })
            EXPECT(Count(nightDraw, name) == 0, "the night grade names neither the atmosphere's hooks nor depth of field's: it asks its own dispatcher\n");
        EXPECT(Count(nightDraw, "chainAfterNightScene(") == 2 && Count(nightDraw, "toneMapAfterNightOwner(") == 3, "the night grade asks its dispatchers in exactly five places\n");
        EXPECT(Count(nightDraw, "chainAfterGi") == 0 && Count(nightDraw, "toneMapAfterGi") == 0 && Count(nightDraw, "chainGiScene") == 0 && Count(nightDraw, "toneMapGiOwner") == 0,
               "and never an earlier stage's\n");
        for (const char* name : { "chainNightScene", "toneMapNightOwner", "chainAfterNight", "toneMapAfterNight", "chainAfterGi", "toneMapAfterGi", "chainGiScene", "toneMapGiOwner" })
            EXPECT(Count(atmoDraw, name) == 0, "the atmosphere asks depth of field and nothing else (it never reaches back)\n");
        EXPECT(Count(atmoDraw, "chainDofScene(") == 1 && Count(atmoDraw, "toneMapDofOwner(") == 1,
               "the atmosphere's only two calls into depth of field are its two helpers' (AskDof, AskDofOwner)\n");
        for (const char* name : { "chainNightScene", "toneMapNightOwner", "chainAtmoScene", "toneMapAtmoOwner", "chainAfterNight", "toneMapAfterNight", "chainAfterGi", "toneMapAfterGi", "chainGiScene" })
            EXPECT(Count(dofDraw, name) == 0, "depth of field never reaches back to any stage in front of it\n");
        for (const std::string* earlier : { &marchDraw, &puddleDraw, &shaftsDraw })
            for (const char* name : { "chainNightScene", "toneMapNightOwner", "chainAtmoScene", "toneMapAtmoOwner", "chainDofScene", "toneMapDofOwner", "chainAfterNight", "toneMapAfterNight" })
                EXPECT(Count(*earlier, name) == 0, "the stages before the light bounce name none of the stages behind it\n");
        // the registrations: each stage registers its own pair, nobody registers another's
        EXPECT(Has(nightDraw, "chainNightScene=&ext::ffxiv::enginelight::ChainScene;") && Has(nightDraw, "toneMapNightOwner=&ext::ffxiv::enginelight::ToneMapOwner;"), "the night grade registers its pair\n");
        EXPECT(Has(atmoDraw, "chainAtmoScene=&ext::ffxiv::engine::atmo::ChainScene;") && Has(atmoDraw, "toneMapAtmoOwner=&ext::ffxiv::engine::atmo::ToneMapOwner;"), "the atmosphere registers its pair\n");
        EXPECT(Has(dofDraw, "chainDofScene=&ext::ffxiv::dof::ChainScene;") && Has(dofDraw, "toneMapDofOwner=&ext::ffxiv::dof::ToneMapOwner;"), "depth of field registers its pair\n");
        EXPECT(Has(giDraw, "chainGiScene=&ext::ffxiv::engine::gi::ChainScene;") && Has(giDraw, "toneMapGiOwner=&ext::ffxiv::engine::gi::ToneMapOwner;"), "the light bounce registers its pair\n");
    }

    std::printf("-- pins: the seam's fallback-owner order --\n");
    {
        const std::string order = "if(ext::ffxiv::engine::gi::OnSeamDraw(context,anchors,draw,user))return;"
                                  "if(ext::ffxiv::enginelight::OnSeamDraw(context,anchors,draw,user))return;"
                                  "if(ext::ffxiv::engine::atmo::OnSeamDraw(context,anchors,draw,user))return;"
                                  "if(ext::ffxiv::dof::OnSeamDraw(context,anchors,draw,user))return;";
        EXPECT(Has(enginePass, order), "the seam dispatch calls the light bounce, the night grade, the atmosphere, depth of field - in that order, back to back\n");
        EXPECT(Ordered(enginePass, "ext::ffxiv::godrays::OnSeamDraw(context,anchors,draw,user)", "ext::ffxiv::engine::gi::OnSeamDraw(context,anchors,draw,user)"), "behind the march\n");
        EXPECT(Ordered(enginePass, "ext::ffxiv::dof::OnSeamDraw(context,anchors,draw,user)", "flare::OnSeamDraw(context,draw,user)"), "and ahead of the lens flare's card\n");
        for (const char* seamEntry : { "ext::ffxiv::engine::gi::OnSeamDraw(", "ext::ffxiv::enginelight::OnSeamDraw(", "ext::ffxiv::engine::atmo::OnSeamDraw(", "ext::ffxiv::dof::OnSeamDraw(" })
            EXPECT(Count(enginePass, seamEntry) == 1, "each stage's seam entry is called from exactly one place\n");
        // five arming lists: the same order in each, both bits in each
        const std::string arming = "ext::ffxiv::engine::gi::Armed(),ext::ffxiv::enginelight::Armed(),ext::ffxiv::engine::atmo::Armed(),ext::ffxiv::dof::Armed(),flare::Armed()";
        EXPECT(Count(enginePass, arming) == 5, "the draw path's five arming lists carry the four stages in chain order, then the flare\n");
        EXPECT(Count(enginePass, "enginelight::Armed()") == 5 && Count(enginePass, "atmo::Armed()") == 5, "and every list has both stages' arming, and only those five\n");
        EXPECT(Has(enginePass, "ext::F::LensFlare,ext::F::Atmosphere,ext::F::NightGrade))"), "the pixel-shader seam's feature test opens for both bits\n");
        EXPECT(Count(capture, "ext::F::Atmosphere,ext::F::NightGrade") == 2 || (Count(capture, "ext::F::Atmosphere") == 2 && Count(capture, "ext::F::NightGrade") == 2),
               "both light-capture feature tests carry both bits\n");
        EXPECT(Has(enginePass, "ext::ffxiv::engine::atmo::Release();") && Has(enginePass, "ext::ffxiv::enginelight::Release();"), "the teardown releases both\n");
        EXPECT(Has(enginePass, "if(ext::Active(ext::F::Atmosphere))ext::ffxiv::engine::atmo::SampleSceneHdr();") &&
                   Has(enginePass, "if(ext::Active(ext::F::NightGrade))ext::ffxiv::enginelight::SampleSceneHdr();"),
               "the bind seam samples the scene HDR state for both\n");
    }

    std::printf("-- pins: the runtime harness's seam model is the product's order --\n");
    {
        // The WARP harness calls its seam stages through these four names; the order of the calls is pinned here against the product's.
        const std::string model = "if(Seam_Gi(context,anchors,draw,user))returntrue;"
                                  "if(Seam_Night(context,anchors,draw,user))returntrue;"
                                  "if(Seam_Atmo(context,anchors,draw,user))returntrue;"
                                  "if(Seam_Dof(context,anchors,draw,user))returntrue;";
        EXPECT(Has(warpTest, model), "the harness's SeamDispatch calls light bounce, night grade, atmosphere, depth of field in the product's order\n");
        EXPECT(Count(warpTest, "boolSeam_Gi(") == 1 && Count(warpTest, "boolSeam_Night(") == 1 && Count(warpTest, "boolSeam_Atmo(") == 1 && Count(warpTest, "boolSeam_Dof(") == 1,
               "and each of its four seam stages is defined once\n");
        EXPECT(Has(warpTest, "gi::OnSeamDraw(") && Has(warpTest, "ng::OnSeamDraw("), "the model's light bounce and night grade are the REAL stages' seam entries\n");
    }

    std::printf("-- pins: feature bits, stage-dump ids, menu order --\n");
    {
        EXPECT(Has(features, "Atmosphere=1ull<<23,") && Has(features, "NightGrade=1ull<<24,"), "the atmosphere is feature bit 23 and the night grade 24\n");
        std::map<std::string, int> seen;
        int dupes = 0;
        for (size_t at = features.find("=1ull<<"); at != std::string::npos; at = features.find("=1ull<<", at + 7))
        {
            size_t end = at + 7;
            while (end < features.size() && std::isdigit(static_cast<unsigned char>(features[end])))
                ++end;
            if (++seen[features.substr(at + 7, end - at - 7)] > 1)
                ++dupes;
        }
        EXPECT(seen.size() >= 20 && dupes == 0, "no two feature bits are the same (the merge of two stages once gave both bit 23)\n");
        EXPECT(Ordered(stageTable, "Flare,", "Atmosphere,") && Ordered(stageTable, "Atmosphere,", "NightGrade,") && Ordered(stageTable, "NightGrade,", "Count"),
               "the stage ids are appended: the atmosphere is 12, the night grade 13 (so 10 dof and 11 flare keep their numbers)\n");
        EXPECT(Has(stageTable, "{\"flare\",StageGroup::Chain,false},{\"atmosphere\",StageGroup::Chain,true},{\"night-grade\",StageGroup::Chain,true},"),
               "and the table lists them in the same order\n");
        const char* cards[] = { "{\"ffxiv_gi\"", "{\"ffxiv_engine_light\"", "{\"ffxiv_atmosphere\"", "{\"ffxiv_dof\"", "{\"ffxiv_lens_flare\"" };
        for (int i = 1; i < 5; ++i)
            EXPECT(At(layout, cards[i - 1]) < At(layout, cards[i]) && At(layout, cards[i - 1]) != std::string::npos, "the menu cards are in chain order\n");
        EXPECT(Has(layout, "{\"ffxiv_engine_light\",\"FFXIV\",\"ffxiv_gi\"},") && Has(layout, "{\"ffxiv_atmosphere\",\"FFXIV\",\"ffxiv_engine_light\"},") &&
                   Has(layout, "{\"ffxiv_dof\",\"FFXIV\",\"ffxiv_atmosphere\"},"),
               "each card follows the one before it in the chain\n");
    }

    std::printf(g_failures == 0 ? "look_chain_order_pins: all pins hold\n" : "look_chain_order_pins: %d pin(s) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
