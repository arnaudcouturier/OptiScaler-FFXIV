// The menu manifest (ext/core/MenuManifest.h + the layout table in ext/core/MenuLayout.h) against the code it describes:
// upstream's card table and tab list (menu_common.cpp) and the fork card registrations under OptiScaler/ext. The menu
// is ImGui glue and cannot run offline, so the checks read the sources (comments stripped, string literals kept) and
// build the final card order from the same pure code the product uses.
//
// Rules (Check): ids unique; every layout card registered by exactly one source file and the other way round; the
// registered tab is the layout tab and a real tab of the menu; the FFXIV tab row exists once; FFXIV-only cards are
// exactly the cards of the FFXIV tab and their files test the game; every anchor resolves; every card appears once in
// the final order, every tab is one run of it, and none sits between two upstream boxes of one shared function; every
// id that was ever released resolves (live or alias); aliases are sane; ini-only keys are real. Budget: the `// ext:` seam lines of the upstream
// menu files stay at the agreed number.
//
// Each rule is also run against a deliberately broken model and must complain: that is the mutation check of the rule.
#include "ext/core/MenuLayout.h"
#include "ext/core/MenuManifest.h"
#include "menu_scan.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{
using namespace ext::menu;
using layout::Slot;

int failures = 0;
void Check1(bool ok, const char* what, int line)
{
    if (!ok)
    {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(x) Check1((x), #x, __LINE__)

struct Reg
{
    std::string id, tab, file;
    bool gated = false; // the card's own visible predicate tests the game build (ext::game::Current()), see TestsGameBuild
    std::string visible; // name of that predicate ("" or "nullptr" when the registration has none)
    bool titled = false; // a SectionTitle("<picker name>") exists in the card's own directory
    std::string name;    // picker name
};

struct Model
{
    std::vector<menuscan::UpstreamBox> upstream;
    std::vector<std::string> tabLabels;
    std::vector<Reg> regs;
    std::vector<Slot> slots;
    std::vector<std::string> ffxivOnly, persisted, iniOnly, titleExceptions;
    std::vector<std::pair<std::string, std::string>> aliases;
    std::string extText; // squashed ext sources: where the ini keys are defined
};

using Violations = std::vector<std::string>;

bool Has(const Violations& v, const char* code)
{
    return std::any_of(v.begin(), v.end(), [&](const std::string& s) { return s.rfind(code, 0) == 0; });
}

template <class T> bool Contains(const std::vector<T>& v, const T& x)
{
    return std::find(v.begin(), v.end(), x) != v.end();
}

bool IsUpstream(const Model& m, const std::string& id)
{
    return std::any_of(m.upstream.begin(), m.upstream.end(), [&](const auto& b) { return b.id == id; });
}
const Slot* FindSlot(const Model& m, const std::string& id)
{
    for (const Slot& s : m.slots)
        if (id == s.id)
            return &s;
    return nullptr;
}

// The fork card ids of the shipped session-final menu (13 cards), copied here on purpose: APPEND ONLY. The manifest's
// kPersistedForkIds must contain every one of them, so deleting a card and its persisted id together cannot pass unnoticed.
constexpr const char* kShippedForkIds[] = {
    "dlss_nr",     "ffxiv_character_preview", "forced_quality", "ffxiv_jitter", "fg_unlocks",
    "ffxiv_hdr",   "fg_hudless",              "fg_auto_mfg",    "window_mode",  "hudless_diag",
    "mipmap_auto", "ffxiv_companion",         "reshade_addon_probe",
};

// Cards that do not have their SectionTitle yet. Empty when the titles are done: an entry is an error as soon as its card
// has the title. Empty now: every card is titled.
const std::vector<const char*> kTitleExceptions = {};

Violations Check(const Model& m)
{
    Violations v;
    auto add = [&](const char* code, const std::string& what) { v.push_back(std::string(code) + ": " + what); };

    // ids unique, among the layout cards and against the upstream cards
    for (size_t i = 0; i < m.slots.size(); i++)
    {
        for (size_t j = 0; j < i; j++)
            if (std::string(m.slots[i].id) == m.slots[j].id)
                add("unique-id", std::string(m.slots[i].id) + " has two slots");
        if (IsUpstream(m, m.slots[i].id))
            add("unique-id", std::string(m.slots[i].id) + " is also an upstream card id");
    }

    // every layout card is registered by exactly one source file, and every registration has a slot
    for (const Slot& s : m.slots)
    {
        const auto n = std::count_if(m.regs.begin(), m.regs.end(), [&](const Reg& r) { return r.id == s.id; });
        if (n != 1)
            add("registered-once", std::string(s.id) + " is registered " + std::to_string(n) + " times");
    }
    for (const Reg& r : m.regs)
        if (FindSlot(m, r.id) == nullptr)
            add("registered-once", r.id + " (" + r.file + ") has no slot in the layout");

    // the registered tab is the layout tab, and a real tab of the menu
    for (const Reg& r : m.regs)
    {
        const Slot* s = FindSlot(m, r.id);
        if (s != nullptr && r.tab != s->tab)
            add("tab-match", r.id + " is registered for '" + r.tab + "' but the layout says '" + s->tab + "'");
    }
    for (const Slot& s : m.slots)
        if (!Contains<std::string>(m.tabLabels, s.tab))
            add("tab-label", std::string(s.id) + ": '" + s.tab + "' is not a tab of the menu");

    // every card has a title and the picker name equals it: SectionTitle("<picker name>") in the card's own directory
    // (the exception list names the cards that are still being titled; an entry must go as soon as its card has the title)
    for (const Reg& r : m.regs)
    {
        const bool excepted = Contains<std::string>(m.titleExceptions, r.id);
        if (!r.titled && !excepted)
            add("title", r.id + ": no SectionTitle(\"" + r.name + "\") next to its registration (title and picker name must be equal)");
        if (r.titled && excepted)
            add("title", r.id + " has its title now: remove it from the exception list");
    }
    for (const std::string& id : m.titleExceptions)
        if (std::none_of(m.regs.begin(), m.regs.end(), [&](const Reg& r) { return r.id == id; }))
            add("title", id + " is on the title exception list but is not a registered card");

    // the FFXIV tab row exists once
    const auto rows = std::count(m.tabLabels.begin(), m.tabLabels.end(), std::string(manifest::kFfxivTab));
    if (rows != 1)
        add("tab-row", "the FFXIV tab has " + std::to_string(rows) + " rows");

    // FFXIV-only cards: exactly the cards of the FFXIV tab, each file tests the game
    std::set<std::string> only;
    for (const std::string& id : m.ffxivOnly)
        if (!only.insert(id).second)
            add("ffxiv-only", id + " is listed twice");
    for (const std::string& id : only)
    {
        const Slot* s = FindSlot(m, id);
        if (s == nullptr)
            add("ffxiv-only", id + " has no slot");
        else if (std::string(s->tab) != manifest::kFfxivTab)
            add("ffxiv-only", id + " is not in the FFXIV tab");
        for (const Reg& r : m.regs)
            if (r.id == id && !r.gated)
                add("ffxiv-only", id + ": its visible predicate ('" + r.visible + "' in " + r.file +
                                      ") does not test the game build");
    }
    for (const Slot& s : m.slots)
        if (std::string(s.tab) == manifest::kFfxivTab && only.count(s.id) == 0)
            add("ffxiv-only", std::string(s.id) + " is in the FFXIV tab but not listed as FFXIV-only");

    // anchors: an upstream card or an earlier slot
    for (size_t i = 0; i < m.slots.size(); i++)
    {
        if (m.slots[i].after == nullptr)
            continue;
        const std::string a = m.slots[i].after;
        bool ok = IsUpstream(m, a);
        for (size_t j = 0; j < i && !ok; j++)
            ok = a == m.slots[j].id;
        if (!ok)
            add("anchor", std::string(m.slots[i].id) + " follows '" + a + "', which is neither an upstream card nor an earlier slot");
    }

    // the final order (built from the same code as the menu), registered in reverse table order
    std::vector<layout::Item> base, cards;
    for (const auto& b : m.upstream)
        base.push_back({ b.id, b.tab });
    for (size_t i = m.slots.size(); i > 0; i--)
        cards.push_back({ m.slots[i - 1].id, m.slots[i - 1].tab });
    const auto order = layout::MergeOrder(base.data(), base.size(), cards.data(), cards.size(), m.slots.data(), m.slots.size());

    std::vector<std::string> merged;
    std::vector<bool> isFork;
    for (const auto& p : order)
    {
        merged.push_back(std::string(p.fork ? cards[p.index].id : base[p.index].id));
        isFork.push_back(p.fork);
    }
    // each card once, so each fork card is in exactly one tab
    {
        std::set<std::string> seen;
        for (const auto& id : merged)
            if (!seen.insert(id).second)
                add("one-tab", id + " appears twice in the final order");
        if (merged.size() != m.upstream.size() + m.slots.size())
            add("one-tab", "the final order has " + std::to_string(merged.size()) + " cards, expected " +
                               std::to_string(m.upstream.size() + m.slots.size()));
    }
    // every tab is ONE run of the final order (the Custom Tab picker and its pins walk the array and start a heading at
    // each change of tab: a tab that comes back is listed twice)
    {
        std::vector<std::string> closed;
        std::string prev;
        for (size_t i = 0; i < merged.size(); i++)
        {
            std::string tab;
            if (isFork[i])
            {
                if (const Slot* s = FindSlot(m, merged[i]))
                    tab = s->tab;
            }
            else
            {
                for (const auto& b : m.upstream)
                    if (b.id == merged[i])
                        tab = b.tab;
            }
            if (i > 0 && tab == prev)
                continue;
            if (Contains<std::string>(closed, tab))
                add("tab-run", "'" + tab + "' comes back at " + merged[i] + ": its cards are not one run of the final order");
            closed.push_back(tab);
            prev = tab;
        }
    }
    // no fork card between two upstream boxes of one shared function
    std::set<std::string> sources;
    for (const auto& b : m.upstream)
        if (!b.source.empty())
            sources.insert(b.source);
    for (const std::string& src : sources)
    {
        size_t lo = merged.size(), hi = 0;
        for (size_t i = 0; i < merged.size(); i++)
        {
            const auto it = std::find_if(m.upstream.begin(), m.upstream.end(),
                                         [&](const auto& b) { return !isFork[i] && b.id == merged[i] && b.source == src; });
            if (it != m.upstream.end())
            {
                lo = std::min(lo, i);
                hi = std::max(hi, i);
            }
        }
        for (size_t i = lo; i < hi && lo < hi; i++)
            if (isFork[i])
                add("group-split", merged[i] + " sits inside the boxes of " + src);
    }

    // the persisted list only grows: every id of the shipped session-final menu stays in it (removing a card means an alias,
    // never deleting the id from both the layout and the list)
    for (const char* id : kShippedForkIds)
        if (!Contains<std::string>(m.persisted, id))
            add("persisted", std::string(id) + " was shipped but is missing from the persisted id list");
    // every id that was ever released still resolves
    for (const std::string& id : m.persisted)
    {
        const bool live = FindSlot(m, id) != nullptr || IsUpstream(m, id);
        const bool aliased = std::any_of(m.aliases.begin(), m.aliases.end(), [&](const auto& a) { return a.first == id; });
        if (!live && !aliased)
            add("persisted", id + " has no card and no alias");
    }
    // aliases
    for (size_t i = 0; i < m.aliases.size(); i++)
    {
        const auto& a = m.aliases[i];
        if (FindSlot(m, a.first) != nullptr || IsUpstream(m, a.first))
            add("alias", a.first + " is retired but still a live card");
        if (!Contains<std::string>(m.persisted, a.first))
            add("alias", a.first + " is not in the persisted id list, so no released menu had it");
        if (FindSlot(m, a.second) == nullptr && !IsUpstream(m, a.second))
            add("alias", a.first + " points to '" + a.second + "', which is not a card");
        for (size_t j = 0; j < i; j++)
            if (m.aliases[j].first == a.first)
                add("alias", a.first + " is aliased twice");
    }

    // keys that stay ini-only: "[Section] Key", unique, defined in ext
    {
        std::set<std::string> seen;
        for (const std::string& k : m.iniOnly)
        {
            const size_t close = k.find("] ");
            if (k.empty() || k[0] != '[' || close == std::string::npos || close < 2 || close + 2 >= k.size())
            {
                add("ini-only", "'" + k + "' is not of the form [Section] Key");
                continue;
            }
            if (!seen.insert(k).second)
                add("ini-only", k + " is listed twice");
            const std::string section = k.substr(1, close - 1), key = k.substr(close + 2);
            if (m.extText.find("\"" + section + "\",\"" + key + "\"") == std::string::npos)
                add("ini-only", k + " is not defined in ext");
        }
    }
    return v;
}

// `// ext:` markers of the upstream menu files: 14 + 1 at the fork's base, plus the draw-once loop (2 lines) and the
// FFXIV tab row (1 line). More means the menu got a new seam: update the catalog and this number together.
constexpr size_t kExtMarkersMenuCommonCpp = 17;
constexpr size_t kExtMarkersMenuCommonH = 1;

Violations Budget(const std::string& cppRaw, const std::string& hRaw)
{
    Violations v;
    const size_t cpp = menuscan::Count(cppRaw, "// ext:"), h = menuscan::Count(hRaw, "// ext:");
    if (cpp != kExtMarkersMenuCommonCpp)
        v.push_back("seam-budget: menu_common.cpp has " + std::to_string(cpp) + " '// ext:' lines, expected " +
                    std::to_string(kExtMarkersMenuCommonCpp));
    if (h != kExtMarkersMenuCommonH)
        v.push_back("seam-budget: menu_common.h has " + std::to_string(h) + " '// ext:' lines, expected " +
                    std::to_string(kExtMarkersMenuCommonH));
    return v;
}

Model Build(const std::string& repo, std::string& menuCppRaw, std::string& menuHRaw)
{
    namespace fs = std::filesystem;
    Model m;

    menuCppRaw = menuscan::ReadFile(repo + "/OptiScaler/menu/menu_common.cpp");
    menuHRaw = menuscan::ReadFile(repo + "/OptiScaler/menu/menu_common.h");
    const std::string code = menuscan::StripComments(menuCppRaw);
    m.upstream = menuscan::ParseUpstreamBoxes(code);
    for (const auto& t : menuscan::ParseTabs(code))
        m.tabLabels.push_back(t.label);

    size_t tokens = 0;
    for (const auto& r : menuscan::ScanRegistrations(repo, tokens))
    {
        // the title: SectionTitle("<name>") in any .cpp of the registration's directory (the NR card is registered in
        // NrMenu.cpp and drawn by DlssNr_Menu.cpp)
        bool titled = false;
        const fs::path dir = fs::path(repo) / "OptiScaler" / "ext" / fs::path(r.file).parent_path();
        const std::string wanted = "SectionTitle(\"" + r.name + "\")";
        for (const auto& e : fs::directory_iterator(dir))
            if (!r.name.empty() && e.is_regular_file() && e.path().extension() == ".cpp" &&
                menuscan::StripComments(menuscan::ReadFile(e.path().string())).find(wanted) != std::string::npos)
                titled = true;
        m.regs.push_back({ r.id, r.tab, r.file, menuscan::TestsGameBuild(r), r.visible, titled, r.name });
    }
    for (const char* id : kTitleExceptions)
        m.titleExceptions.push_back(id);
    CHECK(tokens == m.regs.size()); // every registration token was understood

    m.slots.assign(std::begin(layout::kSlots), std::end(layout::kSlots));
    for (const char* id : manifest::kFfxivOnlyIds)
        m.ffxivOnly.push_back(id);
    for (const char* id : manifest::kPersistedForkIds)
        m.persisted.push_back(id);
    for (const char* k : manifest::kIniOnlyKeys)
        m.iniOnly.push_back(k);
    for (const auto& a : manifest::kAliases)
        m.aliases.push_back({ a.oldId, a.newId });

    for (const auto& e : fs::recursive_directory_iterator(fs::path(repo) / "OptiScaler" / "ext"))
        if (e.is_regular_file() && e.path().extension() == ".cpp")
            m.extText += menuscan::Squash(menuscan::StripComments(menuscan::ReadFile(e.path().string())));
    return m;
}

void Report(const char* what, const Violations& v)
{
    for (const auto& s : v)
        std::printf("  %s: %s\n", what, s.c_str());
}

// ---- the real menu ----------------------------------------------------------------------------------------------------
void RealMenu(const Model& m, const std::string& cppRaw, const std::string& hRaw)
{
    CHECK(m.upstream.size() >= 40);
    CHECK(m.slots.size() == 15);
    CHECK(m.regs.size() == 15);

    const Violations v = Check(m);
    Report("manifest", v);
    CHECK(v.empty());

    const Violations b = Budget(cppRaw, hRaw);
    Report("budget", b);
    CHECK(b.empty());

    // facts the manifest test relies on, so a broken scanner cannot make the rules vacuous
    CHECK(Contains<std::string>(m.tabLabels, "FFXIV") && Contains<std::string>(m.tabLabels, "Frame Gen"));
    CHECK(m.ffxivOnly.size() == 9);
    CHECK(m.persisted.size() == 15);
    bool anyShared = false;
    for (const auto& b2 : m.upstream)
        anyShared = anyShared || b2.source == "RenderFrameGenerationRuntimeSettings";
    CHECK(anyShared);
}

// ---- the registration scanner: which cards test the game build in THEIR OWN visible predicate -----------------------------
void ScannerUnits()
{
    using menuscan::StripComments;
    auto scan = [](const std::string& text)
    {
        size_t tokens = 0;
        std::vector<menuscan::Registration> found;
        menuscan::ScanRegistrationText(StripComments(text), "x/Card.cpp", tokens, found);
        CHECK(tokens == found.size());
        return found;
    };

    const std::string pred = "bool Vis(const ext::menu::Ctx&) { return ext::game::Current().hdr != nullptr; }\n";

    { // a gated card
        const auto f = scan(pred + "const ext::menu::AutoRegister r { { \"x\", \"FFXIV\", \"X\", Draw, nullptr, Vis } };");
        CHECK(f.size() == 1 && f[0].id == "x" && f[0].tab == "FFXIV" && f[0].visible == "Vis" && f[0].name == "X");
        CHECK(f.size() == 1 && menuscan::TestsGameBuild(f[0]));
    }
    { // the Register(...) form, the argument on the next line
        const auto f = scan(pred + "void Init() { ext::menu::Register({ \"x\", \"FFXIV\", \"X\", Draw, \"fg_selection\",\n   Vis }); }");
        CHECK(f.size() == 1 && f[0].visible == "Vis" && menuscan::TestsGameBuild(f[0]));
    }
    { // visible argument null: the file still mentions game::Current( in a predicate nobody registers
        const auto f = scan(pred + "const ext::menu::AutoRegister r { { \"x\", \"FFXIV\", \"X\", Draw, nullptr, nullptr } };");
        CHECK(f.size() == 1 && f[0].visible == "nullptr" && !menuscan::TestsGameBuild(f[0]));
    }
    { // no visible argument at all
        const auto f = scan(pred + "const ext::menu::AutoRegister r { { \"x\", \"FFXIV\", \"X\", Draw } };");
        CHECK(f.size() == 1 && f[0].visible.empty() && !menuscan::TestsGameBuild(f[0]));
        const auto g = scan(pred + "const ext::menu::AutoRegister r { { \"x\", \"FFXIV\", \"X\", Draw, \"fg_selection\" } };");
        CHECK(g.size() == 1 && g[0].visible.empty() && !menuscan::TestsGameBuild(g[0]));
    }
    { // a predicate that does not look at the game, next to a function that does
        const auto f = scan("bool Vis(const ext::menu::Ctx&) { return true; }\nvoid Other() { ext::game::Current(); }\n"
                            "const ext::menu::AutoRegister r { { \"x\", \"FFXIV\", \"X\", Draw, nullptr, Vis } };");
        CHECK(f.size() == 1 && f[0].visible == "Vis" && !menuscan::TestsGameBuild(f[0]));
    }
    { // the game test only in a comment
        const auto f = scan("bool Vis(const ext::menu::Ctx&) { return true; /* ext::game::Current() */ }\n"
                            "const ext::menu::AutoRegister r { { \"x\", \"FFXIV\", \"X\", Draw, nullptr, Vis } };");
        CHECK(f.size() == 1 && !menuscan::TestsGameBuild(f[0]));
    }
    { // the registration names a predicate that is not defined in the file
        const auto f = scan(pred + "const ext::menu::AutoRegister r { { \"x\", \"FFXIV\", \"X\", Draw, nullptr, Missing } };");
        CHECK(f.size() == 1 && f[0].visible == "Missing" && f[0].visibleBody.empty() && !menuscan::TestsGameBuild(f[0]));
    }
    { // a similar name must not match: the registration points at Vis, the game test is in Vis2
        const auto f = scan("bool Vis2(const ext::menu::Ctx&) { return ext::game::Current().hdr != nullptr; }\n"
                            "bool Vis(const ext::menu::Ctx&) { return false; }\n"
                            "const ext::menu::AutoRegister r { { \"x\", \"FFXIV\", \"X\", Draw, nullptr, Vis } };");
        CHECK(f.size() == 1 && !menuscan::TestsGameBuild(f[0]));
    }
}

// ---- every rule against a broken model ---------------------------------------------------------------------------------
Slot* Find(Model& m, const char* id)
{
    for (Slot& s : m.slots)
        if (std::string(s.id) == id)
            return &s;
    return nullptr;
}

void Broken(const Model& real)
{
    // sanity: the copy starts clean
    CHECK(Check(real).empty());

    { // two slots for one id
        Model m = real;
        m.slots.push_back({ "fg_unlocks", "Frame Gen", nullptr });
        CHECK(Has(Check(m), "unique-id"));
    }
    { // a fork id that is an upstream id
        Model m = real;
        Find(m, "fg_unlocks")->id = "sharpness";
        CHECK(Has(Check(m), "unique-id"));
    }
    { // a layout card nobody registers
        Model m = real;
        m.slots.push_back({ "ghost_card", "Frame Gen", nullptr });
        CHECK(Has(Check(m), "registered-once"));
    }
    { // a card registered twice
        Model m = real;
        m.regs.push_back(m.regs.front());
        CHECK(Has(Check(m), "registered-once"));
    }
    { // a registration without a slot
        Model m = real;
        m.regs.push_back({ "stray_card", "Frame Gen", "x/Stray.cpp", false });
        CHECK(Has(Check(m), "registered-once"));
    }
    { // registered for another tab than the layout says
        Model m = real;
        for (Reg& r : m.regs)
            if (r.id == "mipmap_auto")
                r.tab = "Compatibility";
        CHECK(Has(Check(m), "tab-match"));
    }
    { // a tab that does not exist
        Model m = real;
        Find(m, "mipmap_auto")->tab = "Textuers";
        CHECK(Has(Check(m), "tab-label"));
    }
    { // the FFXIV tab row missing, or twice
        Model m = real;
        m.tabLabels.erase(std::find(m.tabLabels.begin(), m.tabLabels.end(), "FFXIV"));
        CHECK(Has(Check(m), "tab-row"));
        m = real;
        m.tabLabels.push_back("FFXIV");
        CHECK(Has(Check(m), "tab-row"));
    }
    { // an FFXIV-only card outside the FFXIV tab
        Model m = real;
        Find(m, "window_mode")->tab = "Frame Gen";
        for (Reg& r : m.regs)
            if (r.id == "window_mode")
                r.tab = "Frame Gen";
        CHECK(Has(Check(m), "ffxiv-only"));
    }
    { // a card in the FFXIV tab that is not listed as FFXIV-only
        Model m = real;
        Find(m, "mipmap_auto")->tab = "FFXIV";
        for (Reg& r : m.regs)
            if (r.id == "mipmap_auto")
                r.tab = "FFXIV";
        CHECK(Has(Check(m), "ffxiv-only"));
    }
    { // an FFXIV-only card whose file never tests the game
        Model m = real;
        for (Reg& r : m.regs)
            if (r.id == "ffxiv_hdr")
                r.gated = false;
        CHECK(Has(Check(m), "ffxiv-only"));
    }
    { // an FFXIV-only id listed twice or without a slot
        Model m = real;
        m.ffxivOnly.push_back("hudless_diag");
        CHECK(Has(Check(m), "ffxiv-only"));
        m = real;
        m.ffxivOnly.push_back("nothing_here");
        CHECK(Has(Check(m), "ffxiv-only"));
    }
    { // anchors: unknown, and a card that comes later in the table
        Model m = real;
        Find(m, "dlss_nr")->after = "upscalr";
        CHECK(Has(Check(m), "anchor"));
        m = real;
        Find(m, "window_mode")->after = "hudless_diag";
        CHECK(Has(Check(m), "anchor"));
    }
    { // titles: a card whose title differs from the picker name, an exception that is no longer needed, a ghost exception
        Model m = real;
        for (Reg& r : m.regs)
            if (r.id == "ffxiv_hdr")
                r.titled = false;
        CHECK(Has(Check(m), "title"));
        m = real;
        for (Reg& r : m.regs)
            if (r.id == "mipmap_auto")
                m.titleExceptions.push_back(r.id);
        CHECK(Has(Check(m), "title"));
        m = real;
        m.titleExceptions.push_back("no_such_card");
        CHECK(Has(Check(m), "title"));
    }
    { // a tab that comes back: a Textures card anchored on an FFXIV card lands behind the FFXIV block, before Latency & FPS
        Model m = real;
        m.slots.push_back({ "stray_textures", "Textures", "window_mode" });
        CHECK(Has(Check(m), "tab-run"));
        // the same card anchored in its own tab is fine
        m = real;
        m.slots.push_back({ "stray_textures", "Textures", "anisotropy" });
        CHECK(!Has(Check(m), "tab-run"));
    }
    { // a fork card inside a shared group (Frame Gen runtime cards, Textures)
        Model m = real;
        Find(m, "fg_auto_mfg")->after = "fg_dlssg";
        CHECK(Has(Check(m), "group-split"));
        m = real;
        Find(m, "mipmap_auto")->after = "mipmap";
        CHECK(Has(Check(m), "group-split"));
        // before the group is fine: it is the position of the whole group that matters
        m = real;
        Find(m, "fg_unlocks")->after = "fg_selection";
        CHECK(!Has(Check(m), "group-split"));
    }
    { // a shipped id dropped from the persisted list together with its card
        Model m = real;
        m.persisted.erase(std::remove(m.persisted.begin(), m.persisted.end(), std::string("mipmap_auto")), m.persisted.end());
        m.slots.erase(std::remove_if(m.slots.begin(), m.slots.end(), [](const Slot& s) { return std::string(s.id) == "mipmap_auto"; }),
                      m.slots.end());
        m.regs.erase(std::remove_if(m.regs.begin(), m.regs.end(), [](const Reg& r) { return r.id == "mipmap_auto"; }), m.regs.end());
        CHECK(Has(Check(m), "persisted"));
    }
    { // a released id with no card and no alias; an alias fixes it
        Model m = real;
        m.slots.erase(std::remove_if(m.slots.begin(), m.slots.end(), [](const Slot& s) { return std::string(s.id) == "reshade_addon_probe"; }),
                      m.slots.end());
        m.regs.erase(std::remove_if(m.regs.begin(), m.regs.end(), [](const Reg& r) { return r.id == "reshade_addon_probe"; }), m.regs.end());
        CHECK(Has(Check(m), "persisted"));
        m.aliases.push_back({ "reshade_addon_probe", "fg_hudless" });
        CHECK(!Has(Check(m), "persisted"));
        CHECK(!Has(Check(m), "alias"));
    }
    { // aliases: a live old id, a missing target, a duplicate
        Model m = real;
        m.aliases.push_back({ "fg_hudless", "fg_unlocks" });
        CHECK(Has(Check(m), "alias"));
        m = real;
        m.aliases.push_back({ "old_card", "no_such_card" });
        CHECK(Has(Check(m), "alias"));
        m = real;
        m.aliases.push_back({ "old_card", "fg_unlocks" });
        m.aliases.push_back({ "old_card", "fg_unlocks" });
        CHECK(Has(Check(m), "alias"));
    }
    { // an alias for an id no released menu had
        Model m = real;
        m.aliases.push_back({ "never_shipped", "fg_unlocks" });
        CHECK(Has(Check(m), "alias"));
    }
    { // ini-only keys: format, duplicate, not defined
        Model m = real;
        m.iniOnly.push_back("HudLess Diagnostics");
        CHECK(Has(Check(m), "ini-only"));
        m = real;
        m.iniOnly.push_back(m.iniOnly.front());
        CHECK(Has(Check(m), "ini-only"));
        m = real;
        m.iniOnly.push_back("[HudLess] NoSuchKey");
        CHECK(Has(Check(m), "ini-only"));
    }
    { // the seam budget
        std::string cpp, h;
        for (size_t i = 0; i < kExtMarkersMenuCommonCpp; i++)
            cpp += "// ext: a\n";
        for (size_t i = 0; i < kExtMarkersMenuCommonH; i++)
            h += "// ext: b\n";
        CHECK(Budget(cpp, h).empty());
        CHECK(!Budget(cpp + "// ext: one more\n", h).empty());
        CHECK(!Budget(cpp, h + "// ext: one more\n").empty());
    }
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";

    std::string cppRaw, hRaw;
    const Model real = Build(repo, cppRaw, hRaw);
    RealMenu(real, cppRaw, hRaw);
    ScannerUnits();
    Broken(real);

    if (failures)
    {
        std::printf("menu_manifest: %d failure(s)\n", failures);
        return 2;
    }
    std::printf("menu_manifest: all passed\n");
    return 0;
}
