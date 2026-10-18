#pragma once
// The one place where the decision "which tint does this alpha pass make" (nr::TintKind, CharaNrPlumbing.h) meets the helper pass's own tint enum
// (guides::Tint, CharaGuides.h). It used to be an inline conversion in RunSlot, where swapping the cyan and the magenta case passed every test.

#include "ext/ffxiv/chara/CharaGuides.h"
#include "ext/ffxiv/chara/CharaNrPlumbing.h"

namespace ext::ffxiv::chara
{
constexpr guides::Tint ToGuideTint(nr::TintKind kind)
{
    switch (kind)
    {
    case nr::TintKind::Nr: return guides::Tint::Nr;
    case nr::TintKind::Sr: return guides::Tint::Sr;
    case nr::TintKind::None: break;
    }
    return guides::Tint::None;
}
} // namespace ext::ffxiv::chara
