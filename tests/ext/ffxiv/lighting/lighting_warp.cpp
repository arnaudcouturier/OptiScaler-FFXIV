// The lighting tab's chara ambient: the 160-byte block's GPU scale (ext/ffxiv/engine/CbPatch.cpp drive from the chara rules) on the D3D11
// WARP software adapter.
//
// The contract a player would feel if it broke: at the identity the block's bytes come out bit for bit as they went in; at a real lift the
// block's reciprocal-scale field (the .w of the float4 at byte 48) carries the multiplied value and every byte beside it - the colour, the
// attenuation, the visibility, the bake rate, the second SH row set, whatever payload they hold - is unaffected; the game's own buffer is
// never written; and a request the primitive cannot honour hands back null so the game's draw runs untouched.
#include "check.h"

#include "ext/ffxiv/engine/CbPatch.h" // the shared patch primitive, driven by this module's entries
#include "ext/ffxiv/lighting/CharaAmbientRules.h"

#include <d3d11.h>

#include <cstdio>
#include <cstring>
#include <vector>
#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::lighting::chara;

namespace
{
constexpr uint32_t kBytes = kCharaAmbientBytes; // 160
constexpr uint32_t kFloat4s = kBytes / 16;      // 10

// A float from its bit pattern, so the fixture can hold values a conversion would change.
float Bits(uint32_t bits)
{
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

// The values the game's block holds: the fields beside the knob cycle through the specials (a denormal, NaN payloads, negative zero,
// integer-as-float scales), and the knob's own .w holds an exact, representable scale so the CPU's expectation and the dispatch's multiply
// agree bit for bit. The block's own sky SH rows (bytes 0..47 and 96..143) and the rest keep the specials: the proof that the scale does not
// touch them is in every one of those bytes.
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

std::vector<float> BlockSource()
{
    const std::vector<float>& special = Specials();
    std::vector<float> values(size_t(kFloat4s) * 4);
    for (uint32_t i = 0; i < kFloat4s; ++i)
    {
        for (uint32_t c = 0; c < 4; ++c)
            values[size_t(i) * 4 + c] = special[(size_t(i) + c * 2) % special.size()];
    }
    // the knob's own field: the .w of the float4 at 48 (bytes 60..63), an exact value per scale the test scales
    values[(kCharaRecipFloat4 + kCharaRecipComponent * 4u) / 4u] = 3.0f;
    return values;
}

// The bytes the pass must produce: the game's own bytes with ONLY the addressed component's multiplier applied where it is not exactly 1.
std::vector<unsigned char> Expected(const std::vector<float>& source, const ext::ffxiv::engine::ScaleEntry& entry)
{
    std::vector<unsigned char> bytes(size_t(kBytes), 0);
    std::memcpy(bytes.data(), source.data(), kBytes);
    for (uint32_t i = 0; i < kFloat4s; ++i)
    {
        const uint32_t at = i * 16;
        if (at != entry.byteOffset || entry.stride != 16)
            continue;
        for (uint32_t c = 0; c < 4; ++c)
        {
            if (entry.multiplier[c] == 1.0f)
                continue; // a multiplier of exactly 1 keeps the bits it was read with
            const float original = source[size_t(i) * 4 + c];
            const float scaled = original * entry.multiplier[c];
            std::memcpy(bytes.data() + at + c * 4, &scaled, sizeof scaled);
        }
    }
    return bytes;
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
        return Finish("lighting_warp");

    const std::vector<float> source = BlockSource();
    ComPtr<ID3D11Buffer> game = scene.ConstantBuffer(source.data(), kBytes);
    CHECK(game != nullptr);
    if (!game)
        return Finish("lighting_warp");

    // ---- the byte-exact identity: the lift the keys carry at their idle multiplies nothing (the whole schedule)
    {
        ext::ffxiv::engine::CbPatch patch;
        const ext::ffxiv::engine::ScaleEntry idle = CharaRecipEntry(1.0f);
        CHECK(EntryFitsChara(idle));
        ID3D11Buffer* patched = patch.Scale(scene.context.Get(), game.Get(), kBytes, &idle, 1);
        CHECK(patched != nullptr);
        CHECK(patch.Failure() == nullptr);
        // one comparison for the whole block: every byte bit for bit the game's, and the .w also unchanged because the identity multiplier
        // passes its bits through (a pass that multiplied .w by 1.0 would still be byte-exact for a representable 3.0f, but the whole-buffer
        // contract is what the equality gate rides on)
        std::vector<unsigned char> bytes;
        CHECK(scene.ReadBack(patched, kBytes, bytes));
        CHECK_EQ(bytes.size(), size_t(kBytes));
        CHECK(std::memcmp(bytes.data(), reinterpret_cast<const unsigned char*>(source.data()), kBytes) == 0);
        // the game's own buffer is never written
        std::vector<unsigned char> after;
        CHECK(scene.ReadBack(game.Get(), kBytes, after));
        CHECK(std::memcmp(after.data(), reinterpret_cast<const unsigned char*>(source.data()), kBytes) == 0);
    }

    // ---- a real lift at 1.5: the .w carries it, every other byte of the block bit for bit the game's
    {
        ext::ffxiv::engine::CbPatch patch;
        const float lift = 1.5f;
        const ext::ffxiv::engine::ScaleEntry entry = CharaRecipEntry(lift);
        CHECK(EntryFitsChara(entry));
        ID3D11Buffer* patched = patch.Scale(scene.context.Get(), game.Get(), kBytes, &entry, 1);
        CHECK(patched != nullptr);
        CHECK(patch.Failure() == nullptr);
        std::vector<unsigned char> bytes;
        CHECK(scene.ReadBack(patched, kBytes, bytes));
        const std::vector<unsigned char> expected = Expected(source, entry);
        CHECK(std::memcmp(bytes.data(), expected.data(), kBytes) == 0);
        // the knob's own field moved (the comparison above would also pass if the pass did nothing at all)
        CHECK(std::memcmp(bytes.data(), reinterpret_cast<const unsigned char*>(source.data()), kBytes) != 0);
        // the denormal, the NaN payloads and the specials beside the knob came through unconverted: compare the .xyz of the knob's float4 and
        // the two float4s around it byte for byte
        const uint32_t knobStart = kCharaRecipFloat4;
        for (uint32_t idx = 0; idx < kBytes; ++idx)
            if (idx < 60 || idx >= 64) // the knob's own field is the only one that moves
                CHECK_EQ(bytes[idx], reinterpret_cast<const unsigned char*>(source.data())[idx]);
        CHECK(std::memcmp(bytes.data() + 0, reinterpret_cast<const unsigned char*>(source.data()) + 0, 48) == 0);              // the first SH rows
        CHECK(std::memcmp(bytes.data() + 64, reinterpret_cast<const unsigned char*>(source.data()) + 64, 32) == 0);            // the attenuation and the fields beside
        CHECK(std::memcmp(bytes.data() + 96, reinterpret_cast<const unsigned char*>(source.data()) + 96, 48) == 0);            // the second SH rows
        CHECK(std::memcmp(bytes.data() + 144, reinterpret_cast<const unsigned char*>(source.data()) + 144, 16) == 0);          // skyShRecipScale and the values after it
        // and the game's own buffer is still the game's
        std::vector<unsigned char> after;
        CHECK(scene.ReadBack(game.Get(), kBytes, after));
        CHECK(std::memcmp(after.data(), reinterpret_cast<const unsigned char*>(source.data()), kBytes) == 0);
    }

    // ---- a smaller lift (0.5, the dimming half) given the same byte-exact treatment
    {
        ext::ffxiv::engine::CbPatch patch;
        const ext::ffxiv::engine::ScaleEntry entry = CharaRecipEntry(0.5f);
        ID3D11Buffer* patched = patch.Scale(scene.context.Get(), game.Get(), kBytes, &entry, 1);
        CHECK(patched != nullptr);
        std::vector<unsigned char> bytes;
        CHECK(scene.ReadBack(patched, kBytes, bytes));
        const std::vector<unsigned char> expected = Expected(source, entry);
        CHECK(std::memcmp(bytes.data(), expected.data(), kBytes) == 0);
        // the scaled field also moved: 3.0f * 0.5 = 1.5, a different value
        float scaled = Bits(0);
        std::memcpy(&scaled, bytes.data() + kCharaRecipFloat4 + kCharaRecipComponent * 4, sizeof scaled);
        CHECK_EQ(scaled, 1.5f);
    }

    // ---- a request the primitive cannot honour: a wrong size, an entry past the end, no entry
    {
        ext::ffxiv::engine::CbPatch patch;
        // the bound width is not the chara block: CbPatch refuses the mismatch before anything runs
        ComPtr<ID3D11Buffer> other = scene.ConstantBuffer(source.data(), 176u);
        CHECK(other != nullptr);
        const ext::ffxiv::engine::ScaleEntry entry = CharaRecipEntry(1.5f);
        CHECK(patch.Scale(scene.context.Get(), other.Get(), kBytes, &entry, 1) == nullptr);
        CHECK(patch.Failure() != nullptr);
        // an entry that does not stay inside the block (count past the end) is refused before the dispatch
        ext::ffxiv::engine::ScaleEntry oversized = entry;
        oversized.count = 2; // 48 + 1*16 + 16 = 80 fits; push the stride count past the end instead
        oversized.byteOffset = kCharaAmbientBytes - 16;
        oversized.count = 2;
        CHECK(!ext::ffxiv::engine::EntryFits(oversized, kBytes));
        CHECK(patch.Scale(scene.context.Get(), game.Get(), kBytes, &oversized, 1) == nullptr);
        CHECK(patch.Failure() != nullptr);
        CHECK_EQ(patch.Scales(), 0u); // neither refused request dispatched anything
    }

    // ---- a second run reuses the size's three buffers
    {
        ext::ffxiv::engine::CbPatch patch;
        const ext::ffxiv::engine::ScaleEntry entry = CharaRecipEntry(1.5f);
        ID3D11Buffer* first = patch.Scale(scene.context.Get(), game.Get(), kBytes, &entry, 1);
        CHECK(first != nullptr);
        ID3D11Buffer* again = patch.Scale(scene.context.Get(), game.Get(), kBytes, &entry, 1);
        CHECK(again != nullptr);
        CHECK_EQ(patch.SizesCached(), 1u);
        CHECK_EQ(patch.Scales(), 2u);
        CHECK_EQ(first, again); // the same patched buffer answers both runs until its generation says it is gone
        // a release bumps the generation a cached entry is keyed on
        const uint32_t generation = patch.Generation();
        patch.Release();
        CHECK(patch.Generation() != generation);
    }

    return Finish("lighting_warp");
}
