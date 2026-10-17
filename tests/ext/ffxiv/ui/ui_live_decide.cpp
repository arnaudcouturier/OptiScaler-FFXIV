// S10-E: the live source's decisions without a GPU: when it acts (R1 only, scene stage seen, engine asked in the previous
// Present, once per frame), what the render target guard accepts (size, format, sample count, array/mips, backbuffer known),
// the arming rule driven by Take(), the reason texts the engine shows, and the log lines (state changes, 600-frame summary).
#define NOMINMAX
#include "check.h"

#include "ext/ffxiv/ui/UiLive.h"

#include <string>

using namespace ext::ffxiv::ui;
using namespace ext::ffxiv::ui::live;
using ext::hudless::FrameImage;
using ext::hudless::TakeResult;

namespace
{
D3D11_TEXTURE2D_DESC Desc(UINT w, UINT h, DXGI_FORMAT f)
{
    D3D11_TEXTURE2D_DESC d {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    return d;
}

void Acts()
{
    FrameFacts f;
    f.armed = f.r1JustSet = f.sawSceneStage = true;
    CHECK(ShouldAct(f));
    { auto g = f; g.armed = false; CHECK(!ShouldAct(g)); }          // the engine did not ask in the previous Present
    { auto g = f; g.r1JustSet = false; CHECK(!ShouldAct(g)); }     // not the R1 candidate
    { auto g = f; g.sawSceneStage = false; CHECK(!ShouldAct(g)); } // menu / loading frame
    { auto g = f; g.handled = true; CHECK(!ShouldAct(g)); }        // once per frame
}

void Guard()
{
    const auto ok = Desc(3840, 2160, DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(CheckTarget(capture::Result::Ok, ok, 3840, 2160) == Reason::None);
    CHECK(CheckTarget(capture::Result::Ok, Desc(3840, 2160, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB), 3840, 2160) == Reason::None);
    CHECK(CheckTarget(capture::Result::Ok, Desc(3840, 2160, DXGI_FORMAT_R8G8B8A8_TYPELESS), 3840, 2160) == Reason::None);
    // the 103 Session 1 frames: character previews
    CHECK(CheckTarget(capture::Result::Ok, Desc(352, 352, DXGI_FORMAT_B8G8R8A8_UNORM), 3840, 2160) == Reason::SizeMismatch);
    CHECK(CheckTarget(capture::Result::Ok, Desc(1024, 1024, DXGI_FORMAT_B8G8R8A8_UNORM), 3840, 2160) == Reason::SizeMismatch);
    CHECK(CheckTarget(capture::Result::Ok, Desc(3840, 2160, DXGI_FORMAT_B8G8R8A8_UNORM), 3840, 2160) == Reason::FormatUnsupported);
    CHECK(CheckTarget(capture::Result::Ok, Desc(3840, 2160, DXGI_FORMAT_R10G10B10A2_UNORM), 3840, 2160) == Reason::FormatUnsupported);
    CHECK(CheckTarget(capture::Result::Ok, Desc(3840, 2160, DXGI_FORMAT_R16G16B16A16_FLOAT), 3840, 2160) == Reason::FormatUnsupported);
    CHECK(CheckTarget(capture::Result::Ok, Desc(3840, 2159, DXGI_FORMAT_R8G8B8A8_UNORM), 3840, 2160) == Reason::SizeMismatch);
    CHECK(CheckTarget(capture::Result::Ok, ok, 0, 0) == Reason::NoBackbuffer); // backbuffer unknown: cannot verify
    CHECK(CheckTarget(capture::Result::NoRenderTarget, {}, 3840, 2160) == Reason::NoRenderTarget);
    CHECK(CheckTarget(capture::Result::NotTexture2D, {}, 3840, 2160) == Reason::NotTexture2D);
    CHECK(CheckTarget(capture::Result::Multisampled, ok, 3840, 2160) == Reason::Multisampled);
    CHECK(CheckTarget(capture::Result::ArrayOrMips, ok, 3840, 2160) == Reason::ArrayOrMips);

    // every guard reason shows the engine the same text
    for (Reason r : { Reason::NoRenderTarget, Reason::NotTexture2D, Reason::Multisampled, Reason::ArrayOrMips, Reason::NoBackbuffer,
                      Reason::SizeMismatch, Reason::FormatUnsupported })
    {
        CHECK(IsGuardReason(r));
        CHECK(std::string(ReasonText(r)) == "render target is not the full-size backbuffer");
    }
    CHECK(!IsGuardReason(Reason::EffectsFailed) && !IsGuardReason(Reason::Superseded) && !IsGuardReason(Reason::None));
    CHECK(std::string(ReasonText(Reason::EffectsFailed)) == "ReShade effects failed");
}

void Arming()
{
    LiveSource s(Env {});
    FrameImage img;
    CHECK(!s.Armed());

    // nothing asked yet: no frame, and the call itself arms the next frame
    CHECK(s.Take(img) == TakeResult::NotThisFrame);
    CHECK(img.texture == nullptr);
    CHECK(std::string(s.LastReason()).find("waiting") != std::string::npos);
    auto lines = s.EndFrame();
    CHECK(s.Armed());
    CHECK(lines.find("armed") != std::string::npos);

    // the engine did not call Take() in that Present (frame generation off, coverage guard, ...): disarmed again
    lines = s.EndFrame();
    CHECK(!s.Armed());
    CHECK(lines.empty()); // rate limited: no second state line right away
    CHECK(s.GetStats().frames == 1);
    CHECK(s.GetStats().noBoundary == 1);

    // a frame without an R1 event: the engine sees "no boundary", the frame is counted
    CHECK(s.Take(img) == TakeResult::NotThisFrame);
    s.EndFrame();
    CHECK(s.Armed());
    CHECK(s.Take(img) == TakeResult::NotThisFrame);
    CHECK(std::string(s.LastReason()) == "no boundary in this frame");
    s.EndFrame();
    CHECK(s.GetStats().noBoundary == 2);
    CHECK(s.GetStats().takes == 3);
}

void Lines()
{
    LiveSource s(Env {});
    FrameImage img;
    std::string all;
    for (uint64_t i = 0; i < LiveSource::kSummaryEvery + 5; ++i)
    {
        s.Take(img);
        all += s.EndFrame();
        all += '\n';
    }
    // one state line ("armed") and exactly one summary (after kSummaryEvery acting frames)
    size_t armed = 0, summaries = 0;
    for (size_t p = 0; (p = all.find("live capture armed", p)) != std::string::npos; ++p)
        ++armed;
    for (size_t p = 0; (p = all.find("FFXIV UI boundary: 600 frames:", p)) != std::string::npos; ++p)
        ++summaries;
    CHECK_EQ(armed, 1u);
    CHECK_EQ(summaries, 1u);
    const auto summary = s.Summary();
    CHECK(summary.find("captured 0") != std::string::npos);
    CHECK(summary.find("rejected 0") != std::string::npos);
    CHECK(summary.find("ReShade presets: applied 0") != std::string::npos);

    // release drops the armed state
    s.Release();
    CHECK(!s.Armed());
}
} // namespace

int main()
{
    Acts();
    Guard();
    Arming();
    Lines();
    return Finish("ui_live_decide");
}
