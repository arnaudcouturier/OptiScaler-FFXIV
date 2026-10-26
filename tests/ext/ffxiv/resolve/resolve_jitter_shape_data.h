#pragma once
// The LITERAL facts of the six strict shapes and of the deviceSize shape, kept in the tests and independent of the registry and of the pin text
// (which `resolve_jitter_anchors --dump` regenerates): where each shape sits on the committed build-C layout, how long it is, which bytes are
// wildcards and why, which VerifyAnchors block refuses it, and which window of it the table's Guard array covers.
//
// Used by resolve_jitter_shapes (every non-wildcard byte of every shape is flipped on an UNKNOWN build) and resolve_jitter_known (the same on a
// KNOWN build). Because the flips are generated from THIS table and not from the registry, a registry that masks a byte the table says is fixed is
// caught behaviourally (the flip is not refused), not only by the comparison of the mask map.
#include "resolve_jitter_image.h"

#include "ext/ffxiv/jitter/FfxivJitterSourceAnchors.h"

#include <cstring>

namespace jt
{
// a wildcard run: a RIP displacement, a call target or the TLS slot immediate (4 bytes), or one short-jump displacement (1 byte)
struct MaskRun
{
    std::uint32_t at;
    std::uint32_t len;
    const char* why;
};

struct ShapeFacts
{
    const char* shape;     // the shape seam
    const char* location;  // the location seam it hangs from (== shape for deviceSize)
    const char* guardKey;  // the GameBuild Guard field bound to the shape seam
    const char* block;     // the VerifyAnchors block that refuses it
    std::uint32_t site;    // RVA of the first byte of the pattern on the build-C layout
    std::size_t size;      // pattern bytes
    std::vector<MaskRun> masks;
    std::uint32_t guardAt; // offset, inside the pattern, where the table's Guard array starts (the producer's starts at the call)
    std::size_t guardLen;  // the table's Guard array length (the strict pattern is longer than that for the generator, strength, producer)
};

// clang-format off
inline const std::vector<ShapeFacts>& Shapes()
{
    static const std::vector<ShapeFacts> shapes = {
        { "jitterSource.generatorShape", "jitterSource.generator", "jitterSource.generatorInstructions", "generator", 0x2ebd90, 306,
          { { 0x32, 4, "rip: the 0.5 constant" }, { 0x4d, 4, "rip: the 1.0 constant" }, { 0x8d, 4, "call: float-to-int helper" },
            { 0x9f, 4, "rip: the 3.0 constant" }, { 0xe8, 4, "call: float-to-int helper" } }, 0, 16 },
        { "jitterSource.cameraGetterShape", "jitterSource.cameraGetter", "jitterSource.cameraGetterInstructions", "cameraGetter", 0x25f610, 8,
          {}, 0, 8 },
        { "jitterSource.cameraHeadShape", "jitterSource.cameraHead", "jitterSource.cameraHeadInstructions", "cameraHead", 0x25f690, 78,
          { { 0x15, 4, "rip: the stack cookie" }, { 0x37, 4, "rip: the settings global" },
            { 0x4d, 1, "rel8: the jne over 15 unpinned bytes (the hooks do not read it, see AnchorsJitter.h)" } }, 0, 78 },
        { "jitterSource.phasesShape", "jitterSource.phases", "jitterSource.phasesInstructions", "phases", 0x25f721, 120,
          { { 0x03, 4, "rip: the renderer global" }, { 0x34, 4, "rip: the device global" }, { 0x3f, 4, "rip: a float constant" },
            { 0x53, 4, "call: the phase-count helper" }, { 0x5b, 4, "rip: a float constant" } }, 0, 120 },
        { "jitterSource.strengthShape", "jitterSource.strength", "jitterSource.strengthInstructions", "strength", 0x25f799, 204,
          { { 0x01, 4, "call: the generator (the location seam checks it)" }, { 0x08, 4, "rip: the renderer global" }, { 0x3f, 4, "rip: the settings global" },
            { 0x4f, 4, "rip: the strength floor (seam strengthFloor)" }, { 0x70, 4, "rip: the strength floor (seam strengthFloor)" },
            { 0x8a, 4, "rip: the NDC sign mask (seam ndcSignMask)" }, { 0x95, 4, "rip: an unrelated matrix constant" },
            { 0xa4, 4, "rip: an unrelated matrix constant" }, { 0xb1, 4, "rip: an unrelated matrix constant" } }, 0, 157 },
        { "jitterSource.producerCallShape", "jitterSource.producerCall", "jitterSource.producerCallInstructions", "producerCall", 0x3740c9, 111,
          { { 0x03, 4, "rip: the camera global" }, { 0x0a, 4, "rip: the renderer global" }, { 0x11, 4, "rip: the device global" },
            { 0x55, 4, "call: the camera getter (the location seam checks it)" }, { 0x5e, 4, "imm: the thread-local slot of mov r13d" } }, 0x54, 13 },
        { "jitterSource.deviceSize", "jitterSource.deviceSize", "jitterSource.deviceSizeInstructions", "deviceSize", 0xd3f73, 20,
          { { 0x03, 4, "rip: the device global" } }, 0, 20 },
    };
    return shapes;
}
// clang-format on

inline bool IsMaskedAt(const ShapeFacts& s, std::size_t offset)
{
    for (const MaskRun& m : s.masks)
    {
        if (offset >= m.at && offset < m.at + m.len)
            return true;
    }
    return false;
}

inline std::size_t FixedBytes(const ShapeFacts& s)
{
    std::size_t wild = 0;
    for (const MaskRun& m : s.masks)
        wild += m.len;
    return s.size - wild;
}

// the bytes of a Guard field of a resolved build are all zero (the field the shape seam would have filled)
inline bool GuardIsZero(const ResolvedBuild& rb, const char* guardKey)
{
    const FieldRow* row = FindField(guardKey);
    if (row == nullptr)
        return false;
    const std::uint8_t* b = FieldBytes(rb.build, *row);
    for (std::size_t i = 0; i < row->size; ++i)
    {
        if (b[i] != 0)
            return false;
    }
    return true;
}

// One flip: the byte at `rva` of the image is changed (xor 0xff), the real registry is resolved (on a KNOWN build when `known`: the image carries
// build C's stamp and the table is passed), the real VerifyAnchors runs over the resolved build, and the byte is restored.
struct FlipRun
{
    ResolvedBuild rb;
    bool verifyOk = true;
    const char* failedBlock = "";
};

inline FlipRun FlipAndResolve(DenseImage& img, const Registry& reg, std::uint32_t rva, bool known)
{
    FlipRun run;
    std::uint8_t& byte = img.Bytes()[rva];
    const std::uint8_t original = byte;
    byte = static_cast<std::uint8_t>(original ^ 0xff);
    ResolveOptions opts;
    if (known)
        opts.knownTable = &ext::ffxiv::kCurrentBuild;
    run.rb = ResolveImage(img, reg, opts);
    const auto r = ext::ffxiv::jitter::source::VerifyAnchors(ReaderOf(img), run.rb.build.jitterSource, kBase);
    run.verifyOk = r.ok;
    run.failedBlock = r.failed;
    byte = original;
    return run;
}
} // namespace jt
