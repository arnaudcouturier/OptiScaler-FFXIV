#include "pch.h"

#include "ext/Seams.h"
#include "ext/core/MenuRegistry.h"

#include <menu/menu_common.h>

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

namespace detail
{
const Card* Cards(size_t& count)
{
    std::scoped_lock lock(RegistryMutex());
    count = Registry().size();
    return Registry().data(); // vector is no longer modified once the menu merged it
}

void RunCard(size_t index, void* rawContext)
{
    size_t count = 0;
    const Card* cards = Cards(count);

    if (index >= count)
        return;

    State& state = State::Instance();
    Ctx ctx { state, Config::Instance(), ImGui::GetIO(), state.currentFeature, 1.0f, rawContext };

    const Card& card = cards[index];
    if (card.visible != nullptr && !card.visible(ctx))
        return;

    ScopedCard scoped;
    card.draw(ctx);
}
} // namespace detail
} // namespace ext::menu
