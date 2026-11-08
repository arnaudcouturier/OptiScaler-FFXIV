// Engine pass overrides: the GPU set mode of the constant-buffer patch (ext/ffxiv/engine/CbPatch.cpp) on the D3D11 WARP software adapter.
//
// The contract a player would feel if it broke: the wetness patch replaces the two components the engine's wetness knob owns - the wetness amount
// and the dryness blend that follows it - and every other byte of the engine's 32-byte buffer comes out bit for bit as it went in. The fixture
// puts a NaN payload and a denormal in the .zw of the first float4 and more of them in the second one, and compares the whole buffer with
// memcmp: a shader that converted the whole float4 would flush or canonicalise those bits, and a float comparison would not see it.
//
// The engine's own buffer is never written, and a request the primitive cannot honour hands back null so the game's draw runs untouched. The
// last case patches a DYNAMIC buffer twice with new bytes in between (Map(WRITE_DISCARD)), which is the shape a per-frame game constant buffer
// has: the second result must carry the second frame's unmasked words.
#include "check.h"

#include "ext/ffxiv/engine/CbPatch.h"

#include <d3d11.h>

#include <cstdio>
#include <cstring>
#include <vector>
#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::engine;

namespace
{
constexpr uint32_t kBytes = 32; // g_WetnessParameter: 8 floats, the engine's own wetness buffer

float Bits(uint32_t bits)
{
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

// The bytes the engine's buffer holds: a wetness amount and dryness blend the knob overwrites, and six values a conversion would change - a quiet
// NaN with a payload, the smallest denormal, the smallest normal, a NaN filling every bit, negative zero and an infinity.
std::vector<float> SourceValues()
{
    return {
        0.46f,             // the wetness amount of a ramping weather
        0.77f,             // the engine's dryness blend for it, 1 - 0.46/2
        Bits(0x7FC00001u), // .z: a quiet NaN with a payload
        Bits(0x00000001u), // .w: the smallest denormal
        Bits(0x00800000u), // .x of the second float4: the smallest normal
        Bits(0xFFFFFFFFu), // .y: a NaN whose payload fills every bit
        Bits(0x80000000u), // .z: negative zero
        Bits(0x7F800000u), // .w: an infinity
    };
}

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

    // The bytes of a buffer, read back through a staging copy.
    bool ReadBack(ID3D11Buffer* buffer, uint32_t byteWidth, std::vector<unsigned char>& out)
    {
        D3D11_BUFFER_DESC desc {};
        desc.ByteWidth = byteWidth;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> staging;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &staging)))
            return false;
        context->CopyResource(staging.Get(), buffer);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)) || mapped.pData == nullptr)
            return false;
        out.assign(static_cast<unsigned char*>(mapped.pData), static_cast<unsigned char*>(mapped.pData) + byteWidth);
        context->Unmap(staging.Get(), 0);
        return true;
    }
};
} // namespace

int main()
{
    Scene scene;
    CHECK(scene.Create());
    if (!scene.context)
        return Finish("cbpatch_set");

    const std::vector<float> source = SourceValues();
    BYTE sourceBytes[kBytes] {};
    std::memcpy(sourceBytes, source.data(), kBytes);
    ComPtr<ID3D11Buffer> game = scene.ConstantBuffer(sourceBytes, kBytes);
    CHECK(game != nullptr);
    if (!game)
        return Finish("cbpatch_set");

    CbPatch patch;

    // ---- the wetness entry: .x and .y replaced with the caller's exact bits, the other six words bit for bit the engine's
    {
        const SetEntry entry = WetnessEntry(1.0f);
        ID3D11Buffer* patched = patch.Set(scene.context.Get(), game.Get(), kBytes, &entry, 1);
        CHECK(patched != nullptr);
        CHECK(patch.Failure() == nullptr);
        CHECK_EQ(patch.Sets(), 1u);
        CHECK_EQ(patch.Scales(), 0u);
        CHECK_EQ(patch.SizesCached(), 1u);
        if (patched != nullptr)
        {
            std::vector<unsigned char> bytes;
            CHECK(scene.ReadBack(patched, kBytes, bytes));
            CHECK_EQ(bytes.size(), size_t(kBytes));
            // the expected buffer: the engine's bytes, with the two masked components written as the entry's bits
            BYTE expected[kBytes] {};
            std::memcpy(expected, sourceBytes, kBytes);
            std::memcpy(expected + 0, &entry.value[0], sizeof entry.value[0]);
            std::memcpy(expected + 4, &entry.value[1], sizeof entry.value[1]);
            CHECK(bytes.size() == size_t(kBytes) && std::memcmp(bytes.data(), expected, kBytes) == 0);
            // the patch did something: a pass that copied the buffer would pass the comparison above if the entry were the engine's own
            CHECK(std::memcmp(bytes.data(), sourceBytes, kBytes) != 0);
            // and the value the draw reads is the caller's, exactly
            CHECK_EQ(bytes[0], entry.value[0] & 0xFFu);
            CHECK_EQ(bytes[1], (entry.value[0] >> 8) & 0xFFu);
            CHECK_EQ(bytes[4], entry.value[1] & 0xFFu);
            CHECK_EQ(bytes[5], (entry.value[1] >> 8) & 0xFFu);
        }
    }

    // ---- a half-wet amount: the dryness blend follows the engine's own rule, 1 - w/2, and nothing else moves
    {
        const SetEntry entry = WetnessEntry(0.46f);
        ID3D11Buffer* patched = patch.Set(scene.context.Get(), game.Get(), kBytes, &entry, 1);
        CHECK(patched != nullptr);
        CHECK_EQ(patch.Sets(), 2u);
        std::vector<unsigned char> bytes;
        CHECK(scene.ReadBack(patched, kBytes, bytes));
        BYTE expected[kBytes] {};
        std::memcpy(expected, sourceBytes, kBytes);
        std::memcpy(expected + 0, &entry.value[0], sizeof entry.value[0]);
        std::memcpy(expected + 4, &entry.value[1], sizeof entry.value[1]);
        CHECK(bytes.size() == size_t(kBytes) && std::memcmp(bytes.data(), expected, kBytes) == 0);
    }

    // ---- an entry that addresses the second float4 only: the first one keeps its NaN payload and its denormal
    {
        SetEntry entry;
        entry.byteOffset = 16;
        entry.stride = 16;
        entry.count = 1;
        entry.mask = kComponentZ | kComponentW;
        entry.value[2] = std::bit_cast<uint32_t>(2.5f);
        entry.value[3] = std::bit_cast<uint32_t>(-3.0f);
        ID3D11Buffer* patched = patch.Set(scene.context.Get(), game.Get(), kBytes, &entry, 1);
        CHECK(patched != nullptr);
        CHECK_EQ(patch.Sets(), 3u);
        std::vector<unsigned char> bytes;
        CHECK(scene.ReadBack(patched, kBytes, bytes));
        BYTE expected[kBytes] {};
        std::memcpy(expected, sourceBytes, kBytes);
        std::memcpy(expected + 24, &entry.value[2], sizeof entry.value[2]);
        std::memcpy(expected + 28, &entry.value[3], sizeof entry.value[3]);
        CHECK(bytes.size() == size_t(kBytes) && std::memcmp(bytes.data(), expected, kBytes) == 0);
    }

    // ---- a mask of zero is a plain copy: a set entry that addresses nothing changes nothing at all
    {
        SetEntry entry = WetnessEntry(1.0f);
        entry.mask = 0;
        ID3D11Buffer* patched = patch.Set(scene.context.Get(), game.Get(), kBytes, &entry, 1);
        CHECK(patched != nullptr);
        std::vector<unsigned char> bytes;
        CHECK(scene.ReadBack(patched, kBytes, bytes));
        CHECK(bytes.size() == size_t(kBytes) && std::memcmp(bytes.data(), sourceBytes, kBytes) == 0);
    }

    // ---- the same pointer with new content: the engine's buffer is dynamic and rewritten every frame, so a second patch of the same pointer
    // must carry the new bytes in the words the entry does not mask (this also pins CopyResource from a DYNAMIC source on WARP)
    {
        D3D11_BUFFER_DESC desc {};
        desc.ByteWidth = kBytes;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        D3D11_SUBRESOURCE_DATA initial { sourceBytes, 0, 0 };
        ComPtr<ID3D11Buffer> dynamic;
        CHECK(SUCCEEDED(scene.device->CreateBuffer(&desc, &initial, &dynamic)) && dynamic != nullptr);

        const SetEntry entry = WetnessEntry(1.0f);
        ID3D11Buffer* first = patch.Set(scene.context.Get(), dynamic.Get(), kBytes, &entry, 1);
        CHECK(first != nullptr);
        std::vector<unsigned char> firstBytes;
        CHECK(scene.ReadBack(first, kBytes, firstBytes));
        BYTE expected[kBytes] {};
        std::memcpy(expected, sourceBytes, kBytes);
        std::memcpy(expected + 0, &entry.value[0], sizeof entry.value[0]);
        std::memcpy(expected + 4, &entry.value[1], sizeof entry.value[1]);
        CHECK(firstBytes.size() == size_t(kBytes) && std::memcmp(firstBytes.data(), expected, kBytes) == 0);

        // the game's next frame: Map(WRITE_DISCARD) on the same object, other angles and tuning constants in the words the entry keeps
        std::vector<float> next = SourceValues();
        next[2] = -90.0f; // the sun angle the session measured outdoors
        next[3] = 3.7f;
        next[4] = 12.0f;
        next[7] = 0.0f;
        BYTE nextBytes[kBytes] {};
        std::memcpy(nextBytes, next.data(), kBytes);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        CHECK(SUCCEEDED(scene.context->Map(dynamic.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) && mapped.pData != nullptr);
        if (mapped.pData != nullptr)
        {
            std::memcpy(mapped.pData, nextBytes, kBytes);
            scene.context->Unmap(dynamic.Get(), 0);
        }

        ID3D11Buffer* second = patch.Set(scene.context.Get(), dynamic.Get(), kBytes, &entry, 1);
        CHECK(second != nullptr);
        std::vector<unsigned char> secondBytes;
        CHECK(scene.ReadBack(second, kBytes, secondBytes));
        std::memcpy(expected, nextBytes, kBytes);
        std::memcpy(expected + 0, &entry.value[0], sizeof entry.value[0]);
        std::memcpy(expected + 4, &entry.value[1], sizeof entry.value[1]);
        CHECK(secondBytes.size() == size_t(kBytes) && std::memcmp(secondBytes.data(), expected, kBytes) == 0);
        // the copy really took the new content: the unmasked words are this frame's, not the first patch's
        CHECK(secondBytes.size() == size_t(kBytes) && secondBytes[8] == nextBytes[8] && secondBytes[9] == nextBytes[9]);
        CHECK(secondBytes.size() == size_t(kBytes) && std::memcmp(secondBytes.data() + 8, firstBytes.data() + 8, 24) != 0);
    }

    // ---- the engine's own buffer is never written
    {
        std::vector<unsigned char> after;
        CHECK(scene.ReadBack(game.Get(), kBytes, after));
        CHECK(after.size() == size_t(kBytes) && std::memcmp(after.data(), sourceBytes, kBytes) == 0);
    }

    // ---- the requests the primitive refuses: null, a reason, and nothing was dispatched
    {
        const uint32_t setsBefore = patch.Sets();
        const SetEntry entry = WetnessEntry(0.5f);

        SetEntry past = entry;
        past.count = 3; // three float4s do not fit a 32-byte buffer
        CHECK(patch.Set(scene.context.Get(), game.Get(), kBytes, &past, 1) == nullptr);
        CHECK(patch.Failure() != nullptr);

        CHECK(patch.Set(scene.context.Get(), game.Get(), kBytes, &entry, 0) == nullptr);
        CHECK(patch.Set(scene.context.Get(), game.Get(), kBytes, nullptr, 1) == nullptr);
        CHECK(patch.Set(scene.context.Get(), nullptr, kBytes, &entry, 1) == nullptr);
        CHECK(patch.Set(scene.context.Get(), game.Get(), kBytes, &entry, kMaxSetEntries + 1) == nullptr);
        // the buffer the draw bound is not the size it says, and a size that is not a constant buffer's
        CHECK(patch.Set(scene.context.Get(), game.Get(), 48, &entry, 1) == nullptr);
        CHECK(patch.Set(scene.context.Get(), game.Get(), 12, &entry, 1) == nullptr);
        CHECK_EQ(patch.Sets(), setsBefore);
    }

    // ---- the release path leaves nothing behind: a fresh set after Release creates its buffers again
    {
        patch.Release();
        CHECK_EQ(patch.SizesCached(), 0u);
        const SetEntry entry = WetnessEntry(0.2f);
        CHECK(patch.Set(scene.context.Get(), game.Get(), kBytes, &entry, 1) != nullptr);
        CHECK_EQ(patch.SizesCached(), 1u);
    }

    return Finish("cbpatch_set");
}
