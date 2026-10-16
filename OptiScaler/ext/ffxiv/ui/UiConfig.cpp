#include "pch.h"

#include "ext/ffxiv/ui/UiConfig.h"

#include "ext/core/ExtConfig.h"
#include "ext/hudless/HudlessState.h"

namespace ext::ffxiv::ui
{
bool DiagnosticsOn() { return ext::Cfg().ui.diagnostics.value_or_default(); }

bool EvaluateArmed() { return DiagnosticsOn() || ext::hudless::Wanted(); }

// First evaluation after the hooks exist (they are installed after the config is loaded): settles the cache, so the draw hook
// is one byte test from here on even when no bridge Present ever refreshes it (frame generation off).
bool ArmedSlow()
{
    const bool armed = EvaluateArmed();
    g_armState.store(armed ? 2 : 1, std::memory_order_relaxed);
    return armed;
}

void RefreshArmed() { g_armState.store(EvaluateArmed() ? 2 : 1, std::memory_order_relaxed); }
} // namespace ext::ffxiv::ui
