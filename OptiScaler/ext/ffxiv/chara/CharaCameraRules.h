#pragma once
// Pure rules of the camera constant-buffer snapshot (CharaCamera.cpp). The preview's G-buffer pixel shader reads a
// per-pass camera block as pixel-shader constant buffer 2: 59 float4 vectors (944 bytes) or a view into a larger buffer.
// The helper passes later read its last vector, so a copy of the whole buffer is kept.

#include <cstdint>

namespace ext::ffxiv::chara::camera
{
inline constexpr uint32_t kSlot = 2;                  // pixel-shader constant buffer slot of the G-buffer pass
inline constexpr uint32_t kVectors = 0x3b;            // 59
inline constexpr uint32_t kMinBytes = kVectors * 16;  // 0x3b0
inline constexpr uint32_t kMaxBytes = kMinBytes + 0xFC50;
inline constexpr uint32_t kBoundSlot = 1;             // the copy is bound to slot 1 of every helper pass

// Whether the buffer bound at slot 2 is a camera block: its size is in [0x3b0, 0x3b0 + 0xFC50], the bound range
// (`first`, `count` in 16-byte constants) starts inside it and covers at least 59 vectors.
constexpr bool Accept(uint32_t byteWidth, uint32_t first, uint32_t count)
{
    if (byteWidth < kMinBytes || byteWidth > kMaxBytes)
        return false;
    if (count < kVectors)
        return false;
    return static_cast<uint64_t>(byteWidth) >= (static_cast<uint64_t>(first) + kVectors) * 16u;
}

// The copy buffer is reused while the source keeps its size.
constexpr bool CanReuseCopy(uint32_t copyBytes, uint32_t sourceBytes) { return copyBytes != 0 && copyBytes == sourceBytes; }
} // namespace ext::ffxiv::chara::camera
