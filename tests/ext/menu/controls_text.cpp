// WP4 controls of the menu redesign, pinned as source text (ImGui glue cannot run offline). Each row is one control: the
// file under OptiScaler/ext that draws it, the widget call, the config write that must sit inside that widget's own block
// (from the widget to the next input widget, fold or TreePop), the phrases its tooltip/label must carry (a restart key says
// "applies after a restart", a live key does not), optionally the fold it must be inside and a marker it must come before.
//
// Why: the maintainer's rule (decision Q4): a user-facing key gets a control in the card where it belongs, a diagnostic key
// goes into a collapsed Advanced fold, and the tooltip says whether a change applies at once or after a restart.
//
// Scope: the widget, its config write, its phrases, its fold, gate and marker are looked for in the brace-matched BODY of the
// function named in the row's `home` (not in the whole file), that function must be reachable from the card's entry inside
// the file, and the entry must be registered / called where `home.useFile` says. So a widget in a dead function, in another
// function of the file or in an `#if 0` block no longer satisfies a pin. The key-write count and the `fileHas` phrases stay
// file-wide on purpose (a key written twice anywhere is a finding).
//
// Every row is also run against broken copies of the code (write removed or moved out of its block, a phrase removed, the
// widget renamed or doubled, the fold closed early, the widget moved behind its marker, the widget and its write moved into
// a function nothing calls, the function renamed away or its calls renamed, the entry not registered), so each pin is
// mutation-checked by construction; the product mutations (change the key by hand, see the test fail, restore) are recorded
// in MENU-LOG.md.
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

// Where a control lives: the function that holds the widget and its config write (brace matched, so a widget in a dead
// function or in another function of the file does not count), the entry the menu calls (the function must be reachable
// from it inside the file) and the one place that makes the entry live (the card registration, or the upstream call site of
// a seam): `useFile` (under the repo root) must contain `useText`.
struct Home
{
    const char* func = "";
    const char* entry = "";
    const char* useFile = "";
    const char* useText = "";
};

constexpr Home kHudless { "DrawHudless", "DrawHudless", "OptiScaler/ext/hudless/HudlessMenu.cpp",
                          "\"fg_hudless\", \"FFXIV\", \"HUD-less (UI-free frame generation)\", DrawHudless," };
constexpr Home kXeFg { "XeFgExtras", "XeFgExtras", "OptiScaler/menu/menu_common.cpp", "ext::menu::XeFgExtras(fgOutput);" };
constexpr Home kNr { "RenderMenu", "RenderMenu", "OptiScaler/ext/nr/menu/NrMenu.cpp", "DlssNr::RenderMenu(ctx.menuResScale);" };
constexpr Home kChara { "DrawDebugSection", "DrawCharaCard", "OptiScaler/ext/ffxiv/chara/CharaMenu.cpp",
                        "\"ffxiv_character_preview\", \"FFXIV\", \"Character preview\", DrawCharaCard," };
constexpr Home kAmpere { "DrawAmpereAdvanced", "DrawFgUnlocks", "OptiScaler/ext/fg/MenuFgUnlocks.cpp",
                         "\"fg_unlocks\", \"Frame Gen\", \"Frame generation unlocks\", DrawFgUnlocks," };

constexpr Home kDepth { "Draw", "Draw", "OptiScaler/ext/reshade/ReShadeDepthMenu.cpp",
                        "\"reshade_depth\", \"FFXIV\", \"ReShade depth compatibility\", Draw," };

struct Row
{
    Home home;                        // where the control lives (function, entry, registration)
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


constexpr const char* kAmpereAdvanced = "ImGui::TreeNode(\"Advanced (RTX 20/30 runtime)##ampere_advanced\")"; // closed by default

const Row kRows[] = {
    // Automatic ReShade scene depth: [FFXIV] AutomaticReShadeDepth, the switch of the "ReShade depth compatibility" card. Read at every
    // evaluate (the facade keeps it in one atomic word), so a change applies from the next frame, no restart; the tooltip says what it
    // needs (ReShade 6.8 or newer) and what it stands down for, and the toggle goes through SetEnabled so the binder follows it.
    { .home = kDepth, .file = "reshade/ReShadeDepthMenu.cpp", .widget = "ImGui::Checkbox(\"Automatic scene depth\"",
      .write = "cfg.automaticReShadeDepth=on", .keyPrefix = "cfg.automaticReShadeDepth=",
      .block = { "Needs ReShade 6.8 or newer (add-on API 20)", "RESHADE_DEPTH_INPUT_IS_REVERSED=1", "Takes effect from the next frame" },
      .fileHas = { "ext::reshade::depth::SetEnabled(on);", "ext::reshade::depth::Status()", "ext::menu::SectionTitle(\"ReShade depth compatibility\");" },
      .blockNot = { "restart" } },

    // WP4 item 1: [HudLess] ReShadePresets, main part of the HUD-less card. The mode is read at every UI boundary, so switching to Off
    // applies at once; but the ReShade runtime tracking is only installed at start when presets are not Off (ReShadeAddon.cpp
    // OnReshadeLoaded), so switching on again after a start with Off applies after a restart: the tooltip says both.
    { .home = kHudless, .file = "hudless/HudlessMenu.cpp", .widget = "ImGui::Combo(\"ReShade presets at the UI boundary\"",
      .write = "ext::Cfg().reshade.presets=std::string(kPresetNames[preset])", .keyPrefix = "ext::Cfg().reshade.presets=",
      .block = { "Switching to Off applies at once", "applies after a restart", "Only used in UI-free mode" },
      .fileHas = { "ParsePresetsMode(ext::Cfg().reshade.presets.value_or_default())" }, .before = "ImGui::TreeNode(\"Advanced\")" },
    // WP4 item 1: [DLSSG] UIRecomposition (auto/on/off), inside the existing Advanced fold, read when DLSS-G is created = restart
    { .home = kHudless, .file = "hudless/HudlessMenu.cpp", .widget = "ImGui::Combo(\"UI recomposition (restart)\"",
      .write = "recompositionSetting=recomposition==0?std::optional<bool>{}:std::optional<bool>(recomposition==1)",
      .keyPrefix = "recompositionSetting=recomposition==0", .block = { "applies after a restart", "Recomposition mode only" },
      .fileHas = { "auto&recompositionSetting=ext::Cfg().fg.uiRecomposition;" }, .fold = "ImGui::TreeNode(\"Advanced\")" },
    // WP4 item 2: [XeFG] UnlockMFG and MaxInterpolatedFrames, drawn inside upstream's XeFG card (XeFgExtras); both are read when
    // the provider is loaded / the swapchain is created = restart
    { .home = kXeFg, .file = "fg/xefg/XefgSeams.cpp", .widget = "ImGui::Checkbox(\"Unlock multi frame generation (restart)\"",
      .write = "fgCfg.xefgUnlock=unlockMfg", .keyPrefix = "fgCfg.xefgUnlock=", .block = { "applies after a restart", "never changed" } },
    { .home = kXeFg, .file = "fg/xefg/XefgSeams.cpp", .widget = "ImGui::SliderInt(\"Max XeFG multiplier (restart)\"",
      .write = "fgCfg.xefgMaxInterpolatedFrames=maxMultiplier-1", .keyPrefix = "fgCfg.xefgMaxInterpolatedFrames=",
      .block = { "applies after a restart", "handed to the provider", "ImGuiSliderFlags_AlwaysClamp" }, .fileHas = { "BeginDisabled(!unlockMfg)" } },

    // WP4 item 4 (a): the two FFXIV-only NR options stay in the NR card, drawn only on the FFXIV build match, "(FFXIV)" in the label
    { .home = kNr, .file = "nr/menu/DlssNr_Menu.cpp", .widget = "ImGui::Checkbox(\"Spread alternating NR across two frames (FFXIV, experimental)\"",
      .write = "nr.splitFrameWork=splitWork", .keyPrefix = "nr.splitFrameWork=", .block = { "FFXIV: run NR on one frame" },
      .fileHas = { "ImGui::BeginDisabled(!residualFg || !approxCamera);" }, .guard = "if (ext::nr::BridgedNativeTitle())\n{" },
    { .home = kNr, .file = "nr/menu/DlssNr_Menu.cpp", .widget = "ImGui::Combo(\"Exposure source (FFXIV)\"", .write = "nr.whitePointSource=(uint32_t)source;",
      .block = { "verified native DX11 lighting shaders" },
      .fileHas = { "(FFXIV) The game applies exposure before NR.", "\"Scanned exposure (FFXIV native DX11)\"" },
      .guard = "if (nativeLighting)\n{" },
    { .home = kNr, .file = "nr/menu/DlssNr_Menu.cpp", .widget = "ImGui::Checkbox(\"Reject abrupt lighting history (FFXIV, experimental)\"",
      .write = "nr.lightingHistory=history", .keyPrefix = "nr.lightingHistory=", .block = { "Restart NR and its private PreSR" },
      .guard = "if (nativeLighting)\n{" },

    // WP4 item 4 (b): the closed "Advanced" fold at the end of the NR card: developer/diagnostic keys and the spatial placement
    { .home = kNr, .file = "nr/menu/DlssNr_Menu.cpp", .widget = "ImGui::Checkbox(\"Scan for the game's exposure (developer)\"",
      .write = "nr.scanExposure=scanExposure", .keyPrefix = "nr.scanExposure=", .block = { "applies at once", "no effect in FFXIV" },
      .fileHas = { "ImGui::BeginDisabled(ext::nr::BridgedNativeTitle());", "ImGui::TreePop();\n}\n\nImGui::PopItemWidth();" },
      .fold = kNrAdvanced },
    { .home = kNr, .file = "nr/menu/DlssNr_Menu.cpp", .widget = "ImGui::Checkbox(\"Capture frames automatically (once per session)\"",
      .write = "nr.autoCapture=autoCapture", .keyPrefix = "nr.autoCapture=", .block = { "one capture per session", "applies at once" },
      .fold = kNrAdvanced },
    { .home = kNr, .file = "nr/menu/DlssNr_Menu.cpp", .widget = "ImGui::Checkbox(\"Log NR kernel launches (profiling, restart)\"",
      .write = "nr.vitCapture=vitCapture", .keyPrefix = "nr.vitCapture=", .block = { "applies after a restart", "log only" },
      .fold = kNrAdvanced },
    { .home = kNr, .file = "nr/menu/DlssNr_Menu.cpp", .widget = "DeferredSlider(\"Central region offset (horizontal)\"",
      .write = "&nr.spatialOffsetX,-maxOffsetX,maxOffsetX,0.0f", .keyPrefix = "&nr.spatialOffsetX",
      .block = { "applied when the slider is released" }, .fold = kNrAdvanced, .guard = "if (spatialOn)\n{" },
    { .home = kNr, .file = "nr/menu/DlssNr_Menu.cpp", .widget = "DeferredSlider(\"Central region offset (vertical)\"",
      .write = "&nr.spatialOffsetY,-maxOffsetY,maxOffsetY,0.0f", .keyPrefix = "&nr.spatialOffsetY",
      .block = { "applied when the slider is released" }, .fold = kNrAdvanced, .guard = "if (spatialOn)\n{" },
    { .home = kNr, .file = "nr/menu/DlssNr_Menu.cpp", .widget = "DeferredSlider(\"Work shift (horizontal)\"",
      .write = "&nr.spatialShiftX,shiftLimitsX.first,shiftLimitsX.second,0.0f", .keyPrefix = "&nr.spatialShiftX",
      .block = { "applied when the slider is released" }, .fold = kNrAdvanced, .guard = "if (spatialOn)\n{" },
    { .home = kNr, .file = "nr/menu/DlssNr_Menu.cpp", .widget = "DeferredSlider(\"Work shift (vertical)\"",
      .write = "&nr.spatialShiftY,shiftLimitsY.first,shiftLimitsY.second,0.0f", .keyPrefix = "&nr.spatialShiftY",
      .block = { "applied when the slider is released" }, .fold = kNrAdvanced, .guard = "if (spatialOn)\n{" },

    // WP4 item 5: the four diagnostic/tuning keys of the Character preview, inside DrawDebugSection (= the closed "Advanced (debug)"
    // fold: chara_menu_text pins that DrawDebugSection is called only from that fold). All four are read live.
    { .home = kChara, .file = "ffxiv/chara/CharaMenu.cpp", .widget = "ImGui::SliderInt(\"Keep preview NR models after closing (seconds)\"",
      .write = "cfg.previewNrLingerSeconds=static_cast<uint32_t>(lingerSeconds)", .keyPrefix = "cfg.previewNrLingerSeconds=",
      .block = { "applies at once", "0 releases it", "ImGuiSliderFlags_AlwaysClamp" } },
    { .home = kChara, .file = "ffxiv/chara/CharaMenu.cpp", .widget = "ImGui::Checkbox(\"Preview NR: show the previous frame's result (bring-up)\"",
      .write = "cfg.previewNrLate=late", .keyPrefix = "cfg.previewNrLate=", .block = { "applies at once", "one frame of lag" } },
    { .home = kChara, .file = "ffxiv/chara/CharaMenu.cpp", .widget = "ImGui::Checkbox(\"Write preview DLSS guide dumps (diagnostics)\"",
      .write = "cfg.previewGuideCapture=guideCapture", .keyPrefix = "cfg.previewGuideCapture=",
      .block = { "applies at once", "Leave off unless you are investigating" } },
    { .home = kChara, .file = "ffxiv/chara/CharaMenu.cpp", .widget = "ImGui::Checkbox(\"Trace preview NR in the log (diagnostics)\"",
      .write = "cfg.previewNrTrace=trace", .keyPrefix = "cfg.previewNrTrace=", .block = { "applies at once", "changes nothing" } },

    // WP4 item 3: the RTX 20/30 runtime tuning keys, in a collapsed fold inside the runtime fold, drawn only while the runtime path is
    // switched on; they are written to the runtime's own settings file when it is loaded = restart
    { .home = kAmpere, .file = "fg/MenuFgUnlocks.cpp", .widget = "ImGui::Combo(\"Kernel image (restart)\"",
      .write = "cfg.ampereMfgKernelImage=std::string(kKernelImages[kernelImage])", .keyPrefix = "cfg.ampereMfgKernelImage=",
      .block = { "applies after a restart", "PTX or Cubin force it" },
      .fileHas = { "if (enabled)\nDrawAmpereAdvanced(cfg);", "dlssg_sm86.ini" }, .fold = kAmpereAdvanced },
    { .home = kAmpere, .file = "fg/MenuFgUnlocks.cpp", .widget = "ImGui::Combo(\"Kernel family (restart)\"",
      .write = "cfg.ampereMfgRouter=std::string(kRouters[router])", .keyPrefix = "cfg.ampereMfgRouter=",
      .block = { "applies after a restart", "no SM75 kernels" }, .fold = kAmpereAdvanced },
    { .home = kAmpere, .file = "fg/MenuFgUnlocks.cpp", .widget = "ImGui::Checkbox(\"Hardware bilinear sampling (restart)\"",
      .write = "cfg.ampereMfgHardwareBilinear=hardwareBilinear", .keyPrefix = "cfg.ampereMfgHardwareBilinear=",
      .block = { "applies after a restart", "Off by default" }, .fold = kAmpereAdvanced },
    { .home = kAmpere, .file = "fg/MenuFgUnlocks.cpp", .widget = "ImGui::SliderInt(\"Optimization level (restart)\"",
      .write = "cfg.ampereMfgOptimized=optimized", .keyPrefix = "cfg.ampereMfgOptimized=",
      .block = { "applies after a restart", "0 to 3", "ImGuiSliderFlags_AlwaysClamp" }, .fold = kAmpereAdvanced },
    { .home = kAmpere, .file = "fg/MenuFgUnlocks.cpp", .widget = "ImGui::Combo(\"Preset (restart)\"",
      .write = "cfg.ampereMfgPreset=std::string(kPresets[preset])", .keyPrefix = "cfg.ampereMfgPreset=",
      .block = { "applies after a restart", "A or B" }, .fold = kAmpereAdvanced },
    { .home = kAmpere, .file = "fg/MenuFgUnlocks.cpp", .widget = "ImGui::Combo(\"Report GPU architecture to the game (restart)\"",
      .write = "cfg.ampereMfgSpoofArchToGame=std::string(kSpoofArch[spoofArch])", .keyPrefix = "cfg.ampereMfgSpoofArchToGame=",
      .block = { "applies after a restart", "Auto writes nothing" }, .fold = kAmpereAdvanced },
    { .home = kAmpere, .file = "fg/MenuFgUnlocks.cpp", .widget = "ImGui::SliderInt(\"Runtime log level (restart)\"",
      .write = "cfg.ampereMfgLogLevel=logLevel", .keyPrefix = "cfg.ampereMfgLogLevel=",
      .block = { "applies after a restart", "diagnostic", "ImGuiSliderFlags_AlwaysClamp" }, .fold = kAmpereAdvanced },
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

// [begin, end) of `needle` (squashed text) in `text`, ignoring the blanks of `text`; begin == npos when it is absent.
// `startAt` is an index into `text`.
size_t FindLoose(const std::string& text, const std::string& needle, size_t startAt, size_t& end)
{
    if (needle.empty())
        return std::string::npos;
    for (size_t i = startAt; i < text.size(); ++i)
    {
        if (text[i] != needle[0])
            continue;
        size_t j = i, k = 0;
        while (j < text.size() && k < needle.size())
        {
            if (std::isspace(static_cast<unsigned char>(text[j])))
            {
                ++j;
                continue;
            }
            if (text[j] != needle[k])
                break;
            ++j;
            ++k;
        }
        if (k == needle.size())
        {
            end = j;
            return i;
        }
    }
    return std::string::npos;
}

// `text` with the first blank-insensitive match of `from` (squashed) at or after `startAt` replaced by `to`
std::string ReplaceLoose(std::string text, const std::string& from, const std::string& to, size_t startAt = 0)
{
    size_t end = 0;
    const size_t at = FindLoose(text, from, startAt, end);
    if (at != std::string::npos)
        text.replace(at, end - at, to);
    return text;
}

// `text` with `name(` renamed to `name` + "Zz(" at its definitions (wantDefinition) or at its calls (otherwise); a
// definition is an occurrence right behind a type (an identifier character, '&' or '*')
std::string RenameOccurrences(std::string text, const std::string& name, bool wantDefinition)
{
    const std::string needle = name + "(";
    for (size_t p = text.find(needle); p != std::string::npos; p = text.find(needle, p + needle.size() + 2))
    {
        if (p > 0 && menuscan::IsIdentChar(text[p - 1]))
            continue; // a longer name that ends in this one
        size_t b = p;
        while (b > 0 && std::isspace(static_cast<unsigned char>(text[b - 1])))
            --b;
        const bool definition = b > 0 && (menuscan::IsIdentChar(text[b - 1]) || text[b - 1] == '&' || text[b - 1] == '*');
        if (definition == wantDefinition)
            text.insert(p + name.size(), "Zz");
    }
    return text;
}

// `stripped` is the comment-stripped text of the file (blanks kept), `useFile` the squashed text of the file that makes the
// entry live. The widget block, fold, gate and marker checks look at the squashed body of the row's function; the key-write
// count and the file phrases look at the whole file.
std::vector<std::string> Problems(const Row& r, const std::string& stripped, const std::string& useFile)
{
    std::vector<std::string> p;
    auto bad = [&](const std::string& s) { p.push_back(std::string(r.file) + ": " + s); };
    const std::string widget = Squash(r.widget);
    const std::string file = Squash(stripped);

    if (Count(file, widget) != 1)
    {
        bad(std::string("the widget does not occur exactly once: ") + r.widget);
        return p;
    }

    std::string code = file;
    if (r.home.func[0] != '\0')
    {
        const std::string body = menuscan::FunctionBody(stripped, r.home.func);
        if (body.empty())
        {
            bad(std::string("the function is not defined exactly once: ") + r.home.func);
            return p;
        }
        if (!menuscan::Reaches(stripped, r.home.entry, r.home.func))
            bad(std::string("the function is not reachable from its entry (dead code): ") + r.home.func + " <- " + r.home.entry);
        if (useFile.find(Squash(r.home.useText)) == std::string::npos)
            bad(std::string("the entry is not registered or called: ") + r.home.useText);
        code = Squash(body);
        if (Count(code, widget) != 1)
        {
            bad(std::string("the widget is not inside the body of ") + r.home.func + ": " + r.widget);
            return p;
        }
    }

    const std::string block = BlockOf(code, widget);
    if (block.find(Squash(r.write)) == std::string::npos)
        bad(std::string("the key is not written from its own widget: ") + r.write);
    if (r.keyPrefix[0] != '\0' && Count(file, Squash(r.keyPrefix)) != static_cast<size_t>(r.writes))
        bad(std::string("the key is not written exactly the expected number of times: ") + r.keyPrefix);
    for (const char* s : r.block)
        if (block.find(Squash(s)) == std::string::npos)
            bad(std::string("the widget block lacks: ") + s);
    for (const char* s : r.blockNot)
        if (block.find(Squash(s)) != std::string::npos)
            bad(std::string("the widget block must not say: ") + s);
    for (const char* s : r.fileHas)
        if (file.find(Squash(s)) == std::string::npos)
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
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";

    for (const Row& r : kRows)
    {
        std::string raw = menuscan::ReadFile(repo + "/OptiScaler/ext/" + r.file);
        raw.erase(std::remove(raw.begin(), raw.end(), '\r'), raw.end()); // the working copy may have CRLF line ends
        CHECK(!raw.empty());
        const std::string S = menuscan::StripComments(raw);
        CHECK(r.home.func[0] != '\0' && r.home.entry[0] != '\0' && r.home.useFile[0] != '\0' && r.home.useText[0] != '\0');

        std::string useRaw = menuscan::ReadFile(repo + "/" + r.home.useFile);
        useRaw.erase(std::remove(useRaw.begin(), useRaw.end(), '\r'), useRaw.end());
        const std::string use = Squash(menuscan::StripComments(useRaw));
        CHECK(!use.empty());

        const auto problems = Problems(r, S, use);
        for (const auto& s : problems)
            std::printf("  %s\n", s.c_str());
        CHECK(problems.empty());

        // negative controls: every pin complains about its own breakage
        const std::string widget = Squash(r.widget);
        const std::string write = Squash(r.write);
        CHECK(!Problems(r, ReplaceLoose(S, write, "x=0"), use).empty());                                  // the write is gone
        CHECK(!Problems(r, ReplaceLoose(ReplaceLoose(S, write, ""), widget, write + ";" + widget), use).empty()); // the write left its widget
        CHECK(!Problems(r, ReplaceLoose(S, widget, "ImGui::Combo(\"zz\""), use).empty());                 // the widget is gone
        CHECK(!Problems(r, S + "\n" + widget, use).empty());                                              // the widget is doubled
        size_t widgetEnd = 0;
        const size_t widgetAt = FindLoose(S, widget, 0, widgetEnd); // a phrase may also occur earlier in the file: cut the one in the block
        for (const char* s : r.block)
            CHECK(!Problems(r, ReplaceLoose(S, Squash(s), "", widgetAt), use).empty());
        for (const char* s : r.blockNot)
            CHECK(!Problems(r, ReplaceLoose(S, write, write + Squash(s)), use).empty());
        for (const char* s : r.fileHas)
            CHECK(!Problems(r, ReplaceLoose(S, Squash(s), ""), use).empty());
        if (r.keyPrefix[0] != '\0')
            CHECK(!Problems(r, S + "\n" + Squash(r.keyPrefix) + "1;", use).empty()); // a second write of the key
        if (r.fold[0] != '\0')
        {
            const std::string node = Squash(r.fold);
            CHECK(!Problems(r, ReplaceLoose(S, node, node + "ImGui::TreePop();"), use).empty()); // the fold closes before the widget
            CHECK(!Problems(r, ReplaceLoose(S, node, "if(true){"), use).empty());                  // no fold at all
        }
        if (r.guard[0] != '\0')
        {
            const std::string gate = Squash(r.guard);
            CHECK(!Problems(r, ReplaceLoose(S, gate, "if(true){"), use).empty());   // no gate at all
            CHECK(!Problems(r, ReplaceLoose(S, gate, gate + "}{"), use).empty());    // the gate closes before the widget
        }
        if (r.before[0] != '\0')
        {
            const std::string marker = Squash(r.before);
            CHECK(!Problems(r, ReplaceLoose(ReplaceLoose(S, widget, "x"), marker, marker + widget), use).empty()); // behind the marker
        }

        // dead code and scope: the widget and its write moved into a function nothing calls (the old whole-file pins passed
        // this), the function renamed away, its calls renamed (nothing reaches it), the entry not registered
        {
            std::string moved = ReplaceLoose(S, widget, "ImGui::Text(\"zz\"");
            moved = ReplaceLoose(moved, write, "x=0");
            moved += "\nvoid ZzDead()\n{\n" + widget + ";" + write + ";\n}\n";
            CHECK(!Problems(r, moved, use).empty());
        }
        CHECK(!Problems(r, RenameOccurrences(S, r.home.func, true), use).empty()); // no such function any more
        if (std::string(r.home.func) != r.home.entry)
            CHECK(!Problems(r, RenameOccurrences(S, r.home.func, false), use).empty()); // defined, but nobody calls it
        CHECK(!Problems(r, S, ReplaceLoose(use, Squash(r.home.useText), "")).empty()); // the card is not registered / the seam not called
    }

    if (failures)
    {
        std::printf("controls_text: %d failure(s)\n", failures);
        return 2;
    }
    std::printf("controls_text: all passed\n");
    return 0;
}
