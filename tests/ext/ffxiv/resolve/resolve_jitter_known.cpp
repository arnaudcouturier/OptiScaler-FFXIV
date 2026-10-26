// The jitter-source shapes on a KNOWN build (the image carries build C's stamp and size and the resolver is given the table, as P3 will do in the
// game when the executable is the audited one). The strict shapes are the only check of the bytes the hooks rely on, and the table's Guard arrays
// are SHORT prefixes of them (generator 16 of 306 bytes, strength 157 of 204, producerCall 13 of 111): if a shape that does not match were replaced
// by the table's guard (the resolver's known-build fallback), the hooks would install over code that differs from the audited file (found in review,
// with the PoC rows below). SeamSpec::failClosed on every shape seam prevents that: the Guard field stays ZERO and the hooks' own
// byte compare refuses.
//
// What is checked here, all through the real ResolveImage and the real VerifyAnchors:
//   * the unmodified build: every shape Found, nothing filled from the table, the hooks' compare passes;
//   * the review's PoC rows: location Found, shape NotFound -> the Guard is zero, UsableOrTable(shape) is false, the hooks refuse; the CONTROL
//     (the same registry with failClosed switched off) lets every one of them through, so the test fails if the flag is ever lost;
//   * every non-wildcard byte of every shape, flipped one at a time on the known build, is refused (the table of facts is in
//     resolve_jitter_shape_data.h).
#include "resolve_jitter_shape_data.h"

#include <cstring>

using namespace jt;

namespace
{
struct Poc
{
    const char* what; // what the byte carries
    std::uint32_t rva;
    const char* shape; // the shape seam the byte lies in
};

// The rows of the review (finding 1). Every byte lies BEYOND the table's Guard window of its shape, so without failClosed the table guard stands in
// for the failed shape and does not look at it.
const Poc kPoc[] = {
    { "generator: a Halton loop byte (jne back, 75 C8 at +0x6e)", 0x2ebd90 + 0x6e, "jitterSource.generatorShape" },
    { "generator: x - 0.5 (subss xmm6,xmm9 modrm)", 0x2ebd90 + 0x102, "jitterSource.generatorShape" },
    { "generator: output store through rsi (movss [rsi],xmm6)", 0x2ebeaa, "jitterSource.generatorShape" },
    { "strength: NDC store [rcx+8] -> [rcx+0xc]", 0x25f864, "jitterSource.strengthShape" },
    { "producer: world camera load [rax+0x3028] (high byte)", 0x3740e2, "jitterSource.producerCallShape" },
    { "producer: a write to rcx between the camera load and the call", 0x37410d, "jitterSource.producerCallShape" },
    { "producer: mov rcx,[rax] reads the returned object", 0x374130, "jitterSource.producerCallShape" },
};

const ShapeFacts* FactsOf(const std::string& shape)
{
    for (const ShapeFacts& s : Shapes())
    {
        if (shape == s.shape)
            return &s;
    }
    return nullptr;
}

// the registry with failClosed switched off on every seam (the behaviour before the fix)
Registry WithoutFailClosed()
{
    Registry reg = JitterRegistry();
    for (SeamSpec& s : reg.seams)
        s.failClosed = false;
    return reg;
}

void TestUnmodifiedKnownBuild()
{
    auto img = MakeJitterImage(kStampC);
    REQUIRE(img != nullptr);
    ResolveOptions opts;
    opts.knownTable = &ext::ffxiv::kCurrentBuild;
    const Registry reg = JitterRegistry();
    const ResolvedBuild rb = ResolveImage(*img, reg, opts);
    CHECK(rb.knownBuild && rb.Count(SeamStatus::Found) == reg.seams.size());
    for (const SeamResult& s : rb.seams)
        CHECK(!s.fellBackToTable);
    CHECK(ext::ffxiv::jitter::source::VerifyAnchors(ReaderOf(*img), rb.build.jitterSource, kBase).ok);
    for (const ShapeFacts& f : Shapes())
        CHECK(rb.Usable(f.shape) && !GuardIsZero(rb, f.guardKey));
}

// the table's Guard window of every shape is what the facts say (so "beyond the window" in the PoC rows is a fact, not a claim)
void TestGuardWindows()
{
    const auto& t = ext::ffxiv::kCurrentBuild.jitterSource;
    CHECK(t.generatorInstructions.size() == 16 && t.cameraGetterInstructions.size() == 8 && t.cameraHeadInstructions.size() == 78);
    CHECK(t.phasesInstructions.size() == 120 && t.strengthInstructions.size() == 157 && t.producerCallInstructions.size() == 13);
    CHECK(t.deviceSizeInstructions.size() == 20);
    for (const ShapeFacts& f : Shapes())
    {
        const FieldRow* row = FindField(f.guardKey);
        REQUIRE(row != nullptr);
        CHECK(row->size == f.guardLen);
    }
    for (const Poc& p : kPoc)
    {
        const ShapeFacts* f = FactsOf(p.shape);
        REQUIRE(f != nullptr);
        CHECK(p.rva >= f->site && p.rva < f->site + f->size);
        const std::uint32_t at = p.rva - f->site;
        CHECK(at < f->guardAt || at >= f->guardAt + f->guardLen); // beyond the Guard window
        CHECK(!IsMaskedAt(*f, at));
    }
}

void TestPocRows()
{
    const Registry reg = JitterRegistry();
    const Registry open = WithoutFailClosed();
    for (const Poc& p : kPoc)
    {
        const ShapeFacts* f = FactsOf(p.shape);
        REQUIRE(f != nullptr);
        auto img = MakeJitterImage(kStampC);
        REQUIRE(img != nullptr);
        const FlipRun run = FlipAndResolve(*img, reg, p.rva, true);
        const SeamResult& shape = Get(run.rb, f->shape);
        const SeamResult& location = Get(run.rb, f->location);
        if (run.verifyOk || shape.status != SeamStatus::NotFound)
            std::printf("FAIL: %s: the hooks would install (shape %s)\n", p.what, StatusName(shape.status));
        CHECK(run.rb.knownBuild);
        CHECK(!run.verifyOk && std::strcmp(run.failedBlock, f->block) == 0);
        CHECK(shape.status == SeamStatus::NotFound && !shape.fellBackToTable && !run.rb.UsableOrTable(f->shape));
        CHECK(GuardIsZero(run.rb, f->guardKey));
        // the location seam is found (the byte lies in the shape, not in a location anchor, or leaves one anchor): the shape alone refuses
        CHECK(run.rb.Usable(f->location) && !location.fellBackToTable);

        // CONTROL: without failClosed the table's short guard replaces the failed shape and the hooks install (what the review reproduced)
        auto img2 = MakeJitterImage(kStampC);
        REQUIRE(img2 != nullptr);
        const FlipRun old = FlipAndResolve(*img2, open, p.rva, true);
        const SeamResult& oldShape = Get(old.rb, f->shape);
        if (!old.verifyOk || !oldShape.fellBackToTable)
            std::printf("FAIL: control: %s is refused even without failClosed (so the test would not notice a lost flag)\n", p.what);
        CHECK(old.verifyOk && oldShape.fellBackToTable && old.rb.UsableOrTable(f->shape));
        std::printf("    refused: %-66s -> %s (without failClosed: installs)\n", p.what, run.failedBlock);
    }
}

// Every non-wildcard byte of every shape, one at a time, on the known build: location Found or single-anchor, shape NotFound with a zero Guard, never
// filled from the table, and the real VerifyAnchors refuses (always through the block of that shape).
void TestEveryFixedByteOnAKnownBuild()
{
    const Registry reg = JitterRegistry();
    auto img = MakeJitterImage(kStampC);
    REQUIRE(img != nullptr);
    std::size_t flips = 0, beyondWindow = 0;
    for (const ShapeFacts& f : Shapes())
    {
        std::size_t shapeFlips = 0;
        for (std::size_t at = 0; at < f.size; ++at)
        {
            if (IsMaskedAt(f, at))
                continue;
            const std::uint32_t rva = f.site + static_cast<std::uint32_t>(at);
            const FlipRun run = FlipAndResolve(*img, reg, rva, true);
            const SeamResult& shape = Get(run.rb, f.shape);
            // NotFound (a guard anchor that does not match) or, for deviceSize whose anchor is scanned for two copies, Ambiguous (one copy left)
            const bool ok = !run.verifyOk && std::strcmp(run.failedBlock, f.block) == 0 && !IsUsable(shape.status) && !shape.fellBackToTable &&
                            !run.rb.UsableOrTable(f.shape) && GuardIsZero(run.rb, f.guardKey);
            if (!ok)
                std::printf("FAIL: known build: flipping %s +0x%zx (rva 0x%x): verifyOk=%d block=%s shape=%s fellBack=%d\n", f.shape, at, rva, run.verifyOk ? 1 : 0,
                            run.failedBlock, StatusName(shape.status), shape.fellBackToTable ? 1 : 0);
            CHECK(ok);
            ++shapeFlips;
            if (at < f.guardAt || at >= f.guardAt + f.guardLen)
                ++beyondWindow;
        }
        CHECK(shapeFlips == FixedBytes(f));
        flips += shapeFlips;
    }
    std::printf("    known build: %zu fixed bytes flipped, %zu of them beyond the table's Guard window, all refused\n", flips, beyondWindow);
    CHECK(flips == 286 + 8 + 69 + 100 + 168 + 91 + 16);
    CHECK(beyondWindow == 270 + 39 + 85); // generator, strength, producer: where the table guard could not have seen the byte
}
} // namespace

int main()
{
    TestGuardWindows();
    TestUnmodifiedKnownBuild();
    TestPocRows();
    TestEveryFixedByteOnAKnownBuild();
    return TEST_RESULT("resolve_jitter_known");
}
