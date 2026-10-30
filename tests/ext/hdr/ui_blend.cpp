// HDR census, part 1 (pure): the blend classification shared by the transmittance mask and the UI draw census, and the text of
// the 600-frame mask summary line.
//
// 1. uiblend::Classify(desc).Replayable() equals, for EVERY combination of the fields it reads, the refusal rule that
//    FfxivHdrPreview::BeforeDraw had before the rule was moved into UiBlend.h (kept here verbatim as the oracle). If the two ever
//    disagree the mask replays a draw the census calls refused, or the other way round.
// 2. The kind and the first refusal reason are right for the states the game and the mask care about.
// 3. SummaryText prints exactly the counters it is given, only the reasons that occurred, in enum order.
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#define LOG_INFO(...) ((void)0)
#include "ext/hdr/FfxivHdrPreview.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{
int g_failed = 0;
void Expect(bool ok, const char* what, unsigned long detail = 0)
{
    if (!ok)
    {
        std::printf("FAIL: %s (%lu)\n", what, detail);
        ++g_failed;
    }
}

// The rule as it stood in FfxivHdrPreview::BeforeDraw before the move (opaque, then the refusal condition), verbatim.
bool OldReplayable(const D3D11_BLEND_DESC& bd, bool haveState)
{
    const auto& nativeBlend = bd.RenderTarget[0];
    const bool opaque = !nativeBlend.BlendEnable || (nativeBlend.BlendOp == D3D11_BLEND_OP_ADD && nativeBlend.DestBlend == D3D11_BLEND_ZERO);
    const bool refused =
        haveState && (bd.AlphaToCoverageEnable || (nativeBlend.RenderTargetWriteMask & 7) != 7 ||
                      (nativeBlend.BlendEnable && !opaque &&
                       (nativeBlend.BlendOp != D3D11_BLEND_OP_ADD ||
                        (nativeBlend.SrcBlend != D3D11_BLEND_SRC_ALPHA && nativeBlend.SrcBlend != D3D11_BLEND_ONE) ||
                        nativeBlend.DestBlend != D3D11_BLEND_INV_SRC_ALPHA)));
    return !refused;
}

D3D11_BLEND_DESC Blend(bool enable, D3D11_BLEND src, D3D11_BLEND dest, D3D11_BLEND_OP op = D3D11_BLEND_OP_ADD, UINT mask = 15, bool a2c = false)
{
    D3D11_BLEND_DESC bd {};
    bd.AlphaToCoverageEnable = a2c;
    auto& r = bd.RenderTarget[0];
    r.BlendEnable = enable;
    r.SrcBlend = src;
    r.DestBlend = dest;
    r.BlendOp = op;
    r.SrcBlendAlpha = D3D11_BLEND_ONE;
    r.DestBlendAlpha = D3D11_BLEND_ZERO;
    r.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    r.RenderTargetWriteMask = static_cast<UINT8>(mask);
    return bd;
}

using ext::hdr::uiblend::Kind;
using ext::hdr::uiblend::Refusal;

void Exhaustive()
{
    // every blend factor (1..19), every op (1..5), every write mask (0..15), blend on/off, alpha to coverage on/off
    unsigned long cases = 0, replayable = 0;
    for (int enable = 0; enable < 2; ++enable)
        for (int a2c = 0; a2c < 2; ++a2c)
            for (UINT mask = 0; mask < 16; ++mask)
                for (int op = 1; op <= 5; ++op)
                    for (int src = 1; src <= 19; ++src)
                        for (int dest = 1; dest <= 19; ++dest)
                        {
                            const auto bd = Blend(enable != 0, static_cast<D3D11_BLEND>(src), static_cast<D3D11_BLEND>(dest),
                                                  static_cast<D3D11_BLEND_OP>(op), mask, a2c != 0);
                            const auto v = ext::hdr::uiblend::Classify(&bd);
                            const bool old = OldReplayable(bd, true);
                            ++cases;
                            replayable += old;
                            if (v.Replayable() != old)
                            {
                                std::printf("FAIL: disagreement enable=%d a2c=%d mask=%u op=%d src=%d dest=%d\n", enable, a2c, mask, op, src, dest);
                                ++g_failed;
                                return;
                            }
                            // Opaque() must be the old `opaque` flag: the mask blend (ZERO/ZERO or ZERO/INV_SRC_ALPHA) is chosen by it.
                            const bool oldOpaque = !enable || (op == D3D11_BLEND_OP_ADD && dest == D3D11_BLEND_ZERO);
                            if (v.Opaque() != oldOpaque)
                            {
                                std::printf("FAIL: opaque flag differs enable=%d op=%d dest=%d\n", enable, op, dest);
                                ++g_failed;
                                return;
                            }
                        }
    // no blend state bound: the D3D11 default (blend off, full write mask), never refused
    Expect(ext::hdr::uiblend::Classify(nullptr).Replayable() && ext::hdr::uiblend::Classify(nullptr).Opaque(), "no state = default, replayable");
    Expect(OldReplayable({}, false), "oracle: no state replayable");
    Expect(cases == 2ul * 2 * 16 * 5 * 19 * 19 && replayable > 0 && replayable < cases, "the sweep covers accepted and refused states", cases);
}

void Named()
{
    using namespace ext::hdr::uiblend;
    const auto standard = Blend(true, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA);
    const auto premultiplied = Blend(true, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA);
    const auto glow = Blend(true, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE);
    const auto additive = Blend(true, D3D11_BLEND_ONE, D3D11_BLEND_ONE);
    const auto opaque = Blend(false, D3D11_BLEND_ONE, D3D11_BLEND_ZERO);
    const auto replaceByBlend = Blend(true, D3D11_BLEND_ONE, D3D11_BLEND_ZERO);
    const auto multiply = Blend(true, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_ZERO);
    const auto subtract = Blend(true, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_SUBTRACT);
    const auto colourOnly = Blend(true, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, 7);
    const auto alphaMissing = Blend(true, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, 3);
    const auto coverage = Blend(false, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, 15, true);

    auto is = [](const D3D11_BLEND_DESC& d, Kind k, Refusal r, const char* what)
    {
        const auto v = Classify(&d);
        Expect(v.kind == k, what, static_cast<unsigned long>(v.kind));
        Expect(v.refusal == r, what, static_cast<unsigned long>(v.refusal));
    };
    is(standard, Kind::Alpha, Refusal::None, "SRC_ALPHA / INV_SRC_ALPHA is the standard blend");
    is(premultiplied, Kind::Alpha, Refusal::None, "ONE / INV_SRC_ALPHA is the standard (premultiplied) blend");
    is(glow, Kind::Additive, Refusal::Additive, "SRC_ALPHA / ONE is an additive glow, refused");
    is(additive, Kind::Additive, Refusal::Additive, "ONE / ONE is additive, refused");
    is(opaque, Kind::Opaque, Refusal::None, "blend off is opaque");
    is(replaceByBlend, Kind::Opaque, Refusal::None, "ADD with a ZERO destination is opaque");
    is(multiply, Kind::Opaque, Refusal::None, "ADD with a ZERO destination is opaque even when it multiplies (the old rule)");
    is(subtract, Kind::Other, Refusal::OtherBlend, "SUBTRACT is refused");
    is(colourOnly, Kind::Alpha, Refusal::None, "RGB write mask (7) is accepted");
    is(alphaMissing, Kind::Alpha, Refusal::WriteMask, "a partial write mask is refused");
    is(coverage, Kind::Opaque, Refusal::AlphaToCoverage, "alpha to coverage is refused");

    // the Skip reasons of the mask census map one to one on the refusals
    using ext::hdr::FfxivHdrPreview::Skip;
    using ext::hdr::FfxivHdrPreview::SkipOf;
    Expect(SkipOf(Refusal::AlphaToCoverage) == Skip::BlendAlphaToCoverage, "skip: alpha to coverage");
    Expect(SkipOf(Refusal::WriteMask) == Skip::BlendWriteMask, "skip: write mask");
    Expect(SkipOf(Refusal::Additive) == Skip::BlendAdditive, "skip: additive");
    Expect(SkipOf(Refusal::OtherBlend) == Skip::BlendOther, "skip: other blend");
}

void Summary()
{
    using namespace ext::hdr::FfxivHdrPreview;
    uint64_t skipped[kSkipCount] {};
    // nothing refused: no parenthesis
    Expect(SummaryText(600, 590, 100, 100, skipped) == "HDR UI mask: 600 frames (590 with a mask), 100 tagged draws, 100 replayed, 0 refused",
           "summary without refusals");
    skipped[static_cast<unsigned>(Skip::BlendAdditive)] = 100;
    skipped[static_cast<unsigned>(Skip::NoSource)] = 245;
    const std::string line = SummaryText(1200, 1180, 12345, 12000, skipped);
    Expect(line == "HDR UI mask: 1200 frames (1180 with a mask), 12345 tagged draws, 12000 replayed, 345 refused (no_source 245, blend_additive 100)",
           "summary lists the reasons that occurred in enum order");
    if (line.find("(no_source") == std::string::npos)
        std::printf("      got: %s\n", line.c_str());
    // every reason has a distinct non-empty name
    for (unsigned i = 0; i < kSkipCount; ++i)
    {
        Expect(std::string(SkipName(static_cast<Skip>(i))).size() > 1 && std::string(SkipName(static_cast<Skip>(i))) != "?", "skip name", i);
        for (unsigned j = i + 1; j < kSkipCount; ++j)
            Expect(std::string(SkipName(static_cast<Skip>(i))) != SkipName(static_cast<Skip>(j)), "skip names are distinct", i);
    }
}
} // namespace

int main()
{
    Exhaustive();
    Named();
    Summary();
    if (g_failed)
    {
        std::printf("%d check(s) failed\n", g_failed);
        return 1;
    }
    std::puts("PASS: UiBlend.h equals the old BeforeDraw refusal rule on all 115520 blend states, kinds and reasons are right, the mask summary text is exact");
    return 0;
}
