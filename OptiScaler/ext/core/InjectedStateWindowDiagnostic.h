#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <atomic>
#include <cstdint>

// Bounded observation of foreign-thread native draw calls made while an injected pass owns the context state.
namespace ext::core::state_window
{
struct Stats
{
    uint64_t count = 0, token = 0;
    DWORD owner = 0, caller = 0;
    const char* kind = "none";
};

inline std::atomic<bool> enabled { false };
inline std::atomic<bool> active { false };
inline std::atomic<ID3D11DeviceContext*> context { nullptr };
inline std::atomic<DWORD> owner { 0 };
inline std::atomic<uint64_t> serial { 0 };
inline std::atomic<uint64_t> count { 0 };
inline std::atomic<DWORD> lastOwner { 0 }, lastCaller { 0 };
inline std::atomic<const char*> lastKind { "none" };
inline std::atomic<ID3D11DeviceContext*> trackedOmContext { nullptr };
inline std::atomic<UINT> trackedOmCount { 0 };

inline void PublishTrackedOm(ID3D11DeviceContext* ctx, UINT rtvCount) noexcept
{
    if (!enabled.load(std::memory_order_relaxed) || !ctx) return;
    trackedOmCount.store(rtvCount, std::memory_order_relaxed);
    trackedOmContext.store(ctx, std::memory_order_release);
}
inline bool ReadTrackedOm(ID3D11DeviceContext* ctx, UINT& rtvCount) noexcept
{
    if (!enabled.load(std::memory_order_relaxed) || !ctx || trackedOmContext.load(std::memory_order_acquire) != ctx) return false;
    rtvCount = trackedOmCount.load(std::memory_order_relaxed);
    return true;
}

inline uint64_t Begin(ID3D11DeviceContext* ctx, DWORD ownerThread = GetCurrentThreadId()) noexcept
{
    if (!enabled.load(std::memory_order_relaxed) || !ctx) return 0;
    if (active.load(std::memory_order_acquire)) return 0;
    const uint64_t token = serial.fetch_add(1, std::memory_order_relaxed) + 1;
    context.store(ctx, std::memory_order_relaxed);
    owner.store(ownerThread, std::memory_order_relaxed);
    serial.store(token, std::memory_order_relaxed);
    active.store(true, std::memory_order_release);
    return token;
}

inline void End(uint64_t token) noexcept
{
    if (token && serial.load(std::memory_order_relaxed) == token) active.store(false, std::memory_order_release);
}

class NativeDrawScope
{
  public:
    NativeDrawScope(ID3D11DeviceContext* ctx, const char* kind, DWORD callerThread = GetCurrentThreadId()) noexcept : _ctx(ctx), _kind(kind)
    {
        if (!enabled.load(std::memory_order_relaxed) || !ctx || !active.load(std::memory_order_acquire)) return;
        _token = serial.load(std::memory_order_relaxed);
        _owner = owner.load(std::memory_order_relaxed);
        _caller = callerThread;
        if (context.load(std::memory_order_relaxed) != ctx || _caller == _owner) _token = 0;
    }
    ~NativeDrawScope()
    {
        if (_token && active.load(std::memory_order_acquire) && serial.load(std::memory_order_relaxed) == _token &&
            context.load(std::memory_order_relaxed) == _ctx && owner.load(std::memory_order_relaxed) == _owner)
        {
            lastOwner.store(_owner, std::memory_order_relaxed);
            lastCaller.store(_caller, std::memory_order_relaxed);
            lastKind.store(_kind, std::memory_order_relaxed);
            count.fetch_add(1, std::memory_order_release);
        }
    }
    NativeDrawScope(const NativeDrawScope&) = delete;
    NativeDrawScope& operator=(const NativeDrawScope&) = delete;
  private:
    ID3D11DeviceContext* _ctx = nullptr;
    const char* _kind = nullptr;
    DWORD _owner = 0, _caller = 0;
    uint64_t _token = 0;
};

inline Stats Read() noexcept
{
    return { count.load(std::memory_order_acquire), serial.load(std::memory_order_relaxed), lastOwner.load(std::memory_order_relaxed),
             lastCaller.load(std::memory_order_relaxed), lastKind.load(std::memory_order_relaxed) };
}
} // namespace ext::core::state_window
