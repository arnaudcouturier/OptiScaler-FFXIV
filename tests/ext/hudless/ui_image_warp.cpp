// S10 package C: the recomposition mask (ext/hudless/UiImage.cpp + shaders/UiRecomposition.hlsl) on a WARP D3D12 device.
// The real module and the real shader run against hand-built image pairs; the oracle's expected values
// (tests/external_hudless_warp.cpp of the reference build) are the starting data.
#include "ui_harness.h"

#include "ext/gpu/Restrack.h"
#include "ext/hudless/HudlessGpu.h"

using namespace ui_test;
using namespace ext::hudless;

namespace
{
struct Scene
{
    Rig& rig;
    int slot = 0;

    // Runs UiImage::Build on a fresh provider-style list and returns the packed RGBA8 result (empty on null).
    std::vector<uint8_t> Mask(const Image8& finalImage, const Image8& hudless, const MaskParams& params)
    {
        auto finalTex = rig.Upload(finalImage.width, finalImage.height, DXGI_FORMAT_R8G8B8A8_UNORM, finalImage.px.data(), 4);
        auto hudlessTex = rig.Upload(hudless.width, hudless.height, DXGI_FORMAT_R8G8B8A8_UNORM, hudless.px.data(), 4);
        return Mask(finalTex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, hudlessTex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, params, 4);
    }

    std::vector<uint8_t> Mask(ID3D12Resource* finalTex, DXGI_FORMAT finalFormat, ID3D12Resource* hudlessTex,
                              DXGI_FORMAT hudlessFormat, const MaskParams& params, UINT outBytes)
    {
        Check(rig.allocator->Reset(), "allocator Reset");
        Check(ext::gpu::ResetCommands(rig.list.Get(), rig.allocator.Get()), "ResetCommands");
        ID3D12Resource* image =
            UiImage::Build(rig.device.Get(), rig.list.Get(), slot, finalTex, finalFormat, hudlessTex, hudlessFormat, params);
        Check(rig.list->Close(), "Close");
        if (image == nullptr)
            return {};
        ID3D12CommandList* lists[] = { rig.list.Get() };
        ext::gpu::ExecuteCommands(rig.queue.Get(), 1, lists);
        rig.WaitIdle();
        return rig.Download(image, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, outBytes);
    }
};

MaskParams Params(float threshold, uint32_t dilation)
{
    MaskParams params;
    params.threshold = threshold;
    params.dilation = dilation;
    return params;
}

size_t CountAlpha(const std::vector<uint8_t>& out, uint8_t alpha)
{
    size_t n = 0;
    for (size_t i = 3; i < out.size(); i += 4)
        n += out[i] == alpha;
    return n;
}

// Every pixel is either (final.rgb, 255) or (0, 0, 0, 0): alpha strictly 0 or 255, nothing in between.
bool StrictlyBinary(const std::vector<uint8_t>& out, const Image8& finalImage)
{
    for (size_t i = 0; i < out.size(); i += 4)
    {
        if (out[i + 3] == 255)
        {
            if (std::memcmp(&out[i], &finalImage.px[i], 3) != 0)
                return false;
        }
        else if (out[i] != 0 || out[i + 1] != 0 || out[i + 2] != 0 || out[i + 3] != 0)
            return false;
    }
    return true;
}

bool BoxExact(const std::vector<uint8_t>& out, UINT w, UINT h, int x0, int y0, int x1, int y1)
{
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x)
        {
            const bool inside = int(x) >= x0 && int(x) <= x1 && int(y) >= y0 && int(y) <= y1;
            if ((out[(size_t(y) * w + x) * 4 + 3] == 255) != inside)
                return false;
        }
    return true;
}

void OracleFivePixels(Scene& scene)
{
    std::printf("  oracle 5x1 data\n");
    // scene 0.2 everywhere, final differs at pixel 2 (0.8 red): threshold 0.01
    Image8 hudless(5, 1, 51, 51, 51), final = hudless;
    final.at(2, 0)[0] = 204;
    auto r0 = scene.Mask(final, hudless, Params(0.01f, 0));
    EXPECT(r0.size() == 20, "got %zu bytes", r0.size());
    for (UINT i = 0; i < 5 && r0.size() == 20; ++i)
        EXPECT((r0[i * 4 + 3] == 255) == (i == 2), "radius 0 pixel %u alpha %u", i, r0[i * 4 + 3]);
    EXPECT(StrictlyBinary(r0, final), "radius 0 strictly binary");
    EXPECT(r0.size() == 20 && r0[2 * 4] == 204 && r0[2 * 4 + 1] == 51, "flagged pixel keeps the final colour");

    auto r1 = scene.Mask(final, hudless, Params(0.01f, 1));
    for (UINT i = 0; i < 5 && r1.size() == 20; ++i)
        EXPECT((r1[i * 4 + 3] == 255) == (i >= 1 && i <= 3), "radius 1 pixel %u alpha %u", i, r1[i * 4 + 3]);
    EXPECT(StrictlyBinary(r1, final), "radius 1 strictly binary");

    // unchanged scene -> everything zero
    auto none = scene.Mask(hudless, hudless, Params(0.008f, 4));
    EXPECT(CountAlpha(none, 0) == 5 && none == std::vector<uint8_t>(20, 0), "unchanged scene is all zero");
}

void ThresholdEdges(Scene& scene)
{
    std::printf("  threshold edges (8 bit, default 0.008)\n");
    // 2/255 = 0.00784 is below 0.008, 3/255 = 0.01176 above
    Image8 hudless(3, 1, 100, 100, 100), final = hudless;
    final.at(0, 0)[1] = 102;
    final.at(1, 0)[2] = 103;
    auto out = scene.Mask(final, hudless, Params(0.008f, 0));
    EXPECT(out.size() == 12, "size");
    if (out.size() == 12)
    {
        EXPECT(out[3] == 0, "2/255 below threshold flagged (alpha %u)", out[3]);
        EXPECT(out[7] == 255, "3/255 above threshold not flagged (alpha %u)", out[7]);
        EXPECT(out[11] == 0, "identical pixel flagged");
    }
    // the largest channel decides, in any channel
    Image8 f2 = hudless;
    f2.at(0, 0)[0] = 120;
    f2.at(1, 0)[1] = 120;
    f2.at(2, 0)[2] = 120;
    auto channels = scene.Mask(f2, hudless, Params(0.008f, 0));
    EXPECT(CountAlpha(channels, 255) == 3, "each of r,g,b triggers (%zu)", CountAlpha(channels, 255));

    // threshold 1.0: nothing can exceed it
    Image8 black(3, 1, 0, 0, 0), white(3, 1, 255, 255, 255);
    EXPECT(CountAlpha(scene.Mask(white, black, Params(1.0f, 0)), 255) == 0, "threshold 1.0 flags nothing");
    EXPECT(CountAlpha(scene.Mask(white, black, Params(0.999f, 0)), 255) == 3, "threshold 0.999 flags a full swing");
    // NaN falls back to the default instead of disabling the mask
    EXPECT(CountAlpha(scene.Mask(white, black, Params(NAN, 0)), 255) == 3, "NaN threshold uses the default");
}

void HalfFloatEquality(Scene& scene)
{
    std::printf("  threshold equality (R16G16B16A16_FLOAT)\n");
    // |0.75 - 0.5| == 0.25 == threshold exactly: not UI. The next half above 0.75 is: UI.
    const uint16_t one = Half(1.0f);
    uint16_t hudlessPx[3 * 4], finalPx[3 * 4];
    for (int p = 0; p < 3; ++p)
    {
        hudlessPx[p * 4 + 0] = hudlessPx[p * 4 + 1] = hudlessPx[p * 4 + 2] = Half(0.5f);
        hudlessPx[p * 4 + 3] = one;
        finalPx[p * 4 + 0] = finalPx[p * 4 + 1] = finalPx[p * 4 + 2] = Half(0.5f);
        finalPx[p * 4 + 3] = one;
    }
    finalPx[0 * 4 + 0] = Half(0.75f);
    finalPx[1 * 4 + 1] = static_cast<uint16_t>(Half(0.75f) + 1);
    auto finalTex = scene.rig.Upload(3, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, finalPx, 8);
    auto hudlessTex = scene.rig.Upload(3, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, hudlessPx, 8);
    auto out = scene.Mask(finalTex.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, hudlessTex.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                          Params(0.25f, 0), 4);
    EXPECT(out.size() == 12, "size");
    if (out.size() == 12)
    {
        EXPECT(out[3] == 0, "d == threshold must not be flagged (alpha %u)", out[3]);
        EXPECT(out[7] == 255, "d just above the threshold must be flagged (alpha %u)", out[7]);
        EXPECT(out[11] == 0, "equal pixel");
        EXPECT(out[4 + 1] > 190, "FP16 input is read as colour: green %u", out[4 + 1]);
    }
}

void DilationAndClamping(Scene& scene)
{
    std::printf("  dilation 0..5 and image-edge clamping\n");
    const UINT w = 40, h = 32;
    Image8 hudless(w, h, 60, 60, 60);

    struct Spot { UINT x, y; };
    const Spot spots[] = { { 20, 16 }, { 15, 15 }, { 16, 16 }, { 0, 0 }, { 39, 31 }, { 0, 31 }, { 39, 0 } };
    for (const auto& spot : spots)
    {
        Image8 final = hudless;
        final.at(spot.x, spot.y)[0] = 250;
        for (uint32_t d = 0; d <= 5; ++d)
        {
            auto out = scene.Mask(final, hudless, Params(0.008f, d));
            const int r = int(std::min<uint32_t>(d, 4));
            const int x0 = std::max(0, int(spot.x) - r), x1 = std::min(int(w) - 1, int(spot.x) + r);
            const int y0 = std::max(0, int(spot.y) - r), y1 = std::min(int(h) - 1, int(spot.y) + r);
            EXPECT(out.size() == size_t(w) * h * 4 && BoxExact(out, w, h, x0, y0, x1, y1),
                   "spot (%u,%u) dilation %u: expected box (%d,%d)-(%d,%d), %zu flagged", spot.x, spot.y, d, x0, y0, x1, y1,
                   CountAlpha(out, 255));
            EXPECT(StrictlyBinary(out, final), "spot (%u,%u) dilation %u strictly binary", spot.x, spot.y, d);
        }
    }

    // dilation reaches across a group boundary and merges two nearby spots into one blob
    Image8 two = hudless;
    two.at(10, 10)[0] = 250;
    two.at(14, 10)[0] = 250;
    auto out = scene.Mask(two, hudless, Params(0.008f, 2));
    EXPECT(BoxExact(out, w, h, 8, 8, 16, 12), "two spots 4 px apart at dilation 2 form one 9x5 blob");
}

void LargeImages(Scene& scene)
{
    for (const auto size : { std::pair<UINT, UINT> { 5120, 2160 }, std::pair<UINT, UINT> { 5119, 2158 } })
    {
        const UINT w = size.first, h = size.second;
        std::printf("  %ux%u: corners + centre, thresholds\n", w, h);
        Image8 hudless(w, h, 51, 51, 51), final = hudless;
        const std::pair<UINT, UINT> marks[] = { { 0, 0 }, { w - 1, h - 1 }, { w - 1, 0 }, { 0, h - 1 }, { w / 2, h / 2 } };
        for (int i = 0; i < 5; ++i)
            final.at(marks[i].first, marks[i].second)[0] = i < 4 ? 204 : 89;

        auto finalTex = scene.rig.Upload(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, final.px.data(), 4);
        auto hudlessTex = scene.rig.Upload(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, hudless.px.data(), 4);
        // corner marks differ by 153/255 = 0.6, the centre mark by 38/255 = 0.149
        for (const float threshold : { 0.125f, 0.5f, 0.75f })
        {
            auto out = scene.Mask(finalTex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, hudlessTex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                  Params(threshold, 0), 4);
            EXPECT(out.size() == size_t(w) * h * 4, "size");
            if (out.size() != size_t(w) * h * 4)
                continue;
            const size_t flagged = CountAlpha(out, 255);
            EXPECT(flagged == (threshold < 0.15f ? 5u : threshold < 0.6f ? 4u : 0u), "threshold %.3f flagged %zu", threshold,
                   flagged);
            for (int i = 0; i < 5; ++i)
            {
                const auto* px = &out[(size_t(marks[i].second) * w + marks[i].first) * 4];
                const bool expected = threshold < (i < 4 ? 0.6f : 0.149f);
                EXPECT((px[3] == 255) == expected, "threshold %.3f mark %d alpha %u", threshold, i, px[3]);
                if (expected)
                    EXPECT(px[0] == (i < 4 ? 204 : 89), "mark %d keeps the final colour", i);
            }
            EXPECT(CountAlpha(out, 0) + CountAlpha(out, 255) == size_t(w) * h, "alpha strictly 0 or 255");
        }
    }
}

void Lifecycle(Scene& scene)
{
    std::printf("  argument checks, slots, Release\n");
    auto& rig = scene.rig;
    Image8 a(8, 8, 10, 10, 10), b = a;
    b.at(4, 4)[0] = 200;
    auto finalTex = rig.Upload(8, 8, DXGI_FORMAT_R8G8B8A8_UNORM, b.px.data(), 4);
    auto hudlessTex = rig.Upload(8, 8, DXGI_FORMAT_R8G8B8A8_UNORM, a.px.data(), 4);
    auto smallTex = rig.Upload(4, 4, DXGI_FORMAT_R8G8B8A8_UNORM, a.px.data(), 4);
    const MaskParams params;

    // list recording is not needed to reject: nothing is recorded on a null result
    Check(rig.allocator->Reset(), "reset");
    Check(rig.list->Reset(rig.allocator.Get(), nullptr), "reset list");
    auto build = [&](ID3D12Device* d, ID3D12GraphicsCommandList* l, int slot, ID3D12Resource* f, ID3D12Resource* h)
    { return UiImage::Build(d, l, slot, f, DXGI_FORMAT_R8G8B8A8_UNORM, h, DXGI_FORMAT_R8G8B8A8_UNORM, params); };
    EXPECT(build(nullptr, rig.list.Get(), 0, finalTex.Get(), hudlessTex.Get()) == nullptr, "null device");
    EXPECT(build(rig.device.Get(), nullptr, 0, finalTex.Get(), hudlessTex.Get()) == nullptr, "null list");
    EXPECT(build(rig.device.Get(), rig.list.Get(), 0, nullptr, hudlessTex.Get()) == nullptr, "null final");
    EXPECT(build(rig.device.Get(), rig.list.Get(), 0, finalTex.Get(), nullptr) == nullptr, "null hud-less");
    EXPECT(build(rig.device.Get(), rig.list.Get(), -1, finalTex.Get(), hudlessTex.Get()) == nullptr, "slot -1");
    EXPECT(build(rig.device.Get(), rig.list.Get(), 8, finalTex.Get(), hudlessTex.Get()) == nullptr, "slot 8");
    EXPECT(build(rig.device.Get(), rig.list.Get(), 0, finalTex.Get(), smallTex.Get()) == nullptr, "size mismatch");
    EXPECT(build(rig.device.Get(), rig.list.Get(), 0, finalTex.Get(), finalTex.Get()) == nullptr, "same resource twice");
    EXPECT(UiImage::Build(rig.device.Get(), rig.list.Get(), 0, finalTex.Get(), DXGI_FORMAT_D32_FLOAT, hudlessTex.Get(),
                          DXGI_FORMAT_R8G8B8A8_UNORM, params) == nullptr,
           "non-colour format");
    rig.list->Close();

    // same slot -> same image object; different slot -> different; Release then Build works again
    ID3D12Resource* first = nullptr;
    {
        Check(rig.allocator->Reset(), "reset");
        Check(ext::gpu::ResetCommands(rig.list.Get(), rig.allocator.Get()), "reset");
        first = build(rig.device.Get(), rig.list.Get(), 2, finalTex.Get(), hudlessTex.Get());
        EXPECT(first != nullptr, "build ok");
        Check(rig.list->Close(), "close");
        ID3D12CommandList* lists[] = { rig.list.Get() };
        ext::gpu::ExecuteCommands(rig.queue.Get(), 1, lists);
        rig.WaitIdle();
    }
    Microsoft::WRL::ComPtr<ID3D12Resource> held(first);
    scene.slot = 2;
    auto again = scene.Mask(b, a, params);
    EXPECT(again.size() == 8 * 8 * 4 && CountAlpha(again, 255) == 9, "slot 2 rebuilt: %zu flagged", CountAlpha(again, 255));
    scene.slot = 3;
    EXPECT(scene.Mask(b, a, params).size() == 8 * 8 * 4, "slot 3");
    scene.slot = 0;

    // Release retires behind the lists that used the image: the image survives until they were reset AND completed.
    UiImage::Release();
    UiImage::Release();
    EXPECT(held->AddRef() > 2, "retired image is still referenced by the tracker (lists not reset yet)");
    held->Release();
    // Resetting the recorded list notifies the owner; the next Build/Release collects.
    Check(rig.allocator->Reset(), "reset");
    Check(ext::gpu::ResetCommands(rig.list.Get(), rig.allocator.Get()), "reset");
    rig.list->Close();
    UiImage::Release();
    const ULONG remaining = held->AddRef() - 1;
    held->Release();
    EXPECT(remaining == 1, "after the list was reset and finished only the test's reference is left (%lu)", remaining);

    auto afterRelease = scene.Mask(b, a, params);
    EXPECT(afterRelease.size() == 8 * 8 * 4 && CountAlpha(afterRelease, 255) == 9, "Build works after Release");

    // a different size on the same slot replaces the image
    Image8 big(16, 16, 10, 10, 10), bigFinal = big;
    bigFinal.at(8, 8)[0] = 200;
    auto resized = scene.Mask(bigFinal, big, params);
    EXPECT(resized.size() == 16 * 16 * 4 && CountAlpha(resized, 255) == 9, "size change on a used slot");
}
} // namespace

int main()
try
{
    Rig rig;
    std::printf("[ui_image_warp] WARP device, debug layer %s\n", rig.debugLayer ? "on" : "off");
    Scene scene { rig };
    OracleFivePixels(scene);
    ThresholdEdges(scene);
    HalfFloatEquality(scene);
    DilationAndClamping(scene);
    LargeImages(scene);
    Lifecycle(scene);
    std::printf("[ui_image_warp] EnsureHooks calls: %d, failures: %d\n", ext::restrack::ensureHooksCalls, g_failures);
    EXPECT(ext::restrack::ensureHooksCalls >= 1, "owner registered and hooks requested");
    return g_failures == 0 ? 0 : 1;
}
catch (const std::exception& error)
{
    std::printf("[ui_image_warp] exception: %s\n", error.what());
    return 2;
}
