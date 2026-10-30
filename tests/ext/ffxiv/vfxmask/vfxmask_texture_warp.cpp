// [VFXMASK] The mask texture of the VFX mask gate probe (ext/ffxiv/vfxmask/VfxMaskTexture.h) on the WARP software adapter: the
// production code creates the texture, fills it and hands it over, and this test reads the pixels back and checks which half is
// 1.0 for "left" and for "right". Also the re-use rules (same size and mode: the same texture; a new size or mode: a new one) and
// the description guard the probe checks before every write.
//
// A mask whose halves are swapped, or one that is only half filled, would make the in-game comparison silently measure the wrong
// half; the readback is what rules that out.
#define NOMINMAX
#include "ext/ffxiv/vfxmask/VfxMaskTexture.h"

#include <d3d11.h>

#include <cstdio>
#include <tuple>
#include <vector>

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::vfxmask;

namespace
{
int g_failed = 0;
#define CHECK(cond)                                                                                              \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                             \
        {                                                                                                        \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                  \
            ++g_failed;                                                                                          \
        }                                                                                                        \
    } while (0)

struct Device
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;

    bool Create()
    {
        const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                             D3D11_SDK_VERSION, &device, nullptr, &context);
        return SUCCEEDED(hr) && device != nullptr && context != nullptr;
    }
};

// Reads the whole mask back, one row per entry, trimmed to the texture's width. Empty when the copy or the map failed.
std::vector<std::vector<unsigned char>> ReadBack(ID3D11Device* device, ID3D11DeviceContext* context,
                                                 ID3D11Texture2D* texture)
{
    std::vector<std::vector<unsigned char>> rows;
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC staging = desc;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> copy;
    if (FAILED(device->CreateTexture2D(&staging, nullptr, &copy)) || copy == nullptr)
        return rows;

    context->CopyResource(copy.Get(), texture);

    D3D11_MAPPED_SUBRESOURCE map = {};
    if (FAILED(context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &map)) || map.pData == nullptr)
        return rows;

    const auto* bytes = static_cast<const unsigned char*>(map.pData);
    for (unsigned y = 0; y < desc.Height; ++y)
    {
        const unsigned char* row = bytes + static_cast<std::size_t>(y) * map.RowPitch;
        rows.emplace_back(row, row + desc.Width);
    }

    context->Unmap(copy.Get(), 0);
    return rows;
}

// Every pixel of every row is checked against the rule the core states, and both values have to occur.
void CheckHalves(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture, unsigned width,
                 unsigned height, ProbeMode mode)
{
    const auto rows = ReadBack(device, context, texture);
    CHECK(rows.size() == height);
    if (rows.size() != height)
        return;

    bool all = true;
    bool sawMasked = false, sawClear = false;
    for (unsigned y = 0; y < height; ++y)
    {
        CHECK(rows[y].size() == width);
        for (unsigned x = 0; x < width; ++x)
        {
            const bool inside = rows[y][x] == kMaskValue;
            sawMasked = sawMasked || inside;
            sawClear = sawClear || rows[y][x] == kClearValue;
            all = all && inside == MaskedAt(x, width, mode);
        }
    }
    CHECK(all);
    CHECK(sawMasked && sawClear); // both halves are in one texture, not one value everywhere
}

D3D11_TEXTURE2D_DESC Desc(unsigned width, unsigned height)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    return desc;
}
} // namespace

int main()
{
    Device gpu;
    if (!gpu.Create())
    {
        std::puts("SKIP: no WARP device");
        return 0;
    }

    // ---- left and right, on an even and an odd width: the pixels are exactly the rule ------------------------------
    {
        MaskTexture cache;
        // Handles that keep each texture alive, so a rebuilt mask cannot get the address of one that was just released.
        ComPtr<ID3D11Texture2D> leftTexture, oddTexture, resizedTexture, frameTexture;

        const EnsureResult left = EnsureMaskTexture(gpu.context.Get(), 64, 32, ProbeMode::Left, cache);
        leftTexture = left.texture;
        CHECK(left.texture != nullptr && left.reason == nullptr);
        CHECK(cache.width == 64 && cache.height == 32 && cache.mode == ProbeMode::Left);
        if (left.texture != nullptr)
        {
            D3D11_TEXTURE2D_DESC desc = {};
            left.texture->GetDesc(&desc);
            CHECK(desc.Width == 64 && desc.Height == 32);
            CHECK(desc.Format == DXGI_FORMAT_R8_UNORM);
            CHECK(desc.SampleDesc.Count == 1 && desc.MipLevels == 1 && desc.ArraySize == 1);
            CHECK(ValidMaskDesc(desc));
            CheckHalves(gpu.device.Get(), gpu.context.Get(), left.texture, 64, 32, ProbeMode::Left);

            // The two values literally, not only through the constant: 1.0 and 0.0 are what the guide asks for.
            const auto row = ReadBack(gpu.device.Get(), gpu.context.Get(), left.texture);
            CHECK(!row.empty());
            if (!row.empty() && row[0].size() == 64)
            {
                CHECK(row[0][0] == 255 && row[0][31] == 255);
                CHECK(row[0][32] == 0 && row[0][63] == 0);
            }
        }

        // The same size and mode: the texture from the last evaluate is re-used, not rebuilt.
        const EnsureResult again = EnsureMaskTexture(gpu.context.Get(), 64, 32, ProbeMode::Left, cache);
        CHECK(again.texture == leftTexture.Get());

        // The other half: a new texture, filled the other way round.
        const EnsureResult right = EnsureMaskTexture(gpu.context.Get(), 64, 32, ProbeMode::Right, cache);
        CHECK(right.texture != nullptr);
        CHECK(right.texture != leftTexture.Get());
        if (right.texture != nullptr)
            CheckHalves(gpu.device.Get(), gpu.context.Get(), right.texture, 64, 32, ProbeMode::Right);

        // An odd width: the two halves are still exact complements, the right one wider by a column.
        const EnsureResult odd = EnsureMaskTexture(gpu.context.Get(), 5, 3, ProbeMode::Left, cache);
        oddTexture = odd.texture;
        CHECK(odd.texture != nullptr);
        if (odd.texture != nullptr)
            CheckHalves(gpu.device.Get(), gpu.context.Get(), odd.texture, 5, 3, ProbeMode::Left);

        // A resolution change rebuilds it at the new size.
        const EnsureResult resized = EnsureMaskTexture(gpu.context.Get(), 128, 64, ProbeMode::Right, cache);
        resizedTexture = resized.texture;
        CHECK(resized.texture != nullptr);
        CHECK(resized.texture != oddTexture.Get());
        if (resized.texture != nullptr)
        {
            D3D11_TEXTURE2D_DESC desc = {};
            resized.texture->GetDesc(&desc);
            CHECK(desc.Width == 128 && desc.Height == 64);
            CheckHalves(gpu.device.Get(), gpu.context.Get(), resized.texture, 128, 64, ProbeMode::Right);
        }

        // The same mode, another size: a new mask too (the size alone decides, not only a mode change).
        const EnsureResult reshaped = EnsureMaskTexture(gpu.context.Get(), 128, 32, ProbeMode::Right, cache);
        CHECK(reshaped.texture != nullptr);
        CHECK(reshaped.texture != resizedTexture.Get());
        if (reshaped.texture != nullptr)
            CheckHalves(gpu.device.Get(), gpu.context.Get(), reshaped.texture, 128, 32, ProbeMode::Right);

        // A real frame size: only the boundary columns and the last row, the full readback would be 8.3 MB.
        const EnsureResult frame = EnsureMaskTexture(gpu.context.Get(), 3840, 2160, ProbeMode::Left, cache);
        frameTexture = frame.texture;
        CHECK(frame.texture != nullptr);
        if (frame.texture != nullptr)
        {
            const auto rows = ReadBack(gpu.device.Get(), gpu.context.Get(), frame.texture);
            CHECK(rows.size() == 2160);
            if (rows.size() == 2160)
            {
                CHECK(rows[0].size() == 3840);
                CHECK(rows[0][0] == kMaskValue);
                CHECK(rows[0][1919] == kMaskValue);
                CHECK(rows[0][1920] == kClearValue);
                CHECK(rows[0][3839] == kClearValue);
                CHECK(rows[2159][1919] == kMaskValue && rows[2159][1920] == kClearValue);
            }
        }

        // Another device context: the mask belongs to a device, so it is rebuilt there rather than handed across.
        Device second;
        if (second.Create())
        {
            const EnsureResult other = EnsureMaskTexture(second.context.Get(), 3840, 2160, ProbeMode::Left, cache);
            CHECK(other.texture != nullptr);
            CHECK(other.texture != frameTexture.Get());
            if (other.texture != nullptr)
            {
                ComPtr<ID3D11Device> owner;
                other.texture->GetDevice(&owner);
                CHECK(owner.Get() == second.device.Get());
                CheckHalves(second.device.Get(), second.context.Get(), other.texture, 3840, 2160, ProbeMode::Left);
            }
        }
    }

    // ---- the refusals: nothing is created and the caller gets a reason --------------------------------------------
    {
        MaskTexture cache;
        const EnsureResult none = EnsureMaskTexture(nullptr, 64, 32, ProbeMode::Left, cache);
        CHECK(none.texture == nullptr && none.reason != nullptr);
        CHECK(cache.texture == nullptr);

        const std::tuple<unsigned, unsigned, ProbeMode> cases[] = {
            { 64, 32, ProbeMode::Off },          // the key off never builds a mask
            { 1, 32, ProbeMode::Left },          // one column cannot be split
            { 64, 0, ProbeMode::Left },          // nothing to fill
            { kMaxMaskEdge + 1, 32, ProbeMode::Right },
        };
        for (const auto& [w, h, mode] : cases)
        {
            const EnsureResult refused = EnsureMaskTexture(gpu.context.Get(), w, h, mode, cache);
            CHECK(refused.texture == nullptr);
            CHECK(refused.reason != nullptr);
            CHECK(cache.texture == nullptr);
        }
    }

    // ---- the description guard: what the probe checks before every write -----------------------------------------
    {
        const D3D11_TEXTURE2D_DESC good = Desc(3840, 2160);
        CHECK(ValidMaskDesc(good));

        D3D11_TEXTURE2D_DESC d = good;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        CHECK(!ValidMaskDesc(d)); // any format with an R would do in principle; the probe only ever builds one it knows
        d = good;
        d.Format = DXGI_FORMAT_R16_FLOAT;
        CHECK(!ValidMaskDesc(d));
        d = good;
        d.MipLevels = 2;
        CHECK(!ValidMaskDesc(d));
        d = good;
        d.ArraySize = 2;
        CHECK(!ValidMaskDesc(d));
        d = good;
        d.SampleDesc.Count = 4;
        CHECK(!ValidMaskDesc(d));
        d = good;
        d.SampleDesc.Quality = 1; // a quality level with a single sample is not a description a mask may have
        CHECK(!ValidMaskDesc(d));
        d = good;
        d.Usage = D3D11_USAGE_STAGING;
        CHECK(!ValidMaskDesc(d));
        d = good;
        d.Width = 1;
        CHECK(!ValidMaskDesc(d));
        d = good;
        d.Height = 0;
        CHECK(!ValidMaskDesc(d));
        d = good;
        d.Width = kMaxMaskEdge + 1;
        CHECK(!ValidMaskDesc(d));
        d = good;
        d.Height = kMaxMaskEdge + 1;
        CHECK(!ValidMaskDesc(d));
    }

    if (g_failed != 0)
    {
        std::printf("%d check(s) failed\n", g_failed);
        return 1;
    }
    std::puts("PASS: VFX mask probe texture: both halves filled as planned, re-used per size and mode, refusals and the "
              "description guard");
    return 0;
}
