#pragma once
// The two calls out of the live Env (WindowModule.cpp) whose ORDER or ARGUMENTS matter, and the module lock scope around them,
// as templates over the type they call, so a test drives the very same code with fakes that record what they were asked. No
// Windows or upstream types here.

#include <cassert>
#include <cstdint>

namespace ext::window::live
{
// What upstream's hkSetFullscreenState does first while frame generation is active (hooks/FG_Hooks.cpp, hkSetFullscreenState):
// fgChanged = true, then UpdateTarget(), then Deactivate(), in that order. The module asks for the same thing, so the
// feature goes off exactly as it would for a call that is not held back. No feature object: nothing is touched.
template <class Fg>
void RequestOff(Fg* fg, bool& fgChanged)
{
    if (fg == nullptr)
        return;
    fgChanged = true;
    fg->UpdateTarget();
    fg->Deactivate();
}

inline constexpr int32_t kPointerError = static_cast<int32_t>(0x80004003u); // E_POINTER

// IDXGISwapChain::SetFullscreenState(fullscreen, nullptr) on the swapchain ReShade passed (get_native()), not through
// ReShade's wrapper. The target is null on purpose: upstream then restyles for the window's own monitor (the hold only happens
// when the call's target was null or that monitor). A null swapchain is an error, not a call.
template <class Chain>
int32_t ReplaySetFullscreen(Chain* chain, bool fullscreen)
{
    if (chain == nullptr)
        return kPointerError;
    return static_cast<int32_t>(chain->SetFullscreenState(fullscreen ? 1 : 0, nullptr));
}

// ---- the module lock around the calls out (L2) ------------------------------------------------------------------------------

// Entry points take the module's recursive mutex once per call and count it in `depth`, a per-thread number: how often THIS
// thread holds the mutex. An entry point's guard raises it for the length of the call.
class LockDepthGuard
{
  public:
    explicit LockDepthGuard(int& depth) : _depth(depth) { ++_depth; }
    ~LockDepthGuard() { --_depth; }
    LockDepthGuard(const LockDepthGuard&) = delete;
    LockDepthGuard& operator=(const LockDepthGuard&) = delete;

  private:
    int& _depth;
};

// Gives the mutex up completely around a call out (the game's window procedure can run inside it and wait for another thread),
// and takes it back as often as it was held. While it lives the thread holds the mutex NO times, so `depth` is 0 too: an entry
// point that the call out re-enters (a nested resize, a present end) counts from 0, and a call out of that nested entry gives up
// exactly the one level it owns. (Leaving `depth` as it was made the nested scope unlock once more than the thread owned, which
// is undefined behaviour on a recursive mutex and leaves it locked for every other thread afterwards.)
template <class Mutex>
class UnlockedScope
{
  public:
    UnlockedScope(Mutex& mutex, int& depth) : _mutex(mutex), _depth(depth), _held(depth)
    {
        for (int i = 0; i < _held; ++i)
            _mutex.unlock();
        _depth = 0;
    }
    ~UnlockedScope()
    {
        assert(_depth == 0 && "a nested entry left the lock depth raised");
        for (int i = 0; i < _held; ++i)
            _mutex.lock();
        _depth = _held;
    }
    UnlockedScope(const UnlockedScope&) = delete;
    UnlockedScope& operator=(const UnlockedScope&) = delete;

  private:
    Mutex& _mutex;
    int& _depth;
    int _held;
};
} // namespace ext::window::live