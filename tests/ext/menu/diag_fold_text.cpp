// The diagnostics cards of the FFXIV tab and the ReShade add-on probe are ImGui glue: this pins by reading the source that
// their details sit in a fold that is CLOSED by default (plain TreeNode, no DefaultOpen), that the TreeNode/TreePop pair is
// drawn by the card function itself (the body may return early, the card may not), and that the title of the card equals
// its picker name:
//   hudless_diag         ext/ffxiv/ui/UiMenu.cpp          "HUD-less diagnostics"
//   reshade_addon_probe  ext/reshade/ReShadeMenu.cpp      "ReShade built-in add-on (diagnostics)"
//   ffxiv_companion      ext/ffxiv/companion/Companion.cpp "FFXIV Companion (nameplates)" (its diagnostics TreeNode stays closed)
//   fg_hudless           ext/hudless/HudlessMenu.cpp      "HUD-less (UI-free frame generation)" (Advanced fold stays closed)
//   ffxiv_hdr            ext/hdr/menu/HdrMenu.cpp         "FFXIV HDR10 output"
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

struct Case
{
    const char* file;
    const char* id;
    const char* tab;
    const char* name;      // title == picker name
    const char* drawFn;    // signature of the card function
    const char* bodyFn;    // function the fold calls ("" = the card keeps its own fold, see foldText)
    const char* foldLabel; // the closed TreeNode of the card ("" = none to check)
};

const Case kCases[] = {
    { "ffxiv/ui/UiMenu.cpp", "hudless_diag", "FFXIV", "HUD-less diagnostics", "void DrawDiagCard(ext::menu::Ctx&)", "DrawDiagBody",
      "Show diagnostics##hudless_diag" },
    { "reshade/ReShadeMenu.cpp", "reshade_addon_probe", "Compatibility", "ReShade built-in add-on (diagnostics)",
      "void DrawReShadeCard(ext::menu::Ctx&)", "DrawReShadeBody", "Show probe details##reshade_probe" },
    { "ffxiv/companion/Companion.cpp", "ffxiv_companion", "FFXIV", "FFXIV Companion (nameplates)",
      "void DrawCompanionCard(ext::menu::Ctx&)", "", "" },
    { "hudless/HudlessMenu.cpp", "fg_hudless", "FFXIV", "HUD-less (UI-free frame generation)", "void DrawHudless(ext::menu::Ctx&)", "",
      "" },
    { "hdr/menu/HdrMenu.cpp", "ffxiv_hdr", "FFXIV", "FFXIV HDR10 output", "void DrawHdrCard(ext::menu::Ctx& ctx)", "", "" },
};

std::vector<std::string> Problems(const Case& c, const std::string& text)
{
    std::vector<std::string> p;
    auto bad = [&](const std::string& s) { p.push_back(std::string(c.id) + ": " + s); };

    const std::string stripped = menuscan::StripComments(text);
    const std::string all = Squash(stripped);
    const std::string card = Squash(BodyOf(stripped, c.drawFn));
    if (card.empty())
    {
        bad("the card function was not found");
        return p;
    }

    // the title equals the picker name (and the card is registered for its tab with this name)
    size_t tokens = 0;
    std::vector<menuscan::Registration> regs;
    menuscan::ScanRegistrationText(stripped, c.file, tokens, regs);
    if (regs.size() != 1 || regs[0].id != c.id || regs[0].tab != c.tab || regs[0].name != c.name)
        bad("registration is not (id, tab, picker name) as expected");
    const std::string title = "ext::menu::SectionTitle(\"" + Squash(c.name) + "\");";
    if (Count(all, title) != 1)
        bad("the SectionTitle is not exactly the picker name");
    // the first statement of the card; the HDR and Hudless cards start with their config references
    if (card.find(title) == std::string::npos)
        bad("the SectionTitle is not in the card function");

    // no fold of these files is open by default
    for (const char* banned : { "DefaultOpen", "SetNextItemOpen" })
        if (all.find(banned) != std::string::npos)
            bad(std::string("a fold is opened by default: ") + banned);

    if (std::string(c.bodyFn).empty())
        return p;

    // the fold: the card function's last statement is `if (TreeNode(label)) { Body(); TreePop(); }`, no return in the card
    const std::string fold = "if(ImGui::TreeNode(\"" + Squash(c.foldLabel) + "\")){" + c.bodyFn + "();ImGui::TreePop();}}";
    if (card.size() < fold.size() || card.rfind(fold) != card.size() - fold.size())
        bad("the card does not end with the closed fold { Body; TreePop }");
    if (Count(card, "return") != 0)
        bad("the card function has a return");
    if (Count(all, "ImGui::TreeNode(") != 1 || Count(all, "ImGui::TreePop();") != 1)
        bad("TreeNode/TreePop are not exactly one pair");
    if (all.find("TreeNodeEx") != std::string::npos)
        bad("the fold must be a plain TreeNode");
    // the details are only reachable through the fold: the body function is called once, from the card
    if (Count(all, std::string(c.bodyFn) + "();") != 1)
        bad("the body function is called other than once, from the fold");
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

    for (const Case& c : kCases)
    {
        std::string text = menuscan::ReadFile(repo + "/OptiScaler/ext/" + c.file);
        text.erase(std::remove(text.begin(), text.end(), '\r'), text.end()); // the working copy may have CRLF line ends
        CHECK(!text.empty());
        const auto problems = Problems(c, text);
        for (const auto& s : problems)
            std::printf("  %s\n", s.c_str());
        CHECK(problems.empty());

        // negative controls
        CHECK(!Problems(c, "").empty());
        CHECK(!Problems(c, Replace(text, "SectionTitle(\"" + std::string(c.name) + "\")", "SectionTitle(\"" + std::string(c.name) + " (old)\")")).empty());
        CHECK(!Problems(c, Replace(text, "\"" + std::string(c.name) + "\", ", "\"" + std::string(c.name) + " (old)\", ")).empty());
        CHECK(!Problems(c, text + "\nint x = ImGuiTreeNodeFlags_DefaultOpen;\n").empty());
        if (!std::string(c.bodyFn).empty())
        {
            const std::string label = c.foldLabel;
            CHECK(!Problems(c, Replace(text, "ImGui::TreeNode(\"" + label + "\")", "ImGui::TreeNodeEx(\"" + label + "\", ImGuiTreeNodeFlags_DefaultOpen)")).empty());
            CHECK(!Problems(c, Replace(text, "ImGui::TreePop();", "")).empty());
            CHECK(!Problems(c, Replace(text, "ImGui::TreeNode(\"" + label + "\")", "true")).empty());
            CHECK(!Problems(c, Replace(text, std::string(c.bodyFn) + "();\n        ImGui::TreePop();", "ImGui::TreePop();\n        " + std::string(c.bodyFn) + "();")).empty());
            CHECK(!Problems(c, Replace(text, std::string(c.bodyFn) + "();\n        ImGui::TreePop();", "if (false)\n            return;\n        " + std::string(c.bodyFn) + "();\n        ImGui::TreePop();")).empty());
        }
    }

    // the diagnostics TreeNode of the companion card and the Advanced fold of the HUD-less card stay plain TreeNodes
    for (const char* file : { "ffxiv/companion/Companion.cpp", "hudless/HudlessMenu.cpp" })
    {
        const std::string text = menuscan::StripComments(menuscan::ReadFile(repo + std::string("/OptiScaler/ext/") + file));
        CHECK(Count(text, "ImGui::TreeNode(\"") >= 1);
        CHECK(text.find("TreeNodeEx") == std::string::npos);
    }
    {
        const std::string text = menuscan::StripComments(menuscan::ReadFile(repo + "/OptiScaler/ext/ffxiv/companion/Companion.cpp"));
        CHECK(Count(Squash(text), "if(!ImGui::CollapsingHeader(\"FFXIVOptiScalerCompanion\"))return;") == 1);
        CHECK(Count(Squash(text), "ImGui::TreeNode(\"Companiondiagnostics\")") == 1);
    }

    if (failures)
    {
        std::printf("diag_fold_text: %d failure(s)\n", failures);
        return 2;
    }
    std::printf("diag_fold_text: all passed\n");
    return 0;
}
