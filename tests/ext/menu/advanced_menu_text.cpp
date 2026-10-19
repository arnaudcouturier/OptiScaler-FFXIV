// The "Advanced (FFXIV)" card (ext/ffxiv/FfxivAdvancedMenu.cpp) is ImGui glue and cannot run offline: this pins what it
// must do by reading the source (comments stripped, string literals kept):
//   - registered as `ffxiv_advanced` in the FFXIV tab with a visible predicate that tests the game build;
//   - the two keys ([Hotfix] RenderSizeFromWidthHeight, [XeFG] ResetOnUpscalerChange) are each read and written only from
//     their own checkbox, and the tooltips say they are FFXIV only / when the change applies;
//   - the controls sit behind ONE fold that is closed by default, with a balanced TreePop and no other return;
//   - the key names it binds are the ones the config binds (ExtConfig.cpp / FgConfig.cpp), and the ini fragment of the
//     Hotfix key says FFXIV only.
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

// Body (from '{' to the matching '}') of the function whose signature text starts at `signature`
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

// All problems of the card source `text` (the fold and the key wiring); empty = fine
std::vector<std::string> Problems(const std::string& text)
{
    std::vector<std::string> p;
    const std::string stripped = menuscan::StripComments(text);
    const std::string code = Squash(stripped);
    const std::string draw = Squash(BodyOf(stripped, "void Draw(ext::menu::Ctx&)"));
    if (draw.empty())
    {
        p.push_back("Draw() not found");
        return p;
    }

    // the card title is the first widget and equals the picker name
    if (draw.find("{ext::menu::SectionTitle(\"Advanced(FFXIV)\");") != 0)
        p.push_back("the first statement is not SectionTitle(\"Advanced (FFXIV)\")");

    // one fold, closed by default: the guard is `if (!TreeNode(...)) return;`, the only return, one TreePop at the end
    const std::string guard = "if(!ImGui::TreeNode(\"Rarelyneededoptions##ffxiv_advanced\"))return;";
    if (Count(draw, guard) != 1)
        p.push_back("the fold guard is missing or not the plain closed TreeNode");
    if (Count(draw, "return") != 1)
        p.push_back("Draw() has a return other than the fold guard");
    if (Count(draw, "ImGui::TreePop();") != 1 || draw.rfind("ImGui::TreePop();}") != draw.size() - 18)
        p.push_back("TreePop is not exactly once, at the end of Draw()");
    if (code.find("DefaultOpen") != std::string::npos || code.find("TreeNodeEx") != std::string::npos)
        p.push_back("the fold must not be opened by default");

    // both controls are inside the fold (after the guard)
    const size_t g = draw.find(guard);
    const size_t c1 = draw.find("ImGui::Checkbox(\"RendersizefromWidth/Height(FFXIV)\"");
    const size_t c2 = draw.find("ImGui::Checkbox(\"RebuildXeFGwhentheupscalerisreleased(FFXIV)\"");
    if (g == std::string::npos || c1 == std::string::npos || c2 == std::string::npos || c1 < g || c2 < g)
        p.push_back("a checkbox is missing or outside the fold");

    // each key: one read into its own variable, one write from its own checkbox, nothing else
    if (Count(draw, "boolfromWidthHeight=cfg.upscaler.renderSizeFromWidthHeight.value_or_default();") != 1)
        p.push_back("RenderSizeFromWidthHeight is not read once into its variable");
    if (Count(draw, "if(ImGui::Checkbox(\"RendersizefromWidth/Height(FFXIV)\",&fromWidthHeight))"
                    "cfg.upscaler.renderSizeFromWidthHeight=fromWidthHeight;") != 1)
        p.push_back("RenderSizeFromWidthHeight is not written from its own checkbox");
    if (Count(code, "renderSizeFromWidthHeight=") != 1)
        p.push_back("RenderSizeFromWidthHeight is written somewhere else too");
    if (Count(draw, "boolresetOnChange=cfg.fg.xefgResetOnUpscalerChange.value_or_default();") != 1)
        p.push_back("ResetOnUpscalerChange is not read once into its variable");
    if (Count(draw, "if(ImGui::Checkbox(\"RebuildXeFGwhentheupscalerisreleased(FFXIV)\",&resetOnChange))"
                    "cfg.fg.xefgResetOnUpscalerChange=resetOnChange;") != 1)
        p.push_back("ResetOnUpscalerChange is not written from its own checkbox");
    if (Count(code, "xefgResetOnUpscalerChange=") != 1)
        p.push_back("ResetOnUpscalerChange is written somewhere else too");

    // the tooltips: FFXIV only, and when a change applies
    const std::string tip1 = Squash("Only FFXIV uses it");
    const std::string tip2 = Squash("settles when the upscaler is created again");
    const std::string tip3 = Squash("Only used in FFXIV, and only while XeFG");
    const std::string tip4 = Squash("applies at the next release");
    for (const std::string& t : { tip1, tip2, tip3, tip4 })
        if (code.find(t) == std::string::npos)
            p.push_back("a tooltip lost its sentence: " + t);

    // the registration: id, tab, title == picker name, own predicate that tests the game build
    size_t tokens = 0;
    std::vector<menuscan::Registration> regs;
    menuscan::ScanRegistrationText(stripped, "ffxiv/FfxivAdvancedMenu.cpp", tokens, regs);
    if (tokens != 1 || regs.size() != 1)
        p.push_back("expected exactly one registration");
    else
    {
        if (regs[0].id != "ffxiv_advanced" || regs[0].tab != "FFXIV")
            p.push_back("the card is not ffxiv_advanced in the FFXIV tab");
        if (regs[0].visible != "AdvancedVisible" || !menuscan::TestsGameBuild(regs[0]))
            p.push_back("the visible predicate is not AdvancedVisible testing the game build");
    }
    if (Count(code, "\"ffxiv_advanced\",\"FFXIV\",\"Advanced(FFXIV)\",Draw,nullptr,AdvancedVisible") != 1)
        p.push_back("registration fields changed (id, tab, picker name, draw, after, visible)");
    if (Count(code, "returnext::game::Current().lighting!=nullptr;") != 1)
        p.push_back("the predicate is not the FFXIV build match used by the tab");
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
    std::string text = menuscan::ReadFile(repo + "/OptiScaler/ext/ffxiv/FfxivAdvancedMenu.cpp");
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end()); // the working copy may have CRLF line ends
    CHECK(!text.empty());

    const auto problems = Problems(text);
    for (const auto& s : problems)
        std::printf("  card: %s\n", s.c_str());
    CHECK(problems.empty());

    // negative controls: every pin must complain about its own breakage
    CHECK(!Problems(Replace(text, "ImGui::TreeNode(\"Rarely needed options##ffxiv_advanced\")",
                            "ImGui::TreeNodeEx(\"Rarely needed options##ffxiv_advanced\", ImGuiTreeNodeFlags_DefaultOpen)")).empty());
    CHECK(!Problems(Replace(text, "    ImGui::TreePop();\n", "")).empty());
    CHECK(!Problems(Replace(text, "    ImGui::TreePop();\n", "    if (cfg.upscaler.renderSizeFromWidthHeight.value_or_default())\n        return;\n    ImGui::TreePop();\n")).empty());
    CHECK(!Problems(Replace(text, "cfg.upscaler.renderSizeFromWidthHeight = fromWidthHeight;", "cfg.upscaler.renderSizeFromWidthHeight = true;")).empty());
    CHECK(!Problems(Replace(text, "cfg.fg.xefgResetOnUpscalerChange = resetOnChange;", "cfg.fg.xefgResetOnUpscalerChange = false;")).empty());
    CHECK(!Problems(Replace(text, "AutoRegister registration { { \"ffxiv_advanced\", \"FFXIV\"", "AutoRegister registration { { \"ffxiv_advanced\", \"Compatibility\"")).empty());
    CHECK(!Problems(Replace(text, "AdvancedVisible } };", "nullptr } };")).empty());
    CHECK(!Problems(Replace(text, "return ext::game::Current().lighting != nullptr;", "return true;")).empty());
    CHECK(!Problems(Replace(text, "ext::menu::SectionTitle(\"Advanced (FFXIV)\");", "ext::menu::SectionTitle(\"Advanced\");")).empty());
    CHECK(!Problems(Replace(text, "Only FFXIV uses it", "Used everywhere")).empty());
    CHECK(!Problems("").empty());

    // the keys it binds are bound by the config under the same section/key names, and the ini fragments exist
    const std::string ext = menuscan::Squash(menuscan::StripComments(menuscan::ReadFile(repo + "/OptiScaler/ext/core/ExtConfig.cpp")));
    const std::string fg = menuscan::Squash(menuscan::StripComments(menuscan::ReadFile(repo + "/OptiScaler/ext/fg/FgConfig.cpp")));
    CHECK(ext.find("b.Add(\"Hotfix\",\"RenderSizeFromWidthHeight\",renderSizeFromWidthHeight);") != std::string::npos);
    CHECK(fg.find("b.Add(\"XeFG\",\"ResetOnUpscalerChange\",xefgResetOnUpscalerChange);") != std::string::npos);

    // the Hotfix ini fragment says the key is FFXIV only (the code gate is the FFXIV build match)
    const std::string hotfix = menuscan::ReadFile(repo + "/OptiScaler/ext/ini/Hotfix.ini");
    CHECK(hotfix.find("RenderSizeFromWidthHeight=auto") != std::string::npos);
    CHECK(hotfix.find("FFXIV only") != std::string::npos);
    CHECK(hotfix.find("A wrong reading is a wrong picture rather than an error") != std::string::npos);

    if (failures)
    {
        std::printf("advanced_menu_text: %d failure(s)\n", failures);
        return 2;
    }
    std::printf("advanced_menu_text: all passed\n");
    return 0;
}
