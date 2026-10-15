#pragma once
// Fork cards in the OptiScaler menu (docs/SEAMS.md section 3.6).
//
// A feature registers a card from its own directory (static AutoRegister, or Register() from an init function that
// runs before the first menu frame):
//
//   static void DrawNrCard(ext::menu::Ctx& c) { ext::menu::SectionTitle("DLSS NR"); ... }
//   static bool NrVisible(const ext::menu::Ctx& c) { return ext::Active(ext::F::Nr); }
//   static ext::menu::AutoRegister reg { { "nr_settings", "Upscaling", "DLSS Neural Rendering", DrawNrCard,
//                                         "output_scaling" /* after */, NrVisible } };
//
// The draw callback runs inside an upstream ScopedCard, so tab filtering and Custom Tab pinning work like for any
// upstream card. `tab` is the label of an existing upstream tab. `after` is the id of the upstream (or earlier fork)
// card to follow; when it is null or unknown the card goes after the last card of its tab. Cards with the same
// anchor keep their registration order. At most kMaxCards cards.

#include <array>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

class State;
class Config;
class IFeature;
struct ImGuiIO;

namespace ext::menu
{
inline constexpr size_t kMaxCards = 32;

// Narrow view of upstream's RenderMenuContext. Built from the singletons because RenderMenuContext is defined in
// menu_common.cpp; fields are added here (with their source) when a card needs them.
struct Ctx
{
    State& state;
    Config* config;
    ImGuiIO& io;
    IFeature* currentFeature; // state.currentFeature, like upstream's ctx
    float menuResScale = 1.0f; // TODO(S-menu): not exposed by upstream yet (needs a seam or accessor in Access)
    void* raw = nullptr;       // upstream RenderMenuContext*, opaque; only ext::Access may use it
};

struct Card
{
    const char* id;   // saved to the config (pinning), unique
    const char* tab;  // label of the upstream tab: "Upscaling", "Frame Gen", "Latency & FPS", "Textures",
                      // "Compatibility", "Menu", "Quirks / ASI", "Help / Status"
    const char* name; // shown in the Custom Tab picker
    void (*draw)(Ctx&);
    const char* after = nullptr;                 // id of the card to follow
    bool (*visible)(const Ctx&) = nullptr;       // null = always visible
    bool pinnable = true;
};

void Register(const Card& card);

struct AutoRegister
{
    explicit AutoRegister(const Card& card) { Register(card); }
};

namespace detail
{
// Registered cards in registration order
const Card* Cards(size_t& count);
// Draw slot `index` (trampoline target)
void RunCard(size_t index, void* rawContext);

template <class F> struct DrawArg;
template <class R> struct DrawArg<void (*)(R&)>
{
    using type = R;
};
} // namespace detail

// The seam in MenuCommon::GetMenuBoxes (declared through ext/Seams.h). MenuBox and RenderMenuContext are private to
// menu_common.cpp, so this is a template deduced from the real array there: no fork code names those types.
// Returns `base` while no card is registered, otherwise a merged array that lives for the whole process.
template <class Box> const Box* MergeBoxes(const Box* base, size_t& count)
{
    using Ctx = typename detail::DrawArg<decltype(Box::draw)>::type;
    using DrawFn = decltype(Box::draw);

    static std::mutex mutex;
    static std::vector<Box> merged;
    static bool done = false;

    std::scoped_lock lock(mutex);

    if (!done)
    {
        size_t cardCount = 0;
        const Card* cards = detail::Cards(cardCount);

        if (cardCount == 0)
            return base; // registration may still come, try again on the next call

        // MenuBox::draw has no state: one non-capturing trampoline per card slot
        static const auto slots = []<size_t... I>(std::index_sequence<I...>)
        {
            return std::array<DrawFn, sizeof...(I)> { (+[](Ctx& c) { detail::RunCard(I, &c); })... };
        }(std::make_index_sequence<kMaxCards> {});

        struct Entry
        {
            Box box;
            const char* after; // anchor of a fork card
            bool fork;
        };

        std::vector<Entry> entries;
        for (size_t i = 0; i < count; i++)
            entries.push_back({ base[i], nullptr, false });

        for (size_t c = 0; c < cardCount && c < slots.size(); c++)
        {
            const Card& card = cards[c];
            Entry e { Box { card.id, card.tab, card.name, slots[c], nullptr, card.pinnable }, card.after, true };

            size_t pos = SIZE_MAX; // insertion index

            // after the anchor, behind fork cards that already follow the same anchor
            if (card.after != nullptr)
            {
                for (size_t i = 0; i < entries.size(); i++)
                {
                    if (strcmp(entries[i].box.id, card.after) != 0)
                        continue;

                    pos = i + 1;
                    while (pos < entries.size() && entries[pos].fork && entries[pos].after != nullptr &&
                           strcmp(entries[pos].after, card.after) == 0)
                        pos++;
                    break;
                }
            }

            // unknown or no anchor: after the last card of the tab, else at the end
            if (pos == SIZE_MAX)
            {
                pos = entries.size();
                for (size_t i = entries.size(); i > 0; i--)
                {
                    if (strcmp(entries[i - 1].box.tab, card.tab) == 0)
                    {
                        pos = i;
                        break;
                    }
                }
            }

            entries.insert(entries.begin() + pos, e);
        }

        for (const auto& e : entries)
            merged.push_back(e.box);

        done = true;
    }

    count = merged.size();
    return merged.data();
}
} // namespace ext::menu
