#pragma once
// Where each fork card sits in the menu: one explicit table, independent of registration (link) order.
//
// MergeBoxes (ext/core/MenuRegistry.h) takes the fork cards in whatever order they registered and asks MergeOrder where
// each one goes. The answer comes from kSlots only, so the menu looks the same however the linker orders the translation
// units. Pure data and logic (no ImGui, no upstream types): the menu manifest test builds the final order from the same code.
//
// kSlots is processed in TABLE order. Each slot says which card its card follows (`after`): an upstream card or a fork
// card that comes earlier in the table. Cards that follow the same anchor keep their table order. When `after` is null or
// names no card, the card goes after the last card of its own tab, and when the tab has no card yet, at the end of the
// table. A registered card that has no slot is placed after the table cards in id order, at the end of its tab (fail
// open: it shows, it never disappears). A slot whose card is not registered is skipped; the cards after it still find
// their anchors by id.
//
// Rule for the people editing the table: a fork card sits BEFORE or AFTER a group of upstream boxes that share one
// `source` function, never inside it. The tab loop draws a shared function once, at the position of its first box
// (ext/core/SourceTracker.h), so a card inside the group would show after the whole group anyway; the manifest test
// rejects it so the table says what is drawn. Card ids are permanent (the Custom Tab saves pins by id).

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string_view>
#include <vector>

namespace ext::menu::layout
{
struct Slot
{
    const char* id;    // fork card id
    const char* tab;   // tab label the card is registered for (the manifest test compares)
    const char* after; // id of the card to follow, null = after the last card of the tab
};

// clang-format off
inline constexpr Slot kSlots[] = {
    // Upscaling
    { "dlss_nr",                  "Upscaling",     "upscaler" },
    { "ffxiv_character_preview",  "Upscaling",     "dlss_nr" },
    { "forced_quality",           "Upscaling",     "sharpness" },
    { "ffxiv_jitter",             "Upscaling",     "output_scaling" },

    // Frame Gen: the runtime cards (fg_fsrfg .. fg_sl_inputs) are one shared function, our cards follow its last box
    { "fg_unlocks",               "Frame Gen",     "fg_selection" },
    { "ffxiv_hdr",                "Frame Gen",     "fg_unlocks" },
    { "fg_hudless",               "Frame Gen",     "ffxiv_hdr" },
    { "fg_auto_mfg",              "Frame Gen",     "fg_sl_inputs" },
    { "window_mode",              "Frame Gen",     "fg_auto_mfg" },
    { "hudless_diag",             "Frame Gen",     "window_mode" },

    // Textures: Mipmap Bias and Anisotropic Filtering are one shared function
    { "mipmap_auto",              "Textures",      "anisotropy" },

    // Compatibility
    { "ffxiv_companion",          "Compatibility", nullptr },
    { "reshade_addon_probe",      "Compatibility", "ffxiv_companion" },
};
// clang-format on

inline constexpr size_t kSlotCount = std::size(kSlots);

// A card of the menu as the ordering sees it: an upstream box or a registered fork card
struct Item
{
    std::string_view id;
    std::string_view tab;
};

// Position in the final order: the index into the upstream boxes or into the registered cards
struct Placed
{
    bool fork;
    size_t index;
};

// The final order of the menu. `base` = upstream boxes in their order, `cards` = fork cards in registration order.
inline std::vector<Placed> MergeOrder(const Item* base, size_t baseCount, const Item* cards, size_t cardCount,
                                      const Slot* slots, size_t slotCount)
{
    struct Entry
    {
        Placed ref;
        std::string_view id;
        std::string_view tab;
        std::string_view after; // anchor of a fork card, empty for upstream boxes
    };

    std::vector<Entry> entries;
    entries.reserve(baseCount + cardCount);
    for (size_t i = 0; i < baseCount; i++)
        entries.push_back({ { false, i }, base[i].id, base[i].tab, {} });

    std::vector<bool> placed(cardCount, false);

    auto insert = [&](size_t c, std::string_view after)
    {
        size_t pos = entries.size();
        bool found = false;

        // after the anchor, behind the fork cards that already follow the same anchor
        if (!after.empty())
        {
            for (size_t i = 0; i < entries.size(); i++)
            {
                if (entries[i].id != after)
                    continue;

                pos = i + 1;
                while (pos < entries.size() && entries[pos].ref.fork && entries[pos].after == after)
                    pos++;
                found = true;
                break;
            }
        }

        // no anchor: after the last card of the tab, else at the end
        if (!found)
        {
            for (size_t i = entries.size(); i > 0; i--)
            {
                if (entries[i - 1].tab == cards[c].tab)
                {
                    pos = i;
                    break;
                }
            }
        }

        entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(pos),
                       Entry { { true, c }, cards[c].id, cards[c].tab, after });
        placed[c] = true;
    };

    // the table, in table order
    for (size_t s = 0; s < slotCount; s++)
    {
        for (size_t c = 0; c < cardCount; c++)
        {
            if (!placed[c] && cards[c].id == slots[s].id)
            {
                insert(c, slots[s].after != nullptr ? std::string_view(slots[s].after) : std::string_view());
                break;
            }
        }
    }

    // cards without a slot: id order (registration order only breaks a tie between equal ids)
    std::vector<size_t> rest;
    for (size_t c = 0; c < cardCount; c++)
    {
        if (!placed[c])
            rest.push_back(c);
    }
    std::sort(rest.begin(), rest.end(),
              [&](size_t a, size_t b) { return cards[a].id != cards[b].id ? cards[a].id < cards[b].id : a < b; });
    for (size_t c : rest)
        insert(c, {});

    std::vector<Placed> order;
    order.reserve(entries.size());
    for (const Entry& e : entries)
        order.push_back(e.ref);
    return order;
}
} // namespace ext::menu::layout
