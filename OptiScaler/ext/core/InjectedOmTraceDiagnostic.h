#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <atomic>
#include <cstdint>
#include <intrin.h>
#include "ext/core/InjectedStateWindowDiagnostic.h"

// Fixed storage for the short live SSR OM trace. Hook callers only reserve and fill a row.
namespace ext::core::om_trace
{
inline constexpr uint32_t kPassBudget = 4;
inline constexpr uint32_t kRowBudget = 64;
inline constexpr uint32_t kStackRows = 8;
inline constexpr uint32_t kRestoreStackRows = 2;
inline constexpr uint32_t kStackDepth = 12;
inline constexpr UINT kKeep = D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL;
enum class Phase : uint32_t { Idle, BeforePrep, Prep, SwapIn, PrivateBind, SwapOut, Restored, NearlySwap };
enum class Api : uint32_t { Rtv, RtvUav };
enum class Decision : uint32_t { Enter, AfterOriginal, Suppressed, ContextMismatch, InternalCall, Keep, AcceptedSame, AcceptedChanged };
struct Row
{
    std::atomic<uint32_t> ready { 0 };
    uint32_t id = 0, phase = 0, api = 0, decision = 0, requested = 0, oldCount = 0, newCount = 0;
    uint32_t thread = 0, owner = 0, stackCount = 0;
    uintptr_t context = 0, immediateContext = 0, firstRtv = 0, dsv = 0, callsite = 0;
    uintptr_t stack[kStackDepth] {};
};
struct Capture
{
    uint32_t id = 0, owner = 0;
    uintptr_t context = 0, context1 = 0;
    uint32_t telemetryBefore = UINT32_MAX, telemetryAfter = UINT32_MAX;
    HRESULT unwrapHr = E_FAIL;
    uintptr_t unwrapped = 0, unwrappedContext = 0, identity = 0, incomingIdentity = 0, vtable = 0, v33 = 0, v34 = 0, vFirst = 0;
    uintptr_t unwrapVtable = 0, unwrapV33 = 0, unwrapV34 = 0, unwrapVFirst = 0;
    Row rows[kRowBudget];
    std::atomic<uint32_t> reserved { 0 }, completed { 0 }, inFlight { 0 }, overflow { 0 };
    std::atomic<uint32_t> ordinaryStacks { 0 }, restoreStacks { 0 };
    std::atomic<uint32_t> phase { uint32_t(Phase::BeforePrep) };
    std::atomic<bool> active { false }, committed { false }, partial { false }, dumped { false };
};
inline std::atomic<bool> enabled { false };
inline std::atomic<uint32_t> passes { 0 }, nextId { 0 };
inline std::atomic<Capture*> current { nullptr };
inline Capture captures[kPassBudget];
inline thread_local uint32_t nesting = 0;

inline void Record(Api api, Decision decision, ID3D11DeviceContext* ctx, UINT requested, uintptr_t rtv = 0, uintptr_t dsv = 0,
                   uint32_t oldCount = UINT32_MAX, uint32_t newCount = UINT32_MAX, ID3D11DeviceContext* immediateContext = nullptr) noexcept
{
    if (!enabled.load(std::memory_order_relaxed)) return;
    Capture* c = current.load(std::memory_order_acquire);
    if (!c || !c->active.load(std::memory_order_acquire)) return;
    c->inFlight.fetch_add(1, std::memory_order_acq_rel);
    if (!c->active.load(std::memory_order_acquire)) { c->inFlight.fetch_sub(1, std::memory_order_release); return; }
    const uint32_t n = c->reserved.fetch_add(1, std::memory_order_relaxed);
    if (n >= kRowBudget) { c->overflow.fetch_add(1, std::memory_order_relaxed); c->inFlight.fetch_sub(1, std::memory_order_release); return; }
    Row& r = c->rows[n];
    r.id = c->id; r.phase = c->phase.load(std::memory_order_relaxed); r.api = uint32_t(api); r.decision = uint32_t(decision);
    r.requested = requested; r.oldCount = oldCount; r.newCount = newCount;
    r.thread = GetCurrentThreadId(); r.owner = c->owner; r.context = reinterpret_cast<uintptr_t>(ctx);
    r.immediateContext = reinterpret_cast<uintptr_t>(immediateContext); r.firstRtv = rtv; r.dsv = dsv;
    r.callsite = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const bool stackEvent = decision == Decision::Enter || decision == Decision::Suppressed ||
                            decision == Decision::ContextMismatch || decision == Decision::InternalCall || decision == Decision::Keep;
    if (stackEvent)
    {
        const bool restoring = r.phase == uint32_t(Phase::SwapOut) || r.phase == uint32_t(Phase::Restored);
        const uint32_t stackIndex = restoring ? c->restoreStacks.fetch_add(1, std::memory_order_relaxed)
                                              : c->ordinaryStacks.fetch_add(1, std::memory_order_relaxed);
        const uint32_t budget = restoring ? kRestoreStackRows : kStackRows - kRestoreStackRows;
        if (stackIndex < budget)
            r.stackCount = uint32_t(CaptureStackBackTrace(1, kStackDepth, reinterpret_cast<void**>(r.stack), nullptr));
    }
    r.ready.store(1, std::memory_order_release);
    c->completed.fetch_add(1, std::memory_order_release);
    c->inFlight.fetch_sub(1, std::memory_order_release);
}

class PassScope
{
  public:
    explicit PassScope(ID3D11DeviceContext* context) noexcept
    {
        if (!context || !enabled.load(std::memory_order_relaxed)) return;
        ++nesting;
        _nestingHeld = true;
        if (nesting != 1) return;
        const uint32_t n = passes.fetch_add(1, std::memory_order_relaxed);
        if (n >= kPassBudget) return;
        _capture = &captures[n];
        _capture->id = nextId.fetch_add(1, std::memory_order_relaxed) + 1;
        _capture->owner = GetCurrentThreadId(); _capture->context = reinterpret_cast<uintptr_t>(context);
        UINT tracked = 0;
        if (ext::core::state_window::ReadTrackedOm(context, tracked)) _capture->telemetryBefore = tracked;
        _capture->reserved.store(0); _capture->completed.store(0); _capture->overflow.store(0); _capture->inFlight.store(0);
        _capture->partial.store(false); _capture->committed.store(false); _capture->dumped.store(false); _capture->phase.store(uint32_t(Phase::BeforePrep));
        Capture* expected = nullptr;
        if (!current.compare_exchange_strong(expected, _capture, std::memory_order_acq_rel)) { _capture = nullptr; return; }
        _capture->active.store(true, std::memory_order_release);
    }
    ~PassScope()
    {
        if (_capture) {
            _capture->active.store(false, std::memory_order_release);
            UINT tracked = 0;
            if (ext::core::state_window::ReadTrackedOm(reinterpret_cast<ID3D11DeviceContext*>(_capture->context), tracked))
                _capture->telemetryAfter = tracked;
            _capture->partial.store(_capture->inFlight.load(std::memory_order_acquire) != 0, std::memory_order_release);
            _capture->committed.store(true, std::memory_order_release);
            current.store(nullptr, std::memory_order_release);
        }
        if (_nestingHeld) --nesting;
    }
    void PhaseNow(Phase p) noexcept { if (_capture) _capture->phase.store(uint32_t(p), std::memory_order_relaxed); }
    Capture* Get() const noexcept { return _capture; }
  private:
    Capture* _capture = nullptr;
    bool _nestingHeld = false;
};
} // namespace ext::core::om_trace
