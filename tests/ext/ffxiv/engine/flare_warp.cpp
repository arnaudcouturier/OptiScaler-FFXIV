// Lens flare: the ghost-step patch on D3D11 WARP (ext/ffxiv/engine/CbPatch.cpp running the card's own entry).
//
// The contract a player would feel if it broke: one radial-blur pass's 16-byte cRadialBlurParam comes out with its .w (the step the ghost
// chain marches by - the capture's +0x0C field) scaled by the part's key and its .xyz (the zoom centre and the decay) bit for bit as the
// game wrote them, the game's own buffer is never written, and at the engine's own value the card's rules emit no entry at all - the frame
// the WARP test still runs one identity entry through the primitive to prove the equality gate sits above it, not inside it.
#include "check.h"

#include "ext/ffxiv/engine/CbPatch.h"
#include "ext/ffxiv/engine/FlareRules.h"

#include <d3d11.h>

#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>
#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::flare;
namespace engine = ext::ffxiv::engine;

namespace
{
// A float from its bit pattern, so a fixture can hold values a conversion would change.
float Bits(uint32_t bits)
{
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

// The capture's own row: three radial-blur buffers of one frame, one of which the dump sampled -
// centre (0.7004, 0.1776: the sun's own screen UV), decay 0.0, step 0.05.
constexpr unsigned char kCaptureParameters[16] = { 0x70, 0x15, 0x33, 0x3f, 0x42, 0x4c, 0x33, 0x3e,
                                                   0x00, 0x00, 0x00, 0x00, 0xcd, 0xcc, 0x4c, 0x3d };

constexpr uint32_t kBytes = 16;

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

    // The bytes of a buffer, read back through a staging copy of its own.
    bool ReadBack(ID3D11Buffer* buffer, uint32_t byteWidth, std::vector<unsigned char>& out)
    {
        ComPtr<ID3D11Buffer> staging = Staging(byteWidth);
        if (!staging)
            return false;
        context->CopyResource(staging.Get(), buffer);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)) || mapped.pData == nullptr)
            return false;
        out.assign(static_cast<const unsigned char*>(mapped.pData), static_cast<const unsigned char*>(mapped.pData) + byteWidth);
        context->Unmap(staging.Get(), 0);
        return true;
    }

    float Word(uint32_t index) const
    {
        float value = 0.0f;
        std::memcpy(&value, kCaptureParameters + index * 4, sizeof value);
        return value;
    }
};
} // namespace

int main()
{
    Scene scene;
    CHECK(scene.Create());
    if (!scene.context)
        return Finish("flare_warp");

    ComPtr<ID3D11Buffer> game = scene.ConstantBuffer(kCaptureParameters, kBytes);
    CHECK(game != nullptr);
    if (!game)
        return Finish("flare_warp");

    // ---- the card's rule first: a scale at the engine's own value emits no entry, so the frame swaps nothing at all
    {
        const Params identity = BuildParams(1.0f, 1.0f, 1.0f, 7);
        CHECK_EQ(GhostVerdict(identity).kind, VerdictKind::None);
        CHECK(IsIdentityScale(identity.ghost));
        // The primitive agrees in the other direction: the identity entry it would never be handed is a byte-exact copy (below).
    }

    // ---- one patch, the ghost step at half: the .w scaled, the centre and decay bit for bit as the game wrote them
    {
        engine::CbPatch patch;
        const ext::ffxiv::engine::ScaleEntry entry = GhostEntry(0.5f);
        ID3D11Buffer* patched = patch.Scale(scene.context.Get(), game.Get(), kBytes, &entry, 1);
        CHECK(patched != nullptr);
        CHECK(patch.Failure() == nullptr);
        CHECK_EQ(patch.Scales(), 1u);
        if (patched != nullptr)
        {
            std::vector<unsigned char> bytes;
            CHECK(scene.ReadBack(patched, kBytes, bytes));
            CHECK_EQ(bytes.size(), size_t(kBytes));
            if (bytes.size() == size_t(kBytes))
            {
                // The centre and the decay are the game's own bits (the multiplier of exactly 1 keeps them).
                CHECK(std::memcmp(bytes.data() + 0, kCaptureParameters + 0, 12) == 0);
                // The step is the game's own step halved: a float scaled by a power of two keeps its mantissa, so the expectation is
                // the game's own word multiplied on the CPU, bit for bit.
                const float step = scene.Word(3) * 0.5f;
                float scaled = 0.0f;
                std::memcpy(&scaled, bytes.data() + 12, sizeof scaled);
                CHECK_EQ(std::memcmp(&scaled, &step, sizeof scaled), 0);
                // And the whole buffer differs from the game's, so the comparison above is not two untouched copies.
                CHECK(std::memcmp(bytes.data(), kCaptureParameters, kBytes) != 0);
            }
            // The game's own buffer is never written.
            std::vector<unsigned char> before;
            CHECK(scene.ReadBack(game.Get(), kBytes, before));
            CHECK_EQ(before.size(), size_t(kBytes));
            CHECK(std::memcmp(before.data(), kCaptureParameters, kBytes) == 0);
        }
    }

    // ---- the identity entry the rules never emit: a byte-exact copy, which is what keeps the equality gate safe not approximate
    {
        engine::CbPatch patch;
        const ext::ffxiv::engine::ScaleEntry identity = GhostEntry(1.0f);
        CHECK_EQ(identity.multiplier[3], 1.0f);
        ID3D11Buffer* copy = patch.Scale(scene.context.Get(), game.Get(), kBytes, &identity, 1);
        CHECK(copy != nullptr);
        if (copy != nullptr)
        {
            std::vector<unsigned char> bytes;
            CHECK(scene.ReadBack(copy, kBytes, bytes));
            CHECK_EQ(bytes.size(), size_t(kBytes));
            CHECK(bytes.size() == size_t(kBytes) && std::memcmp(bytes.data(), kCaptureParameters, kBytes) == 0);
        }
    }

    // ---- the ring and the sun's entries run their own buffers on the same terms, so one WARP pass covers both shapes
    {
        // The ring's 64-byte table with the capture's colour row at +0x20 and denormals, NaN payloads and integer-as-float values beside
        // it: the colour scaled in .xyz and the mask fill in .w kept bit for bit.
        std::vector<float> ringValues(12); // four float4s: the row the ring's reflection declares, the +0x20 colour row with its neighbours
        ringValues[0] = 0.4768940f;   // cHaloParam[0].x, the capture's uv scale
        ringValues[1] = 1.0f;
        ringValues[2] = 0.1715f;      // cHaloParam[0].z, the capture's sun uv x
        ringValues[3] = 0.2031f;
        ringValues[4] = 0.0f;
        ringValues[5] = Bits(0x00000001u); // a denormal beside the knob
        ringValues[6] = Bits(0x7FC00001u); // a quiet NaN with a payload
        ringValues[7] = 0.0f;
        ringValues[8] = 0.02985f;      // cHaloParam[2].x, the capture's ring colour at +0x20
        ringValues[9] = 0.03423f;      // .y
        ringValues[10] = 0.04785f;     // .z
        ringValues[11] = 0.152f;       // .w, the mask fill the card leaves
        engine::CbPatch patch;
        const ext::ffxiv::engine::ScaleEntry ring = RingEntry(0.5f);
        ComPtr<ID3D11Buffer> halo = scene.ConstantBuffer(ringValues.data(), 64);
        CHECK(halo != nullptr);
        if (halo)
        {
            ID3D11Buffer* patched = patch.Scale(scene.context.Get(), halo.Get(), 64, &ring, 1);
            CHECK(patched != nullptr);
            CHECK(patch.Failure() == nullptr);
            if (patched != nullptr)
            {
                std::vector<unsigned char> bytes;
                CHECK(scene.ReadBack(patched, 64, bytes));
                CHECK_EQ(bytes.size(), size_t(64));
                if (bytes.size() == size_t(64))
                {
                    // The colour's three words are the game's own halves, and the mask fill beside them keeps every bit.
                    const float colour = ringValues[8] * 0.5f;
                    float scaled = 0.0f;
                    std::memcpy(&scaled, bytes.data() + kRingColourOffset, sizeof scaled);
                    CHECK(std::memcmp(&scaled, &colour, sizeof scaled) == 0);
                    CHECK(std::memcmp(bytes.data() + kRingColourOffset + 12, &ringValues[11], 4) == 0);
                    // The float4 beside the colour row is not touched: its denormal and NaN payloads survive to the bit.
                    CHECK(std::memcmp(bytes.data() + kRingColourOffset - 16, &ringValues[4], 16) == 0);
                }
            }
        }
    }

    return Finish("flare_warp");
}
