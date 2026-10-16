// S10 package B: the upstream attachment points (ext/hudless/HudlessSeams.h) are inline wrappers on the feature bit
// ext::F::Hudless. With the bit clear (mode off after the first present) every seam returns at once and never reaches the
// engine; BeforeOverlay also ignores D3D11 chains and a null queue. The detail:: bodies are counted by this test instead
// of the engine.
#include "ext/hudless/HudlessSeams.h"

#include <cassert>
#include <cstdio>

class Dx11wDx12SC
{
};

namespace
{
int calls[5] = {};
}

namespace ext::hudless::detail
{
void PreInteropCopy(Dx11wDx12SC&) { ++calls[0]; }
void UiFreeFrame(Dx11wDx12SC&, unsigned int, ID3D12Resource*&, D3D12_RESOURCE_STATES&) { ++calls[1]; }
void PostInteropCopy(Dx11wDx12SC&) { ++calls[2]; }
void OnReleaseInterop(Dx11wDx12SC&) { ++calls[3]; }
void BeforeOverlay(IDXGISwapChain*, ID3D12CommandQueue*) { ++calls[4]; }
} // namespace ext::hudless::detail

int main()
{
    Dx11wDx12SC bridge;
    ID3D12Resource* source = reinterpret_cast<ID3D12Resource*>(0x10);
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    auto* swapChain = reinterpret_cast<IDXGISwapChain*>(0x20);
    auto* queue = reinterpret_cast<ID3D12CommandQueue*>(0x30);

    auto total = [] { int n = 0; for (int c : calls) n += c; return n; };
    auto fireAll = [&]
    {
        ext::hudless::PreInteropCopy(bridge);
        ext::hudless::UiFreeFrame(bridge, 1, source, state);
        ext::hudless::PostInteropCopy(bridge);
        ext::hudless::OnReleaseInterop(bridge);
        ext::present::BeforeOverlay(swapChain, queue, false);
    };

    // bit clear: nothing reaches the engine, the in/out arguments are left alone
    assert(!ext::Active(ext::F::Hudless));
    fireAll();
    assert(total() == 0 && source == reinterpret_cast<ID3D12Resource*>(0x10) && state == D3D12_RESOURCE_STATE_COMMON);

    // bit set: every seam reaches its body exactly once
    ext::SetActive(ext::F::Hudless, true);
    fireAll();
    for (int c : calls)
        assert(c == 1);

    // the overlay seam ignores a D3D11 chain and a null queue
    ext::present::BeforeOverlay(swapChain, queue, true);
    ext::present::BeforeOverlay(swapChain, nullptr, false);
    assert(calls[4] == 1);

    // the engine clears the bit once it saw mode off: quiet again
    ext::SetActive(ext::F::Hudless, false);
    fireAll();
    for (int c : calls)
        assert(c == 1);

    std::puts("PASS: seam wrappers are inert with the feature bit clear and reach the engine exactly once with it set");
    return 0;
}
