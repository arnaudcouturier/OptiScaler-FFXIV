#pragma once
// Engine pass overrides (ext/ffxiv/engine): the pure rules. No pch, no Windows, D3D or shader call, so every decision here is covered by
// tests/ext/ffxiv/engine on the host.
//
// What the module does, in one line: it recognises one of the engine's own draws by the names its pixel shader declares, patches one of that
// draw's constant buffers for exactly that draw (a float4 scale on the GPU, or an owned buffer the CPU writes), and puts the game's own
// binding back afterwards.
//
// This header holds everything that can be decided without a device: the names a draw is keyed on and the slot table a shader's reflection
// fills, the scale-entry and set-entry tables the GPU path hands to its compute shader, the rules that keep each knob at today's picture when it
// is off, the draw validation (the composite's one render target at the main-scene size; the wetness draw's 32-byte buffer), the per-frame cache
// the wetness patch is bound from, and the refusal counter. The fail-open contract is the drop list below: every reason leaves the game's draw
// exactly as it was, with the game's own buffer still bound.

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>

namespace ext::ffxiv::engine
{
// ---- the names a draw is keyed on ----------------------------------------------------------------------------------------

// Whether a name is a constant buffer (a patch target) or a texture the draw must sample for a knob to make sense (its second anchor).
// Both are looked up the same way; the kind only says what the slot is for, so a validation never mistakes a texture slot for a buffer.
enum class NameKind : uint8_t
{
    ConstantBuffer = 0,
    Texture = 1,
};

struct NameSpec
{
    const char* name;
    NameKind kind;
};

// The engine's own parameter names. They are an ABI of the game, not a contract, so this table is the one place a knob's names live:
// adding a knob is adding a row here plus its validation, never a second index.
inline constexpr NameSpec kNames[] = {
    { "g_AmbientParamArray", NameKind::ConstantBuffer }, // the deferred composite's ambient probe table
    { "g_SamplerLightDiffuse", NameKind::Texture },      // the composite's diffuse lighting: the ambient knob's second anchor
    { "g_WetnessParameter", NameKind::ConstantBuffer },  // the engine's wetness, the ground-wetness patch's target
    { "cCommonTexParam", NameKind::ConstantBuffer },     // the tone map's parameters (its .y is the exposure multiplier)
    { "cBrightPassParam", NameKind::ConstantBuffer },    // the bright pass's threshold and knee
    { "g_SamplerOcclusion", NameKind::Texture },         // the engine's screen-space occlusion, the ambient occlusion pass's anchor
    { "g_LightParam", NameKind::ConstantBuffer },        // a light draw's own parameters: position, direction, colour (the light shafts)
    { "g_SamplerGBuffer1", NameKind::Texture },          // the engine's G-buffer targets, the puddles pass's readers (1: material pack)
    { "g_SamplerGBuffer2", NameKind::Texture },          // 2 and 3 are the two beside it
    { "g_SamplerGBuffer3", NameKind::Texture },
    { "g_SamplerGBuffer", NameKind::Texture },           // G-buffer 0: the primary world-space normal (rgb * 2 - 1, the material id in .a)
    { "g_SamplerReflectionMap", NameKind::Texture },     // the engine's planar reflection map, the screen-space reflections pass's anchor
    { "cSunParam", NameKind::ConstantBuffer },           // the sky's own visual sun: float4[0] = (aspect, 1.0, sunUvX, sunUvY), the disk's screen position
};
inline constexpr uint32_t kNameCount = uint32_t(sizeof(kNames) / sizeof(kNames[0]));

// The index of a name in kNames, or -1. The comparison is made on the name after the effect compiler's `.T`/`.S` suffix has been stripped
// (diag::ResourceBaseName), because that is how the engine's bytecode reflects a texture and its sampler.
constexpr int32_t IndexOfName(std::string_view name) noexcept
{
    for (uint32_t i = 0; i < kNameCount; ++i)
        if (name == kNames[i].name)
            return int32_t(i);
    return -1;
}

// What one pixel shader declares: the slot of every name of kNames it has, and how many pixels its output signature declares (0 when the
// bytecode has no output signature this reader understands). `found` carries one bit per name, so `ok` says "this shader belongs to the
// index" and the per-name test says which knob can use it.
struct PassLayout
{
    bool ok = false;      // at least one of the names was found
    uint32_t found = 0;   // bit i = kNames[i] was found, at slots[i]
    uint32_t slots[kNameCount] {};
    uint32_t outputs = 0; // the pixel shader's declared output elements, 0 = not declared

    constexpr bool Has(uint32_t index) const noexcept { return index < kNameCount && (found & (1u << index)) != 0; }
    constexpr uint32_t SlotOf(uint32_t index) const noexcept { return index < kNameCount ? slots[index] : 0; }
};

// The pixel-shader constant-buffer slots D3D11 exposes (14, b0 to b13). A shader that declares its ambient table past the last one cannot be
// bound by the engine, so it is not the draw this knob patches.
inline constexpr uint32_t kConstantBufferSlots = 14;

// The composite this knob patches declares BOTH anchors: the ambient probe table and the diffuse light it samples. A draw whose shader
// declares only one of them (the wetness buffer, the tone map's parameters, a bright pass) is another pass of the frame, so it is passed on
// before anything is measured, queried or counted - a frame makes far more of those draws than composites, and counting them would stand the
// module down before the first composite of the frame reached it.
constexpr bool IsAmbientDraw(const PassLayout& layout) noexcept
{
    return layout.Has(0) && layout.Has(1) && layout.SlotOf(0) < kConstantBufferSlots;
}

// The wetness draw declares `g_WetnessParameter`: every material pass of a frame binds it (about 1,500 draws), plus the shadow atlas's
// depth-only draws. It has no second anchor: the knob acts on the float alone, and the engine's own rain-exposure mask still decides where the
// ground is sheltered.
constexpr bool IsWetnessDraw(const PassLayout& layout) noexcept
{
    return layout.Has(2) && layout.SlotOf(2) < kConstantBufferSlots;
}

// A reader of the engine's screen-space occlusion texture (the ambient occlusion pass). Every light draw, the composite and the forward draws
// bind it; the pass replaces it for every one of them, and the draw's own reflection says at which slot. D3D11 exposes 128 shader-resource
// slots, so a shader that declares the name past the last one cannot be bound with it there and is not a reader this pass serves.
inline constexpr uint32_t kShaderResourceSlots = 128;

constexpr bool IsOcclusionDraw(const PassLayout& layout) noexcept
{
    return layout.Has(5) && layout.SlotOf(5) < kShaderResourceSlots;
}

// A light draw declares `g_LightParam`, the per-light parameters every light pass of the frame binds. The light shafts module reads the sun's
// direction and colour from it at its own draw; the draw's own shape decides whether that light is the sun (ShaftsRules.h). The name sits after
// the occlusion texture the pass above was added with, and before the G-buffer readers below.
constexpr bool IsLightDraw(const PassLayout& layout) noexcept
{
    return layout.Has(6) && layout.SlotOf(6) < kConstantBufferSlots;
}

// The sky draws its own sun sprite with `cSunParam`, whose first row is the disk's screen UV. That draw says where the sun a player sees is;
// the shadow-casting light draws say where the engine's directional light points, and it is clamped to a minimum elevation so its cascades do not
// stretch to infinity, so at dawn and dusk the two are not the same position. The draw declares no `g_LightParam`, which is what keeps it apart
// from the light pass (its anchors say so as well).
constexpr bool IsSunParamDraw(const PassLayout& layout) noexcept
{
    return layout.Has(12) && layout.SlotOf(12) < kConstantBufferSlots;
}

// The tone map declares `cCommonTexParam` (its .y is the exposure multiplier). The draw's own shape decides whether it really is the tone map
// (ShaftsRules.h: one FP16 target at the scene's size, a t0 that is the scene the composite wrote).
constexpr bool IsToneMapDraw(const PassLayout& layout) noexcept
{
    return layout.Has(3) && layout.SlotOf(3) < kConstantBufferSlots;
}

// A sampler of the engine's planar reflection map (the screen-space reflections pass). Its own shader decides whether it is a water surface this
// pass answers for, so the pass's bit says only that the map is read: a reader it does not serve is passed on without a refusal. It is the last
// row of the table, after the light shafts' and the puddles' anchors.
constexpr bool IsReflectionDraw(const PassLayout& layout) noexcept
{
    return layout.Has(11) && layout.SlotOf(11) < kShaderResourceSlots;
}

// The one value the pixel-shader seam stores for the bound shader, so a draw reads it once and the frame's other binds cost one store. It is
// 64 bits because every knob of this module speaks through it, and the map is fixed so that no two knobs can ever own the same bit:
//
//   bits  0..7    the ambient knob's slot (the deferred composite's g_AmbientParamArray)
//   bits  8..15   the wetness knob's slot (g_WetnessParameter)
//   bits 16..29   the shipped word's free bits, never handed to a knob
//   bit  30       the wetness knob's flag: the shader declares g_WetnessParameter
//   bit  31       the ambient knob's flag: the shader declares both anchors of the composite
//   bit  32       ambient occlusion: the shader reads the engine's occlusion texture
//   bits 33..40   ambient occlusion: that texture's slot
//   bit  41       light shafts: the light pass
//   bit  42       light shafts: the composite
//   bit  43       light shafts: the tone map
//   bits 44..51   light shafts: the slot field those passes read
//   bit  52       the puddles pass: the shader reads one of the engine's G-buffer targets (its own stamp carries the exact slots)
//   bits 53..60   reserved, free: the G-buffer reader's slots travel in the pass's own stamp on the shader, not in this word
//   bit  61       screen-space reflections: the shader reads the engine's planar reflection map
//   bit  62       light shafts: the shader declares the visual sun's parameters (cSunParam)
//   bit  63       free
//
// The two shipped knobs keep the bits and the slot fields they have always had, in the low word, so every value, log line, ini key and check
// that names them keeps its meaning. 0 is "not a draw this module patches", which is every other bind.
using Anchors = uint64_t;

inline constexpr Anchors kAmbientDrawBit = 1ull << 31;
inline constexpr Anchors kWetnessDrawBit = 1ull << 30;
inline constexpr Anchors kAmbientSlotMask = 0xFFull;
inline constexpr Anchors kWetnessSlotMask = 0xFFull << 8;
inline constexpr uint32_t kWetnessSlotShift = 8;

// Ambient occlusion: the shader samples the frame it just wrote, and the slot field says where. DrawAnchors below sets both fields for a reader
// shape, and the draw path reads them through IsOcclusionAnchors and OcclusionSlotOf.
inline constexpr Anchors kOcclusionReaderBit = 1ull << 32;
inline constexpr uint32_t kOcclusionReaderSlotShift = 33;
inline constexpr Anchors kOcclusionReaderSlotMask = 0xFFull << 33;

// Light shafts: one effect spread over three passes of the frame, all three named here so the passes cannot be confused with each other.
inline constexpr Anchors kShaftsLightBit = 1ull << 41;
inline constexpr Anchors kShaftsCompositeBit = 1ull << 42;
inline constexpr Anchors kShaftsToneMapBit = 1ull << 43;
inline constexpr uint32_t kShaftsSlotShift = 44;
inline constexpr Anchors kShaftsSlotMask = 0xFFull << 44;

// The puddles pass: its reader bit, set on a shader that reads one of the engine's G-buffer targets. The slot field is kept free - a reader
// declares up to four of those targets, which one word cannot carry - so the pass stamps the slots it needs on the shader object and this bit
// only says the draw is worth looking at. Nothing sets the slot field.
inline constexpr Anchors kGlossReaderBit = 1ull << 52;
inline constexpr uint32_t kGlossReaderSlotShift = 53;
inline constexpr Anchors kGlossReaderSlotMask = 0xFFull << 53;

// Screen-space reflections: the shader samples the engine's planar reflection map. The pass keeps the map's slot on the shader object itself (its
// own stamp, the ambient occlusion pass's pattern) instead of a slot field here, so one bit is the whole of its field in this word.
inline constexpr Anchors kReflectionReaderBit = 1ull << 61;

// Light shafts: the sky's own visual sun. `cSunParam` is not a light draw (it declares no `g_LightParam`), so its buffer slot travels in the
// light pass's slot field and this bit alone says which of the two draws the word came from. A build that ever declared both names would be the
// light pass, and DrawAnchors below answers that way.
inline constexpr Anchors kShaftsSunParamBit = 1ull << 62;

// The shipped word's bits 16..29 are never handed to a knob: they stay reserved so a knob written against an older map (one that took bit 29
// and the slots at 16..23) fails this check at compile time instead of colliding with the shipped knobs at run time.
inline constexpr Anchors kAnchorsReservedLowBits = 0x3FFF0000ull;

// Every field of the map, flag bits and slot fields together: one entry per owned field. A knob's bits belong in this list, so the check
// below sees them: a knob that keeps its own private constants is not covered.
inline constexpr Anchors kAnchorsFeatureMasks[] = {
    kAnchorsReservedLowBits,
    kAmbientSlotMask,
    kWetnessSlotMask,
    kWetnessDrawBit,
    kAmbientDrawBit,
    kOcclusionReaderBit,
    kOcclusionReaderSlotMask,
    kShaftsLightBit,
    kShaftsCompositeBit,
    kShaftsToneMapBit,
    kShaftsSunParamBit,
    kShaftsSlotMask,
    kGlossReaderBit,
    kGlossReaderSlotMask,
    kReflectionReaderBit,
};

// The map's rule, checked at compile time below: no two fields share a bit, and every slot field is exactly 8 contiguous bits, so a slot of 256
// or more cannot be packed - D3D11 exposes 14 constant-buffer slots and 128 texture slots, so 8 bits is the shipped width for both kinds.
// A shared bit is not a refusal: it would let one knob's draw be classified as another knob's, with that other knob's slot.
constexpr bool AnchorsMapIsSound() noexcept
{
    constexpr uint32_t count = uint32_t(sizeof(kAnchorsFeatureMasks) / sizeof(kAnchorsFeatureMasks[0]));
    for (uint32_t i = 0; i < count; ++i)
        for (uint32_t j = i + 1; j < count; ++j)
            if ((kAnchorsFeatureMasks[i] & kAnchorsFeatureMasks[j]) != 0)
                return false;
    for (const Anchors field : { kAmbientSlotMask, kWetnessSlotMask, kOcclusionReaderSlotMask, kShaftsSlotMask, kGlossReaderSlotMask })
        if (std::popcount(field) != 8 || (field >> std::countr_zero(field)) != 0xFFull)
            return false;
    return true;
}

static_assert(AnchorsMapIsSound(), "the anchors map gives every knob its own bits: every flag bit and every slot field is disjoint from all the "
                                   "others, and a slot field is 8 bits wide");

constexpr Anchors DrawAnchors(const PassLayout& layout) noexcept
{
    Anchors anchors = 0;
    if (IsAmbientDraw(layout))
        anchors |= kAmbientDrawBit | (Anchors(layout.SlotOf(0)) & kAmbientSlotMask);
    if (IsWetnessDraw(layout))
        anchors |= kWetnessDrawBit | ((Anchors(layout.SlotOf(2)) << kWetnessSlotShift) & kWetnessSlotMask);
    if (IsOcclusionDraw(layout))
        anchors |= kOcclusionReaderBit | ((Anchors(layout.SlotOf(5)) << kOcclusionReaderSlotShift) & kOcclusionReaderSlotMask);
    // The light shafts' three recognitions, each with its own bit: the light pass carries the slot its g_LightParam sits at, the deferred
    // composite is the draw the shafts are added before the tone map, and the tone map is the draw the pass runs at. A shader is stored by the
    // index only while its word carries a bit, so a recognition without one would lose its shaders.
    if (IsLightDraw(layout))
        anchors |= kShaftsLightBit | ((Anchors(layout.SlotOf(6)) << kShaftsSlotShift) & kShaftsSlotMask);
    if (IsAmbientDraw(layout))
        anchors |= kShaftsCompositeBit;
    if (IsToneMapDraw(layout))
        anchors |= kShaftsToneMapBit;
    // The sky's visual sun: its buffer slot is packed into the light pass's field, which no light draw can also claim (a layout that declares both
    // is answered as the light draw, because the sun it draws with is the one the shadow cascade is built from).
    if (IsSunParamDraw(layout) && !IsLightDraw(layout))
        anchors |= kShaftsSunParamBit | ((Anchors(layout.SlotOf(12)) << kShaftsSlotShift) & kShaftsSlotMask);
    // The puddles pass: a shader that declares any of the four G-buffer targets is a reader of one the pass may shape (the pass reads the exact
    // slots from its own stamp, so the word carries the bit alone).
    if (layout.Has(7) || layout.Has(8) || layout.Has(9) || layout.Has(10))
        anchors |= kGlossReaderBit;
    if (IsReflectionDraw(layout))
        anchors |= kReflectionReaderBit;
    return anchors;
}

constexpr bool IsAmbientAnchors(Anchors anchors) noexcept { return (anchors & kAmbientDrawBit) != 0; }

constexpr bool IsWetnessAnchors(Anchors anchors) noexcept { return (anchors & kWetnessDrawBit) != 0; }

constexpr bool IsOcclusionAnchors(Anchors anchors) noexcept { return (anchors & kOcclusionReaderBit) != 0; }

constexpr bool IsShaftsLightAnchors(Anchors anchors) noexcept { return (anchors & kShaftsLightBit) != 0; }

constexpr bool IsShaftsCompositeAnchors(Anchors anchors) noexcept { return (anchors & kShaftsCompositeBit) != 0; }

constexpr bool IsShaftsToneMapAnchors(Anchors anchors) noexcept { return (anchors & kShaftsToneMapBit) != 0; }

constexpr bool IsShaftsSunParamAnchors(Anchors anchors) noexcept { return (anchors & kShaftsSunParamBit) != 0; }

constexpr bool IsReflectionAnchors(Anchors anchors) noexcept { return (anchors & kReflectionReaderBit) != 0; }

constexpr uint32_t AmbientSlotOf(Anchors anchors) noexcept { return uint32_t(anchors & kAmbientSlotMask); }

constexpr uint32_t WetnessSlotOf(Anchors anchors) noexcept { return uint32_t((anchors & kWetnessSlotMask) >> kWetnessSlotShift); }

constexpr uint32_t OcclusionSlotOf(Anchors anchors) noexcept
{
    return uint32_t((anchors & kOcclusionReaderSlotMask) >> kOcclusionReaderSlotShift);
}

constexpr uint32_t ShaftsSlotOf(Anchors anchors) noexcept { return uint32_t((anchors & kShaftsSlotMask) >> kShaftsSlotShift); }

// ---- the shader index's table --------------------------------------------------------------------------------------------

// The index answers, for a bound pixel shader, the packed anchors above. It is filled at shader creation and read on every pixel-shader bind,
// so its read takes no lock: one hash, a bounded linear probe, a pointer compare and, on a hit, the stamp the shader object itself carries.
//
// The table is open addressing with linear probing over a power-of-two slot count. A shader never evicts another one, which is the difference
// from a direct-mapped table: that one lost an entry whenever two shader addresses hashed to the same slot, and about 1,600 shaders of a live
// session declare the wetness buffer alone, so a share of every frame's material draws was passed on unclassified.
//
// The slot count is twice the cap on indexed shaders. Linear probing at the cap in half these slots (65,536 for 40,960, a load of 0.61) leaves
// about 1.5% of the inserts without a free slot inside a bounded probe - primary clustering, and a longer probe does not rescue it (about 88 of
// 40,960 still miss at 16). Here the load at the cap is 0.31: a 16-probe insert misses for about one shader in 40,960 on a dense address pattern
// (the table test measures one), and that miss is the module's own fail-open - the full-table line, once, and that shader's draws passed on -
// rather than a shader silently unclassified. A slot is a pointer, the anchors and the layout's id - 24 bytes - so the table is 3 MiB, allocated
// on first use, not part of the module's image.
//
// The cap has to be above the number of patchable bytecodes a long session creates: the game creates about 33,000 pixel shaders in a session and
// a bytecode created after the cap was reached used to be dropped without a line. 40,960 leaves room above that count, and `IndexHasRoom` is the
// rule at the insert so a session that does reach it reads a refusal line and a count instead of a silent pass-on.
inline constexpr uint32_t kMaxIndexedShaders = 40960; // the bytecodes the index keeps; the game creates about 33k pixel shaders a session
inline constexpr uint32_t kSeenSlots = 131072;        // 2x the cap: a linear probe of 16 finds a slot for every shader at the cap
inline constexpr uint32_t kSeenProbeLength = 16;      // the most slots a bind walks; a shader past it is not in the table (fail open)

static_assert((kSeenSlots & (kSeenSlots - 1)) == 0, "the slot count is a power of two, so the hash's low bits are the slot");
static_assert(kSeenSlots >= 2 * kMaxIndexedShaders, "the slot count leaves the table at most half full at the index's cap");

// Whether a bytecode that is new to the index may be stored: the index keeps its first kMaxIndexedShaders bytecodes and refuses every later one,
// so the answer is the one rule the insert reads (and the one a test can pin) instead of an inline comparison.
constexpr bool IndexHasRoom(size_t kept) noexcept { return kept < kMaxIndexedShaders; }

// The slot an address belongs at. The addresses come from the D3D runtime, 16-byte aligned, and the objects of one heap share most of their
// high bits, so the splitmix64 finaliser is applied to the aligned address: every bit of the answer then depends on every bit of the address,
// where the low bits of a plain multiply do not. Pure and constexpr, so a test can check the whole sequence.
constexpr uint32_t SeenSlotOf(uintptr_t shader, uint32_t slotMask) noexcept
{
    uint64_t x = uint64_t(shader >> 4);
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return uint32_t(x) & slotMask;
}

// The slot the linear probe looks at after `probe` steps from the address's home slot. Insert and find walk this one sequence, so stopping a
// find at the first empty slot is sound: nothing is ever removed, so an address stored at or after an empty slot cannot exist.
constexpr uint32_t SeenProbeSlot(uint32_t home, uint32_t probe, uint32_t slotMask) noexcept
{
    return (home + probe) & slotMask;
}

// What an insert came to. `NoAnchors` is the rule the index keeps: a shader that names one of kNames but whose layout carries no anchor bit
// for any knob is not a draw this module can patch, so it is not stored and costs no slot. `Full` is the fail-open of a full table: the shader
// is not stored, so its draws answer "not mine" instead of evicting a shader the knobs patch.
enum class SeenInsert : uint8_t
{
    Inserted = 0,
    NoAnchors,
    Full,
    Invalid,
};

// One slot: the shader object's address, the anchors of its layout (all 64 bits of the map above), and the layout's id. The id is the value
// SetPrivateData stored on the shader object, so a recycled address is caught by the object's own answer instead of trusted.
//
// The fields are written in one order and read in the reverse one, because a bind reads with no lock while shader creation replaces a record:
//
//   write: anchors (relaxed), then the id (release), then - for a slot that was empty - the pointer (release)
//   read:  the pointer (acquire), then the id (acquire), then the anchors (relaxed)
//
// A reader can then see at worst the previous id with the new anchors, which the stamp check in Find refuses, so the draw is left alone. It can
// never see a new id with the previous anchors, which is the pair that would classify a draw by another layout's slots: the release store of
// the id carries the anchors before it, and the release store of the pointer carries both.
struct SeenShader
{
    std::atomic<uintptr_t> shader { 0 }; // the address the record belongs to, published last, 0 = this slot is empty
    std::atomic<Anchors> anchors { 0 };  // the layout's anchors, written first
    std::atomic<uint32_t> id { 0 };      // the layout's stamp on the object, written last but for the pointer
};

// The slots, the probe and the stamp check, with the stamp supplied by the caller (the product asks the shader object; a test answers from a
// map). Writes happen at shader creation only, which the module serialises with its one lock; a bind reads with no lock at all.
template <uint32_t Slots>
class SeenTable
{
    static_assert(Slots >= 2 && (Slots & (Slots - 1)) == 0, "the slot count is a power of two");

  public:
    SeenTable() : _slots(std::make_unique<SeenShader[]>(Slots)) {}

    SeenTable(const SeenTable&) = delete;
    SeenTable& operator=(const SeenTable&) = delete;

    SeenInsert Insert(uintptr_t shader, uint32_t id, Anchors anchors) noexcept
    {
        if (shader == 0 || id == 0)
            return SeenInsert::Invalid;
        if (anchors == 0)
            return SeenInsert::NoAnchors; // names present but no knob's shape: the table holds candidates only
        const uint32_t home = SeenSlotOf(shader, Slots - 1);
        for (uint32_t probe = 0; probe < kSeenProbeLength; ++probe)
        {
            SeenShader& slot = _slots[SeenProbeSlot(home, probe, Slots - 1)];
            const uintptr_t recorded = slot.shader.load(std::memory_order_acquire);
            if (recorded == shader)
            {
                // The same address again: a shader object created twice, or a recycled address whose new layout is indexed in its turn. The
                // record is replaced in place, so one address never takes two slots and a find keeps its stop-at-empty rule. The anchors go
                // first and the id second, so a bind that reads the previous id with the new anchors is refused by its stamp check.
                slot.anchors.store(anchors, std::memory_order_relaxed);
                slot.id.store(id, std::memory_order_release);
                return SeenInsert::Inserted;
            }
            if (recorded != 0)
                continue;
            slot.anchors.store(anchors, std::memory_order_relaxed);
            slot.id.store(id, std::memory_order_release);
            slot.shader.store(shader, std::memory_order_release);
            _count.fetch_add(1, std::memory_order_relaxed);
            return SeenInsert::Inserted;
        }
        return SeenInsert::Full; // the probe is bounded: the caller logs once for the session and the shader's draws fail open
    }

    // The anchors of a stored address, or 0 when the address is not stored, carries no stamp, or carries another layout's id. `stampedId`
    // answers the id the object itself holds (0 for no stamp), which is what makes a recycled address safe and what refuses the one pair a
    // racing replace can leave visible (the previous id with the new anchors).
    template <class Stamp>
    Anchors Find(uintptr_t shader, Stamp&& stampedId) const noexcept
    {
        if (shader == 0)
            return 0;
        const uint32_t home = SeenSlotOf(shader, Slots - 1);
        for (uint32_t probe = 0; probe < kSeenProbeLength; ++probe)
        {
            const SeenShader& slot = _slots[SeenProbeSlot(home, probe, Slots - 1)];
            const uintptr_t recorded = slot.shader.load(std::memory_order_acquire);
            if (recorded == 0)
                return 0; // an empty slot: this address was never stored at or after it
            if (recorded != shader)
                continue;
            const uint32_t id = slot.id.load(std::memory_order_acquire);
            if (id == 0 || stampedId(shader) != id)
                return 0; // a recycled address, or the previous id beside the new anchors: the object's own answer disagrees with the record
            return slot.anchors.load(std::memory_order_relaxed);
        }
        return 0; // the probe is bounded: a shader past it is not in the table, exactly as if the table were full
    }

    // How many addresses the table holds (the product logs nothing with it; the host tests read it to pin the not-inserted rules).
    uint32_t Count() const noexcept { return _count.load(std::memory_order_relaxed); }

  private:
    std::unique_ptr<SeenShader[]> _slots;
    std::atomic<uint32_t> _count { 0 };
};

// ---- the scale the GPU path applies --------------------------------------------------------------------------------------

// One entry of the compute shader's scale table: the float4s at `byteOffset + k * stride` for k < count are multiplied by `multiplier`, and
// every other float4 of the buffer is copied unchanged. The ambient knob multiplies `.x` only, so its multiplier is `{scale, 1, 1, 1}` and
// the bytes beside the knob (ssaoMask at the next float4's .y) come out of the pass exactly as they went in.
struct ScaleEntry
{
    uint32_t byteOffset = 0; // bytes from the buffer's start (a multiple of 16)
    uint32_t stride = 0;     // bytes between two addressed float4s (a multiple of 16)
    uint32_t count = 0;      // how many float4s are addressed
    float multiplier[4] { 1.0f, 1.0f, 1.0f, 1.0f };
};

inline constexpr uint32_t kMaxScaleEntries = 8;

// One entry of the compute shader's set table: the float4s at `byteOffset + k * stride` for k < count have the components a set bit of
// `mask` addresses REPLACED with `value`'s bits; every other component keeps the bits it was read with. The value is a bit pattern, not a
// float, so a replacement never passes through a conversion either (the wetness knob's `.y` is `1 - .x/2`, computed on the CPU and shipped as
// its exact bits).
struct SetEntry
{
    uint32_t byteOffset = 0;                          // bytes from the buffer's start (a multiple of 16)
    uint32_t stride = 0;                              // bytes between two addressed float4s (a multiple of 16)
    uint32_t count = 0;                               // how many float4s are addressed
    uint32_t value[4] { 0, 0, 0, 0 };                 // the bits a masked component is replaced with
    uint32_t mask = 0;                                // bit 0..3 = .x,.y,.z,.w: 1 replaces, 0 keeps the game's bits
};

inline constexpr uint32_t kMaxSetEntries = 8;

inline constexpr uint32_t kComponentX = 1u;
inline constexpr uint32_t kComponentY = 2u;
inline constexpr uint32_t kComponentZ = 4u;
inline constexpr uint32_t kComponentW = 8u;

// Whether an entry stays inside a buffer of `byteWidth`: every addressed float4 must be complete, so a wrong stride or count is refused
// before the dispatch instead of writing past the end of the compute shader's output buffer. The scale and the set tables address the same way.
constexpr bool AddressFits(uint32_t byteOffset, uint32_t stride, uint32_t count, uint32_t byteWidth) noexcept
{
    if (stride == 0 || stride % 16 != 0 || byteOffset % 16 != 0 || count == 0)
        return false;
    if (byteOffset >= byteWidth)
        return false;
    const uint64_t last = uint64_t(byteOffset) + uint64_t(count - 1) * stride;
    return last + 16 <= uint64_t(byteWidth);
}

constexpr bool EntryFits(const ScaleEntry& entry, uint32_t byteWidth) noexcept
{
    return AddressFits(entry.byteOffset, entry.stride, entry.count, byteWidth);
}

constexpr bool SetEntryFits(const SetEntry& entry, uint32_t byteWidth) noexcept
{
    return AddressFits(entry.byteOffset, entry.stride, entry.count, byteWidth);
}

// ---- the ambient knob ----------------------------------------------------------------------------------------------------

// The per-probe multiplier on an ambient probe's sky visibility. The table is a 64-byte header followed by 64 probes of 192 bytes, and
// `skyVisibilityScale` sits at probe offset 0xB0 (read from the shader's own reflection): 0x40 + 0xB0 = 240 bytes, then one per 192-byte
// probe. Scaling it is the engine's own operation, so 1.0 is exactly the picture the artists baked.
inline constexpr uint32_t kAmbientProbes = 64;
inline constexpr uint32_t kAmbientProbeBytes = 192;
inline constexpr uint32_t kAmbientScaleOffset = 240;

// The per-probe field the composite multiplies the ambient with for every probe it uses, with no branch in between: the float4 at probe
// offset 0x30 holds `color.xyz` and `shRecipScale` in `.w`, and the shader reads `.w` on the ambient and again on the indirect specular. Its
// address is 0x40 + 0x30 = 112 bytes, then one per 192-byte probe, and only `.w` is scaled: `.xyz` keeps its multiplier at 1, so the probe's
// colour is untouched. 0xB0 alone is a knob with holes in it - the composite reads it only for probes whose sky visibility is low, and not at
// all on a table that holds one probe - while this field reaches every probe the shader ends up using.
inline constexpr uint32_t kAmbientSkyRecipOffset = 112;

// The knob writes both entries or neither: one on the probe's sky visibility, one on its SH reciprocal scale.
inline constexpr uint32_t kAmbientEntryCount = 2;

// The two sizes the engine binds the ambient table at: the shader declares 12,352 bytes (64 + 64 * 192) and the composite binds a
// 16,384-byte buffer, so both are the same table seen from the two sides. Anything else is another draw's buffer under that name.
inline constexpr uint32_t kAmbientDeclaredBytes = 12352;
inline constexpr uint32_t kAmbientBoundBytes = 16384;

constexpr ScaleEntry AmbientSkyVisibilityEntry(float scale) noexcept
{
    ScaleEntry entry;
    entry.byteOffset = kAmbientScaleOffset;
    entry.stride = kAmbientProbeBytes;
    entry.count = kAmbientProbes;
    entry.multiplier[0] = scale;
    return entry;
}

// The same scale on the field the composite applies to every probe: the probe's SH reciprocal scale. The two entries address float4s of the
// same probes (240 + 192 k and 112 + 192 k, never the same float4), so one dispatch scales both.
constexpr ScaleEntry AmbientSkyRecipEntry(float scale) noexcept
{
    ScaleEntry entry;
    entry.byteOffset = kAmbientSkyRecipOffset;
    entry.stride = kAmbientProbeBytes;
    entry.count = kAmbientProbes;
    entry.multiplier[3] = scale;
    return entry;
}

constexpr bool IsAmbientBufferSize(uint32_t byteWidth) noexcept
{
    return byteWidth == kAmbientBoundBytes || byteWidth == kAmbientDeclaredBytes;
}

// The knob's range. The identity test carries an epsilon because the value comes from the ini and a slider: a thousandth away from 1.0 is
// not a look anyone can see, and running the pass for it would cost a copy and a dispatch to change nothing. A NaN reads as identity, so a
// broken value never turns the pass on.
inline constexpr float kMinAmbientScale = 0.0f;
inline constexpr float kMaxAmbientScale = 2.0f;
inline constexpr float kAmbientScaleDefault = 1.0f;
inline constexpr float kIdentityEpsilon = 0.001f;

constexpr float ClampAmbientScale(float value) noexcept
{
    if (value != value)
        return kAmbientScaleDefault;
    if (value < kMinAmbientScale)
        return kMinAmbientScale;
    return value > kMaxAmbientScale ? kMaxAmbientScale : value;
}

constexpr bool IsIdentityScale(float value) noexcept
{
    return !(value > 1.0f + kIdentityEpsilon) && !(value < 1.0f - kIdentityEpsilon);
}

// ---- the wetness knob ----------------------------------------------------------------------------------------------------

// The engine hands every material draw one 32-byte constant buffer, `g_WetnessParameter`: 8 floats, of which the first is the wetness amount and
// the second is the dryness blend `1 - f0/2`. Both ramp with the weather; the rest are angles and tuning constants the knob leaves alone. A draw
// binds it at the slot its shader declares (4, 5 and 1 were measured), so the slot always comes from the reflection.
inline constexpr uint32_t kWetnessBytes = 32;

// The knob's range. Below zero is "off": -1.0 is the default and means "the engine's own value", and nothing in the dry half of the range
// forces anything. 0.0 forces dry ground, 1.0 full wetness, whatever the weather is.
inline constexpr float kMinWetnessOverride = -1.0f;
inline constexpr float kMaxWetnessOverride = 1.0f;
inline constexpr float kWetnessOverrideDefault = -1.0f;

constexpr float ClampWetnessOverride(float value) noexcept
{
    if (value != value)
        return kWetnessOverrideDefault;
    if (value < kMinWetnessOverride)
        return kMinWetnessOverride;
    return value > kMaxWetnessOverride ? kMaxWetnessOverride : value;
}

// The knob is off unless it asks for a wetness amount. Written so a NaN reads as off, like the ambient knob's identity test.
constexpr bool IsWetnessOff(float value) noexcept { return !(value >= 0.0f); }

constexpr float WetnessAmount(float value) noexcept
{
    if (IsWetnessOff(value))
        return 0.0f;
    return value > 1.0f ? 1.0f : value;
}

// The set entry the wetness knob applies: `.x` becomes the forced amount and `.y` the engine's own dryness blend for it. `.zw` and the second
// float4 keep the bits the game wrote, which is why the mask addresses only the first two components.
constexpr SetEntry WetnessEntry(float wetness) noexcept
{
    const float amount = WetnessAmount(wetness);
    SetEntry entry;
    entry.byteOffset = 0;
    entry.stride = 16; // one float4, addressed once
    entry.count = 1;
    entry.value[0] = std::bit_cast<uint32_t>(amount);
    entry.value[1] = std::bit_cast<uint32_t>(1.0f - amount * 0.5f);
    entry.mask = kComponentX | kComponentY;
    return entry;
}

// The parameters one session runs with, from the keys.
struct Params
{
    float ambientScale = kAmbientScaleDefault;
    float wetnessOverride = kWetnessOverrideDefault;
};

constexpr Params BuildParams(float ambientScale, float wetnessOverride = kWetnessOverrideDefault) noexcept
{
    Params params;
    params.ambientScale = ClampAmbientScale(ambientScale);
    params.wetnessOverride = ClampWetnessOverride(wetnessOverride);
    return params;
}

// The feature-bit rule: the draw path is armed only while at least one knob is off the engine's own value, so a master switch left on with both
// knobs at vanilla costs one relaxed load per draw and nothing else.
constexpr bool AnyKnobOn(const Params& params) noexcept
{
    return !IsIdentityScale(params.ambientScale) || !IsWetnessOff(params.wetnessOverride);
}

// The scale table of the ambient knob. Returns how many entries were written (0 when the knob is at 1.0, which is "do not run", and 0 for a
// caller with room for fewer than the two entries the knob needs, because half the table is a knob that dims one field and not the other).
constexpr uint32_t AmbientEntries(const Params& params, ScaleEntry* out, uint32_t capacity) noexcept
{
    if (out == nullptr || capacity < kAmbientEntryCount || IsIdentityScale(params.ambientScale))
        return 0;
    out[0] = AmbientSkyVisibilityEntry(params.ambientScale);
    out[1] = AmbientSkyRecipEntry(params.ambientScale);
    const bool fits = EntryFits(out[0], kAmbientDeclaredBytes) && EntryFits(out[1], kAmbientDeclaredBytes);
    return fits ? kAmbientEntryCount : 0u;
}

// The set table of the wetness knob. Returns how many entries were written (0 when the knob is off, which is "do not run").
constexpr uint32_t WetnessEntries(const Params& params, SetEntry* out, uint32_t capacity) noexcept
{
    if (out == nullptr || capacity == 0 || IsWetnessOff(params.wetnessOverride))
        return 0;
    out[0] = WetnessEntry(params.wetnessOverride);
    return SetEntryFits(out[0], kWetnessBytes) ? 1u : 0u;
}

// ---- the drop list -------------------------------------------------------------------------------------------------------

// Why a draw was left alone. Every one of these leaves the game's draw exactly as it was, with the game's own buffer bound, and each is one
// line of the log the first time it happens.
enum class Decline : uint8_t
{
    None = 0,
    FeatureOff,        // the key is off, the knob is at 1.0, or the module switched itself off for the session
    NoBuffer,          // nothing is bound at the ambient slot
    BufferSize,        // the bound buffer is neither 16,384 nor 12,352 bytes
    RenderTargetCount, // not exactly one render target
    RenderTargetSize,  // the render target is smaller than a scene
    Window,            // the render window is empty or does not fit inside the render target
    Reentry,           // a draw the module's own work made
    CharaReplay,       // a character-preview replay of a game draw
    TooManyBuffers,    // one frame bound more wetness buffers than the per-frame cache patches
    PatchFailed,       // the owned buffers or the dispatch could not run
    Exception,         // a call threw
};

constexpr const char* DeclineName(Decline reason) noexcept
{
    switch (reason)
    {
    case Decline::None: return "run";
    case Decline::FeatureOff: return "the key is off, or the knob is at 1.0";
    case Decline::NoBuffer: return "nothing is bound at the ambient slot";
    case Decline::BufferSize: return "the ambient buffer is neither 16,384 nor 12,352 bytes";
    case Decline::RenderTargetCount: return "the draw does not write exactly one render target";
    case Decline::RenderTargetSize: return "the render target is not a scene";
    case Decline::Window: return "the render window does not fit inside the render target";
    case Decline::Reentry: return "the call came from this module's own work";
    case Decline::CharaReplay: return "the draw is a character-preview replay";
    case Decline::TooManyBuffers: return "the frame bound more wetness buffers than the per-frame cache patches";
    case Decline::PatchFailed: return "the owned buffers or the compute scale could not run";
    case Decline::Exception: return "a call threw";
    }
    return "unknown";
}

// What the draw-path check measured before it decided. The two anchors are decided when the shader is bound (IsAmbientDraw, so a draw reads
// one integer for them); the rule is deliberately small: an ambient buffer of an accepted size, exactly one render target at least a scene in
// size, and a render window that fits inside it.
struct DrawFacts
{
    uint32_t boundBytes = 0;
    uint32_t renderTargets = 0;
    uint32_t targetWidth = 0;
    uint32_t targetHeight = 0;
    float window[4] { 0.0f, 0.0f, 0.0f, 0.0f }; // x, y, width, height of the viewport the draw rasterises
    bool replay = false;                          // the draw is another module's replay of a game draw
};

// The smallest render target the knob patches: the composite is the size of the display allocation, and a target this small is a preview
// or an effect's scratch surface, never the frame's lighting.
inline constexpr uint32_t kMinSceneWidth = 320;
inline constexpr uint32_t kMinSceneHeight = 180;

constexpr Decline Classify(const DrawFacts& facts) noexcept
{
    if (facts.replay)
        return Decline::CharaReplay;
    if (facts.boundBytes == 0)
        return Decline::NoBuffer;
    if (!IsAmbientBufferSize(facts.boundBytes))
        return Decline::BufferSize;
    if (facts.renderTargets != 1)
        return Decline::RenderTargetCount;
    if (facts.targetWidth < kMinSceneWidth || facts.targetHeight < kMinSceneHeight)
        return Decline::RenderTargetSize;
    if (!(facts.window[2] > 0.0f) || !(facts.window[3] > 0.0f))
        return Decline::Window;
    if (!(facts.window[0] >= 0.0f) || !(facts.window[1] >= 0.0f))
        return Decline::Window;
    // Half a pixel of slack: a viewport origin of 0.5 is the engine's habit and lands here.
    if (facts.window[0] + facts.window[2] > float(facts.targetWidth) + 0.5f)
        return Decline::Window;
    if (facts.window[1] + facts.window[3] > float(facts.targetHeight) + 0.5f)
        return Decline::Window;
    return Decline::None;
}

// ---- the wetness draw -----------------------------------------------------------------------------------------------------

// What the wetness draw measured before it decided. There is deliberately no render-target count, size or viewport rule: the wetness buffer is
// on every material draw of the frame, and a material draw writes the five-target G-buffer while a shadow-atlas draw writes no colour at all.
// Both may carry the patched value (a wetness amount cannot change a draw that writes no colour), so `Classify`'s shape rules would refuse most
// of the frame's draws and stand the module down.
struct WetnessFacts
{
    uint32_t boundBytes = 0; // the buffer bound at the slot the shader declares
    bool replay = false;     // the draw is another module's replay of a game draw
};

// Whether this is a draw the wetness knob patches: the engine's own 32-byte buffer at the reflected slot, and a draw of the frame rather than a
// replay of one. Every other shape - nothing bound under that name, another buffer's size - is another draw of the frame and is passed on without
// a refusal counted: the name sits on about 1,500 shaders, and counting those draws would stand the module down in the first frame.
constexpr bool IsWetnessCandidate(const WetnessFacts& facts) noexcept
{
    return !facts.replay && facts.boundBytes == kWetnessBytes;
}

// ---- what a draw comes to, per knob --------------------------------------------------------------------------------------

// The two knobs of the card. Each keeps its own refusal streak (Session, below): a knob's successes must not hide the other knob's refusals, or a
// knob that is refused on every one of its own draws would never stand down while the other knob works on about 1,500 draws a frame.
enum class Knob : uint8_t
{
    Ambient = 0,
    Wetness = 1,
    LightShafts = 2,
};

// What a knob came to on one draw, decided without a device.
enum class KnobVerdict : uint8_t
{
    NotAsked = 0, // the knob is at the engine's own value, or this draw is not one of its own: neither a run nor a refusal for it
    Patch,        // the knob runs its patch on this draw
    Serve,        // the wetness knob's patched buffer answers this draw from the frame cache
    Refused,      // the knob asked for this draw and cannot patch it: a refusal of this knob, counted against its own streak
};

struct Verdict
{
    KnobVerdict kind = KnobVerdict::NotAsked;
    Decline reason = Decline::None; // the validation's reason when `kind` is Refused
};

// Which knobs ask for a draw, from the packed anchors and the params alone: the knob is off the engine's own value and the draw is its kind. A
// knob that does not ask is never refused by the draw, however many of them a frame makes - a composite draw with the ambient knob at 1.0 is not a
// wetness refusal, and a material draw with the wetness knob off is not an ambient one. The draw path measures a knob's facts only when it asked.
struct KnobIntent
{
    bool ambient = false;
    bool wetness = false;
};

constexpr KnobIntent IntentOf(const Params& params, Anchors anchors) noexcept
{
    KnobIntent intent;
    intent.ambient = IsAmbientAnchors(anchors) && !IsIdentityScale(params.ambientScale);
    intent.wetness = IsWetnessAnchors(anchors) && !IsWetnessOff(params.wetnessOverride);
    return intent;
}

// Whether one of the module's other features wants a draw of the seam. Each feature keeps its own switch and its own switch-off, so the answer is
// an OR of them and nothing else. One argument per feature: adding a feature is one more argument at the call site, and no branch can be written
// that forgets a feature already listed. A call with no feature at all answers false.
template <typename... Features>
constexpr bool AnyFeatureArmed(Features... featuresArmed) noexcept
{
    return (featuresArmed || ...);
}

// Whether the draw path has anything to do at all: one of this module's own knobs is still measuring, or one of the module's other features wants a
// draw. The sources are independent: a knob's switch-off never takes the other knob's work with it, and it must not take a feature's slot swap
// either - a session that stood the ambient knob down would otherwise clear the feature bit and switch a working feature off without saying so.
// The call site passes one arming per feature that shares this seam: the ambient occlusion pass, the light shafts, the puddles and the
// screen-space reflections.
template <typename... Features>
constexpr bool DrawPathWanted(bool sessionArmed, Features... featuresArmed) noexcept
{
    return sessionArmed || AnyFeatureArmed(featuresArmed...);
}

// The feature bit, which decides whether the draw seam is entered and whether the pixel-shader hook tracks a bind: on while this module's own
// knobs have something to patch (the master key is on and a knob is off the engine's own value), and on while any other feature wants a draw - a
// feature's own switch arms the seam on its own, whatever the master key says. Every place that writes the bit reads this one rule, so no branch
// can hold it under the master alone or clear it while a feature still wants a draw.
template <typename... Features>
constexpr bool FeatureBitWanted(bool masterOn, bool anyKnobOff, Features... featuresArmed) noexcept
{
    return (masterOn && anyKnobOff) || AnyFeatureArmed(featuresArmed...);
}

// A draw's intents once the master key and this module's own switch-off are taken into account: with the master off, or after a stand-down, the
// module patches nothing, whatever the anchors say, and the four texture passes (the ambient occlusion, the light shafts, the puddles and the
// screen-space reflections) keep their own arming through DrawPathWanted. The master is asked here and not left to the feature bit, because that
// bit also stays on while another feature wants a draw: without this line a player who turned the master off would still have the two knobs patch
// every draw the seam is entered for.
constexpr KnobIntent IntentOfSession(const Params& params, Anchors anchors, bool sessionArmed, bool masterOn) noexcept
{
    return sessionArmed && masterOn ? IntentOf(params, anchors) : KnobIntent {};
}

// The arming word the card and a capture print ("armed: yes"): the master key, a knob off the engine's own value, and a session that has not
// stood itself down. The feature bit is deliberately not part of it - it stays on while the occlusion pass, the light shafts, the puddles or the
// screen-space reflections want a draw - so a master switch turned off reads "off" even though the seam is still entered for them.
constexpr bool KnobsArmed(bool masterOn, const Params& params, bool sessionArmed) noexcept
{
    return masterOn && sessionArmed && AnyKnobOn(params);
}

// The ambient knob's verdict for one draw: the composite's own validation (the drop list above).
constexpr Verdict AmbientVerdict(const KnobIntent& intent, const DrawFacts& facts) noexcept
{
    if (!intent.ambient)
        return Verdict {};
    const Decline reason = Classify(facts);
    return reason == Decline::None ? Verdict { KnobVerdict::Patch, Decline::None } : Verdict { KnobVerdict::Refused, reason };
}

// The wetness knob's verdict for one draw: the engine's own 32-byte buffer at the reflected slot (and never a replay), then the frame cache - the
// patched buffer a frame already holds answers the draw, a miss patches it while the frame's budget lasts, and a spent budget is this knob's one
// refusal here. A draw whose buffer under that name is another size is not this knob's draw at all.
constexpr Verdict WetnessVerdict(const KnobIntent& intent, const WetnessFacts& facts, bool cached, bool mayPatch) noexcept
{
    if (!intent.wetness || !IsWetnessCandidate(facts))
        return Verdict {};
    if (cached)
        return Verdict { KnobVerdict::Serve, Decline::None };
    return mayPatch ? Verdict { KnobVerdict::Patch, Decline::None } : Verdict { KnobVerdict::Refused, Decline::TooManyBuffers };
}

// ---- the cost meter and the log budget ------------------------------------------------------------------------------------

// The on-cost of the draw path, measured on the game's own thread: after kCostEveryDraws draws, the next kCostSampleDraws have their lookup
// bracketed with the clock and one line reports the average. The clock is never read on the draws between samples.
inline constexpr uint32_t kCostEveryDraws = 262144;
inline constexpr uint32_t kCostSampleDraws = 64;
inline constexpr double kCostBudgetMilliseconds = 0.02;

struct CostMeter
{
    uint32_t skipped = 0;
    uint32_t sampled = 0;
    int64_t totalQpc = 0;
    bool logged = false;

    bool Sampling() const noexcept { return sampled < kCostSampleDraws && skipped >= kCostEveryDraws; }
    bool Done() const noexcept { return sampled >= kCostSampleDraws; }

    void Skip()
    {
        if (!Done())
            ++skipped;
    }

    void Sample(int64_t ticks)
    {
        ++sampled;
        if (ticks > 0)
            totalQpc += ticks;
    }

    double MillisecondsPerDraw(int64_t qpcFrequency) const noexcept
    {
        return sampled == 0 || qpcFrequency <= 0 ? 0.0 : double(totalQpc) * 1000.0 / double(qpcFrequency) / double(sampled);
    }
};

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

// The one line a change of state writes.
inline std::string FormatDecline(Decline reason)
{
    return std::format("Engine pass overrides: the draw was left alone ({})", DeclineName(reason));
}

// The on-cost line, in the numbers the design counts it with: the draw path's lookup at the game's ~9,250 draws a frame, and what the module
// did with the draws it saw. The per-draw figure is an upper bound: the sample's own two clock reads are inside the bracket. Each declined
// reason has its own line (the log gate above), so the total is what this line adds.
inline std::string FormatCost(const CostMeter& draws, uint64_t drawsSeen, uint32_t runs, uint32_t wetnessRuns, uint32_t wetnessSwaps,
                              uint32_t declined, int64_t qpcFrequency)
{
    const double perDraw = draws.MillisecondsPerDraw(qpcFrequency);
    return std::format("Engine pass overrides: on cost: {} draws seen, {} sampled, the draw's lookup {:.5f} ms per draw ({:.4f} ms at 9250 "
                       "draws a frame, the meter's two clock reads included; the design budget is {:.2f} ms); {} ambient patches, {} wetness "
                       "patches on {} binds, {} declined",
                       drawsSeen, draws.sampled, perDraw, perDraw * 9250.0, kCostBudgetMilliseconds, runs, wetnessRuns, wetnessSwaps, declined);
}

// The line of a draw that ran: what was scaled, on which buffer and at which size, and what the GPU work cost once it has been measured.
inline std::string FormatRun(const Params& params, uint32_t boundBytes, uint32_t targetWidth, uint32_t targetHeight, bool haveGpuMilliseconds,
                             double gpuMilliseconds)
{
    const std::string gpu = haveGpuMilliseconds ? std::format(", the patch {:.4f} ms on the GPU", gpuMilliseconds)
                                                : std::string(", its GPU time not measured yet");
    return std::format("Engine pass overrides: scaling the ambient table's sky visibility and SH reciprocal scale by {:.3f} over {} bytes for "
                       "the {}x{} composite{}",
                       double(params.ambientScale), boundBytes, targetWidth, targetHeight, gpu);
}

// The line of a wetness draw that ran: the value forced into the engine's wetness float, the dryness blend that goes with it, and the slot the
// shader declares it at.
inline std::string FormatWetnessRun(const Params& params, uint32_t slot, uint32_t boundBytes, bool haveGpuMilliseconds,
                                    double gpuMilliseconds)
{
    const float amount = WetnessAmount(params.wetnessOverride);
    const std::string gpu = haveGpuMilliseconds ? std::format(", the patch {:.4f} ms on the GPU", gpuMilliseconds)
                                                : std::string(", its GPU time not measured yet");
    return std::format("Engine pass overrides: forcing the engine's wetness to {:.3f} and its dryness blend to {:.3f} over {} bytes at slot "
                       "{}{}",
                       double(amount), double(1.0f - amount * 0.5f), boundBytes, slot, gpu);
}

// ---- the session state -------------------------------------------------------------------------------------------------

// After this many of one knob's own draws in a row are left alone, the module switches itself off for the session with one log line: a build
// whose composite no longer has the shape the knob expects then costs one line and no per-draw work instead of a check on every draw. Only a
// draw that knob asked for is counted at all (IntentOf), and each knob keeps its own streak: the 1,500 material draws a frame can never keep an
// ambient knob that is refused on every composite from standing down.
inline constexpr uint32_t kConsecutiveDeclineLimit = 64;

struct Session
{
    uint32_t ambientDeclines = 0;
    uint32_t wetnessDeclines = 0;
    uint32_t shaftsDeclines = 0;
    bool off = false;

    // One knob's declined draw. Returns true when this decline is the one that switched the module off.
    bool NoteDecline(Knob knob)
    {
        if (off)
            return false;
        uint32_t& streak = Streak(knob);
        if (++streak >= kConsecutiveDeclineLimit)
        {
            off = true;
            return true;
        }
        return false;
    }

    // One knob's patched draw: only that knob's streak starts over.
    void NoteRun(Knob knob) { Streak(knob) = 0; }

    bool Armed() const noexcept { return !off; }

  private:
    uint32_t& Streak(Knob knob) noexcept
    {
        if (knob == Knob::Wetness)
            return wetnessDeclines;
        return knob == Knob::LightShafts ? shaftsDeclines : ambientDeclines;
    }
};

// The one line a full table writes. The shader was not stored, so its draws are passed on as "not mine" - the same fail-open as a shader the
// index never saw, with the difference that this one is the module's own limit. It is written even while the key is off, because a lost index
// entry is what a later switch-on would run into; with candidates only it cannot fire in a session (about 1,600 of 65,536 slots are used).
inline std::string FormatIndexFull()
{
    return "Engine pass overrides: the shader index is full, so a shader created from here on is not indexed and its draws are left alone";
}

// The line a NEW bytecode writes when the index's cap refuses it. It is the same fail-open as the full table one cause further out - a bytecode
// created after the cap was reached is not stored, and its draws are passed on as "not mine" - so a session reads why a late water or composite
// shader is not patched instead of finding it passed on with no line at all.
inline std::string FormatIndexRefused()
{
    return std::format("Engine pass overrides: the shader index is at its cap ({} bytecodes), so a bytecode created from here on is not indexed "
                       "and its draws are left alone",
                       kMaxIndexedShaders);
}

inline std::string FormatSwitchedOff()
{
    return std::format("Engine pass overrides: {} of one knob's draws in a row were left alone, so the module is off for this session",
                       kConsecutiveDeclineLimit);
}

// ---- the per-frame cache of the wetness patch -----------------------------------------------------------------------------

// The game binds one g_WetnessParameter buffer for every material draw of a frame, and the same object in every frame, so the GPU patch runs once
// per (frame, pointer) and every later draw of that frame binds the cached result: what is left per draw is the slot swap and its restore.
//
// The frame token is the caller's (ext/present's output-frame counter, one relaxed load). `hasFrame` separates "no token read yet" from a token
// value of 0, so the first draw of a session starts a frame instead of looking like a change. The epoch is the caller's too: the knob and the
// master publish a new one, and the patch primitive counts its own re-initialisation (a device change, or a size cache eviction that frees the
// buffer the entry points at), so a knob that moved or a device that re-created the buffers clears the entry without any thread but the draw
// thread writing this struct. The entry also expires after kMaxServedDraws binds: a token that never advances then costs one patch every few
// thousand draws instead of a value that stays at the first frame's for the session.
inline constexpr uint32_t kMaxPatchesPerFrame = 4; // a second pointer in one frame is patched, a fifth is passed through
inline constexpr uint32_t kMaxServedDraws = 4096;  // the fail-safe for a frame token that does not move

struct FrameCache
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

    void Reset() noexcept { *this = FrameCache {}; }

    // The tokens the caller read. A new epoch clears the entry first (the knob moved, the master switched, the primitive re-created its buffers),
    // then a different frame token starts a frame: the entry and the patch budget are cleared.
    void Begin(uint64_t frameToken, uint32_t epochToken) noexcept
    {
        if (!hasEpoch || epochToken != epoch)
        {
            hasEpoch = true;
            epoch = epochToken;
            hasFrame = false;
        }
        if (hasFrame && frameToken == frame)
            return;
        hasFrame = true;
        frame = frameToken;
        ClearFrame();
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

    // One bind answered from the cache. A long run without a frame change expires the entry and the patch budget: the token is not advancing, so
    // the next draw runs the patch again rather than binding a result that may belong to an earlier frame.
    void Serve() noexcept
    {
        if (++served < kMaxServedDraws)
            return;
        ClearFrame();
    }
};
} // namespace ext::ffxiv::engine
