// E5: pure pass-through decision of the CreateSwapChainForComposition hook (ext/present/CompositionPassThrough.h).
// No DXGI, no upstream types: a standalone unit test. The hook (ext/present/CompositionSwapchain.cpp) calls the
// trampoline unconditionally and returns right after it whenever this function says true, so "true" is exactly
// "trampoline only, nothing wrapped" -- the structure that keeps the Companion nameplate layer's own composition
// swapchain (FfxivNameplateLive.h Run(), created under ext::core::BypassScope) out of the wrapper.
#include "ext/core/HooksBypass.h"
#include "ext/present/CompositionPassThrough.h"

#include <cassert>
#include <cstdio>

using namespace ext::present;

namespace
{
// Every input in its "a real game composition swapchain" state: nothing bypassed, device/desc/out given, 1920x1080.
struct Inputs
{
    bool hooksBypassed = false;
    bool vulkanCreatingSC = false;
    bool skipFgSwapChain = false;
    bool hasDevice = true;
    bool hasDesc = true;
    bool hasOutParam = true;
    unsigned int width = 1920;
    unsigned int height = 1080;
};

bool Eval(const Inputs& in)
{
    return CompositionPassThrough(in.hooksBypassed, in.vulkanCreatingSC, in.skipFgSwapChain, in.hasDevice, in.hasDesc,
                                  in.hasOutParam, in.width, in.height);
}
} // namespace

int main()
{
    // A normal, large composition swapchain with no guard active is wrapped: not a pass-through.
    assert(!Eval(Inputs {}));

    // The Companion case, in isolation: every other input is the normal wrapped case, only the bypass is active.
    {
        Inputs in;
        in.hooksBypassed = true;
        assert(Eval(in));
    }

    // Each boolean guard on its own.
    {
        Inputs in;
        in.vulkanCreatingSC = true;
        assert(Eval(in));
    }
    {
        Inputs in;
        in.skipFgSwapChain = true;
        assert(Eval(in));
    }

    // Null arguments. A null desc has no usable extent, so the extent inputs must not matter.
    {
        Inputs in;
        in.hasDevice = false;
        assert(Eval(in));
    }
    {
        Inputs in;
        in.hasDesc = false;
        assert(Eval(in));
    }
    {
        Inputs in;
        in.hasDesc = false;
        in.width = 4096;
        in.height = 4096;
        assert(Eval(in));
    }
    {
        Inputs in;
        in.hasOutParam = false;
        assert(Eval(in));
    }

    // Overlay/helper chains: either extent below 100, 100x100 is the first size that is wrapped.
    {
        Inputs in;
        in.width = 99;
        assert(Eval(in));
    }
    {
        Inputs in;
        in.height = 99;
        assert(Eval(in));
    }
    {
        Inputs in;
        in.width = 0;
        in.height = 0;
        assert(Eval(in));
    }
    {
        Inputs in;
        in.width = 100;
        in.height = 100;
        assert(!Eval(in));
    }
    {
        Inputs in;
        in.width = 100;
        in.height = 99;
        assert(Eval(in));
    }

    // Guards win over a good extent and combine freely.
    {
        Inputs in;
        in.hooksBypassed = true;
        in.vulkanCreatingSC = true;
        in.skipFgSwapChain = true;
        in.width = 100;
        in.height = 100;
        assert(Eval(in));
    }

    // The bypass flag the hook feeds in is the real ext::core one: false by default, true inside a BypassScope on
    // this thread, restored on exit, and nested scopes restore the outer state (the Companion's Run() holds one scope
    // for its whole body). Wiring check of the actual input, not just of the boolean function.
    {
        assert(!ext::core::HooksBypassed());
        Inputs in;
        in.hooksBypassed = ext::core::HooksBypassed();
        assert(!Eval(in));
        {
            ext::core::BypassScope outer;
            in.hooksBypassed = ext::core::HooksBypassed();
            assert(Eval(in));
            {
                ext::core::BypassScope inner;
                assert(ext::core::HooksBypassed());
            }
            in.hooksBypassed = ext::core::HooksBypassed();
            assert(Eval(in));
        }
        in.hooksBypassed = ext::core::HooksBypassed();
        assert(!Eval(in));
    }

    std::puts("PASS: composition pass-through decision, including the Companion bypass case");
    return 0;
}
