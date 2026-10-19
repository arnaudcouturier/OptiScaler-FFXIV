// Ini hygiene of the menu redesign (WP4 item 6): every config key the fork binds (`b.Add("Section", "Key", ...)` in the comment-
// stripped sources under OptiScaler/ext) has a documented line `Key=auto` in the ini fragment named after its section
// (OptiScaler/ext/ini/<Section>.ini, merged into the shipped OptiScaler.ini by tools/package.ps1: the section is created when
// upstream has none). The keys that have no menu control on purpose (MenuManifest.h kIniOnlyKeys) say so in their comment.
//
// The comment requirement skips the NR fragment, which documents grouped keys with one shared comment.
// The generated NR Pass4..Pass30 keys are built by a loop in NrConfig.cpp, not by a literal b.Add, so they are outside this rule.
// Each rule also runs against deliberately broken models, so it is mutation-checked by construction.
#include "ext/core/MenuManifest.h"
#include "menu_scan.h"

#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
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

namespace fs = std::filesystem;

struct Bound
{
    std::string section, key, file;
};

// key -> the comment block right above its line
using Fragments = std::map<std::string, std::map<std::string, std::string>>;

std::vector<Bound> ScanBound(const std::string& extDir)
{
    std::vector<Bound> found;
    for (const auto& entry : fs::recursive_directory_iterator(extDir))
    {
        if (!entry.is_regular_file())
            continue;
        const std::string ext = entry.path().extension().string();
        if (ext != ".cpp" && ext != ".h")
            continue;
        const std::string text = menuscan::StripComments(menuscan::ReadFile(entry.path().string()));
        for (const char* call : { "b.Add(\"", "b.AddEnum(\"" })
        {
            for (size_t at = text.find(call); at != std::string::npos; at = text.find(call, at + 1))
            {
                const size_t s0 = at + std::string(call).size();
                const size_t s1 = text.find('"', s0);
                const size_t k0 = text.find('"', s1 + 1);
                const size_t k1 = k0 == std::string::npos ? k0 : text.find('"', k0 + 1);
                if (s1 == std::string::npos || k1 == std::string::npos)
                    continue;
                found.push_back({ text.substr(s0, s1 - s0), text.substr(k0 + 1, k1 - k0 - 1), entry.path().filename().string() });
            }
        }
    }
    return found;
}

Fragments LoadFragments(const std::string& iniDir)
{
    Fragments frags;
    for (const auto& entry : fs::directory_iterator(iniDir))
    {
        if (entry.path().extension() != ".ini")
            continue;
        auto& keys = frags[entry.path().stem().string()];
        std::string block;
        std::string all = menuscan::ReadFile(entry.path().string());
        size_t pos = 0;
        while (pos <= all.size())
        {
            size_t end = all.find('\n', pos);
            if (end == std::string::npos)
                end = all.size();
            std::string line = all.substr(pos, end - pos);
            pos = end + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (line.empty())
            {
                block.clear();
                continue;
            }
            if (line[0] == ';')
            {
                block += line + "\n";
                continue;
            }
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            std::string key = line.substr(0, eq);
            while (!key.empty() && key.back() == ' ')
                key.pop_back();
            keys[key] = block;
            block.clear();
        }
    }
    return frags;
}

std::vector<std::string> Problems(const std::vector<Bound>& bound, const Fragments& frags)
{
    std::vector<std::string> p;
    for (const Bound& b : bound)
    {
        const auto f = frags.find(b.section);
        if (f == frags.end())
        {
            p.push_back("[" + b.section + "] " + b.key + " (" + b.file + "): no ini fragment " + b.section + ".ini");
            continue;
        }
        const auto k = f->second.find(b.key);
        if (k == f->second.end())
            p.push_back("[" + b.section + "] " + b.key + " (" + b.file + "): no line in " + b.section + ".ini");
        else if (k->second.empty() && b.section != "DlssNr") // the NR fragment documents grouped keys with one shared comment
            p.push_back("[" + b.section + "] " + b.key + ": the ini line has no comment above it");
    }
    // keys without a menu control on purpose say so
    for (const char* entry : ext::menu::manifest::kIniOnlyKeys)
    {
        const std::string text = entry; // "[Section] Key"
        const size_t close = text.find(']');
        const std::string section = text.substr(1, close - 1), key = text.substr(close + 2);
        const auto f = frags.find(section);
        const auto k = f == frags.end() ? decltype(f->second.end()) {} : f->second.find(key);
        if (f == frags.end() || k == f->second.end())
            p.push_back(text + ": ini-only key without an ini line");
        else if (k->second.find("No menu control") == std::string::npos)
            p.push_back(text + ": the ini comment does not say \"No menu control\"");
    }
    return p;
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    const auto bound = ScanBound(repo + "/OptiScaler/ext");
    const auto frags = LoadFragments(repo + "/OptiScaler/ext/ini");

    // the scan sees the keys it should (it would pass vacuously on an empty scan)
    auto has = [&](const char* s, const char* k)
    {
        for (const Bound& b : bound)
            if (b.section == s && b.key == k)
                return true;
        return false;
    };
    CHECK(bound.size() >= 140);
    CHECK(has("Companion", "HudReplacement") && has("Companion", "HudInterpolation"));
    CHECK(has("HudLess", "Diagnostics") && has("HudLess", "ReShadeProbe") && has("HudLess", "ReShadePresets"));
    CHECK(has("DLSSG", "UIRecomposition") && has("XeFG", "UnlockMFG") && has("DlssNr", "ScanExposure"));
    CHECK(frags.count("Companion") == 1 && frags.count("HudLess") == 1 && frags.count("DlssNr") == 1);

    const auto problems = Problems(bound, frags);
    for (const auto& s : problems)
        std::printf("  %s\n", s.c_str());
    CHECK(problems.empty());

    // the new fragment documents both Companion keys with their default and their requirement
    {
        auto doc = [&](const char* s, const char* k) -> std::string
        {
            const auto f = frags.find(s);
            if (f == frags.end())
                return {};
            const auto d = f->second.find(k);
            return d == f->second.end() ? std::string {} : d->second;
        };
        const auto companion = frags.find("Companion");
        CHECK(companion != frags.end() && companion->second.size() == 2);
        CHECK(doc("Companion", "HudReplacement").find("OptiFG DLSS-G or XeFG") != std::string::npos);
        CHECK(doc("Companion", "HudReplacement").find("Default (auto) is false") != std::string::npos);
        CHECK(doc("Companion", "HudInterpolation").find("Requires HudReplacement") != std::string::npos);
        CHECK(doc("Companion", "HudInterpolation").find("Default (auto) is false") != std::string::npos);
    }

    // negative controls: each rule complains about its own breakage
    {
        Fragments broken = frags;
        broken["Companion"].erase("HudInterpolation");
        CHECK(!Problems(bound, broken).empty()); // a bound key lost its ini line
    }
    {
        Fragments broken = frags;
        broken.erase("Companion");
        CHECK(!Problems(bound, broken).empty()); // a section lost its fragment
    }
    {
        Fragments broken = frags;
        broken["HudLess"]["Diagnostics"] = "; a developer switch\n";
        CHECK(!Problems(bound, broken).empty()); // the ini-only key forgot to say so
    }
    {
        Fragments broken = frags;
        broken["HudLess"]["ReShadeProbe"] = "";
        CHECK(!Problems(bound, broken).empty()); // an ini line without a comment
    }
    {
        Fragments broken = frags;
        broken["HudLess"].erase("ReShadeProbe");
        CHECK(!Problems(bound, broken).empty()); // an ini-only key without a line
    }
    {
        auto more = bound;
        more.push_back({ "Companion", "NoSuchKey", "test" });
        CHECK(!Problems(more, frags).empty()); // a new bound key without a line
    }

    if (failures)
    {
        std::printf("ini_fragments: %d failure(s)\n", failures);
        return 2;
    }
    std::printf("ini_fragments: all passed\n");
    return 0;
}
