// The reflections' frame-exact A/B: the pure rules (SsrAbRules.h) and the diagnostic's facts lines. No device.
#include "check.h"

#include "ext/ffxiv/engine/SsrAbRules.h"

#include <string>

using namespace ext::ffxiv::engine::ssr::ab;

namespace
{
bool Has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }
} // namespace

int main()
{
    // ---- the verdict: one target, no output-merger UAV, a plain single-sample 2D texture's first subresource ---------------------------------
    {
        OmFacts ok;
        ok.renderTargetMask = 1;
        CHECK(Decide(ok) == Decline::None);

        OmFacts none;
        none.renderTargetMask = 0;
        CHECK(Decide(none) == Decline::NoTarget);

        OmFacts onlyOne; // RT1 alone is not RT0
        onlyOne.renderTargetMask = 2;
        CHECK(Decide(onlyOne) == Decline::NoTarget);

        for (uint32_t extra = 1; extra < 8; ++extra) // any other target bound beside RT0
        {
            OmFacts two;
            two.renderTargetMask = 1u | (1u << extra);
            CHECK(Decide(two) == Decline::ExtraTargets);
        }
        OmFacts gap; // RT0 and RT7 with a gap between: still more than RT0
        gap.renderTargetMask = 0x81;
        CHECK(Decide(gap) == Decline::ExtraTargets);

        OmFacts uav = ok;
        uav.anyUav = true;
        CHECK(Decide(uav) == Decline::OutputUav);

        OmFacts notTexture = ok;
        notTexture.texture2D = false;
        CHECK(Decide(notTexture) == Decline::NotTexture2D);

        OmFacts msaa = ok;
        msaa.samples = 4;
        CHECK(Decide(msaa) == Decline::Multisampled);

        OmFacts mip = ok;
        mip.subresource = 3;
        CHECK(Decide(mip) == Decline::NotBaseSubresource);

        // The order: the most basic reason first, so a draw with several problems says the first.
        OmFacts everything;
        everything.renderTargetMask = 0x3;
        everything.anyUav = true;
        everything.texture2D = false;
        everything.samples = 4;
        everything.subresource = 1;
        CHECK(Decide(everything) == Decline::ExtraTargets);
        everything.renderTargetMask = 1;
        CHECK(Decide(everything) == Decline::OutputUav);
        everything.anyUav = false;
        CHECK(Decide(everything) == Decline::NotTexture2D);
        everything.texture2D = true;
        CHECK(Decide(everything) == Decline::Multisampled);
        everything.samples = 1;
        CHECK(Decide(everything) == Decline::NotBaseSubresource);
    }

    // ---- every reason has its own name, and the names are distinct ----------------------------------------------------------------------------
    {
        for (uint32_t i = 1; i < kDeclineCount; ++i)
        {
            const std::string name = DeclineName(Decline(i));
            CHECK(!name.empty() && name != "unknown" && name != "none");
            for (uint32_t j = i + 1; j < kDeclineCount; ++j)
                CHECK(name != DeclineName(Decline(j)));
        }
        CHECK(std::string(DeclineName(Decline::None)) == "none");
    }

    // ---- one log line a session per reason ---------------------------------------------------------------------------------------------------
    {
        DeclineGate gate;
        CHECK(gate.Take(Decline::ExtraTargets));
        CHECK(!gate.Take(Decline::ExtraTargets));
        CHECK(gate.Take(Decline::OutputUav));
        CHECK(!gate.Take(Decline::OutputUav));
        CHECK(!gate.Take(Decline::ExtraTargets));
    }

    // ---- the facts lines name the draw, the shader and the reason ---------------------------------------------------------------------------------
    {
        const std::string declined = FormatDeclined(Decline::ExtraTargets, 4321, 0x60F6192Cu);
        CHECK(Has(declined, "# ssr-ab declined: the draw binds more than RT0"));
        CHECK(Has(declined, "frame position 4321"));
        CHECK(Has(declined, "shader 0x60F6192C"));
        CHECK(declined.back() == '\n'); // one comment line

        const std::string ran = FormatRan(4321, 0x27447AF4u, 8, 3840, 2160, 10, true, true, true, 0xFF);
        CHECK(Has(ran, "# ssr-ab ran"));
        CHECK(Has(ran, "frame position 4321"));
        CHECK(Has(ran, "shader 0x27447AF4"));
        CHECK(Has(ran, "reflection slot t8"));
        CHECK(Has(ran, "target 3840x2160 format 10"));
        CHECK(Has(ran, "depth view bound"));
        CHECK(Has(ran, "depth state copied from the game's (depth writes on, stencil write mask 0xFF)"));
        CHECK(Has(ran, "wrote neither depth nor stencil"));
        CHECK(ran.back() == '\n');

        const std::string bare = FormatRan(1, 2, 8, 4, 4, 2, false, false, false, 0);
        CHECK(Has(bare, "depth view none"));
        CHECK(Has(bare, "depth state default (depth writes off, stencil write mask 0x00)"));

        CHECK(Has(FormatDeclinedLog(Decline::OutputUav), "an output-merger UAV"));
    }

    // ---- which reader draw is kept: the coverage floor, the verdict, the try line ---------------------------------------------------------------------
    {
        CHECK_EQ(MinCoverage(1920ull * 1080ull), 4147ull); // 0.2 % of the window
        CHECK_EQ(MinCoverage(0), 1ull);                    // never zero: a draw that wrote nothing is never kept
        CHECK_EQ(MinCoverage(96), 1ull);
        CHECK_EQ(MinCoverage(500), 1ull);
        CHECK_EQ(MinCoverage(1000), 2ull);
        const uint64_t window = 1920ull * 1080ull;
        CHECK(Judge(true, 0, window) == Coverage::Empty);
        CHECK(Judge(true, 1, window) == Coverage::Small);
        CHECK(Judge(true, 4146, window) == Coverage::Small);
        CHECK(Judge(true, 4147, window) == Coverage::Enough);
        CHECK(Judge(true, window, window) == Coverage::Enough);
        CHECK(Judge(false, 0, window) == Coverage::Unknown); // not known is not zero
        CHECK(Judge(false, 123456, window) == Coverage::Unknown);
        CHECK(!Keep(Coverage::Empty) && !Keep(Coverage::Small));
        CHECK(Keep(Coverage::Enough) && Keep(Coverage::Unknown));
        // A capture frame offers every reader draw it has (the benchmark's busiest has 58; the first build's 12 and the second's 32 ran out before
        // the draw that covers the picture, and a count of the draw's indices says nothing of its coverage: the tessellated patches of the
        // captures give a 6714-index draw that wrote no pixel and a 6522-index one that wrote 857 181), so the cap is a frame's worth.
        CHECK(kMaxTries >= 96 && kMaxTries <= 256);

        const std::string kept = FormatTry(2, 1367, 0x60F6192Cu, Coverage::Enough, 61000, window);
        CHECK(Has(kept, "# ssr-ab try 2 | frame position 1367 | shader 0x60F6192C | extra draw wrote 61000 pixels (floor 4147 of 2073600) | kept"));
        const std::string empty = FormatTry(1, 1366, 0xB7F64C93u, Coverage::Empty, 0, window);
        CHECK(Has(empty, "extra draw wrote 0 pixels") && Has(empty, "skipped: wrote no pixel"));
        const std::string small = FormatTry(3, 1368, 1u, Coverage::Small, 63, window);
        CHECK(Has(small, "skipped: below the floor") && Has(small, "wrote 63 pixels"));
        const std::string unknown = FormatTry(4, 1369, 2u, Coverage::Unknown, 0, window);
        CHECK(Has(unknown, "coverage unknown") && Has(unknown, "| kept") && !Has(unknown, "wrote 0 pixels"));
        CHECK(kept.back() == '\n' && empty.back() == '\n' && unknown.back() == '\n');

        // The ran line carries both counts, and says when they differ.
        const std::string equal = FormatRan(1, 2, 8, 4, 4, 2, true, true, false, 0, 3, 96, 96);
        CHECK(Has(equal, "| try 3 |") && Has(equal, "extra draw wrote 96 pixels, real draw wrote 96 pixels (equal)"));
        const std::string different = FormatRan(1, 2, 8, 4, 4, 2, true, true, false, 0, 1, 96, 90);
        CHECK(Has(different, "(DIFFERENT)"));
        CHECK(Has(FormatRan(1, 2, 8, 4, 4, 2, true, true, false, 0, 1, 96, ~0ull), "real draw not counted"));
        CHECK(Has(FormatRan(1, 2, 8, 4, 4, 2, true, true, false, 0), "pixels not counted"));
    }

    return Finish("ssr_ab_rules");
}
