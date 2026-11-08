#pragma once
// Frame step and pixel mapping rules the three newer chain stages (depth of field, god rays, light bounce) share. Pure: no pch, no device,
// so every decision here is covered by tests/ext/ffxiv/engine on the host.
//
// The frame step. The game draws its deferred composite twice a frame (two adjacent draws with identical bindings), and the three stages used
// to advance their rendered-frame count at every composite draw, so the count stepped by two. Their staging rings are indexed by that count
// and read at the tone map with ages 1..3, which a step of two pushes the previous frame's copy out of (age 4). A stage now opens a new count
// at the first composite after its previous frame closed, and a frame closes at its tone map (FrameStep).
//
// Why the count opens at the composite and not at the frame's first light draw, as the older shafts module does: the light draws come before
// the composite, so a copy made at a light draw carries the count of the frame before it - one behind - and is age 1 at its own tone map,
// which is the newest age a read may take (ShaftsRules.h kNewestReadAge). Opening the count at the light draw would make it age 0, which is
// never a candidate.
//
// The pixel mapping. The stages walk rays through the scene depth in window pixels and need the pixel-to-NDC mapping the engine's own fog
// post uses. The fog post's bytecode hard-codes it as ndc = uv * (2, -2) + (-1, 1) over the render window and reads only the first float4 of
// its viewport buffer, so the mapping is derived from the window itself (PixelToNdcRow) instead of being read from that buffer's second row.

#include "ext/ffxiv/engine/ShaftsRules.h"

#include <cstdint>
#include <format>
#include <string>

namespace ext::ffxiv::shafts
{
// ---- one count per rendered frame ----------------------------------------------------------------------------------------------

// A stage's frame in progress. `closed` starts true, so the first composite opens a count. A composite while the frame is open (the
// second draw of the pair) opens nothing. The tone map closes the frame; a second tone-shaped draw of the frame (the engine's post-tone
// copy) closes nothing more. The counts of the last closed frame are kept for the log: how many composite draws it saw and how often the
// stage's count moved, which is the step the log line names.
class FrameStep
{
  public:
    // True when this composite opened a new count (and advanced `frames`).
    constexpr bool OpenAtComposite(uint64_t& frames) noexcept
    {
        ++_composites;
        if (!_closed)
            return false;
        _closed = false;
        AdvanceFrame(frames);
        ++_advances;
        return true;
    }

    constexpr void CloseAtToneMap() noexcept
    {
        if (_closed)
            return;
        _closed = true;
        _lastComposites = _composites;
        _lastAdvances = _advances;
        _composites = 0;
        _advances = 0;
        _haveSample = true;
    }

    constexpr bool Closed() const noexcept { return _closed; }
    constexpr bool HaveSample() const noexcept { return _haveSample; }
    // The step: how often the count moved between the last two tone maps. 1 is one count per rendered frame.
    constexpr uint32_t AdvancesPerFrame() const noexcept { return _lastAdvances; }
    constexpr uint32_t CompositesPerFrame() const noexcept { return _lastComposites; }
    constexpr void Reset() noexcept { *this = FrameStep {}; }

  private:
    bool _closed = true;
    bool _haveSample = false;
    uint32_t _composites = 0, _advances = 0, _lastComposites = 0, _lastAdvances = 0;
};

// Whether a tone-map call closes a stage's frame. A replayed tone map (the character preview's) never does. A call that reached the stage
// through the chain's hooks closes it whatever the stage then does with it: the caller measured the facts against its own scene record and
// has classified the draw as its tone map already. A call through the stage's own seam entry closes it when the draw classifies as the tone
// map - including a draw the stage then declines, so a declining frame moves the count on - and not for another draw of the same shader family.
constexpr bool ClosesFrame(const ToneMapFacts& facts, bool chained) noexcept
{
    if (facts.replay)
        return false;
    return chained || ClassifyToneMap(facts) != Decline::NotToneMap;
}

// ---- the ring ages the log names ------------------------------------------------------------------------------------------------

inline constexpr uint32_t kRingNotLanded = 0xFFFFFFFFu;

struct RingAge
{
    const char* name = "";
    uint32_t age = kRingNotLanded;
};

// How many counts old a copy is at the tone map: the count now minus the count it was written at. A copy from the future, or no copy at
// all, is "not landed".
constexpr uint32_t AgeOf(uint64_t now, uint64_t written) noexcept
{
    if (written > now)
        return kRingNotLanded;
    const uint64_t age = now - written;
    return age >= uint64_t(kRingNotLanded) ? kRingNotLanded - 1u : uint32_t(age);
}

inline std::string FormatRingAge(const RingAge& ring)
{
    if (ring.age == kRingNotLanded)
        return std::format("{} not landed", ring.name);
    return std::format("{} age {}", ring.name, ring.age);
}

// The line a live session reads to see the mechanism: the step of the stage's count per tone map (1 once the count moves once a frame, 2
// when it moves at both composite draws) and the age of every copy its read-backs took at the tone map.
inline std::string FormatFrameStep(const char* module, uint32_t advances, uint32_t composites, const RingAge* rings, uint32_t ringCount)
{
    std::string list;
    for (uint32_t i = 0; i < ringCount && rings != nullptr; ++i)
    {
        if (i != 0)
            list += ", ";
        list += FormatRingAge(rings[i]);
    }
    if (list.empty())
        list = "none";
    return std::format("{}: the frame counter stepped {} per tone map ({} composite draws a frame); read-backs at the tone map: {}", module,
                       advances, composites, list);
}

// One line a state: the key packs what the line says (the step, the composite count and the ring ages, a nibble each), and the line is
// written when the key differs from the last one written, at most kMax times a session. A stage's shared log gate alternates with its
// other lines and would repeat this one; this memory is the line's own.
struct ChangeLine
{
    static constexpr uint32_t kMax = 8;
    uint32_t last = 0;
    bool have = false;
    uint32_t lines = 0;

    constexpr bool Take(uint32_t key) noexcept
    {
        if (lines >= kMax || (have && key == last))
            return false;
        have = true;
        last = key;
        ++lines;
        return true;
    }
};

constexpr uint32_t NibbleOf(uint32_t value) noexcept { return value >= 15u ? 15u : value; }

// The key of the step line: advances and composites a nibble each, then up to six rings' ages a nibble each (15 = 15 or older, or not landed).
constexpr uint32_t StepKey(uint32_t advances, uint32_t composites, const RingAge* rings, uint32_t ringCount) noexcept
{
    uint32_t key = NibbleOf(advances) | (NibbleOf(composites) << 4);
    for (uint32_t i = 0; i < ringCount && i < 6 && rings != nullptr; ++i)
        key |= NibbleOf(rings[i].age) << (8 + 4 * i);
    return key;
}

// ---- the pixel-to-NDC mapping ---------------------------------------------------------------------------------------------------

// ndc = pixel * scale + offset, for a pixel in the framebuffer coordinates of the render window (the window's origin included): the
// window's top-left corner is (-1, 1) and its bottom-right corner (1, -1), y running down the texture. This is the inverse of the window
// mapping ProjectSun applies, which reproduces the sun pixel the game itself reported to 0.002 px, and the same mapping the fog post's own
// bytecode applies to its texcoord.
struct NdcRow
{
    bool ok = false;
    float scale[2] {};
    float offset[2] {};
};

constexpr NdcRow PixelToNdcRow(const Window& window) noexcept
{
    NdcRow row;
    if (!WindowUsable(window))
        return row;
    row.scale[0] = 2.0f / window.width;
    row.scale[1] = -2.0f / window.height;
    row.offset[0] = -1.0f - 2.0f * window.x / window.width;
    row.offset[1] = 1.0f + 2.0f * window.y / window.height;
    row.ok = true;
    return row;
}

// The fog post's own viewport buffer, as one line: its second float4 next to the mapping the stage derives. The stage no longer reads that
// row for the mapping; the line is how a session settles what the row holds.
inline std::string FormatViewportRow(const char* module, const float row[4], const NdcRow& derived)
{
    return std::format("{}: the fog post's viewport buffer holds ({:.6g}, {:.6g}, {:.6g}, {:.6g}) in its second row; this stage derives the "
                       "pixel-to-NDC mapping from the render window instead: scale ({:.6g}, {:.6g}), offset ({:.6g}, {:.6g})",
                       module, double(row[0]), double(row[1]), double(row[2]), double(row[3]), double(derived.scale[0]),
                       double(derived.scale[1]), double(derived.offset[0]), double(derived.offset[1]));
}
} // namespace ext::ffxiv::shafts
