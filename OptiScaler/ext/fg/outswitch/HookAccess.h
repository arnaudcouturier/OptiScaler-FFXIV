#pragma once
// The live output switch's view of the swapchain hook set (hooks/FG_Hooks.h, `friend struct ext::Access;`), written as
// one member of ext::Access (Seams.h) because the ten `o_*` originals and their `hk*` bodies are private statics.
// Included only by fork code under ext/ (the rebind glue); upstream files never see it.
//
// The set is described once, here. The rebind does not repeat upstream's attach conditions (which slots exist, which are
// XeFG-only): it asks the pointer whether it is a Detours trampoline (HookRebind.h), so a slot that was never attached is
// skipped and a raw vtable address is never passed to DetourDetach.

#include "ext/Seams.h"

#include "ext/fg/outswitch/HookRebind.h"

#include <hooks/FG_Hooks.h>

#include <cstddef>

inline bool ext::Access::FgPresentHooked()
{
    // The present slot holds a trampoline exactly while a class is hooked: the rehook's proof, and the preflight's.
    return FGHooks::o_FGSCPresent != nullptr;
}

inline size_t ext::Access::FgHookSlots(ext::fg::outswitch::HookSlot* out, size_t maxOut)
{
    using ext::fg::outswitch::HookSlot;
    const HookSlot all[] = {
        { "FGRelease", reinterpret_cast<void**>(&FGHooks::o_FGRelease), reinterpret_cast<void*>(&FGHooks::hkFGRelease) },
        { "FGSCPresent", reinterpret_cast<void**>(&FGHooks::o_FGSCPresent), reinterpret_cast<void*>(&FGHooks::hkFGPresent) },
        { "FGSCResizeTarget", reinterpret_cast<void**>(&FGHooks::o_FGSCResizeTarget),
          reinterpret_cast<void*>(&FGHooks::hkResizeTarget) },
        { "FGSCResizeBuffers", reinterpret_cast<void**>(&FGHooks::o_FGSCResizeBuffers),
          reinterpret_cast<void*>(&FGHooks::hkResizeBuffers) },
        { "FGSCSetFullscreenState", reinterpret_cast<void**>(&FGHooks::o_FGSCSetFullscreenState),
          reinterpret_cast<void*>(&FGHooks::hkSetFullscreenState) },
        { "FGSCPresent1", reinterpret_cast<void**>(&FGHooks::o_FGSCPresent1),
          reinterpret_cast<void*>(&FGHooks::hkFGPresent1) },
        { "FGSCResizeBuffers1", reinterpret_cast<void**>(&FGHooks::o_FGSCResizeBuffers1),
          reinterpret_cast<void*>(&FGHooks::hkResizeBuffers1) },
        { "FGSCGetFullscreenState", reinterpret_cast<void**>(&FGHooks::o_FGSCGetFullscreenState),
          reinterpret_cast<void*>(&FGHooks::hkGetFullscreenState) },
        { "FGSCGetFullscreenDesc", reinterpret_cast<void**>(&FGHooks::o_FGSCGetFullscreenDesc),
          reinterpret_cast<void*>(&FGHooks::hkGetFullscreenDesc) },
        { "FGSCGetFrameLatencyWaitableObject", reinterpret_cast<void**>(&FGHooks::o_FGSCGetFrameLatencyWaitableObject),
          reinterpret_cast<void*>(&FGHooks::hkGetFrameLatencyWaitableObject) },
    };
    const size_t count = sizeof(all) / sizeof(all[0]);
    size_t written = 0;
    for (size_t i = 0; i < count && written < maxOut; ++i)
        out[written++] = all[i];
    return written;
}
