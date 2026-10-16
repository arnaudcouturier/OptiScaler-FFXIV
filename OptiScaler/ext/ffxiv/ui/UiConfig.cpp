#include "pch.h"

#include "ext/ffxiv/ui/UiConfig.h"

#include "ext/core/ExtConfig.h"
#include "ext/hudless/HudlessState.h"

namespace ext::ffxiv::ui
{
bool DiagnosticsOn() { return ext::Cfg().ui.diagnostics.value_or_default(); }

bool EvaluateArmed() { return DiagnosticsOn() || ext::hudless::Wanted(); }

bool ArmedSlow() { return EvaluateArmed(); }

void RefreshArmed() { g_armState.store(EvaluateArmed() ? 2 : 1, std::memory_order_relaxed); }
} // namespace ext::ffxiv::ui
