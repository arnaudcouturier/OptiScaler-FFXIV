// Automatic ReShade scene depth: the pure decisions of the capture glue (ReShadeDepthGlue.h), no NGX, ReShade or D3D: the render size
// (per-axis minimum), the output size and its fallbacks, the base (never clamped), the character-preview filter, the order of the
// gates, the one-load early-out, which draw the pass gets, the once-per-change capture line and the rate-limited result line, and the
// status line of the menu.
#include "ext/reshade/ReShadeDepthCore.h"
#include "ext/reshade/ReShadeDepthGlue.h"
#include "ext/reshade/ReShadeRegister.h"

#include <cstdio>
#include <string>
#include <type_traits>

using namespace ext::reshade::depth;
using namespace ext::reshade::depth::glue;

namespace
{
int g_fail = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            std::printf("CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #c);                                           \
            ++g_fail;                                                                                                  \
        }                                                                                                              \
    } while (0)

Extent E(uint32_t w, uint32_t h) { return Extent { w, h }; }

void RenderSize()
{
    // FFXIV reports the buffer's own size as the subrect and the real render size as Width/Height: the smaller one per axis
    CHECK(RenderExtent(3840, 2160, 1920, 1080) == E(1920, 1080));
    CHECK(RenderExtent(3840, 2160, 3840, 2160) == E(3840, 2160)); // DLAA
    CHECK(RenderExtent(1920, 1080, 3840, 2160) == E(1920, 1080)); // Width/Height never makes the render larger
    // per axis, not as a pair: letterboxing scales one axis only
    CHECK(RenderExtent(3840, 2160, 2258, 2160) == E(2258, 2160));
    CHECK(RenderExtent(3840, 2160, 3840, 1200) == E(3840, 1200));
    CHECK(RenderExtent(2000, 1000, 1800, 1200) == E(1800, 1000)); // one axis from each
    // zero (absent) values do not count
    CHECK(RenderExtent(3840, 2160, 0, 0) == E(3840, 2160));
    CHECK(RenderExtent(3840, 2160, 1920, 0) == E(3840, 2160)); // half a pair is no pair
    CHECK(RenderExtent(0, 0, 1920, 1080) == E(1920, 1080));    // no subrect: the NGX render size
    CHECK(RenderExtent(0, 0, 0, 0) == E(0, 0) && !RenderExtent(0, 0, 0, 0).Valid());
    CHECK(!RenderExtent(0, 2160, 1920, 1080).Valid()); // one subrect axis missing: not a render size
    CHECK(!RenderExtent(0, 2160, 0, 0).Valid());
}

void OutputSize()
{
    CHECK(OutputExtent(3840, 2160, E(1280, 720), E(1920, 1080)) == E(3840, 2160)); // OutWidth/OutHeight first
    CHECK(OutputExtent(0, 0, E(3840, 2160), E(1920, 1080)) == E(3840, 2160));      // then the Output resource
    CHECK(OutputExtent(0, 0, E(0, 0), E(1920, 1080)) == E(1920, 1080));            // then the display size
    CHECK(!OutputExtent(0, 0, E(0, 0), E(0, 0)).Valid());
    // a pair is taken whole: half of OutWidth/OutHeight is not mixed with another source
    CHECK(OutputExtent(3840, 0, E(2560, 1440), E(1920, 1080)) == E(2560, 1440));
    CHECK(OutputExtent(0, 2160, E(0, 1440), E(1920, 1080)) == E(1920, 1080));
}

void Base()
{
    CHECK(BaseFromInt(0) == 0 && BaseFromInt(17) == 17);
    CHECK(BaseFromInt(-1) == 0xFFFFFFFFu); // never clamped to 0: the pass refuses a base outside the source
    CHECK(BaseFromInt(-1) > 0x4000u);
}

void PreviewFilter()
{
    ConsumerInfo c;
    c.seen = true;
    c.width = 3840;
    c.height = 2160;
    CHECK(OutputIsConsumer(E(3840, 2160), c));
    CHECK(!OutputIsConsumer(E(576, 960), c));  // the character preview
    CHECK(!OutputIsConsumer(E(3840, 2161), c));
    CHECK(!OutputIsConsumer(E(3839, 2160), c));
    CHECK(!OutputIsConsumer(E(0, 0), c));
    c.seen = false;
    CHECK(!OutputIsConsumer(E(3840, 2160), c)); // no consumer, no capture
    c.seen = true;
    c.width = c.height = 0;
    CHECK(!OutputIsConsumer(E(0, 0), c));
}

void Gates()
{
    constexpr uint32_t kOn = EvalGate::kEnabled, kReq = EvalGate::kRequestWithdraw;
    // the cheapest first: idle, shutting down, not registered, then the consumer, then the sizes
    CHECK(FirstGate(0, false, true) == Gate::Idle);
    CHECK(FirstGate(0, true, false) == Gate::Idle); // nothing else is even looked at
    CHECK(FirstGate(kOn, true, true) == Gate::ShuttingDown);
    CHECK(FirstGate(kOn, true, false) == Gate::ShuttingDown);
    CHECK(FirstGate(kOn, false, false) == Gate::NotRegistered);
    CHECK(FirstGate(kOn, false, true) == Gate::Go);
    // a word with requests but without the option (the requests are carried out before the gates): nothing else may run, in particular no
    // late registration and no event subscription with the option off
    CHECK(FirstGate(kReq, false, true) == Gate::Idle);
    CHECK(FirstGate(kReq, false, false) == Gate::Idle);
    CHECK(FirstGate(kReq, true, true) == Gate::Idle);
    CHECK(FirstGate(kOn | kReq, false, true) == Gate::Go);

    ConsumerInfo none;
    CHECK(ConsumerGate(none) == Gate::NoConsumer);
    ConsumerInfo seen;
    seen.seen = true;
    seen.width = 3840;
    seen.height = 2160;
    CHECK(ConsumerGate(seen) == Gate::Go);

    CHECK(SizeGate(E(0, 0), E(3840, 2160), seen) == Gate::NoSizes);
    CHECK(SizeGate(E(1920, 1080), E(0, 0), seen) == Gate::NoSizes);
    CHECK(SizeGate(E(1920, 1080), E(576, 960), seen) == Gate::NotConsumerSize); // the preview
    CHECK(SizeGate(E(1920, 1080), E(3840, 2160), seen) == Gate::Go);
    CHECK(SizeGate(E(3840, 2160), E(3840, 2160), seen) == Gate::Go); // DLAA
}

void OneLoad()
{
    static_assert(noexcept(std::declval<const EvalGate&>().Idle()));
    EvalGate g;
    CHECK(g.Idle() && !g.Enabled());
    g.SetEnabled(true);
    CHECK(!g.Idle() && g.Enabled());
    g.SetEnabled(false); // switching off leaves requests for the evaluate thread: it must not be idle yet
    CHECK(!g.Idle() && !g.Enabled());
    const uint32_t requests = g.TakeRequests();
    CHECK((requests & EvalGate::kRequestWithdraw) != 0 && (requests & EvalGate::kRequestRelease) != 0);
    CHECK(g.Idle());
    CHECK(g.TakeRequests() == 0);
    g.SetEnabled(false); // already off: no new request
    CHECK(g.Idle());
    g.Request(EvalGate::kRequestRelease);
    CHECK(!g.Idle());
    CHECK(g.TakeRequests() == EvalGate::kRequestRelease && g.Idle());
    // the word is one atomic
    static_assert(sizeof(EvalGate) == sizeof(uint32_t));
}

void DrawChoice()
{
    int a = 0, b = 0;
    CHECK(UseRawDraw(&a, &a));
    CHECK(!UseRawDraw(&a, &b));
    CHECK(!UseRawDraw(nullptr, &b));     // the lighting hook is not installed
    CHECK(!UseRawDraw(nullptr, nullptr));
    CHECK(!UseRawDraw(&a, nullptr));
}

void LogDiffs()
{
    CaptureSignature s;
    s.srcW = 3840;
    s.srcH = 2160;
    s.format = 39;
    s.bindFlags = 8;
    s.baseX = 0;
    s.baseY = 0;
    s.renderW = 1920;
    s.renderH = 1080;
    s.outW = 3840;
    s.outH = 2160;
    ChangeTracker t;
    CHECK(t.Changed(s));  // the first capture
    CHECK(!t.Changed(s)); // the same again
    CHECK(!t.Changed(s));
    for (int field = 0; field < 10; ++field)
    {
        CaptureSignature c = s;
        switch (field)
        {
        case 0: c.srcW++; break;
        case 1: c.srcH++; break;
        case 2: c.format++; break;
        case 3: c.bindFlags++; break;
        case 4: c.baseX++; break;
        case 5: c.baseY++; break;
        case 6: c.renderW++; break;
        case 7: c.renderH++; break;
        case 8: c.outW++; break;
        default: c.outH++; break;
        }
        ChangeTracker u;
        CHECK(u.Changed(s));
        CHECK(u.Changed(c));  // any field changing is a new line
        CHECK(!u.Changed(c));
        CHECK(u.Changed(s));  // ... and so is going back
    }

    // results: a refusal is logged once per change, then at most once a minute while it repeats; Ok is never a line and ends the run
    ResultLog r;
    CHECK(!r.ShouldLog(0, 1'000));
    CHECK(r.ShouldLog(5, 1'000));
    CHECK(!r.ShouldLog(5, 2'000));
    CHECK(!r.ShouldLog(5, 60'999));
    CHECK(r.ShouldLog(5, 61'000)); // a minute after the line
    CHECK(!r.ShouldLog(5, 61'001));
    CHECK(r.ShouldLog(7, 61'002)); // another result is a new line at once
    CHECK(!r.ShouldLog(0, 61'003));
    CHECK(r.ShouldLog(7, 61'004)); // Ok in between ended the run of 7: it is new again
    CHECK(r.ShouldLog(5, 61'005));
    CHECK(!r.ShouldLog(5, 40'000)); // a clock that went backwards logs nothing
    CHECK(!r.ShouldLog(5, 121'004));
    CHECK(r.ShouldLog(5, 121'005));
}

void Game()
{
    // the game gate is the executable name, whole and case-insensitive
    CHECK(IsFfxivExe("ffxiv_dx11.exe"));
    CHECK(IsFfxivExe("FFXIV_DX11.EXE"));
    CHECK(IsFfxivExe("Ffxiv_Dx11.Exe"));
    CHECK(!IsFfxivExe(""));
    CHECK(!IsFfxivExe("ffxiv.exe"));
    CHECK(!IsFfxivExe("ffxiv_dx11"));
    CHECK(!IsFfxivExe("xffxiv_dx11.exe"));
    CHECK(!IsFfxivExe("ffxiv_dx11.exe.bak"));
    CHECK(!IsFfxivExe("ffxiv_dx12.exe"));
    CHECK(!IsFfxivExe("bf_dx11.exe_"));
}

void Status()
{
    StatusInputs in;
    in.enabled = false;
    in.binderStatus = "Waiting for ReShade depth effects";
    CHECK(ComposeStatus(in) == "Automatic scene depth: off");
    in.enabled = true;
    // another game: not available, whatever else is true; the option off is still said first
    in.gameSupported = false;
    in.registered = true;
    CHECK(ComposeStatus(in) == "Automatic scene depth: not available for this game");
    in.enabled = false;
    CHECK(ComposeStatus(in) == "Automatic scene depth: off");
    in.enabled = true;
    in.gameSupported = true;
    in.registered = false;
    CHECK(ComposeStatus(in) == "ReShade not detected");
    in.lateGaveUp = true;
    CHECK(ComposeStatus(in).find("ReShade not detected") == 0 && ComposeStatus(in).find("stopped looking") != std::string::npos);
    in.lateGaveUp = false;
    // a ReShade that is there but cannot be used is not "not detected"
    using ext::reshade::RegisterStatus;
    for (const RegisterStatus reason : { RegisterStatus::Refused, RegisterStatus::ExportsMissing, RegisterStatus::Exception, RegisterStatus::NoSelf })
    {
        in.reason = reason;
        const std::string s = ComposeStatus(in);
        CHECK(s.find("not detected") == std::string::npos);
        CHECK(s.find("ReShade") != std::string::npos);
        in.lateGaveUp = true; // (the late path stopped for good with a refusal: still the refusal that is said)
        CHECK(ComposeStatus(in) == s);
        in.lateGaveUp = false;
    }
    in.reason = RegisterStatus::Refused;
    CHECK(ComposeStatus(in).find("add-on API 20") != std::string::npos && ComposeStatus(in).find("6.8") != std::string::npos);
    in.reason = RegisterStatus::ExportsMissing;
    CHECK(ComposeStatus(in).find("add-on support") != std::string::npos);
    in.reason = RegisterStatus::NoModule;
    CHECK(ComposeStatus(in) == "ReShade not detected");
    in.registered = true;
    CHECK(ComposeStatus(in) == "Waiting for ReShade depth effects");
    // the facade does not exist yet (the ini said true on the title screen): waiting, not off and not empty
    in.binderStatus.clear();
    CHECK(ComposeStatus(in) == "Waiting for ReShade depth effects");
    in.binderStatus = "Waiting for ReShade depth effects";
    in.subscriptionFailed = true;
    CHECK(ComposeStatus(in).find("could not be subscribed") != std::string::npos);
    in.subscriptionFailed = false;
    in.nativeContextMissing = true;
    CHECK(ComposeStatus(in) == "Automatic scene depth: native context not available");
    in.nativeContextMissing = false;
    // a refused capture is shown while the binder is not bound; a bound pass wins over it
    in.captureRefusal = "render region outside the source";
    CHECK(ComposeStatus(in).find("render region outside the source") != std::string::npos);
    in.binderStatus = "Automatic scene depth: 1920x1080 -> 3840x2160";
    CHECK(ComposeStatus(in) == "Automatic scene depth: 1920x1080 -> 3840x2160");
    in.captureRefusal = nullptr;
    CHECK(ComposeStatus(in) == "Automatic scene depth: 1920x1080 -> 3840x2160");

    // the G-buffers realigned by the last begin are a suffix of the bound line, and only of it
    in.gbuffers = "normals, motion";
    CHECK(ComposeStatus(in) == "Automatic scene depth: 1920x1080 -> 3840x2160; G-buffers realigned: normals, motion");
    in.gbuffers = "material";
    CHECK(ComposeStatus(in) == "Automatic scene depth: 1920x1080 -> 3840x2160; G-buffers realigned: material");
    in.binderStatus = "Scene depth unavailable; generic depth retained";
    CHECK(ComposeStatus(in).find("G-buffers") == std::string::npos); // not bound: no suffix
    in.binderStatus = "Waiting for ReShade depth effects";
    CHECK(ComposeStatus(in) == "Waiting for ReShade depth effects");
    in.binderStatus = "Automatic scene depth: 1920x1080 -> 3840x2160";
    in.gbuffers.clear();
    CHECK(ComposeStatus(in) == "Automatic scene depth: 1920x1080 -> 3840x2160"); // none realigned: no suffix

    // the prefix is the one the binder writes for a bound pass
    CHECK(std::string("Automatic scene depth: 1920x1080 -> 3840x2160").rfind(kBoundPrefix, 0) == 0);
}
} // namespace

int main()
{
    RenderSize();
    OutputSize();
    Base();
    PreviewFilter();
    Gates();
    OneLoad();
    DrawChoice();
    LogDiffs();
    Game();
    Status();
    if (g_fail != 0)
    {
        std::printf("reshade_depth_glue: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::puts("reshade_depth_glue: all checks passed");
    return 0;
}
