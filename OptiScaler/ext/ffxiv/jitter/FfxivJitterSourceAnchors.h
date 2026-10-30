#pragma once
// The instruction guards of the source-level jitter fix: which bytes of the executable must match before either hook is installed.
// Pure: the reader is a parameter (ReadProcessMemory in the game, a fake memory image in tests/ext/ffxiv/jitter/jitter_source_anchors.cpp).
// The values live in GameBuild.h (JitterSourceBuild); a game patch means editing that file and re-verifying every block against the new executable.
//
// One failed block means NO hook: the fix stays off and calibrate-and-scale (when its key is on) keeps working exactly as before.

#include "ext/ffxiv/GameBuild.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ext::ffxiv::jitter::source
{
struct Anchor
{
    const char* name;
    std::uint32_t rva;
    const std::uint8_t* bytes;
    std::size_t size;
};

inline constexpr std::size_t kAnchorCount = 7;

// What each block pins, in the order they are checked:
//   generator     the generator's first instructions (the function the detour replaces)
//   cameraGetter  the camera getter (the other function the detour replaces)
//   producerCall  the world-scene NGX packet producer's call of the getter: its rel32 lands on the getter, its return address is producerReturnRva
//   cameraHead    the camera update's prologue, the load of the settings global and of the mode byte (settings+0x54)
//   phases        the load of the renderer global, the jitter object pointer, the unbounded phase count (mov edx,-1), the scene size (renderer+0x428),
//                 the DLSS-mode branch, the counter load and the other arguments of the generator call; the device global and the output width (device+0x8c)
//   strength      the generator call (its rel32 lands on the generator, its return address is generatorCallReturnRva), the render heights
//                 (renderer+0x700, the height the scene is drawn at, and +0x704, MaxRenderHeight),
//                 the settings global, the amount (settings+0x78), the scale (settings+0x4c), the strength expression, the doubling and the NDC division
//   deviceSize    another reader of the same device global: the output size as the pair (+0x8c width, +0x90 height)
inline std::array<Anchor, kAnchorCount> Anchors(const JitterSourceBuild& b)
{
    return { {
        { "generator", b.generatorRva, b.generatorInstructions.data(), b.generatorInstructions.size() },
        { "cameraGetter", b.cameraGetterRva, b.cameraGetterInstructions.data(), b.cameraGetterInstructions.size() },
        { "producerCall", b.producerCallRva, b.producerCallInstructions.data(), b.producerCallInstructions.size() },
        { "cameraHead", b.cameraHeadRva, b.cameraHeadInstructions.data(), b.cameraHeadInstructions.size() },
        { "phases", b.phasesRva, b.phasesInstructions.data(), b.phasesInstructions.size() },
        { "strength", b.strengthRva, b.strengthInstructions.data(), b.strengthInstructions.size() },
        { "deviceSize", b.deviceSizeRva, b.deviceSizeInstructions.data(), b.deviceSizeInstructions.size() },
    } };
}

struct AnchorResult
{
    bool ok = true;
    const char* failed = ""; // the name of the first block that did not match (or could not be read)
};

// `read(address, out, size)` returns false when any byte is unreadable.
template <class Reader> AnchorResult VerifyAnchors(Reader&& read, const JitterSourceBuild& b, std::uintptr_t moduleBase)
{
    for (const Anchor& anchor : Anchors(b))
    {
        constexpr std::size_t kMax = 256;
        if (anchor.size == 0 || anchor.size > kMax)
            return { false, anchor.name };
        std::uint8_t actual[kMax] {};
        if (!read(moduleBase + anchor.rva, actual, anchor.size))
            return { false, anchor.name };
        for (std::size_t i = 0; i < anchor.size; ++i)
        {
            if (actual[i] != anchor.bytes[i])
                return { false, anchor.name };
        }
    }
    return {};
}
} // namespace ext::ffxiv::jitter::source
