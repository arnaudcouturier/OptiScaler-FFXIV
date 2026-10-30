#pragma once
// Seam of the bridge experiment keys (docs/SEAMS-catalog.md "bridge"). One key today: [Bridge] FlushAfterSignal, the
// opt-in flush at the D3D11 -> D3D12 hand-over (with_dx12/dx11_with_dx12.cpp SyncDx11ToDx12). Included by ext/Seams.h,
// the only fork header upstream files may include (docs/SEAMS.md section 1), so the hand-over's one line finds it.
//
// The key is cached: the hand-over pays one relaxed atomic load per frame with the key on or off, and the config load
// (ext/bridge/BridgeConfig.h PostLoad) is the only writer. Body and startup line: BridgeSeams.cpp.

#include <atomic>

namespace ext::bridge
{
namespace detail
{
// Plain static of a trivial type: no destructor to run under the loader lock (the same shape as
// ext/window/WindowSeams.h).
inline std::atomic<bool> g_flushAfterSignal { false };
} // namespace detail

// True = the hand-over flushes the D3D11 context right after its fence Signal was accepted. False (the default) leaves
// the call sequence exactly as it is upstream.
inline bool FlushAfterSignal() noexcept { return detail::g_flushAfterSignal.load(std::memory_order_relaxed); }

// The cached copy every reader sees. Called by the config load for both states, so a value set by hand and then removed
// from the ini turns the seam back off; a menu control, if one is ever added, publishes here too.
inline void PublishFlushAfterSignal(bool on) noexcept
{
    detail::g_flushAfterSignal.store(on, std::memory_order_relaxed);
}

// One log line when the key is on, and nothing when it is off. Called from ext/core/Banner.cpp: the config load runs
// before the logger exists, so the line is written right after the version banner instead.
void LogStartupState() noexcept;
} // namespace ext::bridge
