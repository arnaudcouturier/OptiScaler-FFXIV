#include "pch.h"

#include "ext/ffxiv/ui/UiProbe.h"

#include "ext/core/ProcessMemory.h"
#include "ext/ffxiv/lighting/FfxivLightingCapture.h"

#include <State.h>

#include <cstdio>

// The vtable half of the probe (needs the swapchain pointers of State).
namespace ext::ffxiv::ui::probe
{
namespace
{
std::string Hex(uintptr_t v)
{
    char b[32];
    snprintf(b, sizeof b, "0x%llX", static_cast<unsigned long long>(v));
    return b;
}

bool Read(uintptr_t address, void* out, size_t size) { return ext::core::ReadProcessBytes(address, out, size); }
} // namespace
namespace
{
std::string Chain(const char* label, uintptr_t entry)
{
    std::string out = std::string("    ") + label + ": entry " + Hex(entry) + "  " + Describe(entry) + "\n";
    uintptr_t at = entry;
    for (int hop = 1; hop <= 3; ++hop)
    {
        const uintptr_t next = DecodeJump(at);
        if (!next)
            break;
        out += "      inline jump hop " + std::to_string(hop) + " -> " + Hex(next) + "  " + Describe(next) + "\n";
        at = next;
    }
    return out;
}

void OneChain(std::string& out, const char* name, IUnknown* object)
{
    out += std::string("  ") + name + ": ";
    if (!object)
    {
        out += "(null)\n";
        return;
    }
    out += Hex(reinterpret_cast<uintptr_t>(object)) + "\n";

    auto vtableOf = [](IUnknown* o, uintptr_t& vt)
    {
        return Read(reinterpret_cast<uintptr_t>(o), &vt, sizeof vt) && vt != 0;
    };

    uintptr_t vt = 0;
    if (!vtableOf(object, vt))
    {
        out += "    vtable unreadable\n";
        return;
    }
    out += "    vtable " + Hex(vt) + "  " + Describe(vt) + "\n";
    uintptr_t slot8 = 0;
    if (Read(vt + 8 * sizeof(uintptr_t), &slot8, sizeof slot8))
        out += Chain("Present  (slot 8)", slot8);

    // Present1 exists only on IDXGISwapChain1: ask for that interface first (its vtable pointer may differ).
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1)
    {
        uintptr_t vt1 = 0;
        uintptr_t slot22 = 0;
        if (vtableOf(sc1, vt1) && Read(vt1 + 22 * sizeof(uintptr_t), &slot22, sizeof slot22))
        {
            if (vt1 != vt)
                out += "    IDXGISwapChain1 vtable " + Hex(vt1) + "  " + Describe(vt1) + "\n";
            out += Chain("Present1 (slot 22)", slot22);
        }
        sc1->Release();
    }
    else
        out += "    Present1: interface not supported by this object\n";
}
} // namespace

std::string VtableText()
{
    auto& st = State::Instance();
    std::string out = "== swapchain vtable owners (who runs when this object's Present is called) ==\n";
    out += "# slot 8 = IDXGISwapChain::Present, slot 22 = IDXGISwapChain1::Present1. An inline patch of the function leaves the\n"
           "# slot pointing into its original module and shows up as an 'inline jump hop' line instead.\n";
    OneChain(out, "real D3D11 swapchain (State::currentRealSwapchain)", st.currentRealSwapchain);
    OneChain(out, "D3D12 frame-generation swapchain (State::currentFGSwapchain)", st.currentFGSwapchain);
    OneChain(out, "swapchain the game uses (State::currentSwapchain)", st.currentSwapchain);
    if (st.currentWrappedSwapchain != st.currentSwapchain)
        OneChain(out, "wrapped swapchain (State::currentWrappedSwapchain)", st.currentWrappedSwapchain);

    // Backbuffer identity: the bridge copies GetBuffer(GetCurrentBackBufferIndex()); the detector compares against it.
    if (auto* chain = st.currentRealSwapchain)
    {
        Microsoft::WRL::ComPtr<IDXGISwapChain3> c3;
        UINT index = 0;
        const bool has3 = SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&c3))) && c3;
        if (has3)
            index = c3->GetCurrentBackBufferIndex();
        Microsoft::WRL::ComPtr<ID3D11Texture2D> b0, bi;
        const HRESULT r0 = chain->GetBuffer(0, IID_PPV_ARGS(&b0));
        const HRESULT ri = chain->GetBuffer(index, IID_PPV_ARGS(&bi));
        DXGI_SWAP_CHAIN_DESC d {};
        chain->GetDesc(&d);
        Microsoft::WRL::ComPtr<ID3D11Device> chainDevice, gameDevice;
        chain->GetDevice(IID_PPV_ARGS(&chainDevice));
        if (auto* context = ext::ffxiv::lighting::FfxivLightingCapture::context)
            context->GetDevice(&gameDevice);
        char b[400];
        snprintf(b, sizeof b,
                 "  backbuffer: IDXGISwapChain3=%s currentIndex=%u GetBuffer(0)=%s hr=0x%08lX GetBuffer(index)=%s hr=0x%08lX same=%s "
                 "buffers=%u swapEffect=%d format=%u %ux%u device_is_latched_context_device=%s\n",
                 has3 ? "yes" : "no", index, Hex(reinterpret_cast<uintptr_t>(b0.Get())).c_str(), static_cast<unsigned long>(r0),
                 Hex(reinterpret_cast<uintptr_t>(bi.Get())).c_str(), static_cast<unsigned long>(ri),
                 b0.Get() == bi.Get() ? "yes" : "NO", d.BufferCount, static_cast<int>(d.SwapEffect),
                 static_cast<unsigned>(d.BufferDesc.Format), d.BufferDesc.Width, d.BufferDesc.Height,
                 chainDevice.Get() && chainDevice.Get() == gameDevice.Get() ? "yes" : "NO");
        out += b;
    }
    return out;
}
} // namespace ext::ffxiv::ui::probe
