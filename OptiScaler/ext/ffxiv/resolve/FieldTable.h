#pragma once
// Every Rva, Offset, Guard and constant field of GameBuild (GameBuild.h) as a row: key, kind, byte offset inside GameBuild, size.
// The resolver fills and compares the table through these rows, so a Binding names a field by key ("charaView.accessorRva") and a P2
// writer never writes an accessor. FROZEN with the other resolver headers: a P2 writer must not edit it.
//
// Kinds (derived from the field name by the generator that wrote this table; tests/ext/ffxiv/resolve/resolve_field_table.cpp reads
// GameBuild.h and fails when a member is missing here or a row names no member, so a reshaped GameBuild cannot drift silently):
//   Rva       ...Rva (uint32)                       an address of the executable: zero in a resolved build unless a seam fills it
//   Guard     ...Instructions (uint8 array)         bytes read at the resolved address: zero unless a seam fills them
//   Offset    ...Offset / ...Offset0 / ...Offset1   a struct offset: keeps the table default ("not self-healing") unless a seam fills it
//   Constant  everything else (uint32)              sizes, counts, limits: never resolved
//   Pattern   the CharaView pattern and mask        the anchor itself, never resolved
//
// Header-only; the table is a constexpr array (no dynamic initialisation).

#include "ext/ffxiv/GameBuild.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace ext::ffxiv::resolve
{
enum class FieldKind : std::uint8_t
{
    Rva,
    Guard,
    Offset,
    Constant,
    Pattern,
};

struct FieldRow
{
    const char* group;        // "nativeQuality", "companion", "charaView", "jitter", "jitterSource"
    const char* name;         // the member name
    FieldKind kind;
    bool isArray;
    std::uint32_t offset;     // byte offset of the member inside GameBuild
    std::uint32_t size;       // 4 for a uint32, the array length in bytes for an array
};

#define FFXIV_U32(member, type, name, kind) \
    { #member, #name, FieldKind::kind, false, static_cast<std::uint32_t>(offsetof(GameBuild, member) + offsetof(type, name)), 4u }
#define FFXIV_ARRAY(member, type, name, kind) \
    { #member, #name, FieldKind::kind, true, static_cast<std::uint32_t>(offsetof(GameBuild, member) + offsetof(type, name)), \
      static_cast<std::uint32_t>(sizeof(type::name)) }

inline constexpr FieldRow kFieldTable[] = {
    FFXIV_U32(nativeQuality, NativeQualityBuild, settingsInstructionsRva, Rva),
    FFXIV_ARRAY(nativeQuality, NativeQualityBuild, settingsInstructions, Guard),
    FFXIV_U32(nativeQuality, NativeQualityBuild, querySizeRva, Rva),
    FFXIV_ARRAY(nativeQuality, NativeQualityBuild, querySizeInstructions, Guard),
    FFXIV_U32(nativeQuality, NativeQualityBuild, rendererUpdateRva, Rva),
    FFXIV_ARRAY(nativeQuality, NativeQualityBuild, rendererUpdateInstructions, Guard),
    FFXIV_U32(nativeQuality, NativeQualityBuild, resizeCallbacksRva, Rva),
    FFXIV_ARRAY(nativeQuality, NativeQualityBuild, resizeCallbacksInstructions, Guard),
    FFXIV_U32(nativeQuality, NativeQualityBuild, settingsPointerRva, Rva),
    FFXIV_U32(nativeQuality, NativeQualityBuild, managerPointerRva, Rva),
    FFXIV_U32(nativeQuality, NativeQualityBuild, devicePointerRva, Rva),
    FFXIV_U32(nativeQuality, NativeQualityBuild, settingsModeOffset, Offset),
    FFXIV_U32(nativeQuality, NativeQualityBuild, settingsQualityOffset, Offset),
    FFXIV_U32(nativeQuality, NativeQualityBuild, settingsDlaaOffset, Offset),
    FFXIV_U32(nativeQuality, NativeQualityBuild, settingsRendererOffset, Offset),
    FFXIV_U32(nativeQuality, NativeQualityBuild, settingsAdditionalOffset, Offset),
    FFXIV_U32(nativeQuality, NativeQualityBuild, settingsSelectorOffset, Offset),
    FFXIV_U32(nativeQuality, NativeQualityBuild, managerContextOffset, Offset),
    FFXIV_U32(nativeQuality, NativeQualityBuild, contextFlagsOffset, Offset),
    FFXIV_U32(nativeQuality, NativeQualityBuild, contextCacheOffset, Offset),
    FFXIV_U32(nativeQuality, NativeQualityBuild, deviceCallbacksOffset, Offset),
    FFXIV_U32(nativeQuality, NativeQualityBuild, rendererSceneSizeOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, batch16Rva, Rva),
    FFXIV_U32(companion, CompanionBuild, batch32Rva, Rva),
    FFXIV_ARRAY(companion, CompanionBuild, batchInstructions, Guard),
    FFXIV_U32(companion, CompanionBuild, enqueueRva, Rva),
    FFXIV_ARRAY(companion, CompanionBuild, enqueueInstructions, Guard),
    FFXIV_U32(companion, CompanionBuild, bindStateRva, Rva),
    FFXIV_ARRAY(companion, CompanionBuild, bindStateInstructions, Guard),
    FFXIV_U32(companion, CompanionBuild, nameplateModulePointerRva, Rva),
    FFXIV_U32(companion, CompanionBuild, layoutSlotTableRva, Rva),
    FFXIV_U32(companion, CompanionBuild, rendererBatchListOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, rendererBatchCountOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, rendererIndexBufferOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, rendererVertexAllocationOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, rendererVertexStrideOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, rendererVertexOffsetOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, rendererQueuesOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, rendererQueueCount, Constant),
    FFXIV_U32(companion, CompanionBuild, rendererQueueStride, Constant),
    FFXIV_U32(companion, CompanionBuild, moduleStatePointerOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, stateQueuePointerOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, resourceWrapperOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, nativeContextOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, drawStateFlagOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, drawStateCommandBackPointerOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, indexBufferCapacityOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, indexBufferFlagsOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, indexBufferCpuStartOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, indexBufferCpuEndOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, queueHeadOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, queueRemainingOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, queueBlocksOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, queueStridesOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, queueNextBlockOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, batchKindOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, batchWrapperOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, batchCountOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, batchVerticesOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, batchFormatOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, batchExtensionOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, entryPointerOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, baseVertexOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, startIndexOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, indexCountOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, drawCommandType, Constant),
    FFXIV_U32(companion, CompanionBuild, drawCommandTypeOffset, Offset),
    FFXIV_U32(companion, CompanionBuild, drawCommandSize, Constant),
    FFXIV_U32(companion, CompanionBuild, nativeBatchEntrySize, Constant),
    FFXIV_U32(companion, CompanionBuild, nativeBatchHeaderSize, Constant),
    FFXIV_U32(companion, CompanionBuild, rendererVertexSlotStride, Constant),
    FFXIV_U32(companion, CompanionBuild, layoutSlotEntrySize, Constant),
    FFXIV_ARRAY(charaView, CharaViewBuild, pattern, Pattern),
    FFXIV_ARRAY(charaView, CharaViewBuild, patternMask, Pattern),
    FFXIV_U32(charaView, CharaViewBuild, ripInstructionLength, Constant),
    FFXIV_U32(charaView, CharaViewBuild, accessorRva, Rva),
    FFXIV_U32(charaView, CharaViewBuild, managerPointerRva, Rva),
    FFXIV_U32(charaView, CharaViewBuild, managerBytes, Constant),
    FFXIV_U32(charaView, CharaViewBuild, targetsOffset, Offset),
    FFXIV_U32(charaView, CharaViewBuild, transparentOffset, Offset),
    FFXIV_U32(charaView, CharaViewBuild, depthOffset0, Offset),
    FFXIV_U32(charaView, CharaViewBuild, depthOffset1, Offset),
    FFXIV_U32(charaView, CharaViewBuild, entryCount, Constant),
    FFXIV_U32(charaView, CharaViewBuild, entryBytes, Constant),
    FFXIV_U32(charaView, CharaViewBuild, entryResourceOffset, Offset),
    FFXIV_U32(charaView, CharaViewBuild, entryWidthOffset, Offset),
    FFXIV_U32(charaView, CharaViewBuild, entryHeightOffset, Offset),
    FFXIV_U32(charaView, CharaViewBuild, maxExtent, Constant),
    FFXIV_U32(charaView, CharaViewBuild, maxTextBytes, Constant),
    FFXIV_U32(jitter, JitterBuild, cameraGlobalRva, Rva),
    FFXIV_U32(jitter, JitterBuild, cameraOffset, Offset),
    FFXIV_U32(jitter, JitterBuild, jitterPointerOffset, Offset),
    FFXIV_U32(jitter, JitterBuild, jitterXOffset, Offset),
    FFXIV_U32(jitter, JitterBuild, jitterYOffset, Offset),
    FFXIV_U32(jitter, JitterBuild, ndcXOffset, Offset),
    FFXIV_U32(jitter, JitterBuild, ndcYOffset, Offset),
    FFXIV_U32(jitter, JitterBuild, counterOffset, Offset),
    FFXIV_U32(jitter, JitterBuild, objectBytes, Constant),
    FFXIV_U32(jitterSource, JitterSourceBuild, generatorRva, Rva),
    FFXIV_ARRAY(jitterSource, JitterSourceBuild, generatorInstructions, Guard),
    FFXIV_U32(jitterSource, JitterSourceBuild, cameraGetterRva, Rva),
    FFXIV_ARRAY(jitterSource, JitterSourceBuild, cameraGetterInstructions, Guard),
    FFXIV_U32(jitterSource, JitterSourceBuild, producerCallRva, Rva),
    FFXIV_U32(jitterSource, JitterSourceBuild, producerReturnRva, Rva),
    FFXIV_ARRAY(jitterSource, JitterSourceBuild, producerCallInstructions, Guard),
    FFXIV_U32(jitterSource, JitterSourceBuild, cameraHeadRva, Rva),
    FFXIV_ARRAY(jitterSource, JitterSourceBuild, cameraHeadInstructions, Guard),
    FFXIV_U32(jitterSource, JitterSourceBuild, phasesRva, Rva),
    FFXIV_ARRAY(jitterSource, JitterSourceBuild, phasesInstructions, Guard),
    FFXIV_U32(jitterSource, JitterSourceBuild, strengthRva, Rva),
    FFXIV_U32(jitterSource, JitterSourceBuild, generatorCallReturnRva, Rva),
    FFXIV_ARRAY(jitterSource, JitterSourceBuild, strengthInstructions, Guard),
    FFXIV_U32(jitterSource, JitterSourceBuild, deviceSizeRva, Rva),
    FFXIV_ARRAY(jitterSource, JitterSourceBuild, deviceSizeInstructions, Guard),
    FFXIV_U32(jitterSource, JitterSourceBuild, settingsPointerRva, Rva),
    FFXIV_U32(jitterSource, JitterSourceBuild, rendererPointerRva, Rva),
    FFXIV_U32(jitterSource, JitterSourceBuild, devicePointerRva, Rva),
    FFXIV_U32(jitterSource, JitterSourceBuild, settingsScaleOffset, Offset),
    FFXIV_U32(jitterSource, JitterSourceBuild, settingsModeOffset, Offset),
    FFXIV_U32(jitterSource, JitterSourceBuild, settingsAmountOffset, Offset),
    FFXIV_U32(jitterSource, JitterSourceBuild, rendererSceneSizeOffset, Offset),
    FFXIV_U32(jitterSource, JitterSourceBuild, rendererExtentOffset, Offset),
    FFXIV_U32(jitterSource, JitterSourceBuild, deviceOutputSizeOffset, Offset),
    FFXIV_U32(jitterSource, JitterSourceBuild, strengthFloorBits, Constant),
    FFXIV_U32(jitterSource, JitterSourceBuild, ndcSignMaskBits, Constant),
};

#undef FFXIV_U32
#undef FFXIV_ARRAY

inline constexpr std::size_t kFieldCount = sizeof(kFieldTable) / sizeof(kFieldTable[0]);

// "group.name" of a row, built into `buffer` (at least 96 bytes). Returns the length.
inline std::size_t FieldKey(const FieldRow& row, char* buffer, std::size_t capacity)
{
    const std::size_t g = std::strlen(row.group), n = std::strlen(row.name);
    if (g + 1 + n + 1 > capacity)
        return 0;
    std::memcpy(buffer, row.group, g);
    buffer[g] = '.';
    std::memcpy(buffer + g + 1, row.name, n);
    buffer[g + 1 + n] = '\0';
    return g + 1 + n;
}

// The row whose "group.name" equals `key`, or nullptr.
inline const FieldRow* FindField(std::string_view key)
{
    const std::size_t dot = key.find('.');
    if (dot == std::string_view::npos)
        return nullptr;
    const std::string_view group = key.substr(0, dot), name = key.substr(dot + 1);
    for (const FieldRow& row : kFieldTable)
    {
        if (group == row.group && name == row.name)
            return &row;
    }
    return nullptr;
}

inline std::uint32_t* FieldU32(GameBuild& b, const FieldRow& row)
{
    return row.isArray ? nullptr : reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(&b) + row.offset);
}
inline const std::uint32_t* FieldU32(const GameBuild& b, const FieldRow& row)
{
    return row.isArray ? nullptr : reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(&b) + row.offset);
}
inline std::uint8_t* FieldBytes(GameBuild& b, const FieldRow& row) { return reinterpret_cast<std::uint8_t*>(&b) + row.offset; }
inline const std::uint8_t* FieldBytes(const GameBuild& b, const FieldRow& row)
{
    return reinterpret_cast<const std::uint8_t*>(&b) + row.offset;
}

// The starting point of every resolved build: each Rva and Guard field is 0 (never the C table value: the 0x25f610 reuse trap) and so are
// the stamp and the size. Offsets and constants keep their defaults. ResolvedBuild::build starts from this.
inline void ZeroResolvedFields(GameBuild& b)
{
    for (const FieldRow& row : kFieldTable)
    {
        if (row.kind == FieldKind::Rva || row.kind == FieldKind::Guard)
            std::memset(FieldBytes(b, row), 0, row.size);
    }
    b.timeDateStamp = 0;
    b.sizeOfImage = 0;
}
inline GameBuild ZeroedBuild()
{
    GameBuild b;
    ZeroResolvedFields(b);
    return b;
}
} // namespace ext::ffxiv::resolve
