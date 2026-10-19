// The "Character preview" card (ext/ffxiv/chara/CharaMenu.cpp) is ImGui glue and cannot run offline: this pins its layout by
// reading the source (comments stripped, string literals kept, brace-matched function bodies):
//   - title == picker name == "Character preview", registered in the FFXIV tab with a predicate that tests the game build,
//     and "(experimental)" stays in the card text;
//   - the user switches (Preview DLSS, Preview spatial AA with 2x upscaling, Preview NR and "Run NR before DLSS") and the
//     one-line states are drawn BEFORE the fold, the reason why the feature is off stays in the main view;
//   - every counter and debug aid (tint, flood, probe, "Save preview images", NR numbers) lives in ONE fold "Advanced
//     (debug)" that is closed by default: TreeNode/TreePop in DrawCharaCard only, no return, no DefaultOpen;
//   - each config key is written from exactly one checkbox.
#include "menu_scan.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
int failures = 0;
void Check(bool ok, const char* what, int line)
{
    if (!ok)
    {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

using menuscan::Count;
using menuscan::Squash;

// Body (from '{' to the matching '}') of the function whose signature starts with `signature`; "" if absent
std::string BodyOf(const std::string& code, const std::string& signature)
{
    const size_t at = code.find(signature);
    if (at == std::string::npos)
        return {};
    const size_t open = code.find('{', at);
    if (open == std::string::npos)
        return {};
    int depth = 0;
    for (size_t i = open; i < code.size();)
    {
        if (code[i] == '"')
        {
            i = menuscan::SkipString(code, i);
            continue;
        }
        if (code[i] == '{')
            ++depth;
        else if (code[i] == '}' && --depth == 0)
            return code.substr(open, i - open + 1);
        ++i;
    }
    return {};
}

std::vector<std::string> Problems(const std::string& text)
{
    std::vector<std::string> p;
    auto bad = [&](const std::string& s) { p.push_back(s); };

    const std::string stripped = menuscan::StripComments(text);
    const std::string all = Squash(stripped);
    const std::string card = Squash(BodyOf(stripped, "void DrawCharaCard(ext::menu::Ctx&)"));
    const std::string dbg = Squash(BodyOf(stripped, "void DrawDebugSection(CharaConfig& cfg, bool inert)"));
    const std::string toggles = Squash(BodyOf(stripped, "void DrawNrToggles(CharaConfig& cfg)"));
    const std::string counters = Squash(BodyOf(stripped, "void DrawNrCounters(const CharaConfig& cfg)"));
    if (card.empty() || dbg.empty() || toggles.empty() || counters.empty())
    {
        bad("a function of the card is missing");
        return p;
    }

    // registration: id, tab, picker name, predicate that tests the game build
    size_t tokens = 0;
    std::vector<menuscan::Registration> regs;
    menuscan::ScanRegistrationText(stripped, "ffxiv/chara/CharaMenu.cpp", tokens, regs);
    if (tokens != 1 || regs.size() != 1)
        bad("expected exactly one registration");
    else if (regs[0].id != "ffxiv_character_preview" || regs[0].tab != "FFXIV" || regs[0].visible != "Visible" ||
             !menuscan::TestsGameBuild(regs[0]))
        bad("registration changed (id, tab, visible predicate testing the game build)");
    if (Count(all, "\"ffxiv_character_preview\",\"FFXIV\",\"Characterpreview\",DrawCharaCard,") != 1)
        bad("the picker name is not \"Character preview\"");

    // title == picker name, first statement; "(experimental)" stays in the text
    if (card.find("{auto&cfg=ext::Cfg().ffxiv.chara;ext::menu::SectionTitle(\"Characterpreview\");") != 0)
        bad("the card does not start with the config reference and SectionTitle(\"Character preview\")");
    if (Count(card, "ImGui::TextDisabled(\"(experimental)") != 1)
        bad("the card text lost \"(experimental)\"");

    // one fold, last statement of the card, closed by default, balanced, no return anywhere in the card
    const std::string fold = "if(ImGui::TreeNode(\"Advanced(debug)##chara_preview\")){DrawDebugSection(cfg,inert);ImGui::TreePop();}}";
    if (card.size() < fold.size() || card.rfind(fold) != card.size() - fold.size())
        bad("the card does not end with the closed fold { DrawDebugSection; TreePop }");
    if (Count(all, "ImGui::TreeNode(") != 1 || Count(all, "ImGui::TreePop();") != 1)
        bad("TreeNode/TreePop are not exactly one pair");
    if (Count(card, "return") != 0)
        bad("DrawCharaCard has a return");
    for (const char* banned : { "DefaultOpen", "TreeNodeEx", "SetNextItemOpen", "CollapsingHeader" })
        if (all.find(banned) != std::string::npos)
            bad(std::string("the fold must stay a plain closed TreeNode: found ") + banned);

    // the reason the feature is off is shown exactly when the table says the feature is inert
    if (Count(card, "constboolinert=reason!=nullptr&&reason[0]!='\\0';") != 1 ||
        Count(card, "if(inert)ImGui::TextWrapped(\"Characterpreviewisoffforthissession:%s.\",reason);") != 1 ||
        Count(card, "constchar*reason=table::InertReason();") != 1)
        bad("the off-reason is not shown exactly when the feature is inert");

    // the user switches and states come before the fold, the reason stays in the main view
    const size_t foldAt = card.find("if(ImGui::TreeNode(");
    for (const std::string& needle : { Squash("ImGui::Checkbox(\"Preview DLSS (2x output, experimental)\""),
                                       Squash("ImGui::Checkbox(\"Preview spatial AA\""),
                                       Squash("ImGui::Checkbox(\"2x preview upscaling\""), std::string("DrawNrToggles(cfg);"),
                                       std::string("DrawNrStatusLine(cfg);"), Squash("table::InertReason()"),
                                       Squash("\"Character preview is off for this session: %s.\"") })
    {
        const size_t at = card.find(needle);
        if (at == std::string::npos || foldAt == std::string::npos || at > foldAt)
            bad("not drawn before the fold: " + needle);
    }
    if (Count(toggles, Squash("ImGui::Checkbox(\"Preview Neural Rendering (NR, experimental)\"")) != 1 ||
        Count(toggles, Squash("ImGui::Checkbox(\"Run NR before DLSS (experimental)\"")) != 1)
        bad("the NR switches are not in DrawNrToggles");

    // every debug aid and counter is in the fold section, nowhere else
    for (const std::string& label : { Squash("\"Show preview processing (debug)\""), Squash("\"Flood the preview with solid magenta (debug)\""),
                                      Squash("\"Preview probe (debug)\""), Squash("\"Save preview images\""),
                                      Squash("\"DLSS previews: %u active"), Squash("\"Processed preview draws: %llu\""),
                                      Squash("\"Preview diagnostics: %u files") })
    {
        if (Count(all, label) != 1 || Count(dbg, label) != 1)
            bad("not (only) in DrawDebugSection: " + label);
    }
    if (Count(counters, Squash("\"NR windows: %u active")) != 1 || Count(all, Squash("\"NR windows: %u active")) != 1)
        bad("the NR counters are not only in DrawNrCounters");
    if (Count(all, "DrawNrCounters(cfg);") != 1 || Count(dbg, "DrawNrCounters(cfg);") != 1)
        bad("DrawNrCounters is not called exactly once, from the fold section");
    if (Count(dbg, "if(!inert&&aa&&") != 1)
        bad("the AA lines no longer respect the 'feature is off' state");

    // each key is written from exactly one checkbox, in the right place
    struct Key
    {
        const char* prefix; // every write of the key in the file
        const char* full;   // the write, from the value of its own checkbox
        const std::string* where;
    };
    const Key keys[] = {
        { "cfg.previewDlss=", "cfg.previewDlss=dlss;", &card },
        { "cfg.previewAA=", "cfg.previewAA=aa;", &card },
        { "cfg.previewUpscale=", "cfg.previewUpscale=upscale;", &card },
        { "cfg.previewNr=", "cfg.previewNr=nrOn;", &toggles },
        { "cfg.previewNrBeforeSr=", "cfg.previewNrBeforeSr=before;", &toggles },
        { "cfg.previewDebugTint=", "cfg.previewDebugTint=tint;", &dbg },
        { "cfg.previewDebugFlood=", "cfg.previewDebugFlood=flood;", &dbg },
        { "cfg.previewProbe=", "cfg.previewProbe=probeOn;", &dbg },
    };
    for (const Key& k : keys)
        if (Count(all, k.prefix) != 1 || Count(*k.where, k.full) != 1)
            bad(std::string("key not written exactly once, from its own checkbox, in its place: ") + k.prefix);
    return p;
}

std::string Replace(std::string s, const std::string& from, const std::string& to)
{
    const size_t at = s.find(from);
    if (at != std::string::npos)
        s.replace(at, from.size(), to);
    return s;
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    std::string text = menuscan::ReadFile(repo + "/OptiScaler/ext/ffxiv/chara/CharaMenu.cpp");
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end()); // the working copy may have CRLF line ends
    CHECK(!text.empty());

    const auto problems = Problems(text);
    for (const auto& s : problems)
        std::printf("  card: %s\n", s.c_str());
    CHECK(problems.empty());

    // negative controls: each pin complains about its own breakage
    const std::string foldHead = "if (ImGui::TreeNode(\"Advanced (debug)##chara_preview\"))";
    const std::string foldBody = "    {\n        DrawDebugSection(cfg, inert);\n        ImGui::TreePop();\n    }";
    CHECK(text.find(foldHead + "\n" + foldBody) != std::string::npos); // the controls below start from this text
    CHECK(!Problems(Replace(text, foldHead, "if (ImGui::TreeNodeEx(\"Advanced (debug)##chara_preview\", ImGuiTreeNodeFlags_DefaultOpen))")).empty());
    CHECK(!Problems(Replace(text, foldHead, "if (true)")).empty());
    CHECK(!Problems(Replace(text, "        ImGui::TreePop();\n", "")).empty());
    CHECK(!Problems(Replace(text, foldBody, "    {\n        ImGui::TreePop();\n    }\n    DrawDebugSection(cfg, inert);")).empty());
    CHECK(!Problems(Replace(text, "        DrawDebugSection(cfg, inert);\n        ImGui::TreePop();",
                            "        if (inert)\n            return;\n        DrawDebugSection(cfg, inert);\n        ImGui::TreePop();")).empty());
    CHECK(!Problems(Replace(text, "    DrawNrToggles(cfg);\n    DrawNrStatusLine(cfg);\n", "")).empty());
    CHECK(!Problems(Replace(text, "    DrawNrToggles(cfg);\n    DrawNrStatusLine(cfg);\n", "")).empty());
    CHECK(!Problems(Replace(text, "ext::menu::SectionTitle(\"Character preview\");", "ext::menu::SectionTitle(\"Character preview (experimental)\");")).empty());
    CHECK(!Problems(Replace(text, "TextDisabled(\"(experimental) Changes", "TextDisabled(\"Changes")).empty());
    CHECK(!Problems(Replace(text, "\"Character preview\", DrawCharaCard", "\"Character preview (experimental)\", DrawCharaCard")).empty());
    CHECK(!Problems(Replace(text, "return ext::game::Current().lighting != nullptr;", "return true;")).empty());
    CHECK(!Problems(Replace(text, "    DrawNrCounters(cfg);\n\n    bool tint", "    bool tint")).empty());
    CHECK(!Problems(Replace(text, "if (!inert && aa &&", "if (aa &&")).empty());
    CHECK(!Problems(Replace(text, "    if (inert)\n        ImGui::TextWrapped(\"Character preview is off",
                            "    if (false)\n        ImGui::TextWrapped(\"Character preview is off")).empty());
    CHECK(!Problems(Replace(text, "cfg.previewDebugTint = tint;", "cfg.previewDebugTint = true;")).empty());
    CHECK(!Problems(Replace(text, "cfg.previewAA = aa;", "cfg.previewAA = true;")).empty());
    CHECK(!Problems("").empty());

    if (failures)
    {
        std::printf("chara_menu_text: %d failure(s)\n", failures);
        return 2;
    }
    std::printf("chara_menu_text: all passed\n");
    return 0;
}
