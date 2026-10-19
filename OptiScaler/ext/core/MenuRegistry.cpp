#include "pch.h"

#include "ext/Seams.h"
#include "ext/core/MenuRegistry.h"
#include "ext/game/Game.h"

#include <menu/menu_common.h>

#include <Config.h>

#include <mutex>
#include <vector>

namespace ext::menu
{
namespace
{
std::mutex& RegistryMutex()
{
    static std::mutex m;
    return m;
}

std::vector<Card>& Registry()
{
    static std::vector<Card> cards;
    return cards;
}

// Fixed-capacity registries: registered during static initialisation, read by the menu seams without allocating
template <class T, size_t N> struct Fixed
{
    std::array<T, N> items {};
    size_t count = 0;
};

Fixed<Shortcut, kMaxShortcuts>& ShortcutList()
{
    static Fixed<Shortcut, kMaxShortcuts> list;
    return list;
}

Fixed<Overlay, kMaxOverlays>& OverlayList()
{
    static Fixed<Overlay, kMaxOverlays> list;
    return list;
}

Fixed<TimeRow, kMaxTimeRows>& TimeRowList()
{
    static Fixed<TimeRow, kMaxTimeRows> list;
    return list;
}

template <class T, size_t N> void AddTo(Fixed<T, N>& list, const T& item, const char* what, const char* id)
{
    std::scoped_lock lock(RegistryMutex());

    if (list.count >= N)
    {
        LOG_ERROR("menu {} limit ({}) reached, '{}' ignored", what, N, id);
        return;
    }

    list.items[list.count++] = item;
}
} // namespace

void Register(const Card& card)
{
    std::scoped_lock lock(RegistryMutex());
    auto& cards = Registry();

    if (cards.size() >= kMaxCards)
    {
        LOG_ERROR("menu card limit ({}) reached, '{}' ignored", kMaxCards, card.id);
        return;
    }

    cards.push_back(card);
}

void RegisterShortcut(const Shortcut& shortcut) { AddTo(ShortcutList(), shortcut, "shortcut", shortcut.name); }

void RegisterOverlay(const Overlay& overlay) { AddTo(OverlayList(), overlay, "overlay", "overlay"); }

void RegisterTimeRow(const TimeRow& row) { AddTo(TimeRowList(), row, "time row", row.label); }

// All the FFXIV pointers (lighting, hdr, hudless, quality, ...) are set together by the build match: this is "the game is FFXIV"
bool FfxivTabShown() { return ext::game::Current().lighting != nullptr; }

bool WantsFrame()
{
    size_t count = 0;
    const Overlay* overlays = detail::Overlays(count);
    for (size_t i = 0; i < count; i++)
    {
        if (overlays[i].wantsFrame != nullptr && overlays[i].wantsFrame())
            return true;
    }
    return false;
}

ExtraTimes ShaderTimeRows()
{
    ExtraTimes times;

    const auto& list = TimeRowList(); // filled during static initialisation, read-only afterwards
    for (size_t i = 0; i < list.count; i++)
    {
        const auto ms = list.items[i].ms();
        if (ms.has_value())
            times.rows[times.count++] = { list.items[i].label, ms.value() };
    }
    return times;
}

void DrawShaderTimeRows(const ExtraTimes& times)
{
    for (size_t i = 0; i < times.count; i++)
    {
        ImGui::TableNextColumn();
        ImGui::Text("%s", times.rows[i].label);
        ImGui::TableNextColumn();
        ImGui::Text("%.2f ms", times.rows[i].ms);
    }
}

namespace detail
{
// The lists are filled during static initialisation and only read afterwards: no lock needed for the pointers
Shortcut* Shortcuts(size_t& count)
{
    auto& list = ShortcutList();
    count = list.count;
    return list.items.data();
}

const Overlay* Overlays(size_t& count)
{
    auto& list = OverlayList();
    count = list.count;
    return list.items.data();
}

void PollShortcuts(void (*check)(void* fn, int vk, bool& flag, const char* log), void* fn)
{
    size_t count = 0;
    Shortcut* shortcuts = Shortcuts(count);
    for (size_t i = 0; i < count; i++)
        check(fn, static_cast<CustomOptional<int>*>(shortcuts[i].option())->value_or_default(), shortcuts[i].pressed,
              shortcuts[i].pollLog);
}

const Card* Cards(size_t& count)
{
    std::scoped_lock lock(RegistryMutex());
    count = Registry().size();
    return Registry().data(); // vector is no longer modified once the menu merged it
}

void RunCard(size_t index, void* rawContext, float menuResScale)
{
    size_t count = 0;
    const Card* cards = Cards(count);

    if (index >= count)
        return;

    State& state = State::Instance();
    Ctx ctx { state, Config::Instance(), ImGui::GetIO(), state.currentFeature, menuResScale, rawContext };

    const Card& card = cards[index];
    if (card.visible != nullptr && !card.visible(ctx))
        return;

    ScopedCard scoped;
    card.draw(ctx);
}
} // namespace detail
} // namespace ext::menu
