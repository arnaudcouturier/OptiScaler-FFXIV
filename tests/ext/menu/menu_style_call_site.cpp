// Ember menu style: the one upstream line. ext::menu::ApplyStyle(style) sits in MenuCommon::ApplyThemeStyle after the classic
// colours are set and BEFORE the HDR tone mapping of the style (MenuHdrCheck), so under HDR output the Ember colours are
// tone-mapped like the classic ones, and before ext::menu::AdjustTheme (the HDR opacity). It appears exactly once, and the
// menu file includes nothing new from the fork (upstream files include only ext/Seams.h).
//
// POSITIONAL, on the text with comments removed. The negative controls run each rule on a mutated copy.
#include "menu_scan.h"

#include <cstdio>
#include <string>

static int g_failed = 0;
#define CHECK(cond)                                                                                                         \
    do                                                                                                                      \
    {                                                                                                                       \
        if (!(cond))                                                                                                        \
        {                                                                                                                   \
            std::printf("CHECK failed line %d: %s\n", __LINE__, #cond);                                                     \
            ++g_failed;                                                                                                     \
        }                                                                                                                   \
    } while (0)

namespace
{
struct Order
{
    bool once = false;
    bool afterClassicColours = false;
    bool beforeHdrTonemap = false;
    bool beforeAdjustTheme = false;
    bool noNewInclude = false;
};

Order Check(const std::string& code)
{
    Order o;
    const std::string call = "ext::menu::ApplyStyle(style);";
    const size_t at = code.find(call);
    o.once = at != std::string::npos && code.find(call, at + 1) == std::string::npos;
    if (!o.once)
        return o;

    const size_t fn = code.find("void MenuCommon::ApplyThemeStyle()");
    const size_t lastColour = code.find("c[ImGuiCol_ModalWindowDimBg] = modalDimBg;", fn);
    const size_t tonemap = code.find("_hdrTonemapApplied = false;", fn);
    const size_t adjust = code.find("ext::menu::AdjustTheme(", fn);
    o.afterClassicColours = fn != std::string::npos && lastColour != std::string::npos && at > lastColour;
    o.beforeHdrTonemap = tonemap != std::string::npos && at < tonemap;
    o.beforeAdjustTheme = adjust != std::string::npos && at < adjust;
    o.noNewInclude = code.find("ext/menu/") == std::string::npos && code.find("EmberPalette") == std::string::npos;
    return o;
}

std::string Replace(std::string text, const std::string& from, const std::string& to)
{
    const size_t at = text.find(from);
    if (at != std::string::npos)
        text.replace(at, from.size(), to);
    return text;
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    const std::string code = menuscan::StripComments(menuscan::ReadFile(repo + "/OptiScaler/menu/menu_common.cpp"));
    CHECK(code.find("void MenuCommon::ApplyThemeStyle()") != std::string::npos);

    const Order o = Check(code);
    CHECK(o.once);
    CHECK(o.afterClassicColours);
    CHECK(o.beforeHdrTonemap);
    CHECK(o.beforeAdjustTheme);
    CHECK(o.noNewInclude);

    // Negative controls: each mutation breaks exactly the rule it targets.
    const std::string call = "ext::menu::ApplyStyle(style);";
    {
        // moved after the tone mapping
        std::string m = Replace(code, call, "");
        m = Replace(m, "ext::menu::AdjustTheme(", call + " ext::menu::AdjustTheme(");
        const Order bad = Check(m);
        CHECK(bad.once && !bad.beforeHdrTonemap);
    }
    {
        // moved before the classic colours
        std::string m = Replace(code, call, "");
        m = Replace(m, "ImVec4* c = ImGui::GetStyle().Colors;", call + " ImVec4* c = ImGui::GetStyle().Colors;");
        const Order bad = Check(m);
        CHECK(bad.once && !bad.afterClassicColours);
    }
    {
        // removed
        CHECK(!Check(Replace(code, call, "")).once);
        // doubled
        CHECK(!Check(Replace(code, call, call + " " + call)).once);
    }
    {
        // a fork include added to the upstream file
        CHECK(!Check("#include \"ext/menu/MenuStyleSeams.h\"\n" + code).noNewInclude);
    }

    return g_failed ? 1 : 0;
}
