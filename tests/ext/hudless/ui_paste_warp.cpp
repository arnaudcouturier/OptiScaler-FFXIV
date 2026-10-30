// S10 package C: the post frame-generation UI paste (ext/hudless/UiPaste.cpp + shaders) on a WARP D3D12 device, through
// the public API only: Produce -> Paste onto a backbuffer -> read the backbuffer back. The real shaders run against
// hand-built image pairs; a CPU reference of docs/HUDLESS.md section 7 checks random images pixel for pixel.
#include "pch.h"
#include "ui_fake_swapchain.h"
#include "ui_harness.h"

#include "ext/gpu/Restrack.h"
#include "ext/hudless/HudlessGpu.h"

#include <algorithm>
#include <cstdlib>
#include <string>

using namespace ui_test;
using namespace ext::hudless;
using Microsoft::WRL::ComPtr;

namespace
{
constexpr UINT8 kBg[4] = { 7, 128, 11, 255 }; // the "generated frame" the paste is drawn over (8 bit)
constexpr uint32_t Pack10(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return r | (g << 10) | (b << 20) | (a << 30);
}
constexpr uint32_t kBg10 = Pack10(11, 128, 0, 3); // 10 bit

struct Bridge
{
    Rig& rig;
    FakeSwapChain swapChain;
    ComPtr<ID3D12CommandQueue> presentQueue = rig.queue;

    void Backbuffer(UINT w, UINT h, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        if (format == DXGI_FORMAT_R8G8B8A8_UNORM)
        {
            Image8 bg(w, h, kBg[0], kBg[1], kBg[2], kBg[3]);
            swapChain.buffer = rig.Upload(w, h, format, bg.px.data(), 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        }
        else
        {
            std::vector<uint32_t> bg(size_t(w) * h, kBg10);
            swapChain.buffer = rig.Upload(w, h, format, bg.data(), 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        }
    }

    ComPtr<ID3D12Resource> Tex(const Image8& image, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        return rig.Upload(image.width, image.height, format, image.px.data(), 4);
    }

    void Produce(const Image8& finalImage, const Image8& hudless, const MaskParams& params,
                 ID3D12Resource* hdrFinal = nullptr, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM,
                 ID3D12CommandQueue* queue = nullptr)
    {
        auto f = Tex(finalImage, format);
        auto h = Tex(hudless, format);
        UiPaste::Produce(rig.device.Get(), queue ? queue : rig.queue.Get(), f.Get(), format, h.Get(), format, hdrFinal, params);
        rig.WaitIdle();
    }

    // Draws the newest image over the backbuffer and returns the backbuffer (RGBA8 or 10:10:10:2 words).
    std::vector<uint8_t> Paste(UINT bytes = 4)
    {
        UiPaste::Paste(&swapChain, presentQueue.Get());
        if (presentQueue.Get() != rig.queue.Get())
            WaitOn(presentQueue.Get());
        rig.WaitIdle();
        return rig.Download(swapChain.buffer.Get(), D3D12_RESOURCE_STATE_COMMON, bytes);
    }

    void WaitOn(ID3D12CommandQueue* queue)
    {
        ComPtr<ID3D12Fence> fence;
        Check(rig.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
        Check(queue->Signal(fence.Get(), 1), "signal");
        Check(fence->SetEventOnCompletion(1, rig.event), "event");
        if (WaitForSingleObject(rig.event, 60000) != WAIT_OBJECT_0)
            throw std::runtime_error("queue wait timed out");
    }
};

MaskParams Params(float threshold, uint32_t dilation, bool cleanup = true)
{
    MaskParams p;
    p.threshold = threshold;
    p.dilation = dilation;
    p.cleanup = cleanup;
    return p;
}

void Fill(Image8& image, UINT x0, UINT y0, UINT x1, UINT y1, uint8_t r, uint8_t g, uint8_t b)
{
    for (UINT y = y0; y <= y1; ++y)
        for (UINT x = x0; x <= x1; ++x)
        {
            image.at(x, y)[0] = r;
            image.at(x, y)[1] = g;
            image.at(x, y)[2] = b;
        }
}

bool IsBg(const uint8_t* px)
{
    return px[0] == kBg[0] && px[1] == kBg[1] && px[2] == kBg[2] && px[3] == kBg[3];
}

size_t CountNonBg(const std::vector<uint8_t>& out)
{
    size_t n = 0;
    for (size_t i = 0; i < out.size(); i += 4)
        n += !IsBg(&out[i]);
    return n;
}

bool Near(int a, int b, int tolerance)
{
    return std::abs(a - b) <= tolerance;
}

// CPU reference of docs/HUDLESS.md section 7: isUi per pixel.
std::vector<uint8_t> ReferenceMask(const Image8& F, const Image8& H, float threshold, uint32_t radius)
{
    const int w = int(F.width), h = int(F.height), r = int(std::min<uint32_t>(radius, 4));
    std::vector<uint8_t> changed(size_t(w) * h), ui(size_t(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            float d = 0.0f;
            for (int c = 0; c < 3; ++c)
                d = std::max(d, std::abs(float(F.at(x, y)[c]) / 255.0f - float(H.at(x, y)[c]) / 255.0f));
            changed[size_t(y) * w + x] = d > threshold;
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            bool hit = false;
            for (int dy = -r; dy <= r && !hit; ++dy)
                for (int dx = -r; dx <= r && !hit; ++dx)
                {
                    const int xx = std::clamp(x + dx, 0, w - 1), yy = std::clamp(y + dy, 0, h - 1);
                    hit = changed[size_t(yy) * w + xx] != 0;
                }
            ui[size_t(y) * w + x] = hit;
        }
    return ui;
}

// CPU reference of the amplitude counters (docs/HUDLESS.md section 7): pixels whose WINDOWED difference reaches `level`.
// It mirrors the shader operation for operation, so the float comparison at a level is the shader's too.
size_t ReferenceAmplitude(const Image8& F, const Image8& H, uint32_t radius, float level)
{
    const int w = int(F.width), h = int(F.height), r = int(std::min<uint32_t>(radius, 4));
    std::vector<float> diff(size_t(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            float d = 0.0f;
            for (int c = 0; c < 3; ++c)
                d = std::max(d, std::abs(float(F.at(x, y)[c]) / 255.0f - float(H.at(x, y)[c]) / 255.0f));
            diff[size_t(y) * w + x] = d;
        }
    size_t count = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            float windowMax = 0.0f;
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx)
                    windowMax =
                        std::max(windowMax, diff[size_t(std::clamp(y + dy, 0, h - 1)) * w + std::clamp(x + dx, 0, w - 1)]);
            count += windowMax > level;
        }
    return count;
}

// A share of the picture, as the readback reports it.
bool ShareIs(float got, size_t count, UINT pixels)
{
    return std::abs(got - float(count) / float(pixels)) < 1e-6f;
}

// ---------------------------------------------------------------------------------------------------------------------

void SafeBeforeInit(Rig& rig)
{
    std::printf("  every call is safe before init\n");
    UiPaste::Release();
    UiPaste::Invalidate("test");
    UiPaste::Invalidate(nullptr);
    UiPaste::SetDebugTint(false);
    UiPaste::SetSteadyTiming(false);
    UiPaste::SetPasteEnabled(true);
    EXPECT(!UiPaste::Ready(rig.device.Get(), 8, 8), "Ready before any Produce");
    EXPECT(!UiPaste::Ready(nullptr, 0, 0), "Ready with nulls");
    const auto s = UiPaste::Snapshot();
    EXPECT(s.produced == 0 && s.pasted == 0 && s.coverage < 0.f, "empty snapshot (coverage %f)", s.coverage);
    EXPECT(s.coverageSequence == 0, "empty snapshot has no coverage sequence (%llu)", (unsigned long long) s.coverageSequence);
    UiPaste::Paste(nullptr, nullptr);
    UiPaste::Paste(nullptr, rig.queue.Get());
    FakeSwapChain fake;
    UiPaste::Paste(&fake, nullptr);
    UiPaste::Paste(&fake, rig.queue.Get()); // no images yet: silently nothing, not counted as a frame-generation skip
    EXPECT(UiPaste::Snapshot().skipped == 0, "no skip counted before the first image");
    UiPaste::Produce(nullptr, nullptr, nullptr, DXGI_FORMAT_UNKNOWN, nullptr, DXGI_FORMAT_UNKNOWN, nullptr, MaskParams {});
    UiPaste::Release();
    UiPaste::Release();
    EXPECT(UiPaste::Snapshot().produced == 0, "nothing produced from null arguments");
}

void BasicBlock(Rig& rig)
{
    std::printf("  basic block, dilation 1, coverage counter, log lines\n");
    Bridge bridge { rig };
    const UINT w = 40, h = 32;
    Image8 hudless(w, h, 60, 60, 60), final = hudless;
    Fill(final, 10, 8, 13, 11, 200, 50, 20);
    bridge.Backbuffer(w, h);
    bridge.Produce(final, hudless, Params(0.008f, 1));
    EXPECT(UiPaste::Ready(rig.device.Get(), w, h), "Ready after a completed Produce");
    EXPECT(!UiPaste::Ready(rig.device.Get(), w + 1, h), "Ready is size specific");
    const auto out = bridge.Paste();
    EXPECT(out.size() == size_t(w) * h * 4, "readback size");
    bool exact = out.size() == size_t(w) * h * 4;
    for (UINT y = 0; exact && y < h; ++y)
        for (UINT x = 0; exact && x < w; ++x)
        {
            const bool inBlock = x >= 10 && x <= 13 && y >= 8 && y <= 11;
            const bool inRing = x >= 9 && x <= 14 && y >= 7 && y <= 12 && !inBlock;
            const uint8_t* px = &out[(size_t(y) * w + x) * 4];
            if (inBlock)
                exact = px[0] == 200 && px[1] == 50 && px[2] == 20 && px[3] == 255;
            else if (inRing)
                exact = px[0] == 60 && px[1] == 60 && px[2] == 60 && px[3] == 255; // dilation: final colour, not background
            else
                exact = IsBg(px);
        }
    EXPECT(exact, "block pasted with its final colour, 1 px dilation ring pasted, the rest untouched");

    const auto status = UiPaste::Snapshot();
    EXPECT(status.produced == 1 && status.pasted == 1 && status.skipped == 0, "counters %llu/%llu/%llu",
           (unsigned long long) status.produced, (unsigned long long) status.pasted, (unsigned long long) status.skipped);
    EXPECT(std::abs(status.coverage - 36.0f / float(w * h)) < 1e-6f, "coverage %f, expected %f", status.coverage,
           36.0f / float(w * h));
    EXPECT(status.coverageSequence == 1, "the first readback belongs to the first image (sequence %llu)",
           (unsigned long long) status.coverageSequence);

    // the backbuffer reference is returned (ResizeBuffers would fail otherwise)
    {
        ID3D12Resource* bb = bridge.swapChain.buffer.Get();
        bb->AddRef();
        const ULONG before = bb->Release();
        UiPaste::Paste(&bridge.swapChain, rig.queue.Get());
        rig.WaitIdle();
        bb->AddRef();
        const ULONG after = bb->Release();
        EXPECT(before == after, "Paste keeps no backbuffer reference (%lu -> %lu)", before, after);
    }

    EXPECT(ui_test::CountInfo("UI paste: producer ready") == 1, "producer ready logged once");
    EXPECT(ui_test::CountInfo("UI paste: first UI image built (40x32)") == 1, "first image logged once");
    EXPECT(ui_test::CountInfo("UI paste: first paste after frame generation (40x32, backbuffer format 28)") == 1,
           "first paste logged once");
    for (int i = 0; i < 3; ++i)
    {
        bridge.Produce(final, hudless, Params(0.008f, 1));
        bridge.Paste();
    }
    EXPECT(ui_test::CountInfo("UI paste: producer ready") == 1 && ui_test::CountInfo("UI paste: first") == 2,
           "nothing is logged per frame");

    // one fresh readback per image: the sequence follows the ordinal of the produced image, strictly increasing
    {
        const auto now = UiPaste::Snapshot();
        EXPECT(now.produced == 4 && now.coverageSequence == 4, "sequence %llu follows produced %llu after four images",
               (unsigned long long) now.coverageSequence, (unsigned long long) now.produced);
        UiPaste::Release();
        const auto released = UiPaste::Snapshot();
        EXPECT(released.coverage < 0.f && released.coverageSequence == 0,
               "Release makes the coverage unknown again (coverage %f, sequence %llu)", released.coverage,
               (unsigned long long) released.coverageSequence);
        EXPECT(released.produced == 4, "the produced ordinal keeps counting through Release");
        bridge.Backbuffer(w, h);
        bridge.Produce(final, hudless, Params(0.008f, 1));
        const auto again = UiPaste::Snapshot();
        EXPECT(again.produced == 5 && again.coverageSequence == 5 && again.coverage >= 0.f,
               "the ordinal of the first image after Release is 5, never reused (sequence %llu)",
               (unsigned long long) again.coverageSequence);
    }
    UiPaste::Release();
}

void UnchangedScene(Rig& rig)
{
    std::printf("  unchanged scene: nothing pasted\n");
    Bridge bridge { rig };
    Image8 scene(33, 17, 90, 80, 70);
    bridge.Backbuffer(33, 17);
    bridge.Produce(scene, scene, Params(0.008f, 4));
    const auto out = bridge.Paste();
    EXPECT(out.size() == 33 * 17 * 4 && CountNonBg(out) == 0, "no pixel touched");
    EXPECT(UiPaste::Snapshot().coverage == 0.0f, "coverage 0 (%f)", UiPaste::Snapshot().coverage);
    UiPaste::Release();
}

void ThresholdAndDilation(Rig& rig)
{
    std::printf("  threshold edges and dilation 0..5 through the paste\n");
    Bridge bridge { rig };
    {
        Image8 hudless(4, 1, 100, 100, 100), final = hudless;
        final.at(0, 0)[1] = 102; // 2/255 < 0.008
        final.at(1, 0)[2] = 103; // 3/255 > 0.008
        bridge.Backbuffer(4, 1);
        bridge.Produce(final, hudless, Params(0.008f, 0));
        const auto out = bridge.Paste();
        EXPECT(out.size() == 16 && IsBg(&out[0]) && !IsBg(&out[4]) && IsBg(&out[8]) && IsBg(&out[12]),
               "only the 3/255 pixel is pasted");
        UiPaste::Release();
    }
    const UINT w = 40, h = 32;
    struct Spot { UINT x, y; };
    for (const Spot spot : { Spot { 20, 16 }, Spot { 15, 15 }, Spot { 0, 0 }, Spot { 39, 31 }, Spot { 0, 31 } })
        for (uint32_t d = 0; d <= 5; ++d)
        {
            Image8 hudless(w, h, 60, 60, 60), final = hudless;
            final.at(spot.x, spot.y)[0] = 250;
            bridge.Backbuffer(w, h);
            bridge.Produce(final, hudless, Params(0.008f, d));
            const auto out = bridge.Paste();
            const int r = int(std::min<uint32_t>(d, 4));
            const int x0 = std::max(0, int(spot.x) - r), x1 = std::min(int(w) - 1, int(spot.x) + r);
            const int y0 = std::max(0, int(spot.y) - r), y1 = std::min(int(h) - 1, int(spot.y) + r);
            bool exact = out.size() == size_t(w) * h * 4;
            for (UINT y = 0; exact && y < h; ++y)
                for (UINT x = 0; exact && x < w; ++x)
                {
                    const bool inside = int(x) >= x0 && int(x) <= x1 && int(y) >= y0 && int(y) <= y1;
                    exact = (!IsBg(&out[(size_t(y) * w + x) * 4])) == inside;
                }
            EXPECT(exact, "spot (%u,%u) dilation %u: pasted area is the clamped (2r+1)^2 box", spot.x, spot.y, d);
            const float expected = float((x1 - x0 + 1) * (y1 - y0 + 1)) / float(w * h);
            EXPECT(std::abs(UiPaste::Snapshot().coverage - expected) < 1e-6f, "spot (%u,%u) dilation %u coverage %f vs %f",
                   spot.x, spot.y, d, UiPaste::Snapshot().coverage, expected);
            UiPaste::Release();
        }
}

void CleanupLevels(Rig& rig)
{
    std::printf("  cleanup: alpha 0.5 appears exactly one frame after the UI is gone (debug tint shows the levels)\n");
    Bridge bridge { rig };
    const UINT w = 24, h = 16;
    Image8 hudless(w, h, 60, 60, 60), withUi = hudless;
    Fill(withUi, 8, 6, 11, 9, 200, 50, 20);
    UiPaste::SetDebugTint(true);

    auto region = [&](const std::vector<uint8_t>& out, UINT x, UINT y) { return &out[(size_t(y) * w + x) * 4]; };

    bridge.Backbuffer(w, h);
    bridge.Produce(withUi, hudless, Params(0.008f, 0));
    auto f1 = bridge.Paste();
    // alpha 1 -> magenta tint: lerp(c, (1,0,1), 0.35)
    EXPECT(Near(region(f1, 9, 7)[0], int(200 * 0.65 + 255 * 0.35), 2) && Near(region(f1, 9, 7)[1], int(50 * 0.65), 2) &&
               Near(region(f1, 9, 7)[2], int(20 * 0.65 + 255 * 0.35), 2),
           "frame with UI: magenta tint (%u,%u,%u)", region(f1, 9, 7)[0], region(f1, 9, 7)[1], region(f1, 9, 7)[2]);

    bridge.Backbuffer(w, h);
    bridge.Produce(hudless, hudless, Params(0.008f, 0)); // UI closed
    auto f2 = bridge.Paste();
    // alpha 0.5 -> cyan tint: lerp(c, (0,1,1), 0.35) with c = the (now UI-free) final colour 60
    EXPECT(Near(region(f2, 9, 7)[0], int(60 * 0.65), 2) && Near(region(f2, 9, 7)[1], int(60 * 0.65 + 255 * 0.35), 2) &&
               Near(region(f2, 9, 7)[2], int(60 * 0.65 + 255 * 0.35), 2),
           "one frame later: cyan cleanup tint (%u,%u,%u)", region(f2, 9, 7)[0], region(f2, 9, 7)[1], region(f2, 9, 7)[2]);
    EXPECT(CountNonBg(f2) == 16, "cleanup covers exactly the old UI area (%zu pixels)", CountNonBg(f2));

    bridge.Backbuffer(w, h);
    bridge.Produce(hudless, hudless, Params(0.008f, 0)); // still no UI
    auto f3 = bridge.Paste();
    EXPECT(CountNonBg(f3) == 0, "two frames later nothing is pasted (%zu pixels)", CountNonBg(f3));

    // without the tint the cleanup pixels carry the final colour: the ghost in the generated frame is overwritten
    UiPaste::SetDebugTint(false);
    bridge.Backbuffer(w, h);
    bridge.Produce(withUi, hudless, Params(0.008f, 0));
    bridge.Paste();
    bridge.Backbuffer(w, h);
    bridge.Produce(hudless, hudless, Params(0.008f, 0));
    auto plain = bridge.Paste();
    EXPECT(CountNonBg(plain) == 16 && region(plain, 9, 7)[0] == 60 && region(plain, 9, 7)[3] == 255,
           "cleanup pastes the final colour");

    // cleanup off: no ghost pass
    bridge.Backbuffer(w, h);
    bridge.Produce(withUi, hudless, Params(0.008f, 0, false));
    bridge.Paste();
    bridge.Backbuffer(w, h);
    bridge.Produce(hudless, hudless, Params(0.008f, 0, false));
    EXPECT(CountNonBg(bridge.Paste()) == 0, "PasteCleanup = false: nothing after the UI closed");

    // a skipped frame breaks the chain: no cleanup after Invalidate
    bridge.Backbuffer(w, h);
    bridge.Produce(withUi, hudless, Params(0.008f, 0));
    UiPaste::Invalidate("test");
    bridge.Produce(hudless, hudless, Params(0.008f, 0));
    EXPECT(CountNonBg(bridge.Paste()) == 0, "no cleanup input after an invalidated frame");
    UiPaste::Release();
}

void RandomAgainstReference(Rig& rig)
{
    std::printf("  random images against the CPU reference (full image, exact)\n");
    Bridge bridge { rig };
    uint64_t seed = 0x1234567899ABCDEFull;
    auto rnd = [&](uint32_t n)
    {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        return uint32_t((seed >> 33) % n);
    };
    struct Case { UINT w, h; uint32_t dilation; float threshold; };
    const Case cases[] = { { 61, 37, 1, 0.0095f }, { 33, 17, 2, 0.0595f }, { 17, 33, 0, 0.201f },
                           { 5, 3, 4, 0.0095f },   { 16, 16, 3, 0.0595f }, { 48, 32, 1, 0.201f },
                           { 1, 1, 1, 0.0095f },   { 130, 9, 2, 0.0095f } };
    for (const auto& c : cases)
    {
        Image8 hudless(c.w, c.h, 0, 0, 0), final = hudless;
        for (UINT y = 0; y < c.h; ++y)
            for (UINT x = 0; x < c.w; ++x)
                for (int ch = 0; ch < 3; ++ch)
                {
                    const uint8_t base = uint8_t(rnd(256));
                    hudless.at(x, y)[ch] = base;
                    int value = base;
                    if (rnd(40) == 0) // ~2.5 % of the channels differ, by 1..40 steps
                        value = std::clamp(int(base) + int(rnd(80)) - 40, 0, 255);
                    final.at(x, y)[ch] = uint8_t(value);
                }
        const auto ui = ReferenceMask(final, hudless, c.threshold, c.dilation);
        size_t flagged = 0;
        for (auto v : ui)
            flagged += v;
        bridge.Backbuffer(c.w, c.h);
        bridge.Produce(final, hudless, Params(c.threshold, c.dilation));
        const auto out = bridge.Paste();
        bool exact = out.size() == size_t(c.w) * c.h * 4;
        size_t mismatches = 0;
        for (UINT y = 0; exact && y < c.h; ++y)
            for (UINT x = 0; x < c.w; ++x)
            {
                const uint8_t* got = &out[(size_t(y) * c.w + x) * 4];
                const bool wantUi = ui[size_t(y) * c.w + x] != 0;
                const bool ok = wantUi ? (got[0] == final.at(x, y)[0] && got[1] == final.at(x, y)[1] &&
                                          got[2] == final.at(x, y)[2] && got[3] == 255)
                                       : IsBg(got);
                mismatches += !ok;
            }
        EXPECT(exact && mismatches == 0, "%ux%u dilation %u threshold %.4f: %zu of %u pixels differ from the reference (%zu flagged)",
               c.w, c.h, c.dilation, c.threshold, mismatches, c.w * c.h, flagged);
        const float coverage = UiPaste::Snapshot().coverage;
        EXPECT(std::abs(coverage - float(flagged) / float(c.w * c.h)) < 1e-6f, "%ux%u coverage %f vs reference %f", c.w, c.h,
               coverage, float(flagged) / float(c.w * c.h));
        const auto amp = UiPaste::Snapshot();
        const size_t low = ReferenceAmplitude(final, hudless, c.dilation, 0.008f);
        const size_t mid = ReferenceAmplitude(final, hudless, c.dilation, 0.05f);
        const size_t high = ReferenceAmplitude(final, hudless, c.dilation, 0.2f);
        EXPECT(ShareIs(amp.coverage008, low, c.w * c.h) && ShareIs(amp.coverage05, mid, c.w * c.h) &&
                   ShareIs(amp.coverage2, high, c.w * c.h),
               "%ux%u dilation %u amplitude %f/%f/%f vs reference %zu/%zu/%zu", c.w, c.h, c.dilation, amp.coverage008,
               amp.coverage05, amp.coverage2, low, mid, high);
        EXPECT(low >= mid && mid >= high, "the bands are nested (%zu/%zu/%zu)", low, mid, high);
        UiPaste::Release();
    }
}

void AmplitudeCounters(Rig& rig)
{
    std::printf("  amplitude counters: fixed 0.008/0.05/0.2 levels, windowed, independent of the threshold\n");
    Bridge bridge { rig };
    const UINT w = 40, h = 32;

    // (a) an opaque block: every level reached, and the guard's own coverage is the number it always was (6x6 at dilation 1)
    {
        Image8 hudless(w, h, 60, 60, 60), final = hudless;
        Fill(final, 10, 8, 13, 11, 200, 50, 20); // d = 140/255
        bridge.Backbuffer(w, h);
        bridge.Produce(final, hudless, Params(0.008f, 1));
        const auto s = UiPaste::Snapshot();
        EXPECT(ShareIs(s.coverage, 36, w * h), "opaque block: guard coverage %f, expected 36 pixels", s.coverage);
        EXPECT(ShareIs(s.coverage008, 36, w * h) && ShareIs(s.coverage05, 36, w * h) && ShareIs(s.coverage2, 36, w * h),
               "opaque block: amplitude %f/%f/%f, expected 36 pixels each", s.coverage008, s.coverage05, s.coverage2);
        UiPaste::Release();
    }

    // (b) the same block at 3/255: the low band only (a 0.05 or 0.2 level would report 36 here too)
    {
        Image8 hudless(w, h, 60, 60, 60), final = hudless;
        Fill(final, 10, 8, 13, 11, 63, 60, 60); // d = 3/255 = 0.0118
        bridge.Backbuffer(w, h);
        bridge.Produce(final, hudless, Params(0.008f, 1));
        const auto s = UiPaste::Snapshot();
        EXPECT(ShareIs(s.coverage, 36, w * h) && ShareIs(s.coverage008, 36, w * h),
               "3/255: guard %f and low band %f, expected 36 pixels each", s.coverage, s.coverage008);
        EXPECT(s.coverage05 == 0.f && s.coverage2 == 0.f, "3/255 stays below the 0.05 and 0.2 levels (%f/%f)",
               s.coverage05, s.coverage2);
        UiPaste::Release();
    }

    // (c) 26/255: low and middle band, not the top one
    {
        Image8 hudless(w, h, 60, 60, 60), final = hudless;
        Fill(final, 10, 8, 13, 11, 86, 60, 60); // d = 26/255 = 0.102
        bridge.Backbuffer(w, h);
        bridge.Produce(final, hudless, Params(0.008f, 1));
        const auto s = UiPaste::Snapshot();
        EXPECT(ShareIs(s.coverage008, 36, w * h) && ShareIs(s.coverage05, 36, w * h),
               "26/255: low %f and middle %f, expected 36 pixels each", s.coverage008, s.coverage05);
        EXPECT(s.coverage2 == 0.f, "26/255 stays below the 0.2 level (%f)", s.coverage2);
        UiPaste::Release();
    }

    // (d) nothing differs: all three zero, and the guard too
    {
        Image8 scene(w, h, 90, 80, 70);
        bridge.Backbuffer(w, h);
        bridge.Produce(scene, scene, Params(0.008f, 2));
        const auto s = UiPaste::Snapshot();
        EXPECT(s.coverage == 0.f && s.coverage008 == 0.f && s.coverage05 == 0.f && s.coverage2 == 0.f,
               "identical images: every counter zero (%f/%f/%f/%f)", s.coverage, s.coverage008, s.coverage05, s.coverage2);
        UiPaste::Release();
    }

    // (e) the counters follow the window, not the pixel: one 3/255 pixel counts 1, 9 and 25 times at dilation 0, 1 and 2
    for (uint32_t d = 0; d <= 2; ++d)
    {
        Image8 hudless(w, h, 60, 60, 60), final = hudless;
        final.at(20, 16)[0] = 63; // one pixel, 3/255
        bridge.Backbuffer(w, h);
        bridge.Produce(final, hudless, Params(0.008f, d));
        const auto s = UiPaste::Snapshot();
        const size_t expected = size_t(2 * int(std::min<uint32_t>(d, 4)) + 1) * (2 * std::min<uint32_t>(d, 4) + 1);
        EXPECT(ShareIs(s.coverage008, expected, w * h), "dilation %u: low band %f, expected %zu pixels", d, s.coverage008,
               expected);
        EXPECT(s.coverage05 == 0.f && s.coverage2 == 0.f, "dilation %u: a 3/255 window never reaches 0.05 (%f/%f)", d,
               s.coverage05, s.coverage2);
        UiPaste::Release();
    }

    // (f) a smooth gradient: all three bands differ, and they agree with the CPU reference
    {
        Image8 hudless(w, h, 60, 60, 60), final = hudless;
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x)
                for (int c = 0; c < 3; ++c)
                    final.at(x, y)[c] = uint8_t(std::min(255, 60 + int(x * 60 / (w - 1)))); // 0..60 steps across
        const size_t low = ReferenceAmplitude(final, hudless, 1, 0.008f);
        const size_t mid = ReferenceAmplitude(final, hudless, 1, 0.05f);
        const size_t high = ReferenceAmplitude(final, hudless, 1, 0.2f);
        EXPECT(low > mid && mid > high && high > 0, "gradient: the bands grow apart (%zu/%zu/%zu)", low, mid, high);

        bridge.Backbuffer(w, h);
        bridge.Produce(final, hudless, Params(0.008f, 1));
        const auto lowThreshold = UiPaste::Snapshot();
        EXPECT(ShareIs(lowThreshold.coverage008, low, w * h) && ShareIs(lowThreshold.coverage05, mid, w * h) &&
                   ShareIs(lowThreshold.coverage2, high, w * h),
               "gradient at threshold 0.008: amplitude %f/%f/%f vs reference %zu/%zu/%zu", lowThreshold.coverage008,
               lowThreshold.coverage05, lowThreshold.coverage2, low, mid, high);

        // the same pair at MaskThreshold = 0.2: the same three numbers, a much smaller guard coverage
        bridge.Backbuffer(w, h);
        bridge.Produce(final, hudless, Params(0.2f, 1));
        const auto highThreshold = UiPaste::Snapshot();
        EXPECT(highThreshold.coverage008 == lowThreshold.coverage008 && highThreshold.coverage05 == lowThreshold.coverage05 &&
                   highThreshold.coverage2 == lowThreshold.coverage2,
               "threshold 0.2 gives the same amplitude numbers (%f/%f/%f vs %f/%f/%f)", highThreshold.coverage008,
               highThreshold.coverage05, highThreshold.coverage2, lowThreshold.coverage008, lowThreshold.coverage05,
               lowThreshold.coverage2);
        EXPECT(highThreshold.coverage < lowThreshold.coverage,
               "the guard follows the configured threshold (%f at 0.008, %f at 0.2)", lowThreshold.coverage,
               highThreshold.coverage);
        UiPaste::Release();
    }

    // (g) cleanup pixels are pasted but never counted: the frame that clears a ghost has no difference of its own
    {
        Image8 hudless(w, h, 60, 60, 60), withUi = hudless;
        Fill(withUi, 10, 8, 13, 11, 200, 50, 20);
        bridge.Backbuffer(w, h);
        bridge.Produce(withUi, hudless, Params(0.008f, 0));
        bridge.Paste();
        bridge.Backbuffer(w, h);
        bridge.Produce(hudless, hudless, Params(0.008f, 0)); // the UI just closed: cleanup pixels are drawn
        const auto out = bridge.Paste();
        const auto s = UiPaste::Snapshot();
        EXPECT(CountNonBg(out) == 16, "cleanup still pastes the old UI area (%zu pixels)", CountNonBg(out));
        EXPECT(s.coverage == 0.f && s.coverage008 == 0.f && s.coverage05 == 0.f && s.coverage2 == 0.f,
               "cleanup pixels are not counted (%f/%f/%f/%f)", s.coverage, s.coverage008, s.coverage05, s.coverage2);
        UiPaste::Release();
    }

    // (h) the counters are zeroed before every dispatch: the same pair five times (one ring wrap) never accumulates
    {
        Image8 hudless(w, h, 60, 60, 60), final = hudless;
        Fill(final, 10, 8, 13, 11, 200, 50, 20);
        bridge.Backbuffer(w, h);
        bool steady = true;
        for (int i = 0; i < 5; ++i)
        {
            bridge.Produce(final, hudless, Params(0.008f, 0));
            const auto s = UiPaste::Snapshot();
            steady = steady && ShareIs(s.coverage, 16, w * h) && ShareIs(s.coverage008, 16, w * h) &&
                     ShareIs(s.coverage05, 16, w * h) && ShareIs(s.coverage2, 16, w * h);
        }
        EXPECT(steady, "five identical images: every counter stays at 16 pixels");
        UiPaste::Release();
    }
}

void TypelessAndHdr(Rig& rig)
{
    std::printf("  typeless inputs, HDR: SDR mask + PQ colours, 10-bit storage keeps the three alpha levels\n");
    Bridge bridge { rig };
    const UINT w = 24, h = 16;
    Image8 hudless(w, h, 60, 60, 60), withUi = hudless;
    Fill(withUi, 8, 6, 11, 9, 200, 50, 20);

    // typeless R8G8B8A8 (the bridge's copies)
    bridge.Backbuffer(w, h);
    bridge.Produce(withUi, hudless, Params(0.008f, 0), nullptr, DXGI_FORMAT_R8G8B8A8_TYPELESS);
    EXPECT(CountNonBg(bridge.Paste()) == 16, "typeless inputs are read as UNORM");
    UiPaste::Release();

    // HDR: mask from the SDR pair, colours from the PQ image (a different value at every pixel)
    std::vector<uint32_t> pq(size_t(w) * h);
    for (UINT i = 0; i < w * h; ++i)
        pq[i] = Pack10(100 + i % 700, 200 + (i * 3) % 600, 300 + (i * 5) % 700, 3);
    auto hdrCommon = rig.Upload(w, h, DXGI_FORMAT_R10G10B10A2_UNORM, pq.data(), 4);
    // the converter leaves its output in NON_PIXEL_SHADER_RESOURCE: a second copy in that state
    auto hdrNpsr = rig.Upload(w, h, DXGI_FORMAT_R10G10B10A2_UNORM, pq.data(), 4);
    rig.Run([&](ID3D12GraphicsCommandList* c)
            { Rig::Barrier(c, hdrNpsr.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); });

    for (int variant = 0; variant < 2; ++variant)
    {
        bridge.Backbuffer(w, h, DXGI_FORMAT_R10G10B10A2_UNORM);
        MaskParams params = Params(0.008f, 1);
        params.hdr = true;
        bridge.Produce(withUi, hudless, params, variant == 0 ? hdrCommon.Get() : hdrNpsr.Get());
        const auto out = bridge.Paste();
        const auto* words = reinterpret_cast<const uint32_t*>(out.data());
        bool exact = out.size() == size_t(w) * h * 4;
        size_t pasted = 0;
        for (UINT y = 0; exact && y < h; ++y)
            for (UINT x = 0; x < w; ++x)
            {
                const bool ui = x >= 7 && x <= 12 && y >= 5 && y <= 10; // 4x4 block grown by 1
                const uint32_t got = words[y * w + x];
                const uint32_t want = ui ? pq[y * w + x] : kBg10;
                if ((got & 0x3FFFFFFFu) != (want & 0x3FFFFFFFu)) // colour only: the paste leaves the alpha alone
                    exact = false;
                pasted += ui;
            }
        EXPECT(exact && pasted == 36, "variant %d (hdr image %s): pasted pixels carry the PQ colours exactly", variant,
               variant == 0 ? "COMMON" : "NPSR");
        EXPECT(std::abs(UiPaste::Snapshot().coverage - 36.0f / float(w * h)) < 1e-6f, "HDR coverage %f",
               UiPaste::Snapshot().coverage);

        // alpha levels in 10-bit storage: 1.0 -> magenta tint, cleanup (2-bit alpha 0.67) -> cyan tint
        UiPaste::SetDebugTint(true);
        bridge.Backbuffer(w, h, DXGI_FORMAT_R10G10B10A2_UNORM);
        bridge.Produce(withUi, hudless, params, hdrCommon.Get());
        const auto lit = bridge.Paste();
        bridge.Backbuffer(w, h, DXGI_FORMAT_R10G10B10A2_UNORM);
        bridge.Produce(hudless, hudless, params, hdrCommon.Get());
        const auto ghost = bridge.Paste();
        UiPaste::SetDebugTint(false);
        const uint32_t litWord = reinterpret_cast<const uint32_t*>(lit.data())[7 * w + 9];
        const uint32_t ghostWord = reinterpret_cast<const uint32_t*>(ghost.data())[7 * w + 9];
        const uint32_t source = pq[7 * w + 9];
        const double sr = (source & 1023) / 1023.0, sg = ((source >> 10) & 1023) / 1023.0, sb = ((source >> 20) & 1023) / 1023.0;
        auto word = [](uint32_t v, int ch) { return int((v >> (10 * ch)) & 1023); };
        EXPECT(Near(word(litWord, 0), int((sr * 0.65 + 0.35) * 1023), 3) && Near(word(litWord, 1), int(sg * 0.65 * 1023), 3) &&
                   Near(word(litWord, 2), int((sb * 0.65 + 0.35) * 1023), 3),
               "alpha 1 in 10-bit storage reads as magenta (%d,%d,%d)", word(litWord, 0), word(litWord, 1), word(litWord, 2));
        EXPECT(Near(word(ghostWord, 0), int(sr * 0.65 * 1023), 3) && Near(word(ghostWord, 1), int((sg * 0.65 + 0.35) * 1023), 3) &&
                   Near(word(ghostWord, 2), int((sb * 0.65 + 0.35) * 1023), 3),
               "cleanup alpha in 10-bit storage reads as cyan, below 0.75 (%d,%d,%d)", word(ghostWord, 0), word(ghostWord, 1),
               word(ghostWord, 2));
        UiPaste::Release();
    }

    // HDR requested without an image / with a wrong one: nothing is produced, nothing crashes
    const auto before = UiPaste::Snapshot().produced;
    MaskParams hdrParams;
    hdrParams.hdr = true;
    bridge.Produce(withUi, hudless, hdrParams, nullptr);
    auto sdrTexture = bridge.Tex(withUi);
    bridge.Produce(withUi, hudless, hdrParams, sdrTexture.Get());
    EXPECT(UiPaste::Snapshot().produced == before, "no image produced for a missing / non-PQ HDR image");
    UiPaste::Release();
}

void OtherFormats(Rig& rig)
{
    std::printf("  SDR final formats: B8G8R8A8 (channel order) and R16G16B16A16_FLOAT (FP16 storage)\n");
    const UINT w = 24, h = 16;
    auto inBlock = [](UINT x, UINT y) { return x >= 8 && x <= 11 && y >= 6 && y <= 9; };

    // --- B8G8R8A8 inputs and backbuffer: the pasted block keeps its logical colour, the rest is untouched ---
    {
        auto swap = [](Image8 image)
        {
            for (size_t i = 0; i < image.px.size(); i += 4)
                std::swap(image.px[i], image.px[i + 2]);
            return image;
        };
        Image8 hudless(w, h, 60, 60, 60), withUi = hudless;
        Fill(withUi, 8, 6, 11, 9, 200, 50, 20);
        const Image8 bgraFinal = swap(withUi), bgraHudless = swap(hudless);
        auto finalTex = rig.Upload(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, bgraFinal.px.data(), 4);
        auto hudTex = rig.Upload(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, bgraHudless.px.data(), 4);
        const Image8 bg = swap(Image8(w, h, kBg[0], kBg[1], kBg[2], kBg[3]));
        FakeSwapChain chain;
        chain.buffer = rig.Upload(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, bg.px.data(), 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        UiPaste::Produce(rig.device.Get(), rig.queue.Get(), finalTex.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, hudTex.Get(),
                         DXGI_FORMAT_B8G8R8A8_UNORM, nullptr, Params(0.008f, 0));
        rig.WaitIdle();
        UiPaste::Paste(&chain, rig.queue.Get());
        rig.WaitIdle();
        const auto out = rig.Download(chain.buffer.Get(), D3D12_RESOURCE_STATE_COMMON, 4);
        bool exact = out.size() == size_t(w) * h * 4;
        for (UINT y = 0; exact && y < h; ++y)
            for (UINT x = 0; x < w; ++x)
            {
                const uint8_t* px = &out[(size_t(y) * w + x) * 4]; // memory order B, G, R, A
                const bool ok = inBlock(x, y) ? (px[0] == 20 && px[1] == 50 && px[2] == 200 && px[3] == 255)
                                              : (px[0] == kBg[2] && px[1] == kBg[1] && px[2] == kBg[0] && px[3] == kBg[3]);
                if (!ok)
                    exact = false;
            }
        EXPECT(exact, "B8G8R8A8: the block keeps its logical colour (no red / blue swap), the rest is untouched");
        EXPECT(std::abs(UiPaste::Snapshot().coverage - 16.0f / float(w * h)) < 1e-6f, "B8G8R8A8 coverage %f",
               UiPaste::Snapshot().coverage);
        UiPaste::Release();
    }

    // --- R16G16B16A16_FLOAT final: the images are FP16, values and the cleanup level survive exactly ---
    {
        auto half4 = [](float r, float g, float b) { return std::vector<uint16_t> { Half(r), Half(g), Half(b), Half(1.0f) }; };
        auto image = [&](bool ui)
        {
            std::vector<uint16_t> px;
            for (UINT y = 0; y < h; ++y)
                for (UINT x = 0; x < w; ++x)
                {
                    const auto p = (ui && inBlock(x, y)) ? half4(0.75f, 0.5f, 0.375f) : half4(0.25f, 0.25f, 0.25f);
                    px.insert(px.end(), p.begin(), p.end());
                }
            return px;
        };
        const auto lit = image(true), plain = image(false);
        FakeSwapChain chain;
        auto background = [&]
        {
            std::vector<uint16_t> px;
            for (UINT i = 0; i < w * h; ++i)
            {
                const auto p = half4(0.0625f, 0.125f, 0.1875f);
                px.insert(px.end(), p.begin(), p.end());
            }
            chain.buffer = rig.Upload(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, px.data(), 8, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        };
        auto frame = [&](const std::vector<uint16_t>& finalPx)
        {
            auto finalTex = rig.Upload(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, finalPx.data(), 8);
            auto hudTex = rig.Upload(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, plain.data(), 8);
            background();
            UiPaste::Produce(rig.device.Get(), rig.queue.Get(), finalTex.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, hudTex.Get(),
                             DXGI_FORMAT_R16G16B16A16_FLOAT, nullptr, Params(0.008f, 0));
            rig.WaitIdle();
            UiPaste::Paste(&chain, rig.queue.Get());
            rig.WaitIdle();
            const auto bytes = rig.Download(chain.buffer.Get(), D3D12_RESOURCE_STATE_COMMON, 8);
            std::vector<uint16_t> words(bytes.size() / 2);
            std::memcpy(words.data(), bytes.data(), bytes.size());
            return words;
        };
        auto expectPixel = [&](const std::vector<uint16_t>& words, UINT x, UINT y, float r, float g, float b)
        {
            const uint16_t* px = &words[(size_t(y) * w + x) * 4];
            return px[0] == Half(r) && px[1] == Half(g) && px[2] == Half(b);
        };

        const auto f1 = frame(lit);
        bool exact = f1.size() == size_t(w) * h * 4;
        for (UINT y = 0; exact && y < h; ++y)
            for (UINT x = 0; x < w; ++x)
                if (!(inBlock(x, y) ? expectPixel(f1, x, y, 0.75f, 0.5f, 0.375f) : expectPixel(f1, x, y, 0.0625f, 0.125f, 0.1875f)))
                    exact = false;
        EXPECT(exact, "FP16: the block is pasted bit-exact, the rest untouched");

        const auto f2 = frame(plain); // UI closed: cleanup level (alpha 0.5) must still paste the final colour
        exact = f2.size() == size_t(w) * h * 4;
        for (UINT y = 0; exact && y < h; ++y)
            for (UINT x = 0; x < w; ++x)
                if (!(inBlock(x, y) ? expectPixel(f2, x, y, 0.25f, 0.25f, 0.25f) : expectPixel(f2, x, y, 0.0625f, 0.125f, 0.1875f)))
                    exact = false;
        EXPECT(exact, "FP16: one frame after the UI closed the old area carries the final colour (cleanup alpha kept)");

        const auto f3 = frame(plain);
        exact = f3.size() == size_t(w) * h * 4;
        for (UINT y = 0; exact && y < h; ++y)
            for (UINT x = 0; x < w; ++x)
                if (!expectPixel(f3, x, y, 0.0625f, 0.125f, 0.1875f))
                    exact = false;
        EXPECT(exact, "FP16: two frames after, nothing is pasted");
        UiPaste::Release();
    }
}

void RingAndFences(Rig& rig)
{
    std::printf("  ring: Produce never waits, a busy slot is never overwritten, pastes pin their image\n");
    Bridge bridge { rig };
    const UINT w = 32, h = 16;
    Image8 hudless(w, h, 60, 60, 60);
    auto uiAt = [&](UINT x0)
    {
        Image8 f = hudless;
        Fill(f, x0, 4, x0 + 3, 7, 200, 50, 20);
        return f;
    };

    // (1) the FG queue is stalled behind a fence the CPU holds: Produce fills the ring, then skips, and returns at once
    {
        bridge.Backbuffer(w, h);
        auto finalTex = bridge.Tex(uiAt(4));
        auto hudTex = bridge.Tex(hudless);
        const auto before = UiPaste::Snapshot();
        ComPtr<ID3D12Fence> gate;
        Check(rig.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "gate");
        Check(rig.queue->Wait(gate.Get(), 1), "queue wait");
        const auto start = GetTickCount64();
        for (int i = 0; i < 8; ++i)
            UiPaste::Produce(rig.device.Get(), rig.queue.Get(), finalTex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, hudTex.Get(),
                             DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, Params(0.008f, 0));
        const auto elapsed = GetTickCount64() - start;
        const auto status = UiPaste::Snapshot();
        EXPECT(elapsed < 5000, "Produce returned without waiting for the stalled queue (%llu ms)", (unsigned long long) elapsed);
        EXPECT(status.produced - before.produced == 4, "four slots filled, the other four frames skipped (produced %llu)",
               (unsigned long long) (status.produced - before.produced));
        EXPECT(std::string(status.lastMessage).find("No free UI image") == 0, "message: %s", status.lastMessage);
        EXPECT(!UiPaste::Ready(rig.device.Get(), w, h), "nothing is ready while no write completed");
        UiPaste::Paste(&bridge.swapChain, rig.queue.Get());
        EXPECT(UiPaste::Snapshot().skipped == before.skipped + 1, "a Paste with nothing completed is a counted skip");
        EXPECT(UiPaste::Snapshot().coverage < 0.f, "coverage unknown until a readback completes");
        EXPECT(UiPaste::Snapshot().coverageSequence == 0, "no coverage sequence until a readback completes");
        gate->Signal(1);
        rig.WaitIdle();
        EXPECT(UiPaste::Ready(rig.device.Get(), w, h), "ready once the writes completed");
        EXPECT(std::abs(UiPaste::Snapshot().coverage - 16.0f / float(w * h)) < 1e-6f, "coverage from the completed readback");
        EXPECT(UiPaste::Snapshot().coverageSequence == UiPaste::Snapshot().produced,
               "the readback of the newest image wins (sequence %llu, produced %llu)",
               (unsigned long long) UiPaste::Snapshot().coverageSequence, (unsigned long long) UiPaste::Snapshot().produced);
        UiPaste::Release();
    }
    // (2) a paste still pending on the presenting queue pins its image: later frames cannot overwrite it, and Release
    //     only retires it behind that paste
    {
        ComPtr<ID3D12CommandQueue> queue2;
        D3D12_COMMAND_QUEUE_DESC desc {};
        Check(rig.device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue2)), "queue2");
        bridge.presentQueue = queue2;
        bridge.Backbuffer(w, h);
        bridge.Produce(uiAt(2), hudless, Params(0.008f, 0)); // image A: UI at x 2..5
        ComPtr<ID3D12Fence> gate;
        Check(rig.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "gate");
        Check(queue2->Wait(gate.Get(), 1), "queue2 wait");
        const auto pastedBefore = UiPaste::Snapshot().pasted;
        UiPaste::Paste(&bridge.swapChain, queue2.Get()); // records and queues the paste of A, blocked behind the gate
        EXPECT(UiPaste::Snapshot().pasted == pastedBefore + 1, "paste of A is accounted when it is submitted");
        for (UINT i = 0; i < 6; ++i)
            bridge.Produce(uiAt(10 + i * 3), hudless, Params(0.008f, 0)); // B..G: UI elsewhere
        UiPaste::Release(); // retires everything, A included, behind the pending paste
        for (UINT i = 0; i < 2; ++i)
            bridge.Produce(uiAt(20), hudless, Params(0.008f, 0)); // new images after the release
        gate->Signal(1);
        bridge.WaitOn(queue2.Get());
        const auto out = rig.Download(bridge.swapChain.buffer.Get(), D3D12_RESOURCE_STATE_COMMON, 4);
        bool onlyA = out.size() == size_t(w) * h * 4;
        for (UINT y = 0; onlyA && y < h; ++y)
            for (UINT x = 0; x < w; ++x)
            {
                const bool inA = x >= 2 && x <= 5 && y >= 4 && y <= 7;
                const uint8_t* px = &out[(size_t(y) * w + x) * 4];
                if (inA ? !(px[0] == 200 && px[1] == 50 && px[2] == 20) : !IsBg(px))
                    onlyA = false;
            }
        EXPECT(onlyA, "the pending paste still drew image A, untouched by later frames and by Release");
        bridge.presentQueue = rig.queue;
        UiPaste::Release();
    }

    // (3) one fence per queue: a second presenting queue is refused while the first has pastes in flight
    {
        ComPtr<ID3D12CommandQueue> queue2;
        D3D12_COMMAND_QUEUE_DESC desc {};
        Check(rig.device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue2)), "queue2");
        bridge.Backbuffer(w, h);
        bridge.Produce(uiAt(2), hudless, Params(0.008f, 0));
        ComPtr<ID3D12Fence> gate;
        Check(rig.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "gate");
        Check(queue2->Wait(gate.Get(), 1), "queue2 wait");
        UiPaste::Paste(&bridge.swapChain, queue2.Get());
        const auto before = UiPaste::Snapshot();
        UiPaste::Paste(&bridge.swapChain, rig.queue.Get()); // another queue while the first still has a paste in flight
        const auto after = UiPaste::Snapshot();
        EXPECT(after.pasted == before.pasted && after.skipped == before.skipped + 1, "refused and counted");
        EXPECT(std::string(after.lastMessage).find("presenting queue changed") != std::string::npos, "message: %s", after.lastMessage);
        gate->Signal(1);
        bridge.WaitOn(queue2.Get());
        UiPaste::Paste(&bridge.swapChain, rig.queue.Get()); // old queue idle: the new one is accepted
        EXPECT(UiPaste::Snapshot().pasted == before.pasted + 1, "rebinding after the first queue went idle");
        rig.WaitIdle();
        UiPaste::Release();
    }
}

void TimingAndLifecycle(Rig& rig)
{
    std::printf("  steady timing, stale images, resize, invalidate, release\n");
    Bridge bridge { rig };
    const UINT w = 32, h = 16;
    Image8 hudless(w, h, 60, 60, 60);
    auto uiAt = [&](UINT x0)
    {
        Image8 f = hudless;
        Fill(f, x0, 4, x0 + 3, 7, 200, 50, 20);
        return f;
    };
    // the UI colour itself: image B also carries the cleanup of A (final colour 60 where A had UI), which is not "UI"
    auto isUiColour = [&](const std::vector<uint8_t>& out, UINT x)
    {
        const uint8_t* px = out.size() == size_t(w) * h * 4 ? &out[(size_t(5) * w + x) * 4] : nullptr;
        return px != nullptr && px[0] == 200 && px[1] == 50 && px[2] == 20;
    };
    auto uiLeft = [&](const std::vector<uint8_t>& out) { return isUiColour(out, 3); };
    auto uiRight = [&](const std::vector<uint8_t>& out) { return isUiColour(out, 21); };

    // steady timing: one real frame behind
    bridge.Produce(uiAt(2), hudless, Params(0.008f, 0));
    bridge.Produce(uiAt(20), hudless, Params(0.008f, 0));
    UiPaste::SetSteadyTiming(true);
    bridge.Backbuffer(w, h);
    auto steady = bridge.Paste();
    EXPECT(uiLeft(steady) && !uiRight(steady), "steady timing shows the image one frame behind");
    UiPaste::SetSteadyTiming(false);
    bridge.Backbuffer(w, h);
    auto normal = bridge.Paste();
    EXPECT(uiRight(normal) && !uiLeft(normal), "normal timing shows the newest image");
    // the UI never goes back in time: after the newest image was shown, steady timing falls back to it, not to the older one
    UiPaste::SetSteadyTiming(true);
    bridge.Backbuffer(w, h);
    auto again = bridge.Paste();
    EXPECT(uiRight(again) && !uiLeft(again), "steady timing does not go back to the older image");
    UiPaste::SetSteadyTiming(false);
    // stale: 250 ms
    Sleep(300);
    const auto skippedBefore = UiPaste::Snapshot().skipped;
    EXPECT(!UiPaste::Ready(rig.device.Get(), w, h), "Ready is false for images older than 250 ms");
    bridge.Backbuffer(w, h);
    EXPECT(CountNonBg(bridge.Paste()) == 0 && UiPaste::Snapshot().skipped == skippedBefore + 1,
           "a stale image is not pasted and the skip is counted");

    // invalidate
    bridge.Produce(uiAt(2), hudless, Params(0.008f, 0));
    EXPECT(UiPaste::Ready(rig.device.Get(), w, h), "fresh image ready");
    UiPaste::Invalidate("No HUD-less image this frame");
    EXPECT(!UiPaste::Ready(rig.device.Get(), w, h), "Invalidate withdraws readiness");
    bridge.Backbuffer(w, h);
    EXPECT(CountNonBg(bridge.Paste()) == 0, "Invalidate stops pasting");
    EXPECT(std::string(UiPaste::Snapshot().lastMessage).find("No completed UI image") == 0 ||
               std::string(UiPaste::Snapshot().lastMessage) == "No HUD-less image this frame",
           "message: %s", UiPaste::Snapshot().lastMessage);

    // resize: new size produces new images, the old size no longer pastes
    bridge.Produce(uiAt(2), hudless, Params(0.008f, 0));
    Image8 tiny(16, 8, 60, 60, 60), tinyUi = tiny;
    Fill(tinyUi, 2, 2, 4, 4, 200, 50, 20);
    bridge.Produce(tinyUi, tiny, Params(0.008f, 0));
    EXPECT(UiPaste::Ready(rig.device.Get(), 16, 8) && !UiPaste::Ready(rig.device.Get(), w, h), "resize: only the new size is ready");
    bridge.Backbuffer(w, h);
    const auto skippedResize = UiPaste::Snapshot().skipped;
    EXPECT(CountNonBg(bridge.Paste()) == 0 && UiPaste::Snapshot().skipped == skippedResize + 1,
           "a backbuffer of the old size is left alone and counted");
    bridge.Backbuffer(16, 8);
    EXPECT(CountNonBg(bridge.Paste()) == 9, "the new size pastes");

    // release: nothing is pasted afterwards, produce works again
    UiPaste::Release();
    UiPaste::Release();
    EXPECT(!UiPaste::Ready(rig.device.Get(), 16, 8), "Release withdraws readiness");
    bridge.Backbuffer(16, 8);
    EXPECT(CountNonBg(bridge.Paste()) == 0, "nothing pasted after Release");
    bridge.Produce(tinyUi, tiny, Params(0.008f, 0));
    bridge.Backbuffer(16, 8);
    EXPECT(CountNonBg(bridge.Paste()) == 9, "Produce works again after Release");
    UiPaste::Release();
}

void PasteGate(Rig& rig)
{
    std::printf("  paste gate: off = no draw and no skip, Produce keeps going, on = draws again\n");
    Bridge bridge { rig };
    const UINT w = 40, h = 32;
    Image8 hudless(w, h, 60, 60, 60), final = hudless;
    Fill(final, 10, 8, 13, 11, 200, 50, 20); // a 4x4 block, 6x6 with the dilation ring
    EXPECT(UiPaste::Snapshot().coverageSequence == 0, "the gate test starts from a released state");

    bridge.Backbuffer(w, h);
    bridge.Produce(final, hudless, Params(0.008f, 1));
    EXPECT(UiPaste::Ready(rig.device.Get(), w, h), "an image is ready");

    // gate off: nothing drawn, nothing counted, even though an image is ready
    UiPaste::SetPasteEnabled(false);
    const auto before = UiPaste::Snapshot();
    const auto gated = bridge.Paste();
    auto after = UiPaste::Snapshot();
    EXPECT(CountNonBg(gated) == 0, "gate off: no pixel drawn (%zu)", CountNonBg(gated));
    EXPECT(after.pasted == before.pasted && after.skipped == before.skipped, "gate off: pasted %llu -> %llu, skipped %llu -> %llu",
           (unsigned long long) before.pasted, (unsigned long long) after.pasted, (unsigned long long) before.skipped,
           (unsigned long long) after.skipped);

    // a paste with nothing to draw is a skip when the gate is on, and not one when it is off
    UiPaste::Invalidate("test");
    const auto invalidated = UiPaste::Snapshot();
    bridge.Paste();
    EXPECT(UiPaste::Snapshot().skipped == invalidated.skipped, "gate off: an empty ring counts no skip");

    // Produce is not gated: the image is built, the readback and its sequence advance
    bridge.Produce(final, hudless, Params(0.008f, 1));
    bridge.Produce(final, hudless, Params(0.008f, 1));
    const auto produced = UiPaste::Snapshot();
    EXPECT(produced.produced == before.produced + 2, "gate off: Produce still builds images (%llu -> %llu)",
           (unsigned long long) before.produced, (unsigned long long) produced.produced);
    EXPECT(produced.coverageSequence == produced.produced && produced.coverage >= 0.f,
           "gate off: the coverage sequence follows Produce (sequence %llu, produced %llu, coverage %f)",
           (unsigned long long) produced.coverageSequence, (unsigned long long) produced.produced, produced.coverage);
    EXPECT(std::abs(produced.coverage - 36.0f / float(w * h)) < 1e-6f, "gate off: coverage %f", produced.coverage);
    EXPECT(UiPaste::Ready(rig.device.Get(), w, h), "gate off: the produced image is ready for the moment the gate opens");
    EXPECT(UiPaste::Snapshot().pasted == before.pasted, "gate off: nothing was drawn meanwhile");

    // gate on: the same image is drawn and counted
    UiPaste::SetPasteEnabled(true);
    bridge.Backbuffer(w, h);
    const auto drawn = bridge.Paste();
    EXPECT(CountNonBg(drawn) == 36, "gate on: the 6x6 block is drawn (%zu pixels)", CountNonBg(drawn));
    EXPECT(UiPaste::Snapshot().pasted == before.pasted + 1, "gate on: counted as pasted");

    // and a paste with nothing to draw counts a skip again
    UiPaste::Invalidate("test");
    const auto empty = UiPaste::Snapshot();
    bridge.Paste();
    EXPECT(UiPaste::Snapshot().skipped == empty.skipped + 1, "gate on: an empty ring counts a skip");

    UiPaste::SetPasteEnabled(true);
    UiPaste::Release();
}
} // namespace

int main()
try
{
    Rig rig;
    std::printf("[ui_paste_warp] WARP device, debug layer %s\n", rig.debugLayer ? "on" : "off");
    SafeBeforeInit(rig);
    BasicBlock(rig);
    UnchangedScene(rig);
    ThresholdAndDilation(rig);
    CleanupLevels(rig);
    RandomAgainstReference(rig);
    AmplitudeCounters(rig);
    TypelessAndHdr(rig);
    OtherFormats(rig);
    RingAndFences(rig);
    TimingAndLifecycle(rig);
    PasteGate(rig);
    std::printf("[ui_paste_warp] failures: %d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
catch (const std::exception& error)
{
    std::printf("[ui_paste_warp] exception: %s\n", error.what());
    return 2;
}
