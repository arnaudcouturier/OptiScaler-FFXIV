// S10 contract check (docs/HUDLESS.md section 4): the two interface headers compile on their own, a game module can
// implement IHudlessSource, and the documented defaults hold. It guards the contract the three W6 work packages build
// against; a change to a header must update this test and the design doc together.
#include "ext/hudless/HudlessGpu.h"
#include "ext/hudless/HudlessSource.h"
#include "ext/hudless/HudlessState.h"

#include <cassert>
#include <cstdio>
#include <type_traits>

using namespace ext::hudless;

namespace
{
struct FakeSource final : IHudlessSource
{
    bool armed = false;
    bool Active() override { return armed; }
    TakeResult Take(FrameImage& out) override
    {
        out = {};
        return armed ? TakeResult::NotThisFrame : TakeResult::NoSource;
    }
    const char* LastReason() override { return "fake"; }
    void EndFrame() override {}
};

static_assert(std::is_abstract_v<IHudlessSource>);
static_assert(std::has_virtual_destructor_v<IHudlessSource>);
static_assert(sizeof(TakeResult) == 1);
} // namespace

int main()
{
    FakeSource source;
    IHudlessSource& iface = source;
    FrameImage image;
    assert(!iface.Active());
    assert(iface.Take(image) == TakeResult::NoSource);
    assert(image.texture == nullptr && image.format == DXGI_FORMAT_UNKNOWN && image.width == 0 && image.height == 0);
    source.armed = true;
    assert(iface.Active());
    assert(iface.Take(image) == TakeResult::NotThisFrame);

    // The defaults are the oracle's tuned values (docs/HUDLESS.md sections 7 and 11) and the user-facing ini defaults.
    const MaskParams params;
    assert(params.threshold == 0.008f);
    assert(params.dilation == 1);
    assert(!params.hdr);
    assert(params.cleanup);

    const PasteStatus status;
    assert(status.produced == 0 && status.pasted == 0 && status.skipped == 0);
    assert(status.coverage < 0.f); // unknown until the first readback completes
    assert(status.lastMessage[0] == '\0');

    // The shared flag: off by default, writable, readable.
    assert(!Wanted());
    SetWanted(true);
    assert(Wanted());
    SetWanted(false);
    assert(!Wanted());

    std::puts("PASS: S10 contract headers compile, fake IHudlessSource works, MaskParams/PasteStatus defaults hold");
    return 0;
}
