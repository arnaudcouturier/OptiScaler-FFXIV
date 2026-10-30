// The key rule of the menu manifest (WP4 item 7): every config key the fork binds (`b.Add("Section", "Key", member)` literals in
// the comment-stripped sources under OptiScaler/ext) is exposed by a control, or is listed in MenuManifest.h as ini-only.
//
// How "exposed" is decided, and its limit: the member the key is bound to (`.member` or `->member`) is named in a source file
// that draws menu widgets (a file under ext that contains `ImGui::` or `ext::menu::`; for keys bound to upstream's Config
// members, upstream's menu_common.cpp). That is the same evidence the menu inventory of the redesign used. It proves the
// control exists in a file of the menu, not that the widget is correct: the per-widget pins of the WP4 controls live in
// controls_text, and the card files in their own text-pin tests. A read-only mention would also count as evidence.
// A second limit: a generic member name (`enabled`, `strength`...) can also be satisfied by an unrelated struct field that a menu
// file happens to name, not only by another config member. The keys of the WP4 brief all have distinctive names and are pinned
// one by one in controls_text, so the rule still covers them; for the generic names it is a safety net, not a proof.
// Member names shared by several configs ("mode" is bound by [HDR], [HudLess] and [Window]) cannot be told apart by name:
// they carry an explicit proof in MenuManifest.h kKeyEvidence (a menu file and a text that must be in it).
// The generated NR Pass4..Pass30 keys are built by a loop, not by a literal b.Add, so they are outside the rule.
//
// Every rule also runs against deliberately broken models, so it is mutation-checked by construction: among them the control of
// EVERY bound key (not one example) is removed in turn, and the rule has to name exactly that key.
#include "ext/core/MenuManifest.h"
#include "menu_scan.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
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
namespace manifest = ext::menu::manifest;

struct Bound
{
    std::string section, key, member, file;
    bool upstream = false; // bound to a member of upstream's Config (Config::Instance()->X): upstream's menu draws it
    std::string Name() const { return "[" + section + "] " + key; }
};

bool IsIdent(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// `.member` or `->member` as a whole identifier
bool Names(const std::string& text, const std::string& member)
{
    if (member.empty())
        return false;
    for (size_t at = text.find(member); at != std::string::npos; at = text.find(member, at + 1))
    {
        const bool after = at + member.size() >= text.size() || !IsIdent(text[at + member.size()]);
        const bool dot = at >= 1 && text[at - 1] == '.';
        const bool arrow = at >= 2 && text[at - 1] == '>' && text[at - 2] == '-';
        if (after && (dot || arrow))
            return true;
    }
    return false;
}


// `text` with every `.member` / `->member` (whole identifier, the form Names() looks for) renamed to `.xx`
std::string RenameMember(std::string text, const std::string& member)
{
    for (size_t at = text.find(member); at != std::string::npos; at = text.find(member, at + 1))
    {
        const bool after = at + member.size() >= text.size() || !IsIdent(text[at + member.size()]);
        const bool dot = at >= 1 && text[at - 1] == '.';
        const bool arrow = at >= 2 && text[at - 1] == '>' && text[at - 2] == '-';
        if (after && (dot || arrow))
            text.replace(at, member.size(), "xx");
    }
    return text;
}

std::string Strip(const std::string& s) { return menuscan::StripComments(s); }

// One source file of the scan: path relative to ext/ and its comment-stripped text
struct Source
{
    std::string rel, text;
};

std::vector<Source> LoadSources(const std::string& extDir)
{
    std::vector<Source> all;
    for (const auto& entry : fs::recursive_directory_iterator(extDir))
    {
        if (!entry.is_regular_file())
            continue;
        const std::string ext = entry.path().extension().string();
        if (ext != ".cpp" && ext != ".h" && ext != ".inl")
            continue;
        all.push_back({ fs::relative(entry.path(), extDir).generic_string(), Strip(menuscan::ReadFile(entry.path().string())) });
    }
    return all;
}

std::vector<Bound> ScanBound(const std::vector<Source>& sources)
{
    std::vector<Bound> found;
    for (const Source& src : sources)
    {
        for (const char* call : { "b.Add(\"", "b.AddEnum(\"" })
        {
            const std::string& text = src.text;
            for (size_t at = text.find(call); at != std::string::npos; at = text.find(call, at + 1))
            {
                const size_t s0 = at + std::string(call).size();
                const size_t s1 = text.find('"', s0);
                const size_t k0 = s1 == std::string::npos ? s1 : text.find('"', s1 + 1);
                const size_t k1 = k0 == std::string::npos ? k0 : text.find('"', k0 + 1);
                if (k1 == std::string::npos)
                    continue;
                // third argument: up to the next top-level ',' or ')'
                size_t p = text.find(',', k1);
                if (p == std::string::npos)
                    continue;
                ++p;
                while (p < text.size() && text[p] == ' ')
                    ++p;
                int depth = 0;
                size_t q = p;
                for (; q < text.size(); ++q)
                {
                    if (text[q] == '(')
                        ++depth;
                    else if (text[q] == ')' && depth-- == 0)
                        break;
                    else if (text[q] == ',' && depth == 0)
                        break;
                }
                std::string arg = text.substr(p, q - p);
                Bound b;
                b.section = text.substr(s0, s1 - s0);
                b.key = text.substr(k0 + 1, k1 - k0 - 1);
                b.file = src.rel;
                b.upstream = arg.rfind("Config::Instance()", 0) == 0;
                const size_t cut = arg.find_last_of(".>");
                b.member = cut == std::string::npos ? arg : arg.substr(cut + 1);
                found.push_back(b);
            }
        }
    }
    return found;
}

struct Evidence
{
    std::string key, file, text;
};

struct World
{
    std::vector<Source> menuFiles;    // files under ext that draw menu widgets
    std::string upstreamMenu;         // upstream's menu_common.cpp (comment-stripped)
    std::vector<std::string> iniOnly; // "[Section] Key"
    std::vector<Evidence> evidence;
    std::vector<Source> allSources;   // for the evidence files
};

std::vector<std::string> Problems(const std::vector<Bound>& bound, const World& w)
{
    std::vector<std::string> p;
    std::set<std::string> boundNames;
    for (const Bound& b : bound)
        boundNames.insert(b.Name());

    for (const Bound& b : bound)
    {
        const std::string name = b.Name();
        if (std::find(w.iniOnly.begin(), w.iniOnly.end(), name) != w.iniOnly.end())
            continue;
        const auto ev = std::find_if(w.evidence.begin(), w.evidence.end(), [&](const Evidence& e) { return e.key == name; });
        if (ev != w.evidence.end())
        {
            bool ok = false;
            for (const Source& s : w.allSources)
                if (s.rel == ev->file && menuscan::Squash(s.text).find(menuscan::Squash(ev->text)) != std::string::npos)
                    ok = true;
            if (!ok)
                p.push_back(name + ": the evidence text is not in " + ev->file);
            continue;
        }
        bool exposed = false;
        if (b.upstream)
            exposed = Names(w.upstreamMenu, b.member);
        else
            for (const Source& s : w.menuFiles)
                if (Names(s.text, b.member))
                {
                    exposed = true;
                    break;
                }
        if (!exposed)
            p.push_back(name + " (" + b.file + ", member " + b.member + "): no control found in the menu files and not on kIniOnlyKeys");
    }
    // stale entries
    for (const std::string& k : w.iniOnly)
        if (!boundNames.count(k))
            p.push_back(k + ": ini-only entry for a key that is not bound");
    for (const Evidence& e : w.evidence)
        if (!boundNames.count(e.key))
            p.push_back(e.key + ": evidence entry for a key that is not bound");
    return p;
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    const std::string extDir = repo + "/OptiScaler/ext";
    const auto sources = LoadSources(extDir);
    const auto bound = ScanBound(sources);

    World world;
    world.allSources = sources;
    for (const Source& s : sources)
        if (s.rel.rfind("core/MenuManifest", 0) != 0 && (s.text.find("ImGui::") != std::string::npos || s.text.find("ext::menu::") != std::string::npos))
            world.menuFiles.push_back(s);
    world.upstreamMenu = Strip(menuscan::ReadFile(repo + "/OptiScaler/menu/menu_common.cpp"));
    for (const char* k : manifest::kIniOnlyKeys)
        world.iniOnly.push_back(k);
    for (const auto& e : manifest::kKeyEvidence)
        world.evidence.push_back({ e.key, e.file, e.text });

    // the scan sees what it should (it would pass vacuously on an empty scan)
    auto has = [&](const char* s, const char* k)
    {
        for (const Bound& b : bound)
            if (b.section == s && b.key == k)
                return true;
        return false;
    };
    CHECK(bound.size() >= 140);
    CHECK(world.menuFiles.size() >= 15 && !world.upstreamMenu.empty());
    CHECK(has("DLSSG", "UIRecomposition") && has("XeFG", "UnlockMFG") && has("XeFG", "MaxInterpolatedFrames"));
    CHECK(has("DLSSG", "AmpereMfgKernelImage") && has("DLSSG", "AmpereMfgLogLevel") && has("DlssNr", "SpatialOffsetX"));
    CHECK(has("DlssNr", "SpatialProfile") && has("DlssNr", "FoveaWidth") && has("DlssNr", "FoveaHeight") && has("DlssNr", "FoveaFalloff"));
    CHECK(has("DlssNr", "FoveaEdgeX") && has("DlssNr", "FoveaEdgeY"));
    CHECK(has("DlssNr", "ScanExposure") && has("DlssNr", "AutoCapture") && has("DlssNr", "VitCapture") && has("HudLess", "ReShadePresets"));
    CHECK(has("DlssNr", "ToggleKey") && has("DlssNr", "ScaleUpKey") && has("DlssNr", "ScaleDownKey") && has("DlssNr", "TransferToggleKey"));
    CHECK(has("FFXIV", "PreviewNRLingerSeconds") && has("FFXIV", "PreviewNRLate") && has("FFXIV", "PreviewGuideCapture") && has("HDR", "PreviewNRTrace"));
    CHECK(has("Hotfix", "RenderSizeFromWidthHeight") && has("XeFG", "ResetOnUpscalerChange"));
    CHECK(has("LowLatency", "Input") && has("HDR", "Mode") && has("HudLess", "Mode") && has("Window", "Mode"));
    for (const Bound& b : bound)
        if (b.section == "LowLatency")
            CHECK(b.upstream);

    const auto problems = Problems(bound, world);
    for (const auto& s : problems)
        std::printf("  %s\n", s.c_str());
    CHECK(problems.empty());

    // ---- negative controls ------------------------------------------------------------------------------------------------
    {   // a key whose control is gone (no menu file names the member any more)
        World broken = world;
        for (Source& s : broken.menuFiles)
        {
            size_t at;
            while ((at = s.text.find("uiRecomposition")) != std::string::npos)
                s.text.replace(at, 15, "xx");
        }
        CHECK(!Problems(bound, broken).empty());
    }
    {   // every key's control, removed one by one: the rule has to name exactly that key (this exercises the rule for each key,
        //  not for one; it does not remove the generic-name limit described in the header)
        size_t mutated = 0;
        for (const Bound& b : bound)
        {
            const std::string name = b.Name();
            if (std::find(world.iniOnly.begin(), world.iniOnly.end(), name) != world.iniOnly.end())
                continue;
            if (std::any_of(world.evidence.begin(), world.evidence.end(), [&](const Evidence& e) { return e.key == name; }))
                continue; // the shared "mode" names carry an evidence entry instead, mutated above
            World broken = world;
            if (b.upstream)
                broken.upstreamMenu = RenameMember(broken.upstreamMenu, b.member);
            else
                for (Source& s : broken.menuFiles)
                    s.text = RenameMember(s.text, b.member);
            bool named = false;
            for (const std::string& problem : Problems(bound, broken))
                named = named || problem.rfind(name + " (", 0) == 0;
            const std::string what = "removing the control of " + name + " (member " + b.member + ") is not reported";
            Check(named, what.c_str(), __LINE__);
            ++mutated;
        }
        CHECK(mutated >= 100);
    }
    {   // an ini-only entry removed: the developer gate has no control and is no longer excused
        World broken = world;
        broken.iniOnly.erase(broken.iniOnly.begin());
        CHECK(!Problems(bound, broken).empty());
    }
    {   // a stale ini-only entry
        World broken = world;
        broken.iniOnly.push_back("[Nothing] Here");
        CHECK(!Problems(bound, broken).empty());
    }
    {   // an evidence entry whose text is not in its file / names an unbound key / is missing for an ambiguous key
        World broken = world;
        broken.evidence[0].text = "this text is nowhere";
        CHECK(!Problems(bound, broken).empty());
        World stale = world;
        stale.evidence.push_back({ "[Nothing] Here", "core/MenuManifest.h", "x" });
        CHECK(!Problems(bound, stale).empty());
    }
    {   // the evidence entries are what keeps the shared name "mode" honest: without them the three Mode keys are excused
        //  by any menu file that names `.mode`, so a removed control would go unnoticed; with a wrong text it is found.
        World broken = world;
        for (auto& e : broken.evidence)
            e.text += " and more";
        CHECK(Problems(bound, broken).size() >= world.evidence.size());
    }
    {   // a new bound key without any control
        auto more = bound;
        Bound b;
        b.section = "Test";
        b.key = "NoControl";
        b.member = "noSuchMemberAnywhere";
        b.file = "test";
        more.push_back(b);
        CHECK(!Problems(more, world).empty());
    }
    {   // the name check: whole identifier after '.' or '->' only; comments are not evidence
        CHECK(Names("a.member = 1;", "member") && Names("p->member;", "member"));
        CHECK(!Names("a.members = 1;", "member") && !Names("member = 1;", "member") && !Names("xmember", "member"));
        CHECK(Names(Strip("// a.member\nb.other;"), "member") == false);
    }
    {   // an upstream-bound key is looked up in upstream's menu, not in the fork's
        auto more = bound;
        Bound b;
        b.section = "Test";
        b.key = "Upstream";
        b.member = "uiRecomposition"; // named in the fork's menu files, but this key claims an upstream member
        b.upstream = true;
        b.file = "test";
        more.push_back(b);
        CHECK(!Problems(more, world).empty());
    }

    if (failures)
    {
        std::printf("menu_key_controls: %d failure(s)\n", failures);
        return 2;
    }
    std::printf("menu_key_controls: all passed\n");
    return 0;
}
