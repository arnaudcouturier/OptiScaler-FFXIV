#include "pch.h"

#include "ext/hudless/HudlessState.h"

#include <atomic>

namespace ext::hudless
{
namespace
{
// Plain static of a trivial type: no destructor to run under the loader lock.
std::atomic<bool> g_wanted {false};
} // namespace

bool Wanted() { return g_wanted.load(std::memory_order_relaxed); }

void SetWanted(bool wanted) { g_wanted.store(wanted, std::memory_order_relaxed); }
} // namespace ext::hudless
