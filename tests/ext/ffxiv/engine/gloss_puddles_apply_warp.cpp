// Puddles: the two things this module does to a game object - the stamp it writes on a shader and the one slot swap a reader draw gets
// (ext/ffxiv/engine/GlossPuddlesSwap.cpp) - on a real D3D11 device.
//
// The stamp is the whole of what the module remembers about a shader, so the failure that matters is the one a bounded table produced: an entry
// lost because the game created more shaders afterwards. This test stamps one shader with the slots its reflection named, creates five thousand
// more pixel shaders, and reads the first one's stamp back; a shader created and never stamped reads the empty word. The product's cost per
// reader draw is the swap, and the two things that would hurt in a session are that the game's own view does not come back and that a reference
// leaks or is released twice while hundreds of draws a frame run through it. So the test also binds a texture at a slot, swaps it the way the
// draw path does, and then checks the slot and the object's reference count after the guard goes out of scope - and again after an exception
// unwinds through the guard, which is what a throwing game draw does.
//
// GlossPuddlesSwap.cpp is compiled on its own here: it needs nothing but D3D11 and the two small types it serves, so the stamp and the swap with
// its restore are pinned without the module's config, log and pass around them.
#include "pch.h" // the test build's stand-in: Windows, D3D11 and the fork's logging macros

#include "check.h"

#include "ext/ffxiv/engine/GlossPuddlesApply.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::engine::gloss;

namespace
{
constexpr uint32_t kSlot = 2;
constexpr uint32_t kSize = 32;
constexpr uint32_t kLaterShaders = 5000; // more shaders than the game creates of one family in a session

// The smallest pixel shader the effect compiler will take: the object is all the stamp needs, and five thousand of them are what a session's
// material shaders look like to it.
constexpr char kShaderSource[] = "float4 main() : SV_Target0 { return 0.0; }";

struct Harness
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;

    bool Create()
    {
        const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
        return SUCCEEDED(hr) && device != nullptr && context != nullptr;
    }

    ComPtr<ID3D11PixelShader> Shader()
    {
        ComPtr<ID3DBlob> code;
        ComPtr<ID3DBlob> errors;
        if (FAILED(D3DCompile(kShaderSource, sizeof kShaderSource, nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &code, &errors)))
            return nullptr;
        ComPtr<ID3D11PixelShader> shader;
        if (FAILED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader)))
            return nullptr;
        return shader;
    }

    ComPtr<ID3D11Texture2D> Texture()
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = kSize;
        desc.Height = kSize;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture)))
            return nullptr;
        return texture;
    }
};

// The reference count of a COM object, without WRL's own helpers.
uint32_t RefCount(IUnknown* object)
{
    object->AddRef();
    const uint32_t count = object->Release();
    return count;
}
} // namespace

int main()
{
    Harness harness;
    if (!harness.Create())
    {
        std::printf("gloss_puddles_apply_warp: no D3D11 WARP device\n");
        return 1;
    }

    // ---- the stamp: the slots a shader declares survive every other shader the game creates, and an unstamped one reads nothing
    {
        // Built field by field: the stamp carries the view position's slot between the camera and the four targets, so a braced list would have to
        // count the fields in the header's order.
        Inputs inputs;
        inputs.depth = 0;
        inputs.camera = 1;
        for (uint32_t t = 0; t < kTargetCount; ++t)
            inputs.targets[t] = uint8_t(4 + t);
        const uint64_t packed = PackInputs(inputs);
        CHECK(packed != 0);
        {
            const ComPtr<ID3D11PixelShader> shader = harness.Shader();
            CHECK(shader != nullptr);
            CHECK(detail::StampShader(shader.Get(), packed));
            CHECK_EQ(detail::ReadStamp(shader.Get()), packed);
            // A stamp of 0 is not written: it would make an unstamped shader indistinguishable from a read one at the same cost.
            CHECK(!detail::StampShader(shader.Get(), 0));
            CHECK_EQ(detail::ReadStamp(shader.Get()), packed);
        }
        // Five thousand more shaders, as a session creates: the stamp lives on the object, so none of them can take it.
        std::vector<ComPtr<ID3D11PixelShader>> later;
        later.reserve(kLaterShaders);
        for (uint32_t i = 0; i < kLaterShaders; ++i)
        {
            const ComPtr<ID3D11PixelShader> shader = harness.Shader();
            if (shader == nullptr)
                break;
            later.push_back(shader);
        }
        CHECK_EQ(later.size(), size_t(kLaterShaders));
        CHECK(later.empty() || detail::ReadStamp(later.front().Get()) == 0);
        {
            const ComPtr<ID3D11PixelShader> shader = harness.Shader();
            CHECK(detail::StampShader(shader.Get(), packed));
            CHECK_EQ(detail::ReadStamp(shader.Get()), packed);
            CHECK_EQ(detail::ReadStamp(nullptr), 0ull);
        }
        // A shader that was created and never stamped reads the empty word, which is "not a draw this module knows".
        const ComPtr<ID3D11PixelShader> plain = harness.Shader();
        CHECK_EQ(detail::ReadStamp(plain.Get()), 0ull);
    }

    // ---- the swap: the game's view comes back, with the reference this module took, and an exception through the guard restores the same way
    {
        const ComPtr<ID3D11Texture2D> game = harness.Texture();
        const ComPtr<ID3D11Texture2D> mine = harness.Texture();
        CHECK(game != nullptr && mine != nullptr);
        ComPtr<ID3D11ShaderResourceView> gameView, mineView;
        CHECK(SUCCEEDED(harness.device->CreateShaderResourceView(game.Get(), nullptr, &gameView)));
        CHECK(SUCCEEDED(harness.device->CreateShaderResourceView(mine.Get(), nullptr, &mineView)));
        const uint32_t gameRefs = RefCount(gameView.Get());

        ID3D11ShaderResourceView* bound[1] { gameView.Get() };
        harness.context->PSSetShaderResources(kSlot, 1, bound);

        {
            ID3D11ShaderResourceView* replaced = detail::SwapSlot(harness.context.Get(), kSlot, mineView.Get());
            CHECK(replaced == gameView.Get());
            // The swap hands the caller the game's view with one reference, and that reference is the guard's to give back.
            CHECK_EQ(RefCount(gameView.Get()), gameRefs + 1);
            BoundReader guard(harness.context.Get(), kSlot, replaced);
            CHECK(guard.Bound());
            ComPtr<ID3D11ShaderResourceView> current;
            harness.context->PSGetShaderResources(kSlot, 1, &current);
            CHECK(current.Get() == mineView.Get()); // the reader draw sees this module's texture
            current.Reset();
        }
        // The guard is gone: the game's own view is back, and no reference was leaked or released twice.
        ComPtr<ID3D11ShaderResourceView> restored;
        harness.context->PSGetShaderResources(kSlot, 1, &restored);
        CHECK(restored.Get() == gameView.Get());
        restored.Reset(); // the read above took a reference of its own
        CHECK_EQ(RefCount(gameView.Get()), gameRefs);

        // The same through an exception: the destructor restores on the way out of a throwing draw.
        bool threw = false;
        try
        {
            ID3D11ShaderResourceView* replaced = detail::SwapSlot(harness.context.Get(), kSlot, mineView.Get());
            BoundReader guard(harness.context.Get(), kSlot, replaced);
            throw 7;
        }
        catch (int)
        {
            threw = true;
        }
        CHECK(threw);
        ComPtr<ID3D11ShaderResourceView> afterThrow;
        harness.context->PSGetShaderResources(kSlot, 1, &afterThrow);
        CHECK(afterThrow.Get() == gameView.Get());
        afterThrow.Reset();
        CHECK_EQ(RefCount(gameView.Get()), gameRefs);

        // An empty slot and a null replacement are no swap at all, and the guard then reports that it has nothing to restore. The empty slot is
        // the case that matters: a draw that binds nothing under the target's name must not have this module's texture put there, because a
        // replaced-nothing swap would hand that draw a reflection it never asked for and leave it bound past the draw.
        ID3D11ShaderResourceView* none[1] { nullptr };
        harness.context->PSSetShaderResources(kSlot, 1, none);
        const uint32_t mineRefs = RefCount(mineView.Get());
        CHECK(detail::SwapSlot(harness.context.Get(), kSlot, mineView.Get()) == nullptr);
        ComPtr<ID3D11ShaderResourceView> afterEmpty;
        harness.context->PSGetShaderResources(kSlot, 1, &afterEmpty);
        CHECK(afterEmpty.Get() == nullptr); // the slot is still empty: nothing was swapped in
        afterEmpty.Reset();
        CHECK_EQ(RefCount(mineView.Get()), mineRefs); // and this module's view was not bound, so no reference was taken for it
        CHECK(detail::SwapSlot(harness.context.Get(), kSlot, nullptr) == nullptr);
        CHECK(detail::SwapSlot(nullptr, kSlot, mineView.Get()) == nullptr);
        BoundReader empty;
        CHECK(!empty.Bound());
        BoundReader fromNothing(harness.context.Get(), kSlot, nullptr);
        CHECK(!fromNothing.Bound()); // a guard with no view to put back binds nothing and restores nothing
    }

    return Finish("gloss_puddles_apply_warp");
}
