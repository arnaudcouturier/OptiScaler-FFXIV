// WP4 controls of the menu redesign, pinned as source text (ImGui glue cannot run offline). Each row is one control: the
// file under OptiScaler/ext that draws it, the widget call, the config write that must sit inside that widget's own block
// (from the widget to the next input widget, fold or TreePop), the phrases its tooltip/label must carry (a restart key says
// "applies after a restart", a live key does not), optionally the fold it must be inside and a marker it must come before.
//
// Why: the maintainer's rule (decision Q4): a user-facing key gets a control in the card where it belongs, a diagnostic key
// goes into a collapsed Advanced fold, and the tooltip says whether a change applies at once or after a restart.
//
// Every row is also run against broken copies of the code (write removed or moved out of its block, a phrase removed, the
// widget renamed or doubled, the fold closed early, the widget moved behind its marker), so each pin is mutation-checked by
// construction; the product mutations (change the key by hand, see the test fail, restore) are recorded in MENU-LOG.md.
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

struct Row
{
    const char* file = "";            // under OptiScaler/ext
    const char* widget = "";          // the widget call up to its label, e.g. ImGui::Combo("ReShade presets"
    const char* write = "";           // must sit inside the widget block: the config write from the widget's own value
    const char* keyPrefix = "";       // every write of the key in the file: "" = not counted
    int writes = 1;                   // how many times keyPrefix occurs in the file
    std::vector<const char*> block;   // phrases that must sit inside the widget block (label end, tooltip)
    std::vector<const char*> fileHas; // phrases that must be somewhere in the file
    const char* fold = "";            // TreeNode(...) call whose span must contain the widget; "" = no fold check
    const char* before = "";          // marker the widget must come before (main view above a fold); "" = no check
    std::vector<const char*> blockNot; // phrases that must NOT sit inside the widget block (e.g. "restart" on a live key)
    const char* guard = "";           // text ending with '{': the widget must sit inside these braces (game or mode gate)
};

constexpr const char* kNrAdvanced = "ImGui::TreeNode(\"Advanced##nr_card\")"; // closed by default: plain TreeNode, no DefaultOpen

const Row kRows[] = {
    // WP4 item 1: [HudLess] ReShadePresets, main part of the HUD-less card, read at every UI boundary = applies at once
    { .file = "hudless/HudlessMenu.cpp", .widget = "ImGui::Combo(\"ReShade presets at the UI boundary\"",
      .write = "ext::Cfg().reshade.presets=std::string(kPresetNames[preset])", .keyPrefix = "ext::Cfg().reshade.presets=",
      .block = { "applies at once", "Only used in UI-free mode" },
      .fileHas = { "ParsePresetsMode(ext::Cfg().reshade.presets.value_or_default())" }, .before = "ImGui::TreeNode(\"Advanced\")",
      .blockNot = { "restart" } },
    // WP4 item 1: [DLSSG] UIRecomposition (auto/on/off), inside the existing Advanced fold, read when DLSS-G is created = restart
    { .file = "hudless/HudlessMenu.cpp", .widget = "ImGui::Combo(\"UI recomposition (restart)\"",
      .write = "recompositionSetting=recomposition==0?std::optional<bool>{}:std::optional<bool>(recomposition==1)",
      .keyPrefix = "recompositionSetting=recomposition==0", .block = { "applies after a restart", "Recomposition mode only" },
      .fileHas = { "auto&recompositionSetting=ext::Cfg().fg.uiRecomposition;" }, .fold = "ImGui::TreeNode(\"Advanced\")" },
    // WP4 item 2: [XeFG] UnlockMFG and MaxInterpolatedFrames, drawn inside upstream's XeFG card (XeFgExtras); both are read when
    // the provider is loaded / the swapchain is created = restart
    { .file = "fg/xefg/XefgSeams.cpp", .widget = "ImGui::Checkbox(\"Unlock multi frame generation (restart)\"",
      .write = "fgCfg.xefgUnlock=unlockMfg", .keyPrefix = "fgCfg.xefgUnlock=", .block = { "applies after a restart", "never changed" } },
    { .file = "fg/xefg/XefgSeams.cpp", .widget = "ImGui::SliderInt(\"Max XeFG multiplier (restart)\"",
      .write = "fgCfg.xefgMaxInterpolatedFrames=maxMultiplier-1", .keyPrefix = "fgCfg.xefgMaxInterpolatedFrames=",
      .block = { "applies after a restart", "handed to the provider" }, .fileHas = { "BeginDisabled(!unlockMfg)" } },

    // WP4 item 4 (a): the two FFXIV-only NR options stay in the NR card, drawn only on the FFXIV build match, "(FFXIV)" in the label
    { .file = "nr/menu/DlssNr_Menu.cpp", .widget = "ImGui::Checkbox(\"Spread alternating NR across two frames (FFXIV, experimental)\"",
      .write = "nr.splitFrameWork=splitWork", .keyPrefix = "nr.splitFrameWork=", .block = { "FFXIV: run NR on one frame" },
      .fileHas = { "ImGui::BeginDisabled(!residualFg || !approxCamera);" }, .guard = "if (ext::nr::BridgedNativeTitle())\n{" },
    { .file = "nr/menu/DlssNr_Menu.cpp", .widget = "ImGui::Combo(\"Exposure source (FFXIV)\"", .write = "nr.whitePointSource=(uint32_t)source;",
      .block = { "verified native DX11 lighting shaders" },
      .fileHas = { "(FFXIV) The game applies exposure before NR.", "\"Scanned exposure (FFXIV native DX11)\"" },
      .guard = "if (nativeLighting)\n{" },
    { .file = "nr/menu/DlssNr_Menu.cpp", .widget = "ImGui::Checkbox(\"Reject abrupt lighting history (FFXIV, experimental)\"",
      .write = "nr.lightingHistory=history", .keyPrefix = "nr.lightingHistory=", .block = { "Restart NR and its private PreSR" },
      .guard = "if (nativeLighting)\n{" },

    // WP4 item 4 (b): the closed "Advanced" fold at the end of the NR card: developer/diagnostic keys and the spatial placement
    { .file = "nr/menu/DlssNr_Menu.cpp", .widget = "ImGui::Checkbox(\"Scan for the game's exposure (developer)\"",
      .write = "nr.scanExposure=scanExposure", .keyPrefix = "nr.scanExposure=", .block = { "applies at once", "no effect in FFXIV" },
      .fileHas = { "ImGui::BeginDisabled(ext::nr::BridgedNativeTitle());", "ImGui::TreePop();\n}\n\nImGui::PopItemWidth();" },
      .fold = kNrAdvanced },
    { .file = "nr/menu/DlssNr_Menu.cpp", .widget = "ImGui::Checkbox(\"Capture frames automatically (once per session)\"",
      .write = "nr.autoCapture=autoCapture", .keyPrefix = "nr.autoCapture=", .block = { "one capture per session", "applies at once" },
      .fold = kNrAdvanced },
    { .file = "nr/menu/DlssNr_Menu.cpp", .widget = "ImGui::Checkbox(\"Log NR kernel launches (profiling, restart)\"",
      .write = "nr.vitCapture=vitCapture", .keyPrefix = "nr.vitCapture=", .block = { "applies after a restart", "log only" },
      .fold = kNrAdvanced },
    { .file = "nr/menu/DlssNr_Menu.cpp", .widget = "DeferredSlider(\"Central region offset (horizontal)\"",
      .write = "&nr.spatialOffsetX,-maxOffsetX,maxOffsetX,0.0f", .keyPrefix = "&nr.spatialOffsetX",
      .block = { "applied when the slider is released" }, .fold = kNrAdvanced, .guard = "if (spatialOn)\n{" },
    { .file = "nr/menu/DlssNr_Menu.cpp", .widget = "DeferredSlider(\"Central region offset (vertical)\"",
      .write = "&nr.spatialOffsetY,-maxOffsetY,maxOffsetY,0.0f", .keyPrefix = "&nr.spatialOffsetY",
      .block = { "applied when the slider is released" }, .fold = kNrAdvanced, .guard = "if (spatialOn)\n{" },
    { .file = "nr/menu/DlssNr_Menu.cpp", .widget = "DeferredSlider(\"Work shift (horizontal)\"",
      .write = "&nr.spatialShiftX,shiftLimitsX.first,shiftLimitsX.second,0.0f", .keyPrefix = "&nr.spatialShiftX",
      .block = { "applied when the slider is released" }, .fold = kNrAdvanced, .guard = "if (spatialOn)\n{" },
    { .file = "nr/menu/DlssNr_Menu.cpp", .widget = "DeferredSlider(\"Work shift (vertical)\"",
      .write = "&nr.spatialShiftY,shiftLimitsY.first,shiftLimitsY.second,0.0f", .keyPrefix = "&nr.spatialShiftY",
      .block = { "applied when the slider is released" }, .fold = kNrAdvanced, .guard = "if (spatialOn)\n{" },
};

// Squashed code of the file with the widget's block cut out: from the widget to the next input widget, fold or TreePop
std::string BlockOf(const std::string& code, const std::string& widget)
{
    const size_t at = code.find(widget);
    if (at == std::string::npos)
        return {};
    static const char* kEnds[] = { "ImGui::Checkbox(", "ImGui::Combo(",       "ImGui::SliderInt(", "ImGui::SliderFloat(",
                                   "ImGui::InputInt(", "ImGui::InputFloat(",  "ImGui::DragInt(",   "ImGui::DragFloat(",
                                   "ImGui::Button(",   "ImGui::SmallButton(", "ImGui::TreeNode(",  "ImGui::TreePop(",
                                   "ImGui::CollapsingHeader(", "SectionTitle(", "DeferredSlider(" };
    size_t end = code.size();
    for (const char* e : kEnds)
    {
        const size_t p = code.find(e, at + widget.size());
        if (p != std::string::npos)
            end = std::min(end, p);
    }
    return code.substr(at, end - at);
}

// [start, end) of the fold opened by `node` (the TreeNode call) up to its matching ImGui::TreePop();
bool FoldSpan(const std::string& code, const std::string& node, size_t& start, size_t& end)
{
    start = code.find(node);
    if (start == std::string::npos || code.find(node, start + node.size()) != std::string::npos)
        return false;
    int depth = 1;
    for (size_t i = start + node.size(); i < code.size(); ++i)
    {
        if (code.compare(i, 16, "ImGui::TreeNode(") == 0)
            ++depth;
        else if (code.compare(i, 17, "ImGui::TreePop();") == 0 && --depth == 0)
        {
            end = i;
            return true;
        }
    }
    return false;
}

// [start, end) of the braces opened by `guard` (squashed text ending with '{'), up to the matching '}'
bool GuardSpan(const std::string& code, const std::string& guard, size_t& start, size_t& end)
{
    start = code.find(guard);
    if (start == std::string::npos || code.find(guard, start + guard.size()) != std::string::npos)
        return false;
    int depth = 1;
    for (size_t i = start + guard.size(); i < code.size();)
    {
        if (code[i] == '"')
        {
            i = menuscan::SkipString(code, i);
            continue;
        }
        if (code[i] == '{')
            ++depth;
        else if (code[i] == '}' && --depth == 0)
        {
            end = i;
            return true;
        }
        ++i;
    }
    return false;
}

// `code` is the squashed, comment-stripped text of the file
std::vector<std::string> Problems(const Row& r, const std::string& code)
{
    std::vector<std::string> p;
    auto bad = [&](const std::string& s) { p.push_back(std::string(r.file) + ": " + s); };
    const std::string widget = Squash(r.widget);

    if (Count(code, widget) != 1)
    {
        bad(std::string("the widget does not occur exactly once: ") + r.widget);
        return p;
    }
    const std::string block = BlockOf(code, widget);
    if (block.find(Squash(r.write)) == std::string::npos)
        bad(std::string("the key is not written from its own widget: ") + r.write);
    if (r.keyPrefix[0] != '\0' && Count(code, Squash(r.keyPrefix)) != static_cast<size_t>(r.writes))
        bad(std::string("the key is not written exactly the expected number of times: ") + r.keyPrefix);
    for (const char* s : r.block)
        if (block.find(Squash(s)) == std::string::npos)
            bad(std::string("the widget block lacks: ") + s);
    for (const char* s : r.blockNot)
        if (block.find(Squash(s)) != std::string::npos)
            bad(std::string("the widget block must not say: ") + s);
    for (const char* s : r.fileHas)
        if (code.find(Squash(s)) == std::string::npos)
            bad(std::string("the file lacks: ") + s);
    const size_t at = code.find(widget);
    if (r.fold[0] != '\0')
    {
        size_t start = 0, end = 0;
        if (!FoldSpan(code, Squash(r.fold), start, end))
            bad(std::string("the fold is missing, doubled or never closed: ") + r.fold);
        else if (at < start || at > end)
            bad(std::string("the widget is outside its fold: ") + r.fold);
    }
    if (r.before[0] != '\0')
    {
        const size_t marker = code.find(Squash(r.before));
        if (marker == std::string::npos || at > marker)
            bad(std::string("the widget does not come before: ") + r.before);
    }
    if (r.guard[0] != '\0')
    {
        size_t start = 0, end = 0;
        if (!GuardSpan(code, Squash(r.guard), start, end))
            bad(std::string("the gate is missing, doubled or never closed: ") + r.guard);
        else if (at < start || at > end)
            bad(std::string("the widget is outside its gate: ") + r.guard);
    }
    return p;
}

std::string Replace(std::string s, const std::string& from, const std::string& to, size_t startAt = 0)
{
    const size_t at = s.find(from, startAt);
    if (at != std::string::npos)
        s.replace(at, from.size(), to);
    return s;
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";

    for (const Row& r : kRows)
    {
        std::string raw = menuscan::ReadFile(repo + "/OptiScaler/ext/" + r.file);
        raw.erase(std::remove(raw.begin(), raw.end(), '\r'), raw.end()); // the working copy may have CRLF line ends
        CHECK(!raw.empty());
        const std::string code = Squash(menuscan::StripComments(raw));

        const auto problems = Problems(r, code);
        for (const auto& s : problems)
            std::printf("  %s\n", s.c_str());
        CHECK(problems.empty());

        // negative controls: every pin complains about its own breakage
        const std::string widget = Squash(r.widget);
        const std::string write = Squash(r.write);
        CHECK(!Problems(r, Replace(code, write, "x=0")).empty());                       // the write is gone
        CHECK(!Problems(r, Replace(Replace(code, write, ""), widget, write + ";" + widget)).empty()); // the write left its widget
        CHECK(!Problems(r, Replace(code, widget, "ImGui::Combo(\"zz\"")).empty());      // the widget is gone
        CHECK(!Problems(r, code + widget).empty());                                      // the widget is doubled
        const size_t widgetAt = code.find(widget); // a phrase may also occur earlier in the file: cut the one in the block
        for (const char* s : r.block)
            CHECK(!Problems(r, Replace(code, Squash(s), "", widgetAt)).empty());
        for (const char* s : r.blockNot)
            CHECK(!Problems(r, Replace(code, write, write + Squash(s))).empty());
        for (const char* s : r.fileHas)
            CHECK(!Problems(r, Replace(code, Squash(s), "")).empty());
        if (r.keyPrefix[0] != '\0')
            CHECK(!Problems(r, code + Squash(r.keyPrefix) + "1;").empty()); // a second write of the key
        if (r.fold[0] != '\0')
        {
            const std::string node = Squash(r.fold);
            CHECK(!Problems(r, Replace(code, node, node + "ImGui::TreePop();")).empty()); // the fold closes before the widget
            CHECK(!Problems(r, Replace(code, node, "if(true){")).empty());                  // no fold at all
        }
        if (r.guard[0] != '\0')
        {
            const std::string gate = Squash(r.guard);
            CHECK(!Problems(r, Replace(code, gate, "if(true){")).empty());   // no gate at all
            CHECK(!Problems(r, Replace(code, gate, gate + "}{")).empty());    // the gate closes before the widget
        }
        if (r.before[0] != '\0')
        {
            const std::string marker = Squash(r.before);
            CHECK(!Problems(r, Replace(Replace(code, widget, "x"), marker, marker + widget)).empty()); // behind the marker
        }
    }

    if (failures)
    {
        std::printf("controls_text: %d failure(s)\n", failures);
        return 2;
    }
    std::printf("controls_text: all passed\n");
    return 0;
}
