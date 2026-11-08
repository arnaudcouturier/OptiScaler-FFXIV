#pragma once
// Screen-space reflections: the frame-exact A/B's pure rules (no D3D, no state). The A/B is a diagnostic of the one-frame draw dump
// (`DrawDiagnosticsDump = frame`): on the capture frame, at a water draw the reflections served, it draws that same water draw ONCE MORE
// with the engine's own reflection map bound, into a private copy of the draw's render target, and the dump keeps that copy next to the real target
// after the real draw (which sampled this module's map). The two pictures differ by the effect of the reflections and by nothing else.
//
// Which water draw: the capture frame's reader draws are tried in order (at most kMaxTries) and the first whose extra draw wrote at least a floor of
// pixels is kept. The first reader draw of a frame is often a small patch, or one that is occluded: in the 2026-11-07 benchmark captures it wrote no
// pixel at all, so the three pictures were identical. The pixels an extra draw writes are counted by an occlusion query around it (and around the real
// draw of the kept one: the two counts must agree, since the water states write no depth), and every try gets one line in facts.txt.
//
// What a draw has to be for the private copy to be the same experiment: it writes ONE colour target (a second target would be written by the
// extra draw and could not be taken back), it binds no unordered access view at the output merger (the extra draw would write it), and its target
// is a plain single-sample 2D texture subresource that one copy can reproduce. Anything else is declined with its own reason and no extra draw.

#include <cstdint>
#include <cstdio>
#include <string>

namespace ext::ffxiv::engine::ssr::ab
{
enum class Decline : uint8_t
{
    None = 0,
    SsrDiagnostic,      // the SSR diagnostic's F7 modes are on: their queries and copies would count (and distort) the extra draw
    NoContext1,         // the context cannot be asked for ID3D11DeviceContext1 (the explicit-state guard needs it)
    NoDevice,           // the context has no device to make the private copy on
    NoTarget,           // the draw writes no colour target
    ExtraTargets,       // the draw binds render targets other than RT0
    OutputUav,          // the draw binds an unordered access view at the output merger
    NotTexture2D,       // RT0 is not a 2D texture
    Multisampled,       // RT0 is multisampled: a copy would not be the same destination
    NotBaseSubresource, // RT0's view writes a mip or an array slice other than the first
    SlotNotBound,       // the reflection slot holds no view the extra draw could swap back (nothing to put the engine's map in place of)
    CreateFailed,       // the private copy, its view or the depth-stencil copy could not be created
    Count
};
inline constexpr uint32_t kDeclineCount = uint32_t(Decline::Count);

constexpr const char* DeclineName(Decline reason) noexcept
{
    switch (reason)
    {
    case Decline::None: return "none";
    case Decline::SsrDiagnostic: return "the SSR diagnostic is on";
    case Decline::NoContext1: return "the context cannot be asked for ID3D11DeviceContext1";
    case Decline::NoDevice: return "the context has no device";
    case Decline::NoTarget: return "the draw writes no colour target";
    case Decline::ExtraTargets: return "the draw binds more than RT0";
    case Decline::OutputUav: return "the draw binds an output-merger UAV";
    case Decline::NotTexture2D: return "RT0 is not a 2D texture";
    case Decline::Multisampled: return "RT0 is multisampled";
    case Decline::NotBaseSubresource: return "RT0's view is not the first mip and slice";
    case Decline::SlotNotBound: return "the reflection slot holds no view";
    case Decline::CreateFailed: return "the private copy could not be created";
    default: return "unknown";
    }
}

// What the draw's output merger and RT0 look like, as the A/B reads them before it does anything.
struct OmFacts
{
    uint32_t renderTargetMask = 0; // bit i set = render target i is bound
    bool anyUav = false;           // an unordered access view is bound at the output merger
    bool texture2D = true;         // RT0's resource is a Texture2D
    uint32_t samples = 1;          // its sample count
    uint32_t subresource = 0;      // the subresource RT0's view writes
};

// The verdict, most basic reason first.
constexpr Decline Decide(const OmFacts& facts) noexcept
{
    if ((facts.renderTargetMask & 1u) == 0)
        return Decline::NoTarget;
    if ((facts.renderTargetMask & ~1u) != 0)
        return Decline::ExtraTargets;
    if (facts.anyUav)
        return Decline::OutputUav;
    if (!facts.texture2D)
        return Decline::NotTexture2D;
    if (facts.samples > 1)
        return Decline::Multisampled;
    if (facts.subresource != 0)
        return Decline::NotBaseSubresource;
    return Decline::None;
}

// ---- which reader draw is kept ---------------------------------------------------------------------------------------------------------------
// Reader draws of one capture frame the A/B tries before it gives up: a frame's worth (the benchmark's busiest has 58). The draws are streamed, so
// they cannot be ranked by coverage before the first is tried, and the draw's index count is no stand-in for it - the tessellated water patches of
// the captures include a 6714-index draw that wrote no pixel next to a 6522-index one that wrote 857 181, and the sea's own 6- and 24-index patches
// are not known to be small on the screen - so every reader draw is tried in order and the first with enough pixels is kept.
inline constexpr uint32_t kMaxTries = 128;

// The fewest pixels an extra draw must write for its pictures to be worth measuring: 0.2 % of the window (4147 pixels at 1920x1080), at least one.
constexpr uint64_t MinCoverage(uint64_t windowPixels) noexcept
{
    const uint64_t floor = windowPixels / 500u;
    return floor < 1u ? 1u : floor;
}

enum class Coverage : uint8_t
{
    Unknown, // the query could not be made or did not finish in time: the draw is kept, said so in the line
    Empty,   // the extra draw wrote no pixel
    Small,   // fewer than the floor
    Enough
};

constexpr Coverage Judge(bool known, uint64_t samples, uint64_t windowPixels) noexcept
{
    if (!known)
        return Coverage::Unknown;
    if (samples == 0)
        return Coverage::Empty;
    return samples < MinCoverage(windowPixels) ? Coverage::Small : Coverage::Enough;
}

// A draw is kept when it has enough coverage, or when its coverage could not be known.
constexpr bool Keep(Coverage coverage) noexcept { return coverage == Coverage::Enough || coverage == Coverage::Unknown; }

constexpr const char* CoverageName(Coverage coverage) noexcept
{
    switch (coverage)
    {
    case Coverage::Unknown: return "coverage unknown";
    case Coverage::Empty: return "the extra draw wrote no pixel";
    case Coverage::Small: return "below the floor";
    default: return "enough";
    }
}

// One log line per reason a session, as the module's other gates do it.
struct DeclineGate
{
    uint32_t seen = 0;
    bool Take(Decline reason) noexcept
    {
        const uint32_t bit = 1u << (uint32_t(reason) & 31u);
        if ((seen & bit) != 0)
            return false;
        seen |= bit;
        return true;
    }
};

// The facts.txt comment for a draw that was declined: "# ssr-ab declined: <reason> | frame position N | shader 0x...".
inline std::string FormatDeclined(Decline reason, uint32_t ordinal, uint32_t psCrc)
{
    char text[256];
    std::snprintf(text, sizeof text, "# ssr-ab declined: %s | frame position %u | shader 0x%08X\n", DeclineName(reason), ordinal, psCrc);
    return text;
}

// The log line of a decline (once a session per reason).
inline std::string FormatDeclinedLog(Decline reason)
{
    return std::string("Screen-space reflections A/B: no extra draw (") + DeclineName(reason) + ")";
}

// One line per try, kept or skipped: the draw, its shader, the pixels its extra draw wrote and what was decided.
inline std::string FormatTry(uint32_t tryIndex, uint32_t ordinal, uint32_t psCrc, Coverage coverage, uint64_t samples, uint64_t windowPixels)
{
    char text[320];
    if (coverage == Coverage::Unknown)
        std::snprintf(text, sizeof text, "# ssr-ab try %u | frame position %u | shader 0x%08X | %s | kept\n", tryIndex, ordinal, psCrc, CoverageName(coverage));
    else
        std::snprintf(text, sizeof text, "# ssr-ab try %u | frame position %u | shader 0x%08X | extra draw wrote %llu pixels (floor %llu of %llu) | %s\n", tryIndex,
                      ordinal, psCrc, static_cast<unsigned long long>(samples), static_cast<unsigned long long>(MinCoverage(windowPixels)),
                      static_cast<unsigned long long>(windowPixels),
                      Keep(coverage) ? "kept" : (coverage == Coverage::Empty ? "skipped: wrote no pixel" : "skipped: below the floor"));
    return text;
}

// The facts.txt comment for the A/B that ran: the draw it was made at, the target it copied, what the extra draw was not allowed to change, and the
// pixels the two draws wrote (the real draw's count is the proof that the two are the same experiment; ~0 = not measured).
inline std::string FormatRan(uint32_t ordinal, uint32_t psCrc, uint32_t slot, uint32_t targetWidth, uint32_t targetHeight, uint32_t targetFormat,
                             bool depthBound, bool hadDepthState, bool depthWritesWere, uint32_t stencilWriteMaskWas, uint32_t tryIndex = 1,
                             uint64_t extraSamples = ~0ull, uint64_t realSamples = ~0ull)
{
    char counts[160];
    if (extraSamples == ~0ull)
        std::snprintf(counts, sizeof counts, "pixels not counted");
    else if (realSamples == ~0ull)
        std::snprintf(counts, sizeof counts, "extra draw wrote %llu pixels, real draw not counted", static_cast<unsigned long long>(extraSamples));
    else
        std::snprintf(counts, sizeof counts, "extra draw wrote %llu pixels, real draw wrote %llu pixels (%s)", static_cast<unsigned long long>(extraSamples),
                      static_cast<unsigned long long>(realSamples), extraSamples == realSamples ? "equal" : "DIFFERENT");
    char text[640];
    std::snprintf(text, sizeof text,
                  "# ssr-ab ran | try %u | frame position %u | shader 0x%08X | reflection slot t%u | target %ux%u format %u | depth view %s | depth state %s "
                  "(depth writes %s, stencil write mask 0x%02X) - the extra draw wrote neither depth nor stencil | %s\n",
                  tryIndex, ordinal, psCrc, slot, targetWidth, targetHeight, targetFormat, depthBound ? "bound" : "none",
                  hadDepthState ? "copied from the game's" : "default", depthWritesWere ? "on" : "off", stencilWriteMaskWas, counts);
    return text;
}
} // namespace ext::ffxiv::engine::ssr::ab
