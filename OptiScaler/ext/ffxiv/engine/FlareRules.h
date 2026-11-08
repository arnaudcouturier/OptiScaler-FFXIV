#pragma once
// Lens flare (ext/ffxiv/engine/FlareDraw.cpp): the pure rules. No pch, no Windows, no D3D call, so every decision here is covered by
// tests/ext/ffxiv/engine on the host.
//
// What the card does, in one line: it recognises the engine's own flare family at the draws the reflected names say (never a CRC) - the ring
// (`cHaloParam`, 64 bytes as bound), the sun sprite (`cSunParam`, 128 bytes as bound) and each radial-blur pass of the ghost chain
// (`cRadialBlurParam`, 16 bytes) - and, for exactly that draw, multiplies the one float4 of its buffer the card owns (the ring's colour,
// the sun's colour, the ghost chain's step) by the part's scale key. Nothing else of the buffer changes: the GPU scale primitive copies the
// game's bytes and keeps every component whose multiplier is exactly 1 as it went in (CbScale.hlsl), so a draw at the defaults swaps nothing.
//
// The fourth draw of the family is the veil (`LensFlarePS`): its colour and bias live in its four vertices' data, not in a buffer, so the
// card never patches it - the one part it can change, it asks with its parts bit (the veil's add is skipped for the draw and nothing else,
// because the glare buffer the draw adds into was written by GodraysMerge and is read exactly once more, by the bloom's add that follows).
//
// The recognition is by reflected name only: `cHaloParam`, `cSunParam` and `cRadialBlurParam` are each declared by exactly one pixel-shader
// blob of the live install (the flare study's table), and the veil is the one shader whose reflection holds sToneMap and no constant buffer
// at all - the two exposure-meter readers that share that reflection are split from it at the draw, by the target's size.
// The three technical offsets are the capture's (`cbdump.csv` rows of session-render-4): the ring's colour row at +0x20, the sun's at +0x40,
// the ghost step at +0x0C - the last one is the .w of the float4 at +0x00, which is why its entry addresses that float4 whole.

#include "ext/ffxiv/diag/ShaderBindings.h"
#include "ext/ffxiv/engine/EnginePassRules.h" // ScaleEntry, the fit rule and the scene floor the entries and checks are made with

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>

namespace ext::ffxiv::flare
{
// ---- the names a flare draw is keyed on ----------------------------------------------------------------------------------

// One reflected resource name the recognition asks for. A constant buffer's name keeps its plain form; a texture's arrives as
// `<name>.T` / `<name>.S` under the one effect-variable name, which ResourceBaseName strips.
struct FlareNameSpec
{
    const char* name;
    ext::ffxiv::diag::BindKind kind;
};

inline constexpr FlareNameSpec kFlareNames[] = {
    { "cHaloParam", ext::ffxiv::diag::BindKind::ConstantBuffer },       // the ring's own constants
    { "cSunParam", ext::ffxiv::diag::BindKind::ConstantBuffer },        // the sun sprite's (also the shafts' sun draw)
    { "cRadialBlurParam", ext::ffxiv::diag::BindKind::ConstantBuffer }, // one per radial-blur pass of the ghost chain
    { "sToneMap", ext::ffxiv::diag::BindKind::Texture },                // the veil's only input (the 1x1 flare-buffer average)
};
inline constexpr uint32_t kFlareNameCount = uint32_t(sizeof(kFlareNames) / sizeof(kFlareNames[0]));

// Whether the bytecode names any of them, the cheap scan that drops the shaders that cannot be a flare draw before the reflection parse.
// Pure, and deliberately a plain scan, like the engine module's own. `false` is the answer for a null pointer or a too-short blob.
inline bool MentionsFlareName(const void* code, size_t size) noexcept
{
    if (code == nullptr)
        return false;
    const auto* bytes = static_cast<const unsigned char*>(code);
    for (const FlareNameSpec& spec : kFlareNames)
    {
        const size_t length = std::strlen(spec.name);
        if (size < length)
            continue;
        for (size_t i = 0; i + length <= size; ++i)
        {
            if (bytes[i] != static_cast<unsigned char>(spec.name[0]))
                continue;
            if (std::memcmp(bytes + i, spec.name, length) == 0)
                return true;
        }
    }
    return false;
}

// ---- the families and the stamp ------------------------------------------------------------------------------------------

// The family's own draws, in the order the fix proposal ranks them. The numbers never share a value with `None`, and a stamp breaks 0 only
// when it names one.
enum class Family : uint8_t
{
    None = 0,
    Ring = 1,  // the annulus: posteffect\Halo.shcd, the parts bit 1
    Sun = 2,   // the sun sprite: posteffect\Sun.shcd, the parts bit 2 (its colour)
    Ghost = 3, // one radial-blur pass: posteffect\RadialBlur.shcd, the parts bit 2 (its step)
    Veil = 4,  // the screen-centre veil: shcd\LensFlarePS.shcd, the parts bit 4 (skipped, never patched)
};

// The stamp one flare-relevant pixel shader is stored under, and the value the bind seam leaves for the draw path: the family in the high
// bits and, for the three patchable families, the slot the patch target's name is declared at in the low eight (the veil has none: its slot
// bits stay 0xFF, the packed word still names it).
inline constexpr uint32_t kStampSlotShift = 8;
inline constexpr uint32_t kStampSlotMask = 0xFFu;

constexpr uint32_t StampOf(Family family, uint32_t slot) noexcept
{
    if (family == Family::None || uint32_t(family) > uint32_t(Family::Veil) || slot > kStampSlotMask)
        return 0;
    return (uint32_t(family) << kStampSlotShift) | (slot & kStampSlotMask);
}

constexpr Family FamilyOf(uint32_t stamp) noexcept
{
    const uint32_t family = uint32_t(stamp) >> kStampSlotShift;
    return family <= uint32_t(Family::Veil) ? Family(family) : Family::None;
}

constexpr uint32_t StampSlotOf(uint32_t stamp) noexcept { return uint32_t(stamp) & kStampSlotMask; }

// ---- the recognition, on a parsed reflection table -----------------------------------------------------------------------

// What one pixel shader's reflection holds of the four names. A slot of kNoBindPoint means the name is not declared as the kind the name
// asks for (`sToneMap` counts as the texture only, so a sampler that carries the name alone is not the veil's input).
struct FlareLayout
{
    uint32_t ringSlot = ext::ffxiv::diag::kNoBindPoint;
    uint32_t sunSlot = ext::ffxiv::diag::kNoBindPoint;
    uint32_t ghostSlot = ext::ffxiv::diag::kNoBindPoint;
    uint32_t veilSlot = ext::ffxiv::diag::kNoBindPoint;
    uint32_t constantBuffers = 0; // how many constant buffers the reflection declares (the veil declares none)
};

inline FlareLayout LayoutOf(const ext::ffxiv::diag::ShaderBinding* parsed, size_t count)
{
    FlareLayout layout;
    if (parsed == nullptr)
        return layout;
    // The slots come from the typed lookup, the one place a binding's kind is compared: the three patch targets are constant buffers and the slot a
    // patch reads them at is the slot their name is declared at; the veil's input is the texture only (the veil's reflected name on a sampler alone
    // is not its input, and the LUT-of-one kind carries the name too - it is declared at the same slot the texture below it binds). The one thing the
    // lookup does not give is how many constant buffers the reflection declares, so that is counted by kind.
    for (size_t i = 0; i < count; ++i)
        if (ext::ffxiv::diag::BindingKindOf(parsed[i]) == ext::ffxiv::diag::BindKind::ConstantBuffer)
            ++layout.constantBuffers;
    layout.ringSlot = ext::ffxiv::diag::BindPointOf(parsed, count, ext::ffxiv::diag::BindKind::ConstantBuffer, kFlareNames[0].name);
    layout.sunSlot = ext::ffxiv::diag::BindPointOf(parsed, count, ext::ffxiv::diag::BindKind::ConstantBuffer, kFlareNames[1].name);
    layout.ghostSlot = ext::ffxiv::diag::BindPointOf(parsed, count, ext::ffxiv::diag::BindKind::ConstantBuffer, kFlareNames[2].name);
    layout.veilSlot = ext::ffxiv::diag::BindPointOf(parsed, count, ext::ffxiv::diag::BindKind::Texture, kFlareNames[3].name);
    return layout;
}

// The layout of one pixel shader's bytecode: its reflection parsed, then read through LayoutOf above. The parse lives here, beside the typed
// lookups, so the draw file hands over bytes and never holds a parsed table of its own.
inline FlareLayout LayoutOfShader(const void* code, size_t size)
{
    ext::ffxiv::diag::ShaderBinding parsed[ext::ffxiv::diag::kMaxBindings];
    const size_t count = ext::ffxiv::diag::ParseBindings(code, size, parsed, ext::ffxiv::diag::kMaxBindings);
    return LayoutOf(parsed, count);
}

// Which family a layout answers, and nothing else. A draw is the ring, the sun or a ghost pass when it declares exactly that one of the
// three buffers (a layout that names two of them is a layout this card does not answer - the same fail-open a stray name gets elsewhere),
// and the veil when it declares sToneMap and no constant buffer at all - which is also the two exposure-meter readers' reflection, and
// they are split from the veil at the draw, by the target's size (VeilTargetIs).
constexpr uint32_t StampOfLayout(const FlareLayout& layout) noexcept
{
    const bool ring = layout.ringSlot != ext::ffxiv::diag::kNoBindPoint;
    const bool sun = layout.sunSlot != ext::ffxiv::diag::kNoBindPoint;
    const bool ghost = layout.ghostSlot != ext::ffxiv::diag::kNoBindPoint;
    const bool veil = layout.veilSlot != ext::ffxiv::diag::kNoBindPoint;
    if (ring && !sun && !ghost)
        return StampOf(Family::Ring, layout.ringSlot);
    if (sun && !ring && !ghost)
        return StampOf(Family::Sun, layout.sunSlot);
    if (ghost && !ring && !sun)
        return StampOf(Family::Ghost, layout.ghostSlot);
    if (!ring && !sun && !ghost && veil && layout.constantBuffers == 0)
        return StampOf(Family::Veil, kStampSlotMask);
    return 0;
}

// ---- the buffers, their sizes and the fields the card owns ---------------------------------------------------------------

// The three buffers as the engine BOUND them (the capture's rows) and as the shaders DECLARE them: a bound size this card does not
// know is a changed build's pool, and the draw is left alone with one line.
inline constexpr uint32_t kRingDeclaredBytes = 48;
inline constexpr uint32_t kRingBufferBytes = 64; // cHaloParam as bound
inline constexpr uint32_t kSunDeclaredBytes = 80;
inline constexpr uint32_t kSunBufferBytes = 128; // cSunParam as bound (the shafts' sun draw reads its first row at this size)
inline constexpr uint32_t kGhostDeclaredBytes = 16;
inline constexpr uint32_t kGhostBufferBytes = 16;

// The float4 each part owns: the ring's colour row (+0x20: .xyz is the colour, .w a mask fill the card leaves alone), the sun's colour
// row (+0x40: .w is the sprite's intensity, also left alone), and the ghost chain's step (the .w of the float4 at +0x00; the entry
// addresses that whole float4).
inline constexpr uint32_t kRingColourOffset = 0x20;
inline constexpr uint32_t kSunColourOffset = 0x40;
inline constexpr uint32_t kGhostStepOffset = 0x0C; // the field's own address; the entry's byteOffset is the float4's, 0x00

constexpr const char* NameOf(Family family) noexcept
{
    switch (family)
    {
    case Family::Ring: return "cHaloParam";
    case Family::Sun: return "cSunParam";
    case Family::Ghost: return "cRadialBlurParam";
    default: return "the flare's";
    }
}

constexpr const char* PartOf(Family family) noexcept
{
    switch (family)
    {
    case Family::Ring: return "the ring's colour";
    case Family::Sun: return "the sun sprite's colour";
    case Family::Ghost: return "the ghost chain's step";
    default: return "the flare's";
    }
}

// Whether the draw's buffer is the size this family's patch is given (always the bound width the capture recorded).
constexpr bool IsBufferSize(Family family, uint32_t byteWidth) noexcept
{
    switch (family)
    {
    case Family::Ring: return byteWidth == kRingBufferBytes;
    case Family::Sun: return byteWidth == kSunBufferBytes;
    case Family::Ghost: return byteWidth == kGhostBufferBytes;
    default: return false;
    }
}

constexpr uint32_t DeclaredBytesOf(Family family) noexcept
{
    switch (family)
    {
    case Family::Ring: return kRingDeclaredBytes;
    case Family::Sun: return kSunDeclaredBytes;
    case Family::Ghost: return kGhostDeclaredBytes;
    default: return 0;
    }
}

// ---- the scales of the three parts and the card's keys -------------------------------------------------------------------

// The engine's own value for each part is 1.0, and the identity check is the ambient knob's: a thousandth away from 1 is not a look anyone
// can see, so the frame costs one relaxed load and nothing else. A NaN reads as the default, so a broken ini value can never turn the card
// on.
inline constexpr float kMinPartScale = 0.0f;
inline constexpr float kMaxPartScale = 2.0f;
inline constexpr float kDefaultPartScale = 1.0f;
inline constexpr float kIdentityEpsilon = 0.001f;

constexpr float ClampScale(float value) noexcept
{
    if (value != value)
        return kDefaultPartScale; // a NaN reads as the engine's own value
    if (value < kMinPartScale)
        return kMinPartScale;
    return value > kMaxPartScale ? kMaxPartScale : value;
}

constexpr bool IsIdentityScale(float value) noexcept
{
    return !(value > 1.0f + kIdentityEpsilon) && !(value < 1.0f - kIdentityEpsilon);
}

// ---- the parts bitfield -------------------------------------------------------------------------------------------------

// The family's three parts, in the brief's numbering: the ring (bit 1), the sun and its ghost orbs together (bit 2), the veil (bit 4).
// A bit CLEAR means the part is removed from the picture: the ring's and the sun's colours go to 0 - a patch at scale 0, never a skip (a
// skip on the ring or the sun draw would leave the last frame's content in the buffers the composite still reads) - the ghost chain
// collapses with its sprite, and the veil's add is skipped for the draw.
inline constexpr uint32_t kPartRing = 1u;
inline constexpr uint32_t kPartSunOrbs = 2u;
inline constexpr uint32_t kPartVeil = 4u;
inline constexpr uint32_t kFlarePartsAll = kPartRing | kPartSunOrbs | kPartVeil;

constexpr uint32_t ClampParts(int32_t value) noexcept
{
    if (value < 0)
        return 0;
    if (value > int32_t(kFlarePartsAll))
        return kFlarePartsAll;
    return uint32_t(value);
}

struct Params
{
    float ring = kDefaultPartScale;  // the ring's colour multiplier
    float sun = kDefaultPartScale;   // the sun sprite's colour multiplier
    float ghost = kDefaultPartScale; // the ghost chain's step multiplier
    uint32_t parts = kFlarePartsAll; // the parts bitfield, clamped to its three bits
};

constexpr Params BuildParams(float ring, float sun, float ghost, int32_t parts) noexcept
{
    Params params;
    params.ring = ClampScale(ring);
    params.sun = ClampScale(sun);
    params.ghost = ClampScale(ghost);
    params.parts = ClampParts(parts);
    return params;
}

// Whether the veil skip is asked at all: the veil part's bit clear.
constexpr bool VeilSkipAsked(const Params& params) noexcept { return (params.parts & kPartVeil) == 0; }

// Whether the draw path has anything to do: any one of the three parts' bits clear (the ring's and the sun's patch at 0, the veil's skip
// are each a change the frame would otherwise not make), or a scale off the engine's own value while all three parts are on. With a bit
// clear the scales of the parts that stay on still apply, which is why the answer is "yes" on the bit alone. A NaN never turns it on (the
// clamp above reads NaNs as the default).
constexpr bool AnyFlareChange(const Params& params) noexcept
{
    if (params.parts != kFlarePartsAll)
        return true; // a part is asked off, whichever one: the ring, the sun with its orbs, or the veil
    return !IsIdentityScale(params.ring) || !IsIdentityScale(params.sun) || !IsIdentityScale(params.ghost);
}

// ---- the entries the GPU scale runs --------------------------------------------------------------------------------------

// The ring's colour row: the float4 at +0x20 of the 64-byte cHaloParam, its .xyz scaled and the mask fill in .w kept.
constexpr ext::ffxiv::engine::ScaleEntry RingEntry(float scale) noexcept
{
    ext::ffxiv::engine::ScaleEntry entry;
    entry.byteOffset = kRingColourOffset;
    entry.stride = 16; // one float4, addressed once (the primitive addresses float4s; a stride of 0 is refused)
    entry.count = 1;
    entry.multiplier[0] = scale;
    entry.multiplier[1] = scale;
    entry.multiplier[2] = scale;
    entry.multiplier[3] = 1.0f;
    return entry;
}

// The sun sprite's colour row, the same shape at +0x40 of the 128-byte cSunParam (its .w, the sprite's intensity, is kept).
constexpr ext::ffxiv::engine::ScaleEntry SunEntry(float scale) noexcept
{
    ext::ffxiv::engine::ScaleEntry entry;
    entry.byteOffset = kSunColourOffset;
    entry.stride = 16;
    entry.count = 1;
    entry.multiplier[0] = scale;
    entry.multiplier[1] = scale;
    entry.multiplier[2] = scale;
    entry.multiplier[3] = 1.0f;
    return entry;
}

// One radial-blur pass's step: the float4 whose .w is +0x0C, its .w scaled and the centre and decay in .xyz kept.
constexpr ext::ffxiv::engine::ScaleEntry GhostEntry(float scale) noexcept
{
    ext::ffxiv::engine::ScaleEntry entry;
    entry.byteOffset = 0x00;
    entry.stride = 16;
    entry.count = 1;
    entry.multiplier[0] = 1.0f;
    entry.multiplier[1] = 1.0f;
    entry.multiplier[2] = 1.0f;
    entry.multiplier[3] = scale;
    return entry;
}

constexpr ext::ffxiv::engine::ScaleEntry EntryOf(Family family, float scale) noexcept
{
    switch (family)
    {
    case Family::Ring: return RingEntry(scale);
    case Family::Sun: return SunEntry(scale);
    case Family::Ghost: return GhostEntry(scale);
    default: return ext::ffxiv::engine::ScaleEntry {};
    }
}

// Whether the family's entry fits the table of `byteWidth` bytes (the patch is given the buffer's bound width) and the shader's DECLARED
// size, which is the part-B proposal's own pin: a build whose pool hands the shader its declared size is still addressed.
constexpr bool EntryFitsFamily(Family family, uint32_t byteWidth) noexcept
{
    return ext::ffxiv::engine::EntryFits(EntryOf(family, 0.0f), byteWidth) &&
           ext::ffxiv::engine::EntryFits(EntryOf(family, 0.0f), DeclaredBytesOf(family));
}

// ---- the draw's verdicts, per family --------------------------------------------------------------------------------------

// What one flare draw comes to: today's picture (the game's own call, nothing counted), a patch, or the veil's skip. A draw the card asked
// for and cannot serve is one of the declines below.
enum class VerdictKind : uint8_t
{
    None = 0, // the game's own draw: the scale is the engine's own value, or the veil's bit is on
    Patch,    // the one entry the primitive scales this draw's buffer by
    Skip,     // the veil's add is skipped for this draw
};

struct Verdict
{
    VerdictKind kind = VerdictKind::None;
    float scale = 1.0f; // the patch's multiplier, for VerdictKind::Patch
};

// The ring's verdict: the part on with the scale at the engine's own value is today's picture; the part off asks it removed (scale 0).
constexpr Verdict RingVerdict(const Params& params) noexcept
{
    float scale = params.ring;
    if ((params.parts & kPartRing) == 0)
        scale = 0.0f;
    if (IsIdentityScale(scale))
        return Verdict {};
    return Verdict { VerdictKind::Patch, scale };
}

// The sun's verdict: the sprite goes with the part, and with it the ghost orbs (the radial blurs copy a zeroed image, and the veil's 1x1
// input empties into the game's own vertex-bias term) - which is why the two are one part.
constexpr Verdict SunVerdict(const Params& params) noexcept
{
    float scale = params.sun;
    if ((params.parts & kPartSunOrbs) == 0)
        scale = 0.0f;
    if (IsIdentityScale(scale))
        return Verdict {};
    return Verdict { VerdictKind::Patch, scale };
}

// One ghost pass's verdict: the step is patched while the sprite is still drawn; when the part is off or the sun's colour is already
// removed, the orbs are gone with the sun's patch and the step has nothing left to march, so the draw costs nothing.
constexpr Verdict GhostVerdict(const Params& params) noexcept
{
    if ((params.parts & kPartSunOrbs) == 0)
        return Verdict {};
    if (!IsIdentityScale(params.sun) && params.sun <= 0.0f)
        return Verdict {}; // the sun's own patch removes the orbs: the step rides along
    return IsIdentityScale(params.ghost) ? Verdict {} : Verdict { VerdictKind::Patch, params.ghost };
}

// The facts a veil draw is judged on. The veil's reflection is also the two exposure-meter readers' (sToneMap, no constant buffers), so the
// split is the target's size: the veil writes the glare buffer, the meters write the 1x1/4x4/16x16 chain of the adaptation block (the
// capture's rows).
struct VeilFacts
{
    uint32_t targetWidth = 0;
    uint32_t targetHeight = 0;
    bool replay = false; // a character-preview replay of a game draw never changes the picture it reproduces
};

// The smallest target a veil draw adds into: the glare buffer is the scene's size (1920x1080 at the capture), and an exposure meter's
// target is far under it. The floor is the engine module's own share (EnginePassRules.h's kMinSceneWidth/Height).
constexpr bool VeilTargetIs(uint32_t width, uint32_t height) noexcept
{
    return width >= ext::ffxiv::engine::kMinSceneWidth && height >= ext::ffxiv::engine::kMinSceneHeight;
}

constexpr Verdict VeilVerdict(const Params& params, const VeilFacts& facts) noexcept
{
    if (facts.replay)
        return Verdict {}; // the replay draws the game's own call, however the parts are set
    if ((params.parts & kPartVeil) != 0)
        return Verdict {}; // the veil is asked on: the game's own draw
    return VeilTargetIs(facts.targetWidth, facts.targetHeight) ? Verdict { VerdictKind::Skip, 0.0f } : Verdict {};
}

// ---- why one draw was left alone ----------------------------------------------------------------------------------------

enum class Decline : uint8_t
{
    None = 0,
    PatchFailed, // the owned buffers or the dispatch could not run
    BufferSize,  // the bound buffer is not the size this family's patch is given (the engine's pool changed)
    NoBuffer,    // nothing is bound at the slot the reflection said
    Exception,   // a call threw
};

constexpr const char* DeclineName(Decline reason) noexcept
{
    switch (reason)
    {
    case Decline::None: return "run";
    case Decline::PatchFailed: return "the owned buffers or the compute scale could not run";
    case Decline::BufferSize: return "the bound buffer is not the size this family's patch is given";
    case Decline::NoBuffer: return "nothing is bound at the slot the shader declares";
    case Decline::Exception: return "a call threw";
    }
    return "unknown";
}

inline std::string FormatDecline(Decline reason)
{
    return std::format("Lens flare: the draw was left alone ({})", DeclineName(reason));
}

// ---- the session --------------------------------------------------------------------------------------------------------

// One card's refusal streak: sixty-four of the card's own draws in a row left alone stand it down for the session, with one line (the
// shafts' own rule). Only a draw the card asked for is counted at all.
inline constexpr uint32_t kConsecutiveDeclineLimit = 64;

struct Session
{
    uint32_t declines = 0;
    bool off = false;

    // One declined draw. Returns true when this decline is the one that stood the card down.
    bool NoteDecline()
    {
        if (off)
            return false;
        if (++declines >= kConsecutiveDeclineLimit)
        {
            off = true;
            return true;
        }
        return false;
    }

    void NoteRun() { declines = 0; }
    bool Armed() const noexcept { return !off; }
};

inline std::string FormatStoodDown()
{
    return std::format("Lens flare: {} of its own draws in a row were left alone, so the card is off for this session",
                       kConsecutiveDeclineLimit);
}

// One line per change of state, and at most this many a session (the fork's habit for a developer-visible gate).
inline constexpr unsigned kMaxLogLines = 32;

struct LogGate
{
    uint32_t last = 0;
    bool have = false;
    unsigned lines = 0;

    bool Take(uint32_t tag) noexcept
    {
        if (lines >= kMaxLogLines || (have && tag == last))
            return false;
        have = true;
        last = tag;
        lines++;
        return true;
    }
};

// ---- the lines a session reads ------------------------------------------------------------------------------------------

// The line of a patch that ran: which part, at which multiplier, over the buffer of which name, at which size and slot - and the GPU time
// once it has been measured (the primitive's timestamp pair, read a run or two later, never waited for).
inline std::string FormatRun(Family family, float scale, uint32_t slot, uint32_t byteWidth, bool haveGpuMilliseconds,
                             double gpuMilliseconds)
{
    const std::string gpu = haveGpuMilliseconds ? std::format(", the patch {:.4f} ms on the GPU", gpuMilliseconds)
                                                : std::string(", its GPU time not measured yet");
    return std::format("Lens flare: {} scaled by {:.3f} over the {}-byte {} at slot {}{}", PartOf(family), double(scale), byteWidth,
                       NameOf(family), slot, gpu);
}

// The line of the first veil add the card skips: why the veil is skipped and not patched.
inline std::string FormatVeilSkip()
{
    return "Lens flare: the veil's add is skipped (the parts bit asks it off); the veil's colour and bias live in its four vertices' "
           "data rather than in a buffer, so a patch cannot recolour it, and the draw runs between the merge that wrote the glare buffer "
           "and the bloom's add that follows, so skipping it removes the veil alone";
}

// The keys' line, once per publish: what the session runs with.
inline std::string FormatKeys(const Params& params)
{
    return std::format("Lens flare: the ring x{:.2f}, the sun x{:.2f}, the ghost step x{:.2f}, parts 0x{:x} (ring 1, sun+orbs 2, veil 4)",
                       double(params.ring), double(params.sun), double(params.ghost), params.parts);
}
} // namespace ext::ffxiv::flare
