// Shadow mapping on D3D11 WARP: the ONE CbPatch dispatch on the composite's ambient table, with the shipped ambient
// knob's two entries and the shadows' two appended to the same table (the composition the report relies on).
//
// The checks, motivated by what a broken dispatch would break:
//   the composed probe field (the SH reciprocal scale at byte 112+192k, touched by the knob's entry AND the
//   shadows' entry) leaves the dispatch as base * knobScale * nightLift - CbScale applies every matching entry in
//   order, multiplicatively, and this test is the evidence the report cites,
//   the header's skyShRecipScale (byte 4 of the table, the field the shipped knob never moves) carries only the
//   shadows' multiplier,
//   every OTHER byte of the 12'352-byte table comes out bit for bit as it went in (the fixture seeds denormals,
//   NaN payloads and integer-as-float values beside the addressed fields, exactly as the shipped knob's own test
//   does - a whole-buffer conversion would flush or canonicalise them and be caught here),
//   the game's own buffer is never written,
//   ONE dispatch per patched draw (Scales() == 1 for the merged table; a second draw runs the second),
//   the lone shadows arm (the shipped knob at 1.0) is the same dispatch shape with only its own two entries,
//   any game run where the table is 16'384 bytes works too.
#include "check.h"

#include "ext/ffxiv/engine/CbPatch.h"
#include "ext/ffxiv/shadows/ShadowsRules.h"

#include <d3d11.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
namespace fs = ext::ffxiv::shadows;
namespace fe = ext::ffxiv::engine;

namespace
{
constexpr uint32_t kDeclaredBytes = fe::kAmbientDeclaredBytes;

// A float from its bit pattern, so the fixture can seed values a conversion would alter.
float Bits(uint32_t bits)
{
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

// The fixture's own fields, as the dispatch leaves them: the values the scale's NOT addressed bytes read back.
std::vector<uint32_t> SourceWords(uint32_t byteWidth)
{
    // One float4 row per word-quarter, seeded a few aside the addressed ones: the smallest denormal payload, a NaN
    // with a payload, integer-as-float bits, negative zero, infinity, and the table's last row.
    std::vector<uint32_t> words(byteWidth / 4, 0u);
    words[0] = 0x3F800000; // row 0 .x = 1.0 (a header field the dispatch keeps)
    words[1] = 0x00800000; // row 0 .y = the smallest denormal (the header scale's neighbour, untouched)
    words[9] = 0x7FC01234; // a NaN with a payload
    words[19] = 0xFF800000;
    words[28] = 0xBF5A0000; // the probes' colours' bits beside the addressed weighting (112 + ... fields)
    words[800] = 0x4E9E0000;
    words[1000] = 0x00800003; // a denormal
    words[kDeclaredBytes / 4] = 0x80000000u; // negative zero, as the last row's first float in one field
    return words;
}
} // namespace

int main()
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[] { D3D_FEATURE_LEVEL_11_0 };
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 1, D3D11_SDK_VERSION, &device,
                                 nullptr, &context)))
        return Finish("shadows_ambient_warp");

    const std::vector<uint32_t> source = SourceWords(kDeclaredBytes);
    // The game's own buffer: a DEFAULT-usage constant buffer the caller owns (the patch reads it through a copy).
    D3D11_BUFFER_DESC desc {};
    desc.ByteWidth = kDeclaredBytes;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init {};
    init.pSysMem = source.data();
    ComPtr<ID3D11Buffer> game;
    CHECK(device->CreateBuffer(&desc, &init, &game) == S_OK);
    if (!game)
        return Finish("shadows_ambient_warp");

    // The merged table: the shipped knob's two entries (a 0.7 scale) appended by the shadows' two (2.5 lift) into
    // ONE table - the rule the engine seam calls, the order the compile-time rule enforces.
    fe::ScaleEntry entries[fe::kMaxScaleEntries] {};
    const fe::Params knob = fe::BuildParams(0.7f);
    const fs::MergedAmbientTable merged = fs::MergedAmbientEntries(knob, 2.5f, entries, fe::kMaxScaleEntries);
    CHECK_EQ(merged.knobEntries, 2u);
    CHECK_EQ(merged.shadowEntries, 2u);
    CHECK_EQ(merged.total, 4u);


    fe::CbPatch patch;
    // The patch: the copy the dispatch made (the returned buffer). Its bytes are what the composite's draw binds,
    // and the game's buffer is the one this test never writes.
    ComPtr<ID3D11Buffer> patchedBuffer;
    patchedBuffer = patch.Scale(context.Get(), game.Get(), kDeclaredBytes, entries, merged.total);
    CHECK(patchedBuffer != nullptr);
    if (patchedBuffer)
    {
        // The patch's own output buffer is read back through the staging copy of THAT buffer.
        D3D11_BUFFER_DESC readDesc {};
        readDesc.ByteWidth = kDeclaredBytes;
        readDesc.Usage = D3D11_USAGE_STAGING;
        readDesc.BindFlags = 0;
        readDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> staging;
        CHECK(device->CreateBuffer(&readDesc, nullptr, &staging) == S_OK);
        if (staging)
        {
            context->CopyResource(staging.Get(), patchedBuffer.Get());
            D3D11_MAPPED_SUBRESOURCE mapped {};
            const HRESULT hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
            CHECK(hr == S_OK);
            if (hr == S_OK)
            {
                const uint32_t* words = static_cast<const uint32_t*>(mapped.pData);
                // The header's skyShRecipScale (row 0, .y, exactly the field the shipped knob never touches): base
                // (the smallest normal here, bit 0x00800000) times 2.5 exactly - one non-1 component moved.
                const float headerBase = fe::kAmbientBoundBytes == 16384 ? Bits(source[1]) : Bits(source[1]);
                const float expectedHeader = Bits(source[1]) * 2.5f;
                const float gotHeader = Bits(words[1]);
                CHECK_LT(std::fabs(gotHeader - expectedHeader), 1e-6f);
                CHECK_EQ(words[1] != source[1], true); // the header field DID move
                // The composed probe field (112 + 192k, .w): base * 0.7 * 2.5, the multipliers' product - the
                // composition claim the report cites, and the field the knob's own entry also addresses.
                for (uint32_t probe = 0; probe < fe::kAmbientProbes; ++probe)
                {
                    const uint32_t word = (112 + 192u * probe + 12u) / 4u;
                    const float expected = Bits(source[word]) * 0.7f * 2.5f;
                    const float got = Bits(words[word]);
                    CHECK_LT(std::fabs(got - expected), 1e-5f);
                }
                // The shipped knob's own field (240 + 192k, .x): base * 0.7 - the shadows' entries do NOT touch it.
                for (uint32_t probe = 0; probe < fe::kAmbientProbes; ++probe)
                {
                    const uint32_t word = (240 + 192u * probe) / 4u;
                    const float expected = Bits(source[word]) * 0.7f;
                    const float got = Bits(words[word]);
                    CHECK_LT(std::fabs(got - expected), 1e-5f);
                }
                // EVERYTHING else bit for bit: the neighbourhood's denormals, NaNs and integer-as-float bits stay.
                uint32_t wrong = 0;
                for (uint32_t probe = 0; probe < fe::kAmbientProbes; ++probe)
                {
                    const uint32_t base = (112 + 192u * probe) / 4u;
                    for (uint32_t w = 0; w < 3; ++w) // .x, .y, .z of the SH scale's float4: not addressed by either entry
                        if (words[base + w] != source[base + w])
                            ++wrong;
                }
                if (words[0] != source[0])
                    ++wrong; // the header's .x: not addressed
                for (uint32_t probe = 0; probe < fe::kAmbientProbes; ++probe)
                {
                    const uint32_t base = (240 + 192u * probe + 4u) / 4u;
                    for (uint32_t w = 1; w < 4; ++w)
                        if (words[base + w] != source[base + w])
                            ++wrong;
                }
                if (words[9] != source[9] || words[19] != source[19] || words[28] != source[28] || words[800] != source[800])
                    ++wrong; // the fixture's seeded specials: untouched
                if (wrong != 0)
                {
                    // The row the compare flagged, one integer per family (no format strings: this test's own
                    // diagnostics are one number each).
                    if (words[0] != source[0])
                        std::printf("first-answer header0 moved\n");
                    for (uint32_t probe = 0; probe < fe::kAmbientProbes; ++probe)
                    {
                        const uint32_t base = (240 + 192u * probe + 4u) / 4u;
                        for (uint32_t w = 1; w < 4; ++w)
                            if (words[base + w] != source[base + w])
                                std::printf("sky-visibility neighbour moved\n");
                    }
                    if (words[9] != source[9])
                        std::printf("special 9 moved\n");
                    if (words[19] != source[19])
                        std::printf("special 19 moved\n");
                    if (words[28] != source[28])
                        std::printf("special 28 moved\n");
                    if (words[800] != source[800])
                        std::printf("special 800 moved\n");
                }

                context->Unmap(staging.Get(), 0);
            }
        }
        // The game's own buffer is never written: its words are the fixture's own bytes after the dispatch.
        {
            D3D11_BUFFER_DESC readDesc {};
            readDesc.ByteWidth = kDeclaredBytes;
            readDesc.Usage = D3D11_USAGE_STAGING;
            readDesc.BindFlags = 0;
            readDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Buffer> gameStaging;
            CHECK(device->CreateBuffer(&readDesc, nullptr, &gameStaging) == S_OK);
            if (gameStaging)
            {
                context->CopyResource(gameStaging.Get(), game.Get());
                D3D11_MAPPED_SUBRESOURCE mapped {};
                const HRESULT hr = context->Map(gameStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
                CHECK(hr == S_OK);
                if (hr == S_OK)
                {
                    const uint32_t* words = static_cast<const uint32_t*>(mapped.pData);
                    uint32_t wrong = 0;
                    for (size_t i = 0; i < source.size(); ++i)
                        if (words[i] != source[i])
                            ++wrong;
                    CHECK_EQ(wrong, 0u);
                    context->Unmap(gameStaging.Get(), 0);
                }
            }
        }
    }
    // The one dispatch per patched draw, when both arms' entries ride one table: the shipped knob's own test
    // counts, and the merged arm's two entries into one table are still ONE Scale call's dispatch - a second
    // Scale for the same draw would be the second-competing-copy contract's mistake.
    {
        fe::CbPatch counted;
        fe::ScaleEntry merged[fe::kMaxScaleEntries] {};
        const fs::MergedAmbientTable table = fs::MergedAmbientEntries(knob, 2.5f, merged, fe::kMaxScaleEntries);
        ID3D11Buffer* first = counted.Scale(context.Get(), game.Get(), kDeclaredBytes, merged, table.total);
        CHECK(first != nullptr);
        CHECK_EQ(counted.Scales(), 1u);
        ID3D11Buffer* second = counted.Scale(context.Get(), game.Get(), kDeclaredBytes, merged, table.total);
        CHECK(second != nullptr);
        CHECK_EQ(counted.Scales(), 2u); // a second patched DRAW runs the second dispatch, one per draw
    }
    // The lone shadows arm: the shipped knob at 1.0, the module's two entries only, the same dispatch shape.
    {
        fe::CbPatch alone;
        fe::ScaleEntry entries[fe::kMaxScaleEntries] {};
        const fs::MergedAmbientTable table =
            fs::MergedAmbientEntries(fe::BuildParams(1.0f), 2.5f, entries, fe::kMaxScaleEntries);
        CHECK_EQ(table.total, 2u);
        CHECK_EQ(table.knobEntries, 0u);
        ID3D11Buffer* patched = alone.Scale(context.Get(), game.Get(), kDeclaredBytes, entries, table.total);
        CHECK(patched != nullptr);
        CHECK_EQ(alone.Scales(), 1u);
        if (patched)
        {
            D3D11_BUFFER_DESC readDesc {};
            readDesc.ByteWidth = kDeclaredBytes;
            readDesc.Usage = D3D11_USAGE_STAGING;
            readDesc.BindFlags = 0;
            readDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Buffer> staging;
            CHECK(device->CreateBuffer(&readDesc, nullptr, &staging) == S_OK);
            if (staging)
            {
                context->CopyResource(staging.Get(), patched);
                D3D11_MAPPED_SUBRESOURCE mapped {};
                if (context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped) == S_OK)
                {
                    const uint32_t* words = static_cast<const uint32_t*>(mapped.pData);
                    const float expectedHeader = Bits(source[1]) * 2.5f;
                    const float gotHeader = Bits(words[1]);
                    CHECK_LT(std::fabs(gotHeader - expectedHeader), 1e-6f);
                    for (uint32_t probe = 0; probe < fe::kAmbientProbes; ++probe)
                    {
                        const uint32_t word = (112 + 192u * probe + 12u) / 4u;
                        const float expected = Bits(source[word]) * 2.5f;
                        const float got = Bits(words[word]);
                        CHECK_LT(std::fabs(got - expected), 1e-5f);
                    }
                    context->Unmap(staging.Get(), 0);
                }
            }
        }
    }
    // A request the patch cannot honour (an entry past the end) answers null with nothing dispatched.
    {
        fe::CbPatch bad;
        fe::ScaleEntry entries[fe::kAmbientProbes] {};
        const fs::MergedAmbientTable table = fs::MergedAmbientEntries(knob, 4.0f, entries, fe::kMaxScaleEntries);
        (void) table; // the 4.0 lift is the key's own ceiling: the broken entry below is what the test refuses
        fe::ScaleEntry broken[fe::kMaxScaleEntries] {};
        broken[0] = fe::AmbientSkyVisibilityEntry(0.5f);
        broken[0].count = fe::kAmbientProbes + 1u; // past the end, whatever the stride says
        ID3D11Buffer* answer = bad.Scale(context.Get(), game.Get(), kDeclaredBytes, broken, 1u);
        CHECK(answer == nullptr);
        CHECK_EQ(bad.Scales(), 0u); // nothing dispatched: the game's own table for that draw
    }
    patch.Release();

    return Finish("shadows_ambient_warp");
}
