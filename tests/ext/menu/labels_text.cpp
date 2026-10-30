// Renamed menu labels of the fork's cards, pinned as source text (the menu is ImGui glue and cannot run offline). Each row
// names a source file under OptiScaler/ext, strings that must be present anywhere in the file, strings that must be gone, and
// "scoped" strings (the widgets and titles themselves) that must sit inside the body of a named function that the card's
// entry reaches. The file text has its comments and `#if 0` blocks stripped (string literals kept). The checks run against the
// real file and against broken copies of it (a wanted string removed, an unwanted string added, a widget moved into a function
// nothing calls, the call to its function renamed), so every row is mutation-checked by construction.
//
// Why: the Custom Tab picker and the card title use these names, the user-facing docs quote them, and an ImGui `##` suffix
// keeps a widget's id stable while its visible label changes. A label that sits in dead code (another function, `#if 0`) is not
// on screen, so a whole-file substring is not enough for a widget.
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

// A string that must sit inside the body of `func`, which the card's `entry` function reaches in the same file
struct Scoped
{
    const char* func;
    const char* entry;
    std::string text;
};

struct Row
{
    const char* file;
    std::vector<std::string> has;    // anywhere in the file (registrations, tooltips of helpers, headers)
    std::vector<std::string> hasNot; // nowhere in the file
    std::vector<Scoped> scoped;      // inside the named function's body
};

const Row kRows[] = {
    // Forced quality level (was "Quality Preset"); id forced_quality never changes
    { "upscaler/menu/UpscalerMenu.cpp",
      { "{ \"forced_quality\", \"Upscaling\", \"Forced quality level\", Draw," },
      { "\"Quality Preset\"" },
      { { "Draw", "Draw", "SectionTitle(\"Forced quality level\")" } } },
    // Frame generation unlocks: titled, and the RTX 20/30 runtime says "frame-generation runtime" ("Companion" means only
    // the RTX 20/30 frame-generation runtime). The Ampere tuning keys are not part of this step.
    { "fg/MenuFgUnlocks.cpp",
      { "{ \"fg_unlocks\", \"Frame Gen\", \"Frame generation unlocks\", DrawFgUnlocks," },
      { "SM86/SM75", "ompanion" },
      { { "DrawFgUnlocks", "DrawFgUnlocks", "SectionTitle(\"Frame generation unlocks\")" },
        { "DrawAmpereRuntime", "DrawFgUnlocks", "TreeNode(\"RTX 20/30 frame-generation runtime (experimental)\")" },
        { "DrawAmpereRuntime", "DrawFgUnlocks", "Checkbox(\"Load RTX 20/30 frame-generation runtime (restart)\"" },
        { "DrawAmpereRuntime", "DrawFgUnlocks", "SliderInt(\"RTX 20/30 max generated frames (restart)\"" },
        { "DrawAmpereRuntime", "DrawFgUnlocks", "Combo(\"Runtime control\"" },
        { "DrawAmpereRuntime", "DrawFgUnlocks", "SliderInt(\"Runtime factor\"" },
        { "DrawAmpereRuntime", "DrawFgUnlocks", "SliderInt(\"Runtime target FPS\"" },
        { "DrawAmpereRuntime", "DrawFgUnlocks", "Button(\"Apply runtime controls\")" } } },
    // Automatic multi frame generation: titled (title == picker name)
    // and its slider says which "Target FPS" it is (upstream's two DMFG sliders keep their names); the `##AutoMfg` suffix stays
    { "fg/MenuAutoMfg.cpp",
      { "{ \"fg_auto_mfg\", \"Frame Gen\", \"Automatic multi frame generation\", DrawAutoMfg," },
      { "\"Target FPS##AutoMfg\"" },
      { { "DrawAutoMfg", "DrawAutoMfg", "SectionTitle(\"Automatic multi frame generation\")" },
        { "DrawAutoMfg", "DrawAutoMfg", "SliderFloat(\"Target FPS (automatic multiplier)##AutoMfg\"" } } },
    // XeFG dynamic extras: the same naming, hidden id `##XeDynamic` unchanged (XeFgExtras is called by upstream's XeFG card,
    // controls_text pins that call)
    { "fg/xefg/XefgSeams.cpp",
      {},
      { "\"Target FPS##XeDynamic\"" },
      { { "XeFgExtras", "XeFgExtras", "SliderFloat(\"Target FPS (XeFG dynamic)##XeDynamic\"" } } },
    // Automatic Mipmap Bias: the Mipmap Bias and Anisotropic Filtering cards are drawn together, so "the card above" would
    // point at Anisotropic Filtering: the texts name the Mipmap Bias card
    { "upscaler/menu/MipBiasMenu.cpp",
      {},
      { "calculator above gives", "\\\"MB Scale Override\\\" in the card above" },
      { { "Draw", "Draw", "calculator in the Mipmap Bias card above gives" },
        { "Draw", "Draw", "\\\"MB Override All Textures\\\" in the Mipmap Bias card above" },
        { "Draw", "Draw", "\\\"MB Scale Override\\\" in the Mipmap Bias card above is noticed" },
        { "Draw", "Draw", "SectionTitle(\"Automatic Mipmap Bias\")" } } },
    { "upscaler/MipBias.h",
      { "\\\"Reset\\\" in the Mipmap Bias card above are overwritten" },
      { "\\\"Reset\\\" in the card above" },
      {} },
    // HUD-less: the debug tint says which feature it belongs to
    { "hudless/HudlessMenu.cpp",
      {},
      { "Checkbox(\"Show detected UI\"" },
      { { "DrawHudless", "DrawHudless", "Checkbox(\"Show detected UI (HUD-less)\", &tint)" },
        { "DrawHudless", "DrawHudless", "SectionTitle(\"HUD-less (UI-free frame generation)\")" } } },
};

std::vector<std::string> Problems(const Row& r, const std::string& text)
{
    std::vector<std::string> p;
    for (const auto& s : r.has)
        if (text.find(s) == std::string::npos)
            p.push_back(std::string(r.file) + ": missing " + s);
    for (const auto& s : r.hasNot)
        if (text.find(s) != std::string::npos)
            p.push_back(std::string(r.file) + ": still contains " + s);
    for (const Scoped& sc : r.scoped)
    {
        const std::string body = menuscan::FunctionBody(text, sc.func);
        if (body.empty())
            p.push_back(std::string(r.file) + ": function not defined exactly once: " + sc.func);
        else if (body.find(sc.text) == std::string::npos)
            p.push_back(std::string(r.file) + ": missing in " + sc.func + ": " + sc.text);
        if (!menuscan::Reaches(text, sc.entry, sc.func))
            p.push_back(std::string(r.file) + ": " + sc.func + " is not reachable from " + sc.entry);
    }
    return p;
}

std::string Replace(std::string s, const std::string& from, const std::string& to)
{
    const size_t at = s.find(from);
    if (at != std::string::npos)
        s.replace(at, from.size(), to);
    return s;
}

// `text` with the lines that hold the first `needle` wrapped in `#if 0` ... `#endif` (the stripper must drop them)
std::string WrapLineInIfZero(std::string text, const std::string& needle)
{
    const size_t at = text.find(needle);
    if (at == std::string::npos)
        return text;
    const size_t lineStart = text.rfind('\n', at) == std::string::npos ? 0 : text.rfind('\n', at) + 1;
    size_t lineEnd = text.find('\n', at + needle.size());
    if (lineEnd == std::string::npos)
        lineEnd = text.size();
    text.insert(lineEnd, "\n#endif");
    text.insert(lineStart, "#if 0\n");
    return text;
}

// `text` with `name(` renamed to `name` + "Zz(" at its calls (an occurrence behind a type is the definition and stays)
std::string RenameCalls(std::string text, const std::string& name)
{
    const std::string needle = name + "(";
    for (size_t p = text.find(needle); p != std::string::npos; p = text.find(needle, p + needle.size() + 2))
    {
        if (p > 0 && menuscan::IsIdentChar(text[p - 1]))
            continue;
        size_t b = p;
        while (b > 0 && std::isspace(static_cast<unsigned char>(text[b - 1])))
            --b;
        const bool definition = b > 0 && (menuscan::IsIdentChar(text[b - 1]) || text[b - 1] == '&' || text[b - 1] == '*');
        if (!definition)
            text.insert(p + name.size(), "Zz");
    }
    return text;
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";

    for (const Row& r : kRows)
    {
        std::string raw = menuscan::ReadFile(repo + "/OptiScaler/ext/" + r.file);
        raw.erase(std::remove(raw.begin(), raw.end(), '\r'), raw.end());
        CHECK(!raw.empty());
        const std::string text = menuscan::StripComments(raw);

        const auto problems = Problems(r, text);
        for (const auto& s : problems)
            std::printf("  %s\n", s.c_str());
        CHECK(problems.empty());

        // the same pins against a broken copy: every wanted string removed, every unwanted string added, must complain
        for (const auto& s : r.has)
            CHECK(!Problems(r, Replace(text, s, "")).empty());
        for (const auto& s : r.hasNot)
            CHECK(!Problems(r, text + "\n" + s + "\n").empty());
        for (const Scoped& sc : r.scoped)
        {
            CHECK(!Problems(r, Replace(text, sc.text, "")).empty()); // the widget or title is gone
            // moved into a function nothing calls: it is still in the file, but not on screen
            CHECK(!Problems(r, Replace(text, sc.text, "") + "\nvoid ZzDead()\n{\n" + sc.text + "\n}\n").empty());
            // the same text inside an `#if 0` block of its own function
            CHECK(!Problems(r, menuscan::StripIfZero(WrapLineInIfZero(text, sc.text))).empty());
            if (std::string(sc.func) != sc.entry)
                CHECK(!Problems(r, RenameCalls(text, sc.func)).empty()); // defined, but nobody calls it any more
        }
    }

    if (failures)
    {
        std::printf("labels_text: %d failure(s)\n", failures);
        return 2;
    }
    std::printf("labels_text: all passed\n");
    return 0;
}
