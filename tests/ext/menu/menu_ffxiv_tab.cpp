// The "FFXIV" tab: one row in upstream's tabs[] array, third in the bar, shown only on the FFXIV build match, and the
// nine FFXIV-gated cards registered for it. The menu is ImGui glue and cannot run offline, so this reads the sources
// (comments stripped, string literals kept; see menu_scan.h).
#include "menu_scan.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace
{
using menuscan::Count;
using menuscan::Squash;

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

void TabRow(const std::string& repo)
{
    const std::string code = menuscan::StripComments(menuscan::ReadFile(repo + "/OptiScaler/menu/menu_common.cpp"));
    const auto tabs = menuscan::ParseTabs(code);
    CHECK(tabs.size() >= 9);

    size_t ffxiv = 0, frameGen = 0, rows = 0;
    for (size_t i = 0; i < tabs.size(); i++)
    {
        if (tabs[i].label == "FFXIV")
        {
            ffxiv = i;
            ++rows;
        }
        if (tabs[i].label == "Frame Gen")
            frameGen = i;
    }
    // exactly one row, right after Frame Gen (third in the bar: Custom, Upscaling, Frame Gen, FFXIV)
    CHECK(rows == 1);
    CHECK(Count(Squash(code), "{\"MAIN\",\"FFXIV\",") == 1);
    CHECK(frameGen == 2);
    CHECK(ffxiv == frameGen + 1);
    if (rows == 1)
    {
        CHECK(tabs[ffxiv].group == "MAIN");
        // group, label, description, draw (none), hideLabelInMenu, visible: the predicate lives in ext
        CHECK(tabs[ffxiv].fields.size() == 6);
        if (tabs[ffxiv].fields.size() == 6)
        {
            CHECK(tabs[ffxiv].fields[3] == "nullptr");
            CHECK(tabs[ffxiv].fields[4] == "false");
            CHECK(Squash(tabs[ffxiv].fields[5]) == "ext::menu::FfxivTabVisible<RenderMenuContext>");
        }
    }

    // tab labels are unique (the box loop and the Custom tab match by label)
    for (size_t i = 0; i < tabs.size(); i++)
        for (size_t j = i + 1; j < tabs.size(); j++)
            CHECK(tabs[i].label != tabs[j].label);
}

void Predicate(const std::string& repo)
{
    const std::string cpp = Squash(
        menuscan::StripComments(menuscan::ReadFile(repo + "/OptiScaler/ext/core/MenuRegistry.cpp")));
    const std::string h = Squash(menuscan::StripComments(menuscan::ReadFile(repo + "/OptiScaler/ext/core/MenuRegistry.h")));

    // the build match: the FFXIV pointers are all set together, `lighting` is the one the other FFXIV cards test
    CHECK(Count(cpp, "boolFfxivTabShown(){returnext::game::Current().lighting!=nullptr;}") == 1);
    CHECK(Count(cpp, "#include\"ext/game/Game.h\"") == 1);
    // the template the row points at forwards to it, whatever the context type is
    CHECK(Count(h, "boolFfxivTabShown();") == 1);
    CHECK(Count(h, "template<classRenderContext>boolFfxivTabVisible(RenderContext&){returnFfxivTabShown();}") == 1);
}

void Cards(const std::string& repo)
{
    size_t tokens = 0;
    const auto regs = menuscan::ScanRegistrations(repo, tokens);
    CHECK(regs.size() == tokens);
    CHECK(regs.size() == 15);

    std::map<std::string, std::string> tabOf;
    for (const auto& r : regs)
        tabOf[r.id] = r.tab;

    // the nine cards that need the FFXIV build match are in the tab, and nothing else is
    const std::vector<std::string> ffxivCards = { "window_mode",     "reshade_depth",   "ffxiv_hdr",    "ffxiv_jitter", "ffxiv_character_preview",
                                                  "fg_hudless",      "ffxiv_companion", "hudless_diag", "ffxiv_advanced" };
    for (const auto& id : ffxivCards)
        CHECK(tabOf.count(id) == 1 && tabOf[id] == "FFXIV");

    size_t inTab = 0;
    for (const auto& r : regs)
        if (r.tab == "FFXIV")
            ++inTab;
    CHECK(inTab == ffxivCards.size());
}
// The Custom Tab picker lists every pinnable box; the FFXIV-tab cards are skipped unless the game is FFXIV, through
// the same predicate that hides the tab (a renamed build still reads the same: the rule never names the file name,
// only the tab label the fork's own cards register under). A dropped skip lists the FFXIV cards in every game again.
void Picker(const std::string& repo)
{
    const std::string code = menuscan::StripComments(menuscan::ReadFile(repo + "/OptiScaler/menu/menu_common.cpp"));
    CHECK(Count(Squash(code), "strcmp(box.tab,\"FFXIV\")==0&&!ext::menu::FfxivTabVisible(ctx)") == 1);
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";

    TabRow(repo);
    Predicate(repo);
    Cards(repo);
    Picker(repo);

    if (failures)
    {
        std::printf("menu_ffxiv_tab: %d failure(s)\n", failures);
        return 2;
    }
    std::printf("menu_ffxiv_tab: all passed\n");
    return 0;
}
