// The real MergeBoxes (ext/core/MenuRegistry.h) executed offline, with fake detail::Cards / detail::RunCard and a local Box
// type shaped like upstream's MenuBox (the header needs no ImGui). The unit under test is the glue between the layout
// table and upstream's box array:
//   - every upstream box is copied unchanged, in the order MergeOrder gives (a flipped fork test or `base[0]` breaks this);
//   - each fork box carries its own card's id/tab/name/pinnable and a trampoline that runs the card with its REGISTRATION
//     index and the context's menuResScale (a trampoline taken from the final position runs another card);
//   - the same menu for any registration order; the merged array is built once and reused;
//   - with no card registered the upstream array is returned untouched, and the merge still happens once cards register.
#include "ext/core/MenuLayout.h"
#include "ext/core/MenuRegistry.h"
#include "menu_scan.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

// ---- the fakes MergeBoxes links against ----------------------------------------------------------------------------------
namespace ext::menu::detail
{
std::vector<Card> g_cards;
struct Call
{
    size_t index;
    void* raw;
    float scale;
};
std::vector<Call> g_calls;

const Card* Cards(size_t& count)
{
    count = g_cards.size();
    return g_cards.data();
}

void RunCard(size_t index, void* rawContext, float menuResScale)
{
    g_calls.push_back({ index, rawContext, menuResScale });
}
} // namespace ext::menu::detail

namespace
{
using namespace ext::menu;
using Ids = std::vector<std::string>;

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

struct FakeCtx
{
    float menuResScale = 0.0f;
};

// shaped like upstream's MenuBox; one type per Tag because MergeBoxes keeps its merged array in function-local statics
template <int Tag> struct FakeBox
{
    const char* id;
    const char* tab;
    const char* name;
    void (*draw)(FakeCtx&);
    void (*source)(FakeCtx&);
    bool pinnable;
};

void BaseDraw(FakeCtx&) {}
void BaseSource(FakeCtx&) {}

struct Fixture
{
    std::vector<menuscan::UpstreamBox> upstream;
    std::vector<std::string> names; // card names, kept alive for the const char* of the cards
};

std::vector<layout::Item> Items(const std::vector<menuscan::UpstreamBox>& up)
{
    std::vector<layout::Item> items;
    for (const auto& b : up)
        items.push_back({ b.id, b.tab });
    return items;
}

// Merge `order` (the registration order, indices into kSlots) with the upstream boxes through the real MergeBoxes and
// check every box. Returns the merged ids.
template <int Tag> Ids RunMerge(Fixture& fx, const std::vector<size_t>& order)
{
    using Box = FakeBox<Tag>;

    // the registered cards, in registration order
    detail::g_cards.clear();
    fx.names.clear();
    fx.names.reserve(order.size());
    for (size_t k = 0; k < order.size(); k++)
    {
        const layout::Slot& s = layout::kSlots[order[k]];
        fx.names.push_back(std::string("name:") + s.id);
        detail::g_cards.push_back(Card { s.id, s.tab, fx.names.back().c_str(), nullptr, nullptr, nullptr, k % 3 != 0 });
    }

    std::vector<Box> base;
    for (const auto& b : fx.upstream)
        base.push_back(Box { b.id.c_str(), b.tab.c_str(), b.id.c_str(), b.source.empty() ? &BaseDraw : nullptr,
                             b.source.empty() ? nullptr : &BaseSource, false });

    size_t count = base.size();
    const Box* merged = MergeBoxes<Box>(base.data(), count);

    CHECK(merged != base.data());
    CHECK(count == base.size() + order.size());

    // the expected order comes straight from the pure ordering over the same inputs
    const auto items = Items(fx.upstream);
    std::vector<layout::Item> cardItems;
    for (const Card& c : detail::g_cards)
        cardItems.push_back({ c.id, c.tab });
    const auto expect = layout::MergeOrder(items.data(), items.size(), cardItems.data(), cardItems.size(), layout::kSlots,
                                           layout::kSlotCount);
    CHECK(expect.size() == count);

    Ids ids;
    for (size_t i = 0; i < count && i < expect.size(); i++)
    {
        const Box& box = merged[i];
        CHECK(box.id != nullptr && box.tab != nullptr && box.name != nullptr);
        if (box.id == nullptr || box.tab == nullptr || box.name == nullptr)
            return ids;

        ids.push_back(box.id);

        if (!expect[i].fork)
        {
            // an upstream box is the same box, field for field
            const Box& orig = base[expect[i].index];
            CHECK(std::strcmp(box.id, orig.id) == 0);
            CHECK(box.id == orig.id && box.tab == orig.tab && box.name == orig.name);
            CHECK(box.draw == orig.draw && box.source == orig.source && box.pinnable == orig.pinnable);
            continue;
        }

        // a fork box carries its own card's data and runs its own card
        const size_t reg = expect[i].index;
        const Card& card = detail::g_cards[reg];
        CHECK(box.id == card.id && box.tab == card.tab && box.name == card.name);
        CHECK(box.pinnable == card.pinnable);
        CHECK(box.source == nullptr);
        CHECK(box.draw != nullptr);
        if (box.draw == nullptr)
            continue;

        detail::g_calls.clear();
        FakeCtx ctx;
        ctx.menuResScale = 2.5f + static_cast<float>(reg);
        box.draw(ctx);
        CHECK(detail::g_calls.size() == 1);
        if (detail::g_calls.size() == 1)
        {
            CHECK(detail::g_calls[0].index == reg);
            CHECK(detail::g_calls[0].raw == &ctx);
            CHECK(detail::g_calls[0].scale == ctx.menuResScale);
        }
    }

    // built once: the next call hands out the same array
    size_t again = base.size();
    CHECK(MergeBoxes<Box>(base.data(), again) == merged && again == count);
    return ids;
}

void Real(const std::string& repo)
{
    Fixture fx;
    fx.upstream = menuscan::ParseUpstreamBoxes(menuscan::StripComments(menuscan::ReadFile(repo + "/OptiScaler/menu/menu_common.cpp")));
    CHECK(fx.upstream.size() >= 40);
    CHECK(layout::kSlotCount <= kMaxCards);

    std::vector<size_t> table(layout::kSlotCount);
    for (size_t i = 0; i < table.size(); i++)
        table[i] = i;
    std::vector<size_t> reversed(table.rbegin(), table.rend());
    std::vector<size_t> shuffledA = table, shuffledB = table;
    std::mt19937 rng(20261019);
    std::shuffle(shuffledA.begin(), shuffledA.end(), rng);
    std::shuffle(shuffledB.begin(), shuffledB.end(), rng);

    const Ids reference = RunMerge<1>(fx, table);
    CHECK(RunMerge<2>(fx, reversed) == reference);
    CHECK(RunMerge<3>(fx, shuffledA) == reference);
    CHECK(RunMerge<4>(fx, shuffledB) == reference);
    CHECK(reference.size() == fx.upstream.size() + layout::kSlotCount);

    // the upstream boxes keep their relative order inside the merged menu
    Ids upstreamIds, merged;
    for (const auto& b : fx.upstream)
        upstreamIds.push_back(b.id);
    for (const auto& id : reference)
        if (std::find(upstreamIds.begin(), upstreamIds.end(), id) != upstreamIds.end())
            merged.push_back(id);
    CHECK(merged == upstreamIds);
}

void NoCardsThenCards(const std::string& repo)
{
    using Box = FakeBox<9>;
    Fixture fx;
    fx.upstream = menuscan::ParseUpstreamBoxes(menuscan::StripComments(menuscan::ReadFile(repo + "/OptiScaler/menu/menu_common.cpp")));

    std::vector<Box> base;
    for (const auto& b : fx.upstream)
        base.push_back(Box { b.id.c_str(), b.tab.c_str(), b.id.c_str(), &BaseDraw, nullptr, false });

    // no card yet: upstream's own array comes back, unchanged, and the merge is not frozen
    detail::g_cards.clear();
    size_t count = base.size();
    CHECK(MergeBoxes<Box>(base.data(), count) == base.data());
    CHECK(count == base.size());

    detail::g_cards.push_back(Card { "dlss_nr", "Upscaling", "DLSS Neural Rendering", nullptr });
    count = base.size();
    const Box* merged = MergeBoxes<Box>(base.data(), count);
    CHECK(merged != base.data());
    CHECK(count == base.size() + 1);
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";

    Real(repo);
    NoCardsThenCards(repo);

    if (failures)
    {
        std::printf("merge_boxes: %d failure(s)\n", failures);
        return 2;
    }
    std::printf("merge_boxes: all passed\n");
    return 0;
}
