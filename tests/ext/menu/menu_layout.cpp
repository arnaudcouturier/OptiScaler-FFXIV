// The order of the fork cards in the menu (ext/core/MenuLayout.h) and its use in MergeBoxes (ext/core/MenuRegistry.h).
//
//  1. A small synthetic menu: the result is the same for EVERY permutation of the registration order, unlisted cards go to
//     the end of their tab in id order, unknown anchors and empty tabs fall back, unregistered slots are skipped.
//  2. The real table over the real upstream card list (parsed from menu_common.cpp): the same result for many shuffles of
//     the registration order, and the expected order of every tab.
//  3. Text pins on MergeBoxes: it takes the order from MergeOrder and builds each card's box from its REGISTRATION index
//     (a box built from the final position would run another card's draw function).
#include "ext/core/MenuLayout.h"
#include "menu_scan.h"

#include <algorithm>
#include <cstdio>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace
{
using namespace ext::menu::layout;
using menuscan::Count;

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

void CheckEq(const std::string& got, const std::string& want, int line)
{
    if (got != want)
    {
        std::printf("FAIL line %d:\n  got  %s\n  want %s\n", line, got.c_str(), want.c_str());
        ++failures;
    }
}
#define CHECK_EQ(got, want) CheckEq((got), (want), __LINE__)

using Ids = std::vector<std::string>;

struct Menu
{
    std::vector<Item> base;
    std::vector<Item> cards; // in registration order
    std::vector<Slot> slots;
};

Ids Merge(const Menu& m)
{
    const auto order = MergeOrder(m.base.data(), m.base.size(), m.cards.data(), m.cards.size(), m.slots.data(), m.slots.size());
    Ids ids;
    for (const Placed& p : order)
        ids.push_back(std::string(p.fork ? m.cards[p.index].id : m.base[p.index].id));
    return ids;
}

std::string Join(const Ids& ids)
{
    std::string s;
    for (const auto& i : ids)
        s += (s.empty() ? "" : " ") + i;
    return s;
}

// Each tab is ONE run of the merged order (the Custom Tab picker and the pinned cards walk the array in order and start
// a new heading at every change of tab, so a tab that comes back would be listed twice)
bool TabsContiguous(const Ids& tabOfEntry)
{
    std::vector<std::string> closed;
    for (size_t i = 0; i < tabOfEntry.size(); i++)
    {
        if (i > 0 && tabOfEntry[i] == tabOfEntry[i - 1])
            continue;
        if (std::find(closed.begin(), closed.end(), tabOfEntry[i]) != closed.end())
            return false;
        closed.push_back(tabOfEntry[i]);
    }
    return true;
}

// ---- 1. synthetic ---------------------------------------------------------------------------------------------------
void Synthetic()
{
    // the contiguity check itself: it must complain about a tab that comes back
    CHECK(TabsContiguous({ "A", "A", "B", "C", "C" }));
    CHECK(!TabsContiguous({ "A", "B", "A" }));
    CHECK(!TabsContiguous({ "A", "A", "B", "B", "A" }));
    CHECK(TabsContiguous({}));

    Menu m;
    m.base = { { "a1", "T1" }, { "a2", "T1" }, { "a3", "T1" }, { "b1", "T2" }, { "b2", "T2" } };
    m.slots = { { "f1", "T1", "a1" }, { "f2", "T1", "f1" }, { "f3", "T1", "a3" },
                { "f4", "T2", nullptr }, { "f5", "T1", "a2" } };
    const std::vector<Item> registered = { { "f1", "T1" }, { "f2", "T1" }, { "f3", "T1" }, { "f4", "T2" },
                                           { "f5", "T1" }, { "zz", "T1" }, { "aa", "T2" } };

    const std::string expected = "a1 f1 f2 a2 f5 a3 f3 zz b1 b2 f4 aa";

    // every permutation of the registration order gives the same menu
    std::vector<size_t> perm(registered.size());
    std::iota(perm.begin(), perm.end(), size_t { 0 });
    size_t permutations = 0, different = 0;
    do
    {
        m.cards.clear();
        for (size_t i : perm)
            m.cards.push_back(registered[i]);
        ++permutations;
        if (Join(Merge(m)) != expected)
        {
            if (different++ == 0)
                std::printf("first different order: %s\n", Join(Merge(m)).c_str());
        }
    } while (std::next_permutation(perm.begin(), perm.end()));
    CHECK(permutations == 5040);
    CHECK(different == 0);

    // two cards after the same anchor keep their TABLE order, whichever registered first
    {
        Menu s;
        s.base = { { "a", "T" }, { "b", "T" } };
        s.slots = { { "x", "T", "a" }, { "y", "T", "a" } };
        s.cards = { { "y", "T" }, { "x", "T" } };
        CHECK(Join(Merge(s)) == "a x y b");
        s.cards = { { "x", "T" }, { "y", "T" } };
        CHECK(Join(Merge(s)) == "a x y b");
    }

    // unlisted cards: end of their tab, in id order, however they registered
    {
        Menu s;
        s.base = { { "a", "T" }, { "b", "U" } };
        s.slots = {};
        s.cards = { { "q", "T" }, { "c", "T" }, { "m", "T" } };
        CHECK(Join(Merge(s)) == "a c m q b");
        s.cards = { { "m", "T" }, { "q", "T" }, { "c", "T" } };
        CHECK(Join(Merge(s)) == "a c m q b");
    }

    // an anchor that does not exist: end of the card's tab; a tab without any card: end of the menu
    {
        Menu s;
        s.base = { { "a", "T" }, { "b", "U" } };
        s.slots = { { "x", "T", "nope" }, { "y", "V", nullptr }, { "z", "W", nullptr } };
        s.cards = { { "z", "W" }, { "y", "V" }, { "x", "T" } };
        CHECK(Join(Merge(s)) == "a x b y z");
    }

    // an unregistered slot is skipped, the cards after it still find their anchors by id
    {
        Menu s;
        s.base = { { "a", "T" }, { "b", "T" } };
        s.slots = { { "x", "T", "a" }, { "gone", "T", "x" }, { "y", "T", "x" } };
        s.cards = { { "y", "T" }, { "x", "T" } };
        CHECK(Join(Merge(s)) == "a x y b");
    }

    // the cards that follow an anchor form a block (its direct followers and the followers of those, and so on): a later
    // card with the same anchor goes behind the WHOLE block, never into the middle of a chain
    {
        Menu s;
        s.base = { { "S", "T" }, { "Z", "U" } };
        s.slots = { { "W", "V", "S" }, { "H", "V", "W" }, { "A", "T", "S" } };
        s.cards = { { "A", "T" }, { "H", "V" }, { "W", "V" } };
        CHECK(Join(Merge(s)) == "S W H A Z");
        s.cards = { { "W", "V" }, { "A", "T" }, { "H", "V" } };
        CHECK(Join(Merge(s)) == "S W H A Z");
        // a longer chain: W's followers (H, then I behind H) stay right behind W, the sibling B comes after that block
        s.slots = { { "W", "V", "S" }, { "B", "V", "S" }, { "H", "V", "W" }, { "I", "V", "H" }, { "A", "T", "S" } };
        s.cards = { { "A", "T" }, { "I", "V" }, { "H", "V" }, { "B", "V" }, { "W", "V" } };
        CHECK(Join(Merge(s)) == "S W H I B A Z");
    }

    // a card that follows an unregistered card does not find its anchor: it goes to the end of its tab
    {
        Menu s;
        s.base = { { "a", "T" }, { "b", "T" }, { "c", "U" } };
        s.slots = { { "x", "T", "a" }, { "gone", "T", "x" }, { "y", "T", "gone" } };
        s.cards = { { "y", "T" }, { "x", "T" } };
        CHECK(Join(Merge(s)) == "a x b y c");
    }

    // a card registered for another tab than the anchor's follows the anchor (cards may move between tabs)
    {
        Menu s;
        s.base = { { "a", "T" }, { "b", "U" } };
        s.slots = { { "x", "U", "a" } };
        s.cards = { { "x", "U" } };
        CHECK(Join(Merge(s)) == "a x b");
    }

    // no cards: the upstream order is untouched
    {
        Menu s;
        s.base = { { "a", "T" }, { "b", "U" } };
        s.slots = { { "x", "T", "a" } };
        CHECK(Join(Merge(s)) == "a b");
    }
}

// ---- 2. the real table over the real upstream cards -------------------------------------------------------------------
Ids OfTab(const Ids& all, const Ids& tabOf, const std::string& tab)
{
    Ids out;
    for (size_t i = 0; i < all.size(); i++)
        if (tabOf[i] == tab)
            out.push_back(all[i]);
    return out;
}

void RealTable(const std::string& repo)
{
    const std::string code = menuscan::StripComments(menuscan::ReadFile(repo + "/OptiScaler/menu/menu_common.cpp"));
    const auto upstream = menuscan::ParseUpstreamBoxes(code);
    CHECK(upstream.size() >= 40);

    Menu m;
    for (const auto& b : upstream)
        m.base.push_back({ b.id, b.tab });
    for (const Slot& s : kSlots)
        m.slots.push_back(s);

    std::vector<Item> registered;
    for (const Slot& s : kSlots)
        registered.push_back({ s.id, s.tab });

    // many registration orders, same menu
    m.cards = registered;
    const Ids reference = Merge(m);
    CHECK(reference.size() == upstream.size() + kSlotCount);

    // the tab of an id of the merged menu: upstream boxes by id, fork cards by their slot
    auto tabOfId = [&](const std::string& id)
    {
        std::string tab;
        for (const auto& b : upstream)
            if (b.id == id)
                tab = b.tab;
        for (const Slot& s : kSlots)
            if (id == s.id)
                tab = s.tab;
        return tab;
    };
    auto contiguous = [&](const Ids& order)
    {
        Ids tabs;
        for (const auto& id : order)
            tabs.push_back(tabOfId(id));
        return TabsContiguous(tabs);
    };
    CHECK(contiguous(reference));

    std::mt19937 rng(20261019);
    size_t different = 0, scattered = 0;
    for (int i = 0; i < 500; i++)
    {
        std::vector<Item> shuffled = registered;
        std::shuffle(shuffled.begin(), shuffled.end(), rng);
        m.cards = shuffled;
        const Ids got = Merge(m);
        if (got != reference)
            ++different;
        if (!contiguous(got))
            ++scattered;
    }
    m.cards = registered;
    std::reverse(m.cards.begin(), m.cards.end());
    if (Merge(m) != reference)
        ++different;
    if (!contiguous(Merge(m)))
        ++scattered;
    CHECK(different == 0);
    CHECK(scattered == 0); // every tab is one run in the merged order, whatever the registration order

    // the expected order of every tab (cards that are not registered would simply be missing)
    Ids tabOf;
    {
        // tab of every entry of the merged menu: upstream boxes by id, fork cards by their slot
        for (const auto& id : reference)
        {
            std::string tab;
            for (const auto& b : upstream)
                if (b.id == id)
                    tab = b.tab;
            for (const Slot& s : kSlots)
                if (id == s.id)
                    tab = s.tab;
            tabOf.push_back(tab);
        }
    }
    auto tabIds = [&](const std::string& tab) { return Join(OfTab(reference, tabOf, tab)); };

    CHECK_EQ(tabIds("Upscaling"), "upscaler dlss_nr sharpness upscale_ratio forced_quality output_scaling shader_times magnifier");
    CHECK_EQ(tabIds("Frame Gen"),
             "fg_selection fg_fsrfg fg_xefg fg_dlssg fg_reprojection fg_optifg fg_nvngx fg_fsrfg_inputs "
             "fg_sl_inputs fg_unlocks fg_auto_mfg");
    CHECK_EQ(tabIds("FFXIV"),
             "window_mode reshade_depth ffxiv_hdr ffxiv_jitter ffxiv_character_preview fg_hudless ffxiv_companion hudless_diag "
             "ffxiv_advanced");
    CHECK_EQ(tabIds("Textures"), "mipmap anisotropy mipmap_auto");
    CHECK_EQ(tabIds("Compatibility"),
             "init_flags fsr_common upscaler_inputs spoofing plugins shaders_limits barriers root_signatures "
             "reshade_addon_probe");

    // the upstream cards keep their relative order
    Ids upstreamOrder, merged;
    for (const auto& b : upstream)
        upstreamOrder.push_back(b.id);
    for (const auto& id : reference)
        if (std::any_of(upstream.begin(), upstream.end(), [&](const auto& b) { return b.id == id; }))
            merged.push_back(id);
    CHECK(merged == upstreamOrder);
}

// ---- 3. MergeBoxes ----------------------------------------------------------------------------------------------------
void MergeBoxesPins(const std::string& repo)
{
    const std::string code =
        menuscan::Squash(menuscan::StripComments(menuscan::ReadFile(repo + "/OptiScaler/ext/core/MenuRegistry.h")));
    CHECK(!code.empty());

    // the order comes from the table, in the one place
    CHECK(Count(code, "layout::MergeOrder(") == 1);
    CHECK(Count(code, "layout::kSlots") == 1);
    // each fork box is built from the card's registration index, never from its final position
    CHECK(Count(code, "slots[p.index]") == 1);
    CHECK(Count(code, "cards[p.index]") >= 1);
    // the registration-order insertion of the old MergeBoxes is gone
    CHECK(code.find("entries.insert") == std::string::npos);
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";

    Synthetic();
    RealTable(repo);
    MergeBoxesPins(repo);

    if (failures)
    {
        std::printf("menu_layout: %d failure(s)\n", failures);
        return 2;
    }
    std::printf("menu_layout: all passed\n");
    return 0;
}
