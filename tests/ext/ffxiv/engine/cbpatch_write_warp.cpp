// Engine pass overrides: the CPU-written mode of the constant-buffer patch (ext/ffxiv/engine/CbPatch.cpp) on the D3D11 WARP software adapter.
//
// What a player would feel if it broke: a draw reads this frame's values once per frame (not once per draw), a frame whose target equals the
// engine's value swaps nothing at all, and the game's own buffer is bound again after the scope - also when the draw threw.
#include "check.h"

#include "ext/ffxiv/engine/CbPatch.h"

#include <d3d11.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>
#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::engine;

namespace
{
constexpr uint32_t kSlot = 3;       // the slot the game binds the buffer at in this test
constexpr uint32_t kWetBytes = 32;  // g_WetnessParameter's declared size
constexpr const char* kName = "g_WetnessParameter";

struct Scene
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;

    bool Create()
    {
        return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                                           &context)) &&
               device && context;
    }

    ComPtr<ID3D11Buffer> ConstantBuffer(const void* bytes, uint32_t byteWidth)
    {
        D3D11_BUFFER_DESC desc {};
        desc.ByteWidth = byteWidth;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA initial { bytes, 0, 0 };
        ComPtr<ID3D11Buffer> buffer;
        if (FAILED(device->CreateBuffer(&desc, &initial, &buffer)))
            return {};
        return buffer;
    }

    // The buffer bound at `slot` right now (the caller releases it).
    ID3D11Buffer* Bound(uint32_t slot)
    {
        ID3D11Buffer* buffer = nullptr;
        context->PSGetConstantBuffers(slot, 1, &buffer);
        return buffer;
    }
};
} // namespace

int main()
{
    Scene scene;
    CHECK(scene.Create());
    if (!scene.context)
        return Finish("cbpatch_write");

    float gameValues[8] { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f };
    float targetValues[8] { 1.0f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f };
    ComPtr<ID3D11Buffer> game = scene.ConstantBuffer(gameValues, kWetBytes);
    CHECK(game != nullptr);
    if (!game)
        return Finish("cbpatch_write");
    ID3D11Buffer* bound[4] {};
    bound[kSlot] = game.Get();
    scene.context->PSSetConstantBuffers(0, 4, bound);

    CbPatch patch;

    // ---- the swap: the draw sees this module's values, the game's buffer comes back afterwards. The first request is frame 0: a buffer
    // that has never been written must be written in frame 0, which the frame token alone cannot tell from a buffer written in frame 0.
    {
        CpuWriteRequest request { kName, kWetBytes, targetValues, /*frame*/ 0, /*equal*/ false };
        ID3D11Buffer* ours = patch.Write(scene.context.Get(), request);
        CHECK(ours != nullptr);
        CHECK(ours != game.Get());
        CHECK_EQ(patch.Writes(), 1u);
        {
            BoundConstantBuffer guard(scene.context.Get(), kSlot, ours);
            CHECK(guard.Bound());
            ID3D11Buffer* inside = scene.Bound(kSlot);
            CHECK(inside == ours);
            if (inside)
                inside->Release();
        }
        ID3D11Buffer* after = scene.Bound(kSlot);
        CHECK(after == game.Get());
        if (after)
            after->Release();
    }

    // ---- once per frame: a second draw of the same frame reuses the buffer without a second write
    {
        CpuWriteRequest request { kName, kWetBytes, targetValues, 0, false };
        ID3D11Buffer* again = patch.Write(scene.context.Get(), request);
        CHECK(again != nullptr);
        CHECK_EQ(patch.Writes(), 1u);
        CHECK_EQ(patch.SizesCached(), 0u); // the written buffer is not one of the three-buffer size caches
        request.frame = 2;
        CHECK(patch.Write(scene.context.Get(), request) != nullptr);
        CHECK_EQ(patch.Writes(), 2u);
    }

    // ---- the equality gate: a frame whose target equals the engine's value swaps nothing
    {
        const uint32_t writesBefore = patch.Writes();
        CpuWriteRequest request { kName, kWetBytes, gameValues, 3, /*equal*/ true };
        CHECK(patch.Write(scene.context.Get(), request) == nullptr);
        CHECK_EQ(patch.Writes(), writesBefore);
    }

    // ---- a request the primitive cannot honour hands back null, and the caller is expected to draw the game's call
    {
        CpuWriteRequest noName { nullptr, kWetBytes, targetValues, 4, false };
        CHECK(patch.Write(scene.context.Get(), noName) == nullptr);
        CHECK(patch.Failure() != nullptr);
        CpuWriteRequest noBytes { kName, kWetBytes, nullptr, 4, false };
        CHECK(patch.Write(scene.context.Get(), noBytes) == nullptr);
        CpuWriteRequest oddSize { kName, 33, targetValues, 4, false };
        CHECK(patch.Write(scene.context.Get(), oddSize) == nullptr);
        CpuWriteRequest tooBig { kName, 32768, targetValues, 4, false };
        CHECK(patch.Write(scene.context.Get(), tooBig) == nullptr);
        CpuWriteRequest noContext { kName, kWetBytes, targetValues, 4, false };
        CHECK(patch.Write(nullptr, noContext) == nullptr);
    }

    // ---- the guard restores on a throw: the game's binding is back after an unwind through the draw
    {
        CpuWriteRequest request { kName, kWetBytes, targetValues, 5, false };
        ID3D11Buffer* ours = patch.Write(scene.context.Get(), request);
        CHECK(ours != nullptr);
        bool threw = false;
        try
        {
            BoundConstantBuffer guard(scene.context.Get(), kSlot, ours);
            throw std::runtime_error("the draw failed");
        }
        catch (const std::exception&)
        {
            threw = true;
        }
        CHECK(threw);
        ID3D11Buffer* after = scene.Bound(kSlot);
        CHECK(after == game.Get());
        if (after)
            after->Release();
    }

    // ---- a guard with no replacement binds nothing and says so: the game's binding was never touched
    {
        BoundConstantBuffer guard(scene.context.Get(), kSlot, nullptr);
        CHECK(!guard.Bound());
        ID3D11Buffer* after = scene.Bound(kSlot);
        CHECK(after == game.Get());
        if (after)
            after->Release();
    }

    // ---- the values the draw reads are the caller's, byte for byte
    {
        float other[8] { 9, 8, 7, 6, 5, 4, 3, 2 };
        CpuWriteRequest request { kName, kWetBytes, other, 6, false };
        ID3D11Buffer* ours = patch.Write(scene.context.Get(), request);
        CHECK(ours != nullptr);
        if (ours)
        {
            // read the owned buffer back through a staging copy: a DYNAMIC constant buffer cannot be mapped for reading
            D3D11_BUFFER_DESC desc {};
            desc.ByteWidth = kWetBytes;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Buffer> staging;
            CHECK(SUCCEEDED(scene.device->CreateBuffer(&desc, nullptr, &staging)));
            if (staging)
            {
                scene.context->CopyResource(staging.Get(), ours);
                D3D11_MAPPED_SUBRESOURCE mapped {};
                CHECK(SUCCEEDED(scene.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
                if (mapped.pData != nullptr)
                {
                    CHECK(std::memcmp(mapped.pData, other, kWetBytes) == 0);
                    scene.context->Unmap(staging.Get(), 0);
                }
            }
        }
    }

    patch.Release();
    return Finish("cbpatch_write");
}
