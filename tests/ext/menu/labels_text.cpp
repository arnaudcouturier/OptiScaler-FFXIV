// Renamed menu labels of the fork's cards, pinned as source text (the menu is ImGui glue and cannot run offline). Each row
// names a source file under OptiScaler/ext, strings that must be present and strings that must be gone, in the file with its
// comments stripped (string literals kept). The checks run against the real file and against a broken copy of it (a wanted
// string removed, an unwanted string added), so every row is mutation-checked by construction.
//
// Why: the Custom Tab picker and the card title use these names, the user-facing docs quote them, and an ImGui `##` suffix
// keeps a widget's id stable while its visible label changes.
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

struct Row
{
    const char* file;
    std::vector<std::string> has;
    std::vector<std::string> hasNot;
};

const Row kRows[] = {
    // Forced quality level (was "Quality Preset"); id forced_quality never changes
    { "upscaler/menu/UpscalerMenu.cpp",
      { "SectionTitle(\"Forced quality level\")", "{ \"forced_quality\", \"Upscaling\", \"Forced quality level\", Draw," },
      { "\"Quality Preset\"" } },
    // Frame generation unlocks: titled, and the RTX 20/30 runtime says "frame-generation runtime" ("Companion" means only
    // the FFXIV nameplates). The Ampere tuning keys are not part of this step.
    { "fg/MenuFgUnlocks.cpp",
      { "SectionTitle(\"Frame generation unlocks\")", "TreeNode(\"RTX 20/30 frame-generation runtime (experimental)\")",
        "Checkbox(\"Load RTX 20/30 frame-generation runtime (restart)\"", "SliderInt(\"RTX 20/30 max generated frames (restart)\"",
        "Combo(\"Runtime control\"", "SliderInt(\"Runtime factor\"", "SliderInt(\"Runtime target FPS\"", "Button(\"Apply runtime controls\")",
        "{ \"fg_unlocks\", \"Frame Gen\", \"Frame generation unlocks\", DrawFgUnlocks," },
      { "SM86/SM75", "ompanion" } },
    // Automatic multi frame generation: titled (title == picker name)
    { "fg/MenuAutoMfg.cpp",
      { "SectionTitle(\"Automatic multi frame generation\")", "{ \"fg_auto_mfg\", \"Frame Gen\", \"Automatic multi frame generation\", DrawAutoMfg," },
      {} },
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
    }

    if (failures)
    {
        std::printf("labels_text: %d failure(s)\n", failures);
        return 2;
    }
    std::printf("labels_text: all passed\n");
    return 0;
}
