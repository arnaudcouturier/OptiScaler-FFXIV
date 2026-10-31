#pragma once
// The two lines a hook body needs for the rebind (HookLifecycle.h), and nothing else.
//
// The whole upstream change a hook body takes is these two statements, at the top of the body:
//
//     const ext::fg::outswitch::HookScope scope { ext::fg::outswitch::HookGateFor() };
//     if (!scope) return scope.Pass([&] { return o_FGSCPresent(This, SyncInterval, Flags); }, S_OK);
//
// The first asks the gate how this call must behave. The second runs the pass-through when the gate is closed: it calls
// the original for this slot and returns its result while a detach is still draining, and returns the slot's fixed
// answer once the detach has restored the originals (SwitchOpsContract.h lists both for all ten bodies). A slot whose
// refused answer is out-parameters rather than a value (the two Get bodies) passes a callable instead of a value, and it
// is run only on the refused path, where the original is gone and nobody else will write them:
//
//     if (!scope) return scope.Pass([&] { return o_FGSCGetFullscreenState(This, pFullscreen, ppTarget); },
//                                   [&] { if (pFullscreen != nullptr) *pFullscreen = FALSE;
//                                         if (ppTarget != nullptr) *ppTarget = nullptr; return S_OK; });
// `Leave()` / `PassDone()` run in the destructor, so a body that returns from anywhere in the middle cannot forget them;
// that is why the scope is an object and not a pair of calls.
//
// The gate is one object for the process, not one per class: only one swapchain class is hooked at a time, all ten slots
// are attached and detached as one Detours transaction, and the next class's slots are written only after the detach
// completed. HookGateFor takes the class only so a later design that needs per-class gates can change one function.
//
// Pure: no Windows or upstream include (tests/ext/fg/fg_switch_runtime.cpp, fg_switch_com_sim.cpp).

#include "ext/fg/outswitch/HookLifecycle.h"
#include "ext/fg/outswitch/SwitchTypes.h"

#include <type_traits>

namespace ext::fg::outswitch
{
// The process-wide gate every hook body of the module uses: one per hook set, not one per class or per slot (see above).
inline HookGate& HookGateFor()
{
    static HookGate gate;
    return gate;
}

// `cls` names the class the caller believes is hooked; it is not part of the answer today.
inline HookGate& HookGateFor(Class cls)
{
    (void)cls;
    return HookGateFor();
}

class HookScope
{
  public:
    explicit HookScope(HookGate& gate) : gate_(gate), entry_(gate.Enter()) {}
    ~HookScope()
    {
        if (entry_ == GateEntry::Counted)
            gate_.Leave();
        else if (entry_ == GateEntry::PassThrough)
            gate_.PassDone();
    }

    HookScope(const HookScope&) = delete;
    HookScope& operator=(const HookScope&) = delete;

    // True: this call runs the hook's logic. It must not be used with Pass().
    bool Counted() const { return entry_ == GateEntry::Counted; }
    explicit operator bool() const { return Counted(); }

    // The pass-through answer for a body whose gate was closed: `call` (the original for this slot, with this call's
    // arguments) runs when a detach is still draining and the original is still valid, and its result is returned; once
    // the detach restored the originals, `fallback` answers instead. `fallback` is either the slot's fixed value or a
    // callable, which is how a slot with out-parameters (the two Get bodies) writes them: the callable runs only on the
    // refused path, so it never races the original on the pass-through path. `call` is not run on a counted scope.
    template <class Call, class Fallback> auto Pass(Call&& call, Fallback&& fallback) const -> decltype(call())
    {
        if (entry_ != GateEntry::PassThrough)
        {
            if constexpr (std::is_invocable_v<Fallback&>)
                return static_cast<decltype(call())>(fallback());
            else
                return static_cast<decltype(call())>(fallback);
        }
        return call();
    }

  private:
    HookGate& gate_;
    GateEntry entry_;
};
} // namespace ext::fg::outswitch
