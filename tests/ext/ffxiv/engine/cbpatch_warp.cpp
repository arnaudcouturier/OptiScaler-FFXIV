// Engine pass overrides: the GPU-scale mode of the constant-buffer patch (ext/ffxiv/engine/CbPatch.cpp) on the D3D11 WARP software adapter.
//
// The contract a player would feel if it broke: the patched buffer carries the scaled float4s and every byte the scale does not address comes
// out bit for bit as it went in, the game's own buffer is never written, and a request the primitive cannot honour hands back null so the
// game's draw runs untouched.
//
// The bit-identical half is why the fixture seeds denormals, NaN payloads and integer-as-float values beside the knob: a shader that converted
// the whole float4 (or multiplied all four components) would flush or canonicalise them, and a float comparison on 2, -3 and 1000 would not
// see it.
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
constexpr uint32_t kBytes = 16384;
constexpr uint32_t kFloat4s = kBytes / 16;

// A float from its bit pattern, so the fixture can hold values a conversion would change.
float Bits(uint32_t bits)
{
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

// The components beside the knob: small and normal, a denormal (asfloat(1)), a quiet NaN with a payload, a signalling NaN, the smallest
// normal, a NaN whose payload fills every bit, negative zero, and an infinity.
const std::vector<float>& Specials()
{
    static const std::vector<float> values {
        2.0f,
        -3.0f,
        Bits(0x00000001u), // the smallest denormal, asfloat(1)
        Bits(0x7FC00001u), // a quiet NaN with a payload
        Bits(0x7F800001u), // a signalling NaN payload
        Bits(0x00800000u), // the smallest normal
        Bits(0xFFFFFFFFu), // a NaN whose payload fills every bit
        Bits(0x80000000u), // negative zero
    };
    return values;
}

// The values the game's buffer holds: a distinct, exactly representable .x per float4 (the component the knob scales), and the other three
// components cycling through the special values so every one of them lands beside the knob.
std::vector<float> SourceValues(uint32_t byteWidth)
{
    const std::vector<float>& special = Specials();
    const uint32_t float4s = byteWidth / 16;
    std::vector<float> values(size_t(float4s) * 4);
    for (uint32_t i = 0; i < float4s; ++i)
    {
        values[size_t(i) * 4 + 0] = float(i) + 0.5f;
        values[size_t(i) * 4 + 1] = special[(size_t(i) + 0) % special.size()];
        values[size_t(i) * 4 + 2] = special[(size_t(i) + 3) % special.size()];
        values[size_t(i) * 4 + 3] = special[(size_t(i) + 5) % special.size()];
    }
    return values;
}

bool Addressed(uint32_t at, const ScaleEntry& entry)
{
    if (at < entry.byteOffset || entry.stride == 0)
        return false;
    const uint32_t delta = at - entry.byteOffset;
    return delta % entry.stride == 0 && delta / entry.stride < entry.count;
}

// The bytes the pass must produce: the game's own bytes, with each addressed float4's components scaled by the entries that address it and
// nothing else touched. A component whose multiplier is exactly 1 keeps its four bytes as they were (that is the primitive's rule, and it is
// what keeps a denormal or a NaN payload beside the knob intact). The comparison is the whole buffer of that size, so a table whose last
// float4s the dispatch never reached is not a pass either.
std::vector<unsigned char> Expected(const std::vector<float>& source, const ScaleEntry* entries, uint32_t count, uint32_t byteWidth)
{
    std::vector<unsigned char> bytes(size_t(byteWidth), 0);
    std::memcpy(bytes.data(), source.data(), byteWidth);
    const uint32_t float4s = byteWidth / 16;
    for (uint32_t i = 0; i < float4s; ++i)
    {
        const uint32_t at = i * 16;
        for (uint32_t c = 0; c < 4; ++c)
        {
            float scaled = source[size_t(i) * 4 + c];
            bool touched = false;
            for (uint32_t e = 0; e < count; ++e)
            {
                if (!Addressed(at, entries[e]) || entries[e].multiplier[c] == 1.0f)
                    continue;
                scaled *= entries[e].multiplier[c];
                touched = true;
            }
            if (touched)
                std::memcpy(bytes.data() + at + c * 4, &scaled, sizeof scaled);
        }
    }
    return bytes;
}

std::vector<unsigned char> Expected(const std::vector<float>& source, const ScaleEntry& entry, uint32_t byteWidth)
{
    return Expected(source, &entry, 1, byteWidth);
}

std::vector<float> AmbientKnobSource(uint32_t byteWidth)
{
    // The two components the ambient knob scales hold exact values, so the CPU's expectation and the dispatch's multiply agree bit for bit;
    // every other component keeps the fixture's specials, which is what shows they come through the pass untouched.
    std::vector<float> values = SourceValues(byteWidth);
    for (uint32_t probe = 0; probe < 64; ++probe)
    {
        const size_t recipScale = size_t(112u + probe * 192u + 12u) / 4; // .w of the 0x30 float4
        const size_t visibility = size_t(240u + probe * 192u) / 4;        // .x of the 0xB0 float4
        if (recipScale * 4 < byteWidth)
            values[recipScale] = float(probe) + 0.25f;
        if (visibility * 4 < byteWidth)
            values[visibility] = float(probe) + 0.5f;
    }
    return values;
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

    // A DYNAMIC constant buffer, which is what the engine's own parameter tables most likely are: the pass has to read it through
    // CopyResource, so the source's usage has to be one the runtime accepts.
    ComPtr<ID3D11Buffer> DynamicBuffer(const void* bytes, uint32_t byteWidth)
    {
        D3D11_BUFFER_DESC desc {};
        desc.ByteWidth = byteWidth;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Buffer> buffer;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &buffer)))
            return {};
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (FAILED(context->Map(buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || mapped.pData == nullptr)
            return {};
        std::memcpy(mapped.pData, bytes, byteWidth);
        context->Unmap(buffer.Get(), 0);
        return buffer;
    }

    ComPtr<ID3D11Buffer> Staging(uint32_t byteWidth)
    {
        D3D11_BUFFER_DESC desc {};
        desc.ByteWidth = byteWidth;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> buffer;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &buffer)))
            return {};
        return buffer;
    }

    // The bytes of a buffer, read back through a staging copy.
    bool ReadBack(ID3D11Buffer* buffer, uint32_t byteWidth, std::vector<unsigned char>& out)
    {
        ComPtr<ID3D11Buffer> staging = Staging(byteWidth);
        if (!staging)
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
        return Finish("cbpatch_scale");

    const std::vector<float> source = SourceValues(kBytes);
    ComPtr<ID3D11Buffer> game = scene.ConstantBuffer(source.data(), kBytes);
    CHECK(game != nullptr);
    if (!game)
        return Finish("cbpatch_scale");

    // ---- one entry, the ambient knob's: 64 float4s at 240 bytes, one every 192, their .x halved and nothing else touched
    const ScaleEntry entry = AmbientSkyVisibilityEntry(0.5f);
    CbPatch patch;
    ID3D11Buffer* patched = patch.Scale(scene.context.Get(), game.Get(), kBytes, &entry, 1);
    CHECK(patched != nullptr);
    CHECK(patch.Failure() == nullptr);
    CHECK_EQ(patch.Scales(), 1u);
    CHECK_EQ(patch.SizesCached(), 1u);
    if (patched != nullptr)
    {
        std::vector<unsigned char> bytes;
        CHECK(scene.ReadBack(patched, kBytes, bytes));
        CHECK_EQ(bytes.size(), size_t(kBytes));
        // one comparison for the whole buffer: the 64 addressed .x values scaled, and every other byte - including the .yzw of an addressed
        // float4, whatever denormal or NaN payload it holds - bit for bit the game's
        const std::vector<unsigned char> expected = Expected(source, entry, kBytes);
        CHECK(bytes.size() == expected.size() && std::memcmp(bytes.data(), expected.data(), kBytes) == 0);
        // no addressed component was skipped: the comparison above would pass if the pass did nothing at all
        CHECK(std::memcmp(bytes.data(), source.data(), kBytes) != 0);
        // exactly 64 of the 1,024 float4s carry the knob
        uint32_t addressed = 0;
        for (uint32_t i = 0; i < kFloat4s; ++i)
            if (Addressed(i * 16, entry))
                ++addressed;
        CHECK_EQ(addressed, 64u);
    }

    // ---- the game's own buffer is never written: its bytes after the patch are the bytes it was created with
    {
        std::vector<unsigned char> before;
        CHECK(scene.ReadBack(game.Get(), kBytes, before));
        CHECK_EQ(before.size(), size_t(kBytes));
        CHECK(std::memcmp(before.data(), source.data(), kBytes) == 0);
    }

    // ---- a second run reuses the size's buffers and the same entry writes the same bytes
    {
        ID3D11Buffer* again = patch.Scale(scene.context.Get(), game.Get(), kBytes, &entry, 1);
        CHECK(again != nullptr);
        CHECK_EQ(patch.Scales(), 2u);
        CHECK_EQ(patch.SizesCached(), 1u);
    }

    // ---- an identity entry (all multipliers 1) changes nothing at all: the pass is a copy in that case, which is why the rules never run it
    {
        ScaleEntry identity = AmbientSkyVisibilityEntry(1.0f);
        ID3D11Buffer* copy = patch.Scale(scene.context.Get(), game.Get(), kBytes, &identity, 1);
        CHECK(copy != nullptr);
        std::vector<unsigned char> bytes;
        CHECK(scene.ReadBack(copy, kBytes, bytes));
        CHECK(bytes.size() == size_t(kBytes) && std::memcmp(bytes.data(), source.data(), kBytes) == 0);
    }

    // ---- a DYNAMIC source, the usage the engine's own parameter tables most likely have: the same bytes come out
    {
        ComPtr<ID3D11Buffer> dynamicGame = scene.DynamicBuffer(source.data(), kBytes);
        CHECK(dynamicGame != nullptr);
        if (dynamicGame)
        {
            ID3D11Buffer* fromDynamic = patch.Scale(scene.context.Get(), dynamicGame.Get(), kBytes, &entry, 1);
            CHECK(fromDynamic != nullptr);
            std::vector<unsigned char> bytes;
            CHECK(scene.ReadBack(fromDynamic, kBytes, bytes));
            const std::vector<unsigned char> expected = Expected(source, entry, kBytes);
            CHECK(bytes.size() == expected.size() && std::memcmp(bytes.data(), expected.data(), kBytes) == 0);
            // and the source itself is untouched
            std::vector<unsigned char> after;
            CHECK(scene.ReadBack(dynamicGame.Get(), kBytes, after));
            CHECK(after.size() == size_t(kBytes) && std::memcmp(after.data(), source.data(), kBytes) == 0);
        }
    }

    // ---- the requests the primitive refuses: null, a reason, and nothing was dispatched
    {
        const uint32_t scalesBefore = patch.Scales();
        ScaleEntry bad = AmbientSkyVisibilityEntry(0.5f);
        bad.count = 4096; // far past the buffer
        CHECK(patch.Scale(scene.context.Get(), game.Get(), kBytes, &bad, 1) == nullptr);
        CHECK(patch.Failure() != nullptr);
        CHECK_EQ(patch.Scales(), scalesBefore);

        CHECK(patch.Scale(scene.context.Get(), game.Get(), kBytes, &entry, 0) == nullptr);
        CHECK(patch.Scale(scene.context.Get(), game.Get(), kBytes, nullptr, 1) == nullptr);
        CHECK(patch.Scale(scene.context.Get(), nullptr, kBytes, &entry, 1) == nullptr);
        CHECK(patch.Scale(scene.context.Get(), game.Get(), kBytes, &entry, kMaxScaleEntries + 1) == nullptr);
        // a size that is not a constant buffer's, and a size the bound buffer does not have
        CHECK(patch.Scale(scene.context.Get(), game.Get(), 12, &entry, 1) == nullptr);
        CHECK(patch.Scale(scene.context.Get(), game.Get(), 4096, &entry, 1) == nullptr);
        // the declared ambient table (12,352 bytes) with its own buffer. Its 64 addressed float4s end on the table's last sixteen bytes, so the
        // whole 772-float4 result is compared, not just the fact that a buffer came back: a dispatch that covers 12 whole groups of 64 leaves
        // the last four float4s as the work buffer had them, and that is a table with four floats the pass never wrote.
        const std::vector<float> declaredSource = SourceValues(kAmbientDeclaredBytes);
        ComPtr<ID3D11Buffer> declaredBuffer = scene.ConstantBuffer(declaredSource.data(), kAmbientDeclaredBytes);
        CHECK(declaredBuffer != nullptr);
        if (declaredBuffer)
        {
            ID3D11Buffer* declaredPatched = patch.Scale(scene.context.Get(), declaredBuffer.Get(), kAmbientDeclaredBytes, &entry, 1);
            CHECK(declaredPatched != nullptr);
            CHECK_EQ(patch.SizesCached(), 2u);
            if (declaredPatched != nullptr)
            {
                std::vector<unsigned char> bytes;
                CHECK(scene.ReadBack(declaredPatched, kAmbientDeclaredBytes, bytes));
                const std::vector<unsigned char> expected = Expected(declaredSource, entry, kAmbientDeclaredBytes);
                CHECK(bytes.size() == expected.size() && std::memcmp(bytes.data(), expected.data(), kAmbientDeclaredBytes) == 0);
                // the last addressed float4 (at 12,336) is scaled, so the comparison above is not the comparison of two untouched copies
                CHECK(std::memcmp(bytes.data(), declaredSource.data(), kAmbientDeclaredBytes) != 0);
            }
        }
    }

    // ---- the two entries the ambient knob writes, on one 12,352-byte table: the probe's sky visibility at 0xB0 and its SH reciprocal scale at
    // 0x3C, each of the 64 probes, and nothing of the float4s beside them - `ssaoMask` at 0xB4 and `color.xyz` at the head of the 0x30 float4
    // whose .w is the second field. A single dispatch carries both, which is what the shipped table gets from AmbientEntries.
    {
        ScaleEntry knobs[kMaxScaleEntries] {};
        const uint32_t knobCount = AmbientEntries(BuildParams(0.70f), knobs, kMaxScaleEntries);
        CHECK_EQ(knobCount, 2u);
        const std::vector<float> tableSource = AmbientKnobSource(kAmbientDeclaredBytes);
        ComPtr<ID3D11Buffer> table = scene.ConstantBuffer(tableSource.data(), kAmbientDeclaredBytes);
        CHECK(table != nullptr);
        if (table && knobCount == 2)
        {
            ID3D11Buffer* both = patch.Scale(scene.context.Get(), table.Get(), kAmbientDeclaredBytes, knobs, knobCount);
            CHECK(both != nullptr);
            CHECK(patch.Failure() == nullptr);
            if (both != nullptr)
            {
                std::vector<unsigned char> bytes;
                CHECK(scene.ReadBack(both, kAmbientDeclaredBytes, bytes));
                const std::vector<unsigned char> expected = Expected(tableSource, knobs, knobCount, kAmbientDeclaredBytes);
                CHECK(bytes.size() == expected.size() && std::memcmp(bytes.data(), expected.data(), kAmbientDeclaredBytes) == 0);
                if (bytes.size() == size_t(kAmbientDeclaredBytes))
                {
                    // both fields moved on the first and on the last probe, so neither entry is the only one that landed
                    const auto unchanged = [&](uint32_t at) {
                        return std::memcmp(bytes.data() + at, &tableSource[size_t(at) / 4], 4) == 0;
                    };
                    CHECK(!unchanged(240u));                    // probe 0's skyVisibilityScale (.x of the 0xB0 float4)
                    CHECK(!unchanged(112u + 12u));              // probe 0's shRecipScale (.w of the 0x30 float4)
                    CHECK(!unchanged(240u + 63u * 192u));       // probe 63's skyVisibilityScale
                    CHECK(!unchanged(112u + 63u * 192u + 12u)); // probe 63's shRecipScale
                    // the neighbours the knob must not touch: `ssaoMask` beside the first field, the probe's colour beside the second, and the
                    // gap between the two tables' last addressed float4s
                    CHECK(unchanged(244u));      // ssaoMask, the .y of the 0xB0 float4
                    CHECK(unchanged(112u));      // color.x
                    CHECK(unchanged(112u + 4u)); // color.y
                    CHECK(unchanged(112u + 8u)); // color.z
                    CHECK(unchanged(240u + 63u * 192u + 4u));
                    CHECK(std::memcmp(bytes.data(), tableSource.data(), 112) == 0); // the header and probe 0's fields below 0x30
                    CHECK(std::memcmp(bytes.data() + 12224, &tableSource[12224 / 4], 12336 - 12224) == 0);
                }
            }
        }
    }

    // ---- the size cache holds four sizes: a fifth evicts the oldest entry, and asking for an evicted size rebuilds it - which bumps the
    // generation the wetness epoch carries, so a cached patched buffer pointer is not read after its entry went away
    {
        CbPatch cache;
        ScaleEntry one = AmbientSkyVisibilityEntry(0.5f);
        one.count = 1; // one float4 at the entry's offset, present in every size below

        // four sizes of a changed build: each has its own buffers, and the 16,384-byte table is the fifth
        const uint32_t smallSizes[4] { 1024, 2048, 4096, 8192 };
        std::vector<ComPtr<ID3D11Buffer>> smallBuffers;
        for (uint32_t byteWidth : smallSizes)
        {
            const std::vector<unsigned char> filler(byteWidth, 0);
            smallBuffers.push_back(scene.ConstantBuffer(filler.data(), byteWidth));
            CHECK(smallBuffers.back() != nullptr);
            if (!smallBuffers.back())
                return Finish("cbpatch_scale");
        }
        // the first patch creates the instance's own objects, which is itself a re-initialisation: the count below starts there
        CHECK(cache.Scale(scene.context.Get(), smallBuffers[0].Get(), smallSizes[0], &one, 1) != nullptr);
        const uint32_t before = cache.Generation();

        for (size_t i = 1; i < smallBuffers.size(); ++i)
            CHECK(cache.Scale(scene.context.Get(), smallBuffers[i].Get(), smallSizes[i], &one, 1) != nullptr);
        CHECK_EQ(cache.SizesCached(), 4u);
        CHECK_EQ(cache.Generation(), before); // four sizes fit: nothing was evicted

        // the fifth size evicts the oldest entry (1,024) and says so, so a caller holding a buffer of that size knows it is gone
        CHECK(cache.Scale(scene.context.Get(), game.Get(), kBytes, &one, 1) != nullptr);
        CHECK_EQ(cache.SizesCached(), 4u);
        CHECK_EQ(cache.Generation(), before + 1);

        // the evicted size is rebuilt on its next use, and the entry that went out with it is the next one to go
        CHECK(cache.Scale(scene.context.Get(), smallBuffers[0].Get(), smallSizes[0], &one, 1) != nullptr);
        CHECK_EQ(cache.SizesCached(), 4u);
        CHECK_EQ(cache.Generation(), before + 2);
        CHECK(cache.Scale(scene.context.Get(), smallBuffers[1].Get(), smallSizes[1], &one, 1) != nullptr);
        CHECK_EQ(cache.SizesCached(), 4u);
        CHECK_EQ(cache.Generation(), before + 3);

        // a size that is still cached costs nothing: no rebuild, no bump
        CHECK(cache.Scale(scene.context.Get(), game.Get(), kBytes, &one, 1) != nullptr);
        CHECK_EQ(cache.SizesCached(), 4u);
        CHECK_EQ(cache.Generation(), before + 3);
    }

    // ---- the release path leaves nothing behind: a fresh scale after Release creates its buffers again
    {
        patch.Release();
        CHECK_EQ(patch.SizesCached(), 0u);
        CHECK(patch.Scale(scene.context.Get(), game.Get(), kBytes, &entry, 1) != nullptr);
        CHECK_EQ(patch.SizesCached(), 1u);
    }

    return Finish("cbpatch_scale");
}
