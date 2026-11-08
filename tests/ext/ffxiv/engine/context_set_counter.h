#pragma once
// A test-only counter of the pipeline-state SETTERS of an immediate D3D11 context (ENG2: "at weight 0 / master off / A-B nothing of ours may rebind or change any game state").
//
// The counter patches the context's vtable (the WARP context's, in the test process only) with thunks that count a call while a Scope is open and then forward to the real
// method. The indices are the documented order of ID3D11DeviceContext's methods; Install() proves each one by calling the real setter once through the context and requiring
// exactly one count, so a wrong index fails the test loudly instead of counting nothing.
#include <d3d11.h>
#include <windows.h>

#include <cstdio>

namespace setcount
{
inline long g_counts = 0;
inline long g_open = 0; // > 0 while a Scope is open

struct Scope
{
    Scope() { ++g_open; }
    ~Scope() { --g_open; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

template <size_t Index, typename... Args> struct Thunk
{
    using Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, Args...);
    static inline Fn original = nullptr;
    static void STDMETHODCALLTYPE Call(ID3D11DeviceContext* self, Args... args)
    {
        if (g_open > 0)
            ++g_counts;
        original(self, args...);
    }
};

template <typename T> bool InstallOne(ID3D11DeviceContext* context, size_t index)
{
    void** vtable = *reinterpret_cast<void***>(context);
    DWORD old = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old))
        return false;
    if (T::original == nullptr)
    {
        T::original = reinterpret_cast<typename T::Fn>(vtable[index]);
        vtable[index] = reinterpret_cast<void*>(&T::Call);
    }
    VirtualProtect(&vtable[index], sizeof(void*), old, &old);
    return true;
}

// Installs the counting thunks and proves them. True when every index counted exactly one call of its own setter.
inline bool Install(ID3D11DeviceContext* context)
{
    const FLOAT factor[4] { 1.0f, 1.0f, 1.0f, 1.0f };
    D3D11_VIEWPORT viewport { 0, 0, 1, 1, 0, 1 };
    bool ok = true;
    auto prove = [&](const char* name, auto&& call) {
        const long before = g_counts;
        {
            Scope scope;
            call();
        }
        if (g_counts != before + 1)
        {
            std::printf("setcount: %s did not count exactly once (vtable layout mismatch)\n", name);
            ok = false;
        }
    };
    ok &= InstallOne<Thunk<8, UINT, UINT, ID3D11ShaderResourceView* const*>>(context, 8);
    prove("PSSetShaderResources", [&] { context->PSSetShaderResources(5, 0, nullptr); });
    ok &= InstallOne<Thunk<9, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT>>(context, 9);
    prove("PSSetShader", [&] {
        ID3D11PixelShader* ps = nullptr;
        context->PSGetShader(&ps, nullptr, nullptr);
        context->PSSetShader(ps, nullptr, 0);
        if (ps != nullptr)
            ps->Release();
    });
    ok &= InstallOne<Thunk<11, ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT>>(context, 11);
    prove("VSSetShader", [&] {
        ID3D11VertexShader* vs = nullptr;
        context->VSGetShader(&vs, nullptr, nullptr);
        context->VSSetShader(vs, nullptr, 0);
        if (vs != nullptr)
            vs->Release();
    });
    ok &= InstallOne<Thunk<16, UINT, UINT, ID3D11Buffer* const*>>(context, 16);
    prove("PSSetConstantBuffers", [&] { context->PSSetConstantBuffers(13, 0, nullptr); });
    ok &= InstallOne<Thunk<17, ID3D11InputLayout*>>(context, 17);
    prove("IASetInputLayout", [&] {
        ID3D11InputLayout* layout = nullptr;
        context->IAGetInputLayout(&layout);
        context->IASetInputLayout(layout);
        if (layout != nullptr)
            layout->Release();
    });
    ok &= InstallOne<Thunk<24, D3D11_PRIMITIVE_TOPOLOGY>>(context, 24);
    prove("IASetPrimitiveTopology", [&] {
        D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        context->IAGetPrimitiveTopology(&topology);
        context->IASetPrimitiveTopology(topology);
    });
    ok &= InstallOne<Thunk<33, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*>>(context, 33);
    prove("OMSetRenderTargets", [&] {
        ID3D11RenderTargetView* views[1] {};
        context->OMGetRenderTargets(1, views, nullptr);
        context->OMSetRenderTargets(1, views, nullptr);
        if (views[0] != nullptr)
            views[0]->Release();
    });
    ok &= InstallOne<Thunk<35, ID3D11BlendState*, const FLOAT*, UINT>>(context, 35);
    prove("OMSetBlendState", [&] { context->OMSetBlendState(nullptr, factor, 0xffffffffu); });
    ok &= InstallOne<Thunk<36, ID3D11DepthStencilState*, UINT>>(context, 36);
    prove("OMSetDepthStencilState", [&] { context->OMSetDepthStencilState(nullptr, 0); });
    ok &= InstallOne<Thunk<43, ID3D11RasterizerState*>>(context, 43);
    prove("RSSetState", [&] { context->RSSetState(nullptr); });
    ok &= InstallOne<Thunk<44, UINT, const D3D11_VIEWPORT*>>(context, 44);
    prove("RSSetViewports", [&] { context->RSSetViewports(1, &viewport); });
    return ok;
}
} // namespace setcount
