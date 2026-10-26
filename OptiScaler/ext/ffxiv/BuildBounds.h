#pragma once
// The fixed maxima the game-address consumers size their buffers with, and the explicit bounds checks that keep a RESOLVED (possibly healed)
// offset from overflowing one of them. Header-only, depends on GameBuild.h only, so a consumer and the gate (resolve/Gate.h) share the one
// definition. The gate refuses a feature whose offsets fail these checks; the consumers check the same bounds again at the point of use.
//
// Array sizes that stay compile-time constants on purpose: nativeBatchHeaderSize, drawCommandSize and the other sizes and counts are FieldKind::Constant rows
// of the field table, which no seam ever resolves, so they are identical on every build (a test pins the kind of each).

#include "GameBuild.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace ext::ffxiv
{
// ---- native quality: the settings struct snapshot ----
// The DLAA gate reads the first bytes of the game's settings struct into a buffer and indexes it with the settings offsets. The buffer is a
// fixed maximum; the number of bytes read is max(offsets) + 1 (0x56 on the known build, exactly what the compile-time table gave before), and a
// healed offset at or above the maximum is refused (the gate and the reader both check).
inline constexpr std::uint32_t kMaxSettingsSnapshotBytes = 0x100;

inline bool SettingsOffsetsInBounds(const NativeQualityBuild& q)
{
    const std::uint32_t all[] = { q.settingsModeOffset,     q.settingsQualityOffset,    q.settingsDlaaOffset,
                                  q.settingsRendererOffset, q.settingsAdditionalOffset, q.settingsSelectorOffset };
    for (const std::uint32_t o : all)
    {
        if (o >= kMaxSettingsSnapshotBytes)
            return false;
    }
    return true;
}

// The number of bytes of the settings struct the DLAA gate reads: the largest offset it indexes, plus one.
inline std::uint32_t SettingsSnapshotBytes(const NativeQualityBuild& q)
{
    return (std::max)({ q.settingsQualityOffset, q.settingsDlaaOffset, q.settingsRendererOffset, q.settingsAdditionalOffset, q.settingsModeOffset,
                      q.settingsSelectorOffset }) +
           1;
}

// The DLAA gate decision from a snapshot, bounds-checked: the mode byte is 2 (DLSS mode) and the DLAA byte is 0. False when any index is outside
// the bytes that were read.
inline bool DlaaRequiredFromSnapshot(const unsigned char* snapshot, std::size_t size, const NativeQualityBuild& q)
{
    if (snapshot == nullptr || q.settingsModeOffset >= size || q.settingsDlaaOffset >= size)
        return false;
    return snapshot[q.settingsModeOffset] == 2 && snapshot[q.settingsDlaaOffset] == 0;
}

// ---- companion: the batch header and the queue entry ----
// Every index into the 64-byte batch header and into the two-pointer queue entry, as the consumers use them (Packets::Supported, Capture, the
// nameplate replay, the GPU copy), must stay inside. The sizes are Constants of the build (never resolved).
inline bool CompanionOffsetsInBounds(const CompanionBuild& c)
{
    const std::uint64_t header = c.nativeBatchHeaderSize;
    return header != 0 && header <= 0x1000 && static_cast<std::uint64_t>(c.batchKindOffset) + 4 <= header &&
           static_cast<std::uint64_t>(c.batchExtensionOffset) + 2 <= header && static_cast<std::uint64_t>(c.batchCountOffset) + 4 <= header &&
           static_cast<std::uint64_t>(c.batchVerticesOffset) + 8 <= header && static_cast<std::uint64_t>(c.batchFormatOffset) + 4 <= header &&
           static_cast<std::uint64_t>(c.batchWrapperOffset) + 8 <= header && c.entryPointerOffset == sizeof(std::uintptr_t);
}

// The sizes of the two byte arrays the companion code declares. They are Constants of the build (FieldKind::Constant: no seam resolves them, a test
// pins that), so a fixed maximum is exact on every build; the offsets that index them are Offset fields and are bounds-checked above.
inline constexpr std::size_t kNativeBatchHeaderBytes = CompanionBuild {}.nativeBatchHeaderSize;
inline constexpr std::size_t kDrawCommandBytes = CompanionBuild {}.drawCommandSize;

// `size` bytes at `offset` inside a buffer of `bytes` bytes (no wrap-around).
inline bool RangeInside(std::uint64_t offset, std::uint64_t size, std::uint64_t bytes) { return size <= bytes && offset <= bytes - size; }
} // namespace ext::ffxiv
