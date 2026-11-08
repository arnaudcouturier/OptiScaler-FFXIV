// Engine light controls, writer 1: the constant-buffer family on D3D11 WARP, through the real CbPatch (ext/ffxiv/engine/CbPatch.cpp).
//
// The contract a player would feel if it broke:
//   the patched draw reads the scaled numbers at exactly the offsets the family claims (the moon's diffuse +32 and specular +48 of the 512-byte
//   g_LightParam, the composite's SH rows and header sky scale, the water block's eight entries) and EVERY other byte - the seeded denormals, NaN
//   payloads and negative zero beside them included - comes through bit for bit,
//   the game's own buffers are never written,
//   a reader inside the swap that maps what it fetched (OwnedGameBuffer) reads the ENGINE's bytes, and one that does not would read the patched ones
//   (so the night curve would feed on its own darkened moon); the mapping ends with the scope, on every path, and nests,
//   the four sizes the family and the shipped knobs patch (16,384 / 32 / 512 / 256) fit the primitive's size cache together: a frame of all four
//   evicts nothing (an eviction would re-create buffers every frame and invalidate the wetness cache).
#include "check.h"

#include "ext/ffxiv/engine/CbPatch.h"
#include "ext/ffxiv/enginelight/ActiveSwap.h"
#include "ext/ffxiv/enginelight/CbFamilyRules.h"

#include <d3d11.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::enginelight;
using namespace ext::ffxiv::enginelight::cb; // the constant-buffer family's own namespace (look3 integration: its names collided with the lamps / exposure module's)
namespace eng = ext::ffxiv::engine;

namespace
{
float Bits(uint32_t bits)
{
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

// A buffer's worth of floats: distinct, exactly representable values, with specials sprinkled so a whole-buffer conversion would be caught.
std::vector<float> Seed(uint32_t byteWidth)
{
    static const float specials[] = { 2.0f, -3.0f, Bits(0x00000001u), Bits(0x7FC00001u), Bits(0x7F800001u), Bits(0x00800000u), Bits(0xFFFFFFFFu), Bits(0x80000000u) };
    std::vector<float> values(byteWidth / 4);
    for (size_t i = 0; i < values.size(); ++i)
        values[i] = (i % 4 == 3) ? specials[(i / 4) % 8] : float(i % 997) * 0.25f + 0.5f;
    return values;
}

struct Scene
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;

    bool Create()
    {
        return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)) &&
               device && context;
    }

    ComPtr<ID3D11Buffer> ConstantBuffer(const std::vector<float>& values)
    {
        D3D11_BUFFER_DESC desc {};
        desc.ByteWidth = uint32_t(values.size() * 4);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA initial { values.data(), 0, 0 };
        ComPtr<ID3D11Buffer> buffer;
        device->CreateBuffer(&desc, &initial, &buffer);
        return buffer;
    }

    std::vector<unsigned char> ReadBack(ID3D11Buffer* buffer)
    {
        D3D11_BUFFER_DESC source {};
        buffer->GetDesc(&source);
        D3D11_BUFFER_DESC desc {};
        desc.ByteWidth = source.ByteWidth;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> staging;
        std::vector<unsigned char> out;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &staging)))
            return out;
        context->CopyResource(staging.Get(), buffer);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        {
            out.assign(static_cast<unsigned char*>(mapped.pData), static_cast<unsigned char*>(mapped.pData) + source.ByteWidth);
            context->Unmap(staging.Get(), 0);
        }
        return out;
    }
};

float FloatAt(const std::vector<unsigned char>& bytes, uint32_t at)
{
    float v = 0.0f;
    std::memcpy(&v, bytes.data() + at, sizeof v);
    return v;
}

bool SameBits(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b, uint32_t at)
{
    return std::memcmp(a.data() + at, b.data() + at, 4) == 0;
}

// The expectation for a table: every float of the source, multiplied in entry order by each entry that addresses its float4 and whose component
// multiplier is not exactly 1 (the primitive's rule); all other bytes as they were.
bool Addressed(uint32_t at, const eng::ScaleEntry& e)
{
    if (at < e.byteOffset || e.stride == 0)
        return false;
    const uint32_t delta = at - e.byteOffset;
    return delta % e.stride == 0 && delta / e.stride < e.count;
}

// The addressed floats hold ordinary numbers (a real SH row or colour is one): a GPU multiply may flush a denormal or canonicalise a NaN payload,
// and the specials are there to prove the bytes the table does NOT address come through untouched. Called after Seed, before the buffer is created.
void NormalAt(std::vector<float>& values, const eng::ScaleEntry* entries, uint32_t count)
{
    for (uint32_t e = 0; e < count; ++e)
        for (uint32_t k = 0; k < entries[e].count; ++k)
        {
            const size_t first = (entries[e].byteOffset + size_t(k) * entries[e].stride) / 4;
            for (size_t c = 0; c < 4; ++c)
                if (first + c < values.size())
                    values[first + c] = float((first + c) % 211) * 0.125f + 1.0625f;
        }
}

std::vector<unsigned char> Expected(const std::vector<float>& source, const eng::ScaleEntry* entries, uint32_t count)
{
    std::vector<unsigned char> bytes(source.size() * 4);
    std::memcpy(bytes.data(), source.data(), bytes.size());
    for (uint32_t i = 0; i < source.size() / 4; ++i)
        for (uint32_t c = 0; c < 4; ++c)
        {
            float v = source[size_t(i) * 4 + c];
            bool touched = false;
            for (uint32_t e = 0; e < count; ++e)
                if (Addressed(i * 16, entries[e]) && entries[e].multiplier[c] != 1.0f)
                {
                    v *= entries[e].multiplier[c];
                    touched = true;
                }
            if (touched)
                std::memcpy(bytes.data() + i * 16 + c * 4, &v, 4);
        }
    return bytes;
}

bool AllBytesEqual(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b)
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

Params Night()
{
    Params p;
    p.master = true;
    p.moonIntensity = 0.1f;
    p.moonTint[0] = 1.25f;
    p.moonTint[1] = 1.0f;
    p.moonTint[2] = 0.75f;
    p.ambientIntensity = 0.5f;
    p.ambientTint[0] = 0.75f;
    p.ambientTint[1] = 1.0f;
    p.ambientTint[2] = 1.25f;
    p.waterMoon = 0.25f;
    p.waterAmbient = 0.5f;
    p.waterTint[0] = 1.25f;
    p.waterTint[1] = 1.0f;
    p.waterTint[2] = 0.5f;
    return p;
}

void Moon(Scene& scene)
{
    eng::CbPatch patch;
    // The 512-byte g_LightParam: position @0, direction @16, diffuse @32 (the 164135 moon), specular @48; the rest seeded.
    std::vector<float> light = Seed(512);
    const float diffuse[3] = { 0.0576f, 0.0917f, 0.16f };
    const float specular[3] = { 0.5f, 0.25f, 0.125f };
    std::memcpy(&light[8], diffuse, sizeof diffuse);
    std::memcpy(&light[12], specular, sizeof specular);
    ComPtr<ID3D11Buffer> game = scene.ConstantBuffer(light);
    CHECK(game != nullptr);
    const std::vector<unsigned char> before = scene.ReadBack(game.Get());

    const Params p = Night();
    eng::ScaleEntry entries[8];
    const uint32_t count = LightEntries(MoonFactors(p, 1.0f), entries, 8);
    CHECK_EQ(count, 2u);
    ID3D11Buffer* patched = patch.Scale(scene.context.Get(), game.Get(), 512, entries, count);
    CHECK(patched != nullptr);
    if (patched == nullptr)
        return;
    const std::vector<unsigned char> after = scene.ReadBack(patched);
    CHECK(AllBytesEqual(after, Expected(light, entries, count)));
    // The numbers the player sees: moon x(0.125, 0.1, 0.075) on the diffuse and the specular, .w beside them untouched.
    CHECK(std::fabs(FloatAt(after, 32) - 0.0576f * 0.125f) < 1e-7f);
    CHECK(std::fabs(FloatAt(after, 36) - 0.0917f * 0.1f) < 1e-7f);
    CHECK(std::fabs(FloatAt(after, 40) - 0.16f * 0.075f) < 1e-7f);
    CHECK(SameBits(after, before, 44));
    CHECK(std::fabs(FloatAt(after, 48) - 0.5f * 0.125f) < 1e-7f);
    CHECK(std::fabs(FloatAt(after, 52) - 0.25f * 0.1f) < 1e-7f);
    CHECK(std::fabs(FloatAt(after, 56) - 0.125f * 0.075f) < 1e-7f);
    CHECK(SameBits(after, before, 60));
    // Position and direction (the sun vector the contact shadows and the shafts read) are bit for bit the game's.
    for (uint32_t at = 0; at < 32; at += 4)
        CHECK(SameBits(after, before, at));
    // The game's own buffer is never written.
    CHECK(AllBytesEqual(scene.ReadBack(game.Get()), before));

    // The water pass's copy: its own factor (waterMoon 0.25 REPLACES the intensity 0.1), the same offsets.
    const uint32_t wcount = LightEntries(WaterMoonFactors(p, 1.0f), entries, 8);
    CHECK_EQ(wcount, 2u);
    ID3D11Buffer* water = patch.Scale(scene.context.Get(), game.Get(), 512, entries, wcount);
    CHECK(water != nullptr);
    if (water != nullptr)
    {
        const std::vector<unsigned char> w = scene.ReadBack(water);
        CHECK(std::fabs(FloatAt(w, 36) - 0.0917f * 0.25f) < 1e-7f);
        CHECK(AllBytesEqual(w, Expected(light, entries, wcount)));
    }
}

void Composite(Scene& scene)
{
    eng::CbPatch patch;
    std::vector<float> table = Seed(eng::kAmbientBoundBytes);
    const Params p = Night();
    // The shipped knob's two entries first, then the family's four: the combined table is one dispatch and they compose on different floats.
    eng::ScaleEntry entries[eng::kMaxScaleEntries];
    uint32_t count = eng::AmbientEntries(eng::BuildParams(0.7f), entries, 8);
    CHECK_EQ(count, 2u);
    const uint32_t mine = CompositeEntries(p, 1.0f, entries + count, 8 - count);
    CHECK_EQ(mine, 4u);
    count += mine;
    NormalAt(table, entries, count);
    ComPtr<ID3D11Buffer> game = scene.ConstantBuffer(table);
    const std::vector<unsigned char> before = scene.ReadBack(game.Get());
    ID3D11Buffer* patched = patch.Scale(scene.context.Get(), game.Get(), eng::kAmbientBoundBytes, entries, count);
    CHECK(patched != nullptr);
    if (patched == nullptr)
        return;
    const std::vector<unsigned char> after = scene.ReadBack(patched);
    CHECK(AllBytesEqual(after, Expected(table, entries, count)));
    // Probe k's SH rows R/G/B carry the per-channel factor (0.5 x tint), the shipped knob's shRecipScale (.w at 112+192k) only the knob.
    const float want[3] = { 0.5f * 0.75f, 0.5f, 0.5f * 1.25f };
    for (uint32_t k : { 0u, 1u, 63u })
        for (uint32_t c = 0; c < 3; ++c)
        {
            const uint32_t row = 64 + 192 * k + 16 * c;
            for (uint32_t i = 0; i < 4; ++i)
                CHECK(std::fabs(FloatAt(after, row + 4 * i) - table[(row + 4 * i) / 4] * want[c]) <= std::fabs(table[(row + 4 * i) / 4] * want[c]) * 1e-6f + 1e-30f ||
                      SameBits(after, before, row + 4 * i));
        }
    // The header: skyShRecipScale (byte 4) x0.5; the header's other floats (bytes 0, 8, 12) bit for bit.
    CHECK(std::fabs(FloatAt(after, 4) - table[1] * 0.5f) <= std::fabs(table[1] * 0.5f) * 1e-6f);
    CHECK(SameBits(after, before, 0) && SameBits(after, before, 8) && SameBits(after, before, 12));
    // The probe's shRecipScale field the shipped knob and the shadows lift own is the knob's alone (x0.7), not the family's.
    CHECK(std::fabs(FloatAt(after, 112 + 12) - table[(112 + 12) / 4] * 0.7f) <= std::fabs(table[(112 + 12) / 4] * 0.7f) * 1e-6f);
    CHECK(AllBytesEqual(scene.ReadBack(game.Get()), before));
    // The declared 12,352-byte size works as well.
    std::vector<float> smallBlock = Seed(eng::kAmbientDeclaredBytes);
    NormalAt(smallBlock, entries + 2, mine);
    ComPtr<ID3D11Buffer> gameSmall = scene.ConstantBuffer(smallBlock);
    ID3D11Buffer* patchedSmall = patch.Scale(scene.context.Get(), gameSmall.Get(), eng::kAmbientDeclaredBytes, entries + 2, mine);
    CHECK(patchedSmall != nullptr);
    if (patchedSmall != nullptr)
        CHECK(AllBytesEqual(scene.ReadBack(patchedSmall), Expected(smallBlock, entries + 2, mine)));
}

void WaterBlock(Scene& scene)
{
    eng::CbPatch patch;
    std::vector<float> block = Seed(kWaterAmbientBoundBytes);
    const Params p = Night();
    eng::ScaleEntry entries[eng::kMaxScaleEntries];
    const uint32_t count = WaterAmbientEntries(p, 1.0f, entries, 8);
    CHECK_EQ(count, 8u);
    NormalAt(block, entries, count);
    ComPtr<ID3D11Buffer> game = scene.ConstantBuffer(block);
    const std::vector<unsigned char> before = scene.ReadBack(game.Get());
    ID3D11Buffer* patched = patch.Scale(scene.context.Get(), game.Get(), kWaterAmbientBoundBytes, entries, count);
    CHECK(patched != nullptr);
    if (patched == nullptr)
        return;
    const std::vector<unsigned char> after = scene.ReadBack(patched);
    CHECK(AllBytesEqual(after, Expected(block, entries, count)));
    // Colour.xyz (+48..59) per channel, shRecipScale (+60) bit for bit; reflection scale/offset (+80, +84) x0.5, bake and index (+88, +92) bit for bit;
    // skyShRecipScale (+144) and the tail untouched (the rows carry the factor, no double application).
    const float m[3] = { 0.5f * 1.25f, 0.5f, 0.5f * 0.5f };
    for (uint32_t c = 0; c < 3; ++c)
        CHECK(std::fabs(FloatAt(after, 48 + 4 * c) - block[(48 + 4 * c) / 4] * m[c]) <= std::fabs(block[(48 + 4 * c) / 4] * m[c]) * 1e-6f);
    CHECK(SameBits(after, before, 60));
    CHECK(std::fabs(FloatAt(after, 80) - block[20] * 0.5f) <= std::fabs(block[20] * 0.5f) * 1e-6f);
    CHECK(SameBits(after, before, 88) && SameBits(after, before, 92));
    CHECK(SameBits(after, before, 144) && SameBits(after, before, 148) && SameBits(after, before, 152) && SameBits(after, before, 156));
    CHECK(AllBytesEqual(scene.ReadBack(game.Get()), before));
    // The declared 160-byte size is the same table.
    std::vector<float> declared = Seed(kWaterAmbientDeclaredBytes);
    NormalAt(declared, entries, count);
    ComPtr<ID3D11Buffer> gameDeclared = scene.ConstantBuffer(declared);
    ID3D11Buffer* patchedDeclared = patch.Scale(scene.context.Get(), gameDeclared.Get(), kWaterAmbientDeclaredBytes, entries, count);
    CHECK(patchedDeclared != nullptr);
}

void Readers(Scene& scene)
{
    eng::CbPatch patch;
    std::vector<float> light = Seed(512);
    const float diffuse[3] = { 0.0576f, 0.0917f, 0.16f };
    std::memcpy(&light[8], diffuse, sizeof diffuse);
    ComPtr<ID3D11Buffer> game = scene.ConstantBuffer(light);
    const std::vector<unsigned char> engineBytes = scene.ReadBack(game.Get());
    eng::ScaleEntry entries[8];
    const uint32_t count = LightEntries(MoonFactors(Night(), 1.0f), entries, 8);
    ID3D11Buffer* patched = patch.Scale(scene.context.Get(), game.Get(), 512, entries, count);
    CHECK(patched != nullptr);
    if (patched == nullptr)
        return;
    ID3D11DeviceContext* ctx = scene.context.Get();
    ctx->PSSetConstantBuffers(2, 1, game.GetAddressOf()); // the game's binding

    CHECK(!SwapInFlight());
    {
        eng::BoundConstantBuffer swap(ctx, 2, patched);
        CHECK(swap.Bound());
        CHECK(swap.Original() == game.Get());
        ActiveSwapScope scope(patched, swap.Original());
        CHECK(SwapInFlight());

        // A reader that fetches the bound buffer and does NOT map it sees the PATCHED bytes: the night curve would read the darkened moon.
        ID3D11Buffer* naive = nullptr;
        ctx->PSGetConstantBuffers(2, 1, &naive);
        CHECK(naive == patched);
        const std::vector<unsigned char> naiveBytes = scene.ReadBack(naive);
        CHECK(!AllBytesEqual(naiveBytes, engineBytes));
        CHECK(std::fabs(FloatAt(naiveBytes, 36) - 0.0917f * 0.1f) < 1e-7f);

        // The same reader after OwnedGameBuffer reads the ENGINE's bytes (and owns exactly one reference on the buffer it holds).
        const ULONG patchedRefs = [&] { patched->AddRef(); return patched->Release(); }();
        ID3D11Buffer* mapped = OwnedGameBuffer(naive);
        CHECK(mapped == game.Get());
        CHECK(AllBytesEqual(scene.ReadBack(mapped), engineBytes));
        const ULONG patchedRefsAfter = [&] { patched->AddRef(); return patched->Release(); }();
        CHECK(patchedRefsAfter + 1 == patchedRefs); // the reader's reference on the patched copy was given back
        mapped->Release();

        // A buffer that is not the patched copy passes through unchanged (the camera table, another slot's buffer).
        ID3D11Buffer* other = scene.ConstantBuffer(Seed(64)).Detach();
        CHECK(OwnedGameBuffer(other) == other);
        other->Release();
        CHECK(OwnedGameBuffer(nullptr) == nullptr);

        // Scopes nest: an inner swap maps its own pair, and the outer pair comes back.
        {
            // Another size, so the primitive hands back another destination (one size shares ONE patched buffer).
            ComPtr<ID3D11Buffer> innerGame = scene.ConstantBuffer(Seed(256));
            eng::ScaleEntry innerEntries[8];
            const uint32_t innerCount = WaterAmbientEntries(Night(), 1.0f, innerEntries, 8);
            ID3D11Buffer* innerPatched = patch.Scale(ctx, innerGame.Get(), 256, innerEntries, innerCount);
            CHECK(innerPatched != nullptr && innerPatched != patched);
            ActiveSwapScope inner(innerPatched, innerGame.Get());
            innerPatched->AddRef(); // a reader owns one reference on what it fetched, and OwnedGameBuffer takes it over
            ID3D11Buffer* a = OwnedGameBuffer(innerPatched);
            CHECK(a == innerGame.Get());
            a->Release();
            // The outer pair is shadowed while the inner scope lives.
            patched->AddRef();
            CHECK(OwnedGameBuffer(patched) == patched);
            patched->Release();
        }
        patched->AddRef();
        ID3D11Buffer* again = OwnedGameBuffer(patched);
        CHECK(again == game.Get());
        again->Release();
    }
    // The scope and the guard are gone: nothing dangles into the next draw, and the game's binding is back.
    CHECK(!SwapInFlight());
    ID3D11Buffer* bound = nullptr;
    ctx->PSGetConstantBuffers(2, 1, &bound);
    CHECK(bound == game.Get());
    patched->AddRef();
    CHECK(OwnedGameBuffer(patched) == patched); // the mapping ended with the scope
    patched->Release();
    bound->Release();

    // A throw inside the draw ends the scope too.
    try
    {
        eng::BoundConstantBuffer swap(ctx, 2, patched);
        ActiveSwapScope scope(patched, swap.Original());
        throw std::runtime_error("the game's draw threw");
    }
    catch (const std::exception&)
    {
    }
    CHECK(!SwapInFlight());
    ctx->PSGetConstantBuffers(2, 1, &bound);
    CHECK(bound == game.Get());
    bound->Release();
}

void SizeCache(Scene& scene)
{
    eng::CbPatch patch;
    // A frame of the composite (16,384), the wetness buffer (32), the light draws (512) and the water block (256), twice: no eviction.
    ComPtr<ID3D11Buffer> composite = scene.ConstantBuffer(Seed(16384));
    ComPtr<ID3D11Buffer> wetness = scene.ConstantBuffer(Seed(32));
    ComPtr<ID3D11Buffer> light = scene.ConstantBuffer(Seed(512));
    ComPtr<ID3D11Buffer> water = scene.ConstantBuffer(Seed(256));
    const Params p = Night();
    eng::ScaleEntry rows[8], lamp[8], block[8];
    const uint32_t rowsCount = CompositeEntries(p, 1.0f, rows, 8);
    const uint32_t lampCount = LightEntries(MoonFactors(p, 1.0f), lamp, 8);
    const uint32_t blockCount = WaterAmbientEntries(p, 1.0f, block, 8);
    eng::SetEntry wet = eng::WetnessEntry(0.5f);
    const uint32_t generation = [&] {
        CHECK(patch.Scale(scene.context.Get(), composite.Get(), 16384, rows, rowsCount) != nullptr);
        CHECK(patch.Set(scene.context.Get(), wetness.Get(), 32, &wet, 1) != nullptr);
        CHECK(patch.Scale(scene.context.Get(), light.Get(), 512, lamp, lampCount) != nullptr);
        CHECK(patch.Scale(scene.context.Get(), water.Get(), 256, block, blockCount) != nullptr);
        return patch.Generation();
    }();
    for (int frame = 0; frame < 3; ++frame)
    {
        CHECK(patch.Scale(scene.context.Get(), composite.Get(), 16384, rows, rowsCount) != nullptr);
        CHECK(patch.Scale(scene.context.Get(), light.Get(), 512, lamp, lampCount) != nullptr);
        CHECK(patch.Scale(scene.context.Get(), light.Get(), 512, lamp, lampCount) != nullptr); // the water pass's copy: the same size
        CHECK(patch.Scale(scene.context.Get(), water.Get(), 256, block, blockCount) != nullptr);
        CHECK(patch.Set(scene.context.Get(), wetness.Get(), 32, &wet, 1) != nullptr);
    }
    CHECK_EQ(patch.SizesCached(), 4u);
    CHECK_EQ(patch.Generation(), generation);
}
} // namespace

int main()
{
    Scene scene;
    CHECK(scene.Create());
    if (!scene.context)
        return Finish("enginelight_warp");
    Moon(scene);
    Composite(scene);
    WaterBlock(scene);
    Readers(scene);
    SizeCache(scene);
    return Finish("enginelight_warp");
}
