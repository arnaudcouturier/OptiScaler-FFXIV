#pragma once
// The lighting tab's chara ambient (ext/ffxiv/lighting): the pure rules. No pch, no Windows, no D3D call, so every decision here is covered
// by tests/ext/ffxiv/lighting on the host - the house style of EnginePassRules.h and ShadowsRules.h.
//
// What the module does, in one line: it recognises a draw of the game's character shading family by the name its pixel shader declares
// (`g_AmbientParam`, a row of the shared name table), and scales ONE float field of that draw's own ambient block - the block's reciprocal
// scale - for exactly that draw, with a day/night curve on the lift. The game's own buffer stays bound before and after; every state the
// module cannot measure leaves the draw exactly as it was, with one log line for the reason.
//
// The block itself, as the game's bytecode declares it (the same struct family the composite's ambient probes use, per draw instead of per
// probe): 160 bytes.
//
//   bytes  0..47   the block's own sky SH rows (three float4s)
//   bytes 48..63   one float4: colour.xyz (48, 52, 56) and shRecipScale as `.w` (byte 60) - the field the chara shaders multiply their
//                  ambient SH term with, read again on the indirect specular
//   bytes 64..79   one float4: the attenuation trio (64, 68, 72) and skyVisibility as `.w` (76)
//   bytes 80..95   reflectionScale/Offset (80, 84) and the two beside them
//   byte  88       bakeLightRate
//   bytes 96..143  the block's own sky SH rows again (the per-environment copy)
//   bytes 144..159 skyShRecipScale (144) and the environment's own values (148..156)
//
// The knob scales the `.w` of the float4 at 48 alone: shRecipScale is the per-block scalar every ambient term the shaders read is multiplied
// with, so 1.0 is exactly the picture the artists baked and one multiplier covers the block's ambient without touching its colour bytes.
// The busier fields to the sides (the attenuation, the visibility, the bake rate) keep their bits through the scale, which is what the scale
// shader's own multiplier rule guarantees: a multiplier of exactly 1 leaves the bits as they went in, so a NaN payload or a denormal beside
// the knob is never converted.
//
// The day/night curve is the shadow mapping module's own: the same pure functions (Rec.709 luminance of the frame's light colour, the same
// smoothstep shape), included from its rules header rather than copied, so one curve lives in this fork and its provisional thresholds stay
// marked there until the capture protocol settles them. The chara knob has no curve toggle of its own: the curve governs whenever the frame
// has produced a light colour reading, and a session with no reading yet runs the identity, so neither the default-off state nor a session
// whose ring has not landed patches anything.
//
// Threads: a patch runs on the render thread, while the switch and its keys are published from the menu (the render thread too) and from a
// config load, which can run on the DLL's start-up thread. The public entries serialise on the patch primitive's own mutex, so a release can
// never clear the buffers under a dispatch in flight; the draw path outside this class keeps its own state atomic.

#include "ext/ffxiv/engine/EnginePassRules.h"   // the name table, the scale-entry type, the identity test, the seen table it rides
#include "ext/ffxiv/shadows/ShadowsRules.h"    // the shared day/night curve: LuminanceOf, CurveAt (its thresholds are THIS curve's)

#include <cstdint>
#include <format>
#include <string>

namespace ext::ffxiv::lighting::chara
{
// ---- the name the chara family declares -----------------------------------------------------------------------------------

// The shared table's row that makes a shader a chara ambient draw, resolved from the table itself so the row and the rule cannot drift apart.
constexpr int32_t ResolveNameIndex()
{
    return ext::ffxiv::engine::IndexOfName("g_AmbientParam");
}
inline constexpr int32_t kCharaNameIndex = ResolveNameIndex();

static_assert(kCharaNameIndex >= 0, "the shared name table must carry a g_AmbientParam row for the chara ambient knob");
static_assert(ext::ffxiv::engine::kNames[kCharaNameIndex].kind == ext::ffxiv::engine::NameKind::ConstantBuffer,
              "the chara family's ambient block is a constant buffer: a texture row under this name would classify the wrong kind");
static_assert(ext::ffxiv::engine::IndexOfName("g_AmbientParamArray") != kCharaNameIndex,
              "the row must stay distinct from the composite's g_AmbientParamArray row");

// ---- which shaders are this module's ---------------------------------------------------------------------------------------

// The engine-wide ambient block is declared by the world's material shaders as well as the character family's: in a played session's shader
// list 48 shaders declare `g_AmbientParam`, 22 of them at slots 5 and 6 beside no customize block (the world's materials), and 26 at slots 7, 8
// and 9 beside `g_CustomizeParameter` (the character family: the skin, hair and gear variants, forward and deferred). The name alone would
// patch the world's draws too, so a character draw is a shader that declares BOTH.
inline constexpr const char* kCharaCustomizeName = "g_CustomizeParameter";

constexpr bool IsCharaShader(bool declaresAmbientBlock, bool declaresCustomizeBlock) noexcept
{
    return declaresAmbientBlock && declaresCustomizeBlock;
}

// The sentinel for "this shader does not declare it": slot 0 is a real constant-buffer slot, so 0 cannot be the answer. It is the value the
// reflection reader (diag::kNoBindPoint) uses, so a lookup's answer passes through unchanged.
inline constexpr uint32_t kNoSlot = 0xFFFFFFFFu;

// The 8-bit pack the shader table keeps per stamped shader: one nibble per name, each slot + 1, so 0 means "not declared". A slot past the last
// constant-buffer slot the engine can bind is refused here: a shader whose block sits past b13 cannot be bound, so it is not a draw this
// knob patches or reads a colour at.
//   bits 0..3  the chara ambient block's slot + 1
//   bits 4..7  the sun light draw's g_LightParam slot + 1
constexpr uint32_t PackOf(uint32_t charaSlot, uint32_t lightSlot) noexcept
{
    const uint32_t chara = charaSlot >= ext::ffxiv::engine::kConstantBufferSlots ? 0u : charaSlot + 1u;
    const uint32_t light = lightSlot >= ext::ffxiv::engine::kConstantBufferSlots ? 0u : lightSlot + 1u;
    return chara | (light << 4);
}

constexpr uint32_t CharaSlotOf(uint32_t pack) noexcept { return (pack & 0xFu) == 0 ? kNoSlot : (pack & 0xFu) - 1u; }
constexpr uint32_t LightSlotOf(uint32_t pack) noexcept { return ((pack >> 4) & 0xFu) == 0 ? kNoSlot : ((pack >> 4) & 0xFu) - 1u; }

// ---- the block's constants -----------------------------------------------------------------------------------------------

// The one size the engine binds the chara ambient block at: the struct the chara shaders declare is 160 bytes, and a buffer of any other
// width under the name is another draw's table, which is never patched and never a refusal (the name carries no shape rule of its own).
inline constexpr uint32_t kCharaAmbientBytes = 160;

// The float4 the knob addresses: byteOffset 48, the block's colour and its reciprocal scale, one entry per block (no stride to walk).
inline constexpr uint32_t kCharaRecipFloat4 = 48;

// The component the scale multiplies: `.w`, the block's shRecipScale (the colour's bytes and the two float4s beside keep their bits).
inline constexpr uint32_t kCharaRecipComponent = 3;

// The entry, at every scale the user asks for. 1.0 in the fourth component passes the byte field through unchanged: the engine's own picture.
constexpr ext::ffxiv::engine::ScaleEntry CharaRecipEntry(float scale) noexcept
{
    ext::ffxiv::engine::ScaleEntry entry;
    entry.byteOffset = kCharaRecipFloat4;
    entry.stride = 16;
    entry.count = 1;
    entry.multiplier[0] = 1.0f;
    entry.multiplier[1] = 1.0f;
    entry.multiplier[2] = 1.0f;
    entry.multiplier[kCharaRecipComponent] = scale;
    return entry;
}

constexpr bool EntryFitsChara(const ext::ffxiv::engine::ScaleEntry& entry) noexcept
{
    return ext::ffxiv::engine::EntryFits(entry, kCharaAmbientBytes);
}

constexpr bool IsCharaBufferSize(uint32_t byteWidth) noexcept { return byteWidth == kCharaAmbientBytes; }

// ---- the lift key ---------------------------------------------------------------------------------------------------------

// The knob's range: 0.5 halves the block's recScale field, 2.0 doubles it, and 1.0 is the engine's own value - today's picture. A NaN reads
// as the default, so a broken value never turns the pass on (the shipped knobs' own test).
inline constexpr float kMinCharaLift = 0.5f;
inline constexpr float kMaxCharaLift = 2.0f;
inline constexpr float kCharaLiftDefault = 1.0f;

constexpr float ClampCharaLift(float value) noexcept
{
    if (value != value)
        return kCharaLiftDefault;
    if (value < kMinCharaLift)
        return kMinCharaLift;
    return value > kMaxCharaLift ? kMaxCharaLift : value;
}

// The identity test carries an epsilon because the value comes from the ini and a slider: a thousandth away from 1.0 is not a look anyone can
// see, and running the pass for it would cost two copies and a dispatch to change nothing.
constexpr bool IsCharaLiftIdentity(float value) noexcept { return ext::ffxiv::engine::IsIdentityScale(value); }

// ---- the blend of one frame -----------------------------------------------------------------------------------------------

// What one chara draw's patch moves, from the keys and the ring's newest landed light colour alone. The curve scales the lift key, anchored
// at the engine's own 1.0 (`1 + (lift - 1) x curve`): full effect at night, the engine's own picture in day, and a session whose ring has
// not landed yet runs the identity, so the default state is byte-exact today's picture whatever the keys say.
struct Blend
{
    float curve = 0.0f;  // what the light colour gave, 0..1
    float scale = 1.0f;  // the effective multiplier (1.0 = the engine's own block)
    bool runs = false;   // the blend has left the identity range: the patch wants to run
};

// The curve is the shadow mapping module's own night curve, with the toggle pinned on (2026-11-07 integration): the visible sun (the fog sun's y)
// while it is fresh, the frame's own light colour otherwise, and the identity with neither. The same curve as the world's mask lift and night
// ambient, so characters and the world never get different night lifts (on the rc6 session's frame 4 the colour alone said full night, the
// visible sun said day).
constexpr Blend FrameBlendNight(bool masterOn, float liftKey, const shadows::NightReading& reading) noexcept
{
    Blend blend;
    if (!masterOn)
        return blend;
    blend.curve = shadows::NightCurve(true, reading).curve;
    blend.scale = 1.0f + (ClampCharaLift(liftKey) - 1.0f) * blend.curve;
    blend.runs = !IsCharaLiftIdentity(blend.scale);
    return blend;
}

// The colour-only reading (no fog sun): kept for the tests that pin the colour curve.
constexpr Blend FrameBlend(bool masterOn, float liftKey, bool haveReading, float luminance) noexcept
{
    shadows::NightReading reading;
    reading.haveColour = haveReading;
    reading.luminance = luminance;
    return FrameBlendNight(masterOn, liftKey, reading);
}

// ---- why a draw was left alone -------------------------------------------------------------------------------------------

enum class Decline : uint8_t
{
    None = 0,
    TooManyBuffers, // the frame bound more of the block than the per-frame budget patches (a distinct buffer past the budget is passed through)
    PatchFailed,    // the owned buffers or the dispatch could not run
    Exception,      // a call threw
};

constexpr const char* DeclineName(Decline reason) noexcept
{
    switch (reason)
    {
    case Decline::None: return "run";
    case Decline::TooManyBuffers: return "the frame bound more chara ambient blocks than the per-frame budget patches";
    case Decline::PatchFailed: return "the owned buffers or the compute scale could not run";
    case Decline::Exception: return "a call threw";
    }
    return "unknown";
}

// After this many of the knob's own draws in a row are left alone, the module switches itself off for the session with one log line - the
// wave's stand-down, the engine seam's ambient arm's budget. Only a draw the knob asked for is counted at all, and each line's reason carries
// its own answer first (the log gate), so a busy frame of passes-through cannot stand the module down.
inline constexpr uint32_t kConsecutiveDeclineLimit = 64;

struct Session
{
    uint32_t consecutiveDeclines = 0;
    bool off = false;

    // One declined draw. Returns true when this decline is the one that switched the module off.
    bool NoteDecline()
    {
        if (off)
            return false;
        if (++consecutiveDeclines >= kConsecutiveDeclineLimit)
        {
            off = true;
            return true;
        }
        return false;
    }

    void NoteRun() { consecutiveDeclines = 0; }

    bool Armed() const noexcept { return !off; }
};

// ---- the per-frame cache of the patched block -----------------------------------------------------------------------------

// The game binds the chara ambient block to the draws of a frame (probably one object, at most a handful of distinct buffers), and the GPU
// patch runs once per (frame, buffer pointer) with every later draw of that pointer answered from the cached result: what is left per draw is
// the slot swap and its restore. The wetness knob's FrameCache shape, carried here so the budget can be the chara knob's own: the block is a
// 160-byte table the engine may bind per character, so a busy frame with many characters would burn a 4-patch budget in about 8 draws, which
// would pass through most of a crowd the whole way through.
//
// The frame token is the caller's (ext/present's output-frame counter, one relaxed load). `hasFrame` separates "no token read yet" from a
// token value of 0. The epoch is the caller's too: the knob and the master publish a new one, and the patch primitive counts its own
// re-initialisation (a device change, or a size cache eviction that frees the buffer the entry points at), so a knob that moved or a device
// that re-created the buffers clears the entry without any thread but the draw thread writing this struct. The entry also expires after
// kMaxServedDraws binds: a token that never advances then costs one patch every few thousand draws instead of a value that stays at the
// first frame's for the session.
inline constexpr uint32_t kMaxPatchesPerFrame = 8; // a ninth distinct pointer in one frame is passed through, logged once
inline constexpr uint32_t kMaxServedDraws = 4096;  // the fail-safe for a frame token that does not move

struct CharaFrameCache
{
    uint64_t frame = 0;   // the token of the frame the entry belongs to
    uint32_t epoch = 0;   // the knob/master/patch generation the entry belongs to
    uint32_t patches = 0; // patches run for that frame
    uint32_t served = 0;  // binds answered from the cached result for that frame
    bool hasFrame = false;
    bool hasEpoch = false;
    bool valid = false; // (game, dst) answers this frame's patch
    const void* game = nullptr;
    const void* dst = nullptr;

    void Reset() noexcept { *this = CharaFrameCache {}; }

    // The tokens the caller read. A new epoch clears the entry first (the knob moved, the master switched, the primitive re-created its
    // buffers), then a different frame token starts a frame. Returns whether this call started a frame: that is when the curve's light
    // colour ring is read, one no-wait read a frame instead of one per draw.
    bool Begin(uint64_t frameToken, uint32_t epochToken) noexcept
    {
        if (!hasEpoch || epochToken != epoch)
        {
            hasEpoch = true;
            epoch = epochToken;
            hasFrame = false;
        }
        if (hasFrame && frameToken == frame)
            return false;
        hasFrame = true;
        frame = frameToken;
        ClearFrame();
        return true;
    }

    void ClearFrame() noexcept
    {
        patches = 0;
        served = 0;
        valid = false;
        game = nullptr;
        dst = nullptr;
    }

    bool Has(const void* buffer) const noexcept { return valid && buffer == game; }
    const void* Dst() const noexcept { return dst; }
    bool MayPatch() const noexcept { return patches < kMaxPatchesPerFrame; }

    void Record(const void* buffer, const void* patched) noexcept
    {
        game = buffer;
        dst = patched;
        valid = true;
        ++patches;
        served = 0;
    }

    // One bind answered from the cache. A long run without a frame change expires the entry and the patch budget: the token is not advancing,
    // so the next draw runs the patch again rather than binding a result that may belong to an earlier frame.
    void Serve() noexcept
    {
        if (++served < kMaxServedDraws)
            return;
        ClearFrame();
    }
};

// ---- the one log line a state change writes -------------------------------------------------------------------------------

inline std::string FormatDecline(Decline reason)
{
    return std::format("Lighting chara ambient: the draw was left alone ({})", DeclineName(reason));
}

inline std::string FormatSwitchedOff()
{
    return std::format("Lighting chara ambient: {} chara draws in a row were left alone, so the chara ambient is off for this session",
                       kConsecutiveDeclineLimit);
}

// The run line: what was scaled, under which curve, on which buffer at which slot, and what the GPU work cost once it has been measured.
inline std::string FormatRun(float scale, float curve, uint32_t slot, uint32_t boundBytes, bool haveGpuMilliseconds,
                             double gpuMilliseconds)
{
    const std::string gpu = haveGpuMilliseconds ? std::format(", the patch {:.4f} ms on the GPU", gpuMilliseconds)
                                                : std::string(", its GPU time not measured yet");
    return std::format("Lighting chara ambient: scaling the block's SH reciprocal scale by {:.3f} (curve {:.3f}, day/night thresholds "
                       "provisional until the capture runs) over {} bytes at slot {}{}",
                       double(scale), double(curve), boundBytes, slot, gpu);
}

// The one line for an armed module whose ring has not landed yet, whatever else happens today.
inline std::string FormatWaitingForColour()
{
    return "Lighting chara ambient: the lift is armed but no light colour has been read yet, so the lift stays at the engine's own picture";
}

// The one line for the draws a frame's budget could not reach (a distinct block past the per-frame budget): why a crowd's later characters
// keep the engine's own block today.
inline std::string FormatBudgetSpent(uint32_t budget)
{
    return std::format("Lighting chara ambient: one frame bound more chara ambient blocks than the {} the budget patches", budget);
}
} // namespace ext::ffxiv::lighting::chara
