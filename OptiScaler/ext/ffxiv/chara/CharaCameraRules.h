#pragma once
// Pure rules of the camera constant buffer of the preview's G-buffer pass (CharaCamera.cpp). The preview's G-buffer pixel shader reads a
// per-pass camera block as pixel-shader constant buffer 2: 59 float4 vectors (944 bytes) or a view into a larger buffer. A depth prepass
// carries the same block in vertex-shader constant buffer 0. The helper passes later read its last vector, so a copy of the whole buffer is
// kept.

#include <cstddef>
#include <cstdint>

namespace ext::ffxiv::chara::camera
{
inline constexpr uint32_t kSlot = 2;                  // pixel-shader constant buffer slot of the G-buffer pass
inline constexpr uint32_t kVertexSlot = 0;            // vertex-shader constant buffer slot of a depth prepass
inline constexpr uint32_t kVectors = 0x3b;            // 59
inline constexpr uint32_t kMinBytes = kVectors * 16;  // 0x3b0
inline constexpr uint32_t kMaxBytes = kMinBytes + 0xFC50;
inline constexpr uint32_t kBoundSlot = 1;             // the copy is bound to slot 1 of every helper pass

// Whether a constant buffer of `byteWidth` bytes can hold the camera block: the size range Accept uses below, without looking at the
// bound range. A G-buffer shader may read the block as a view into a larger buffer, so a draw that only has to prove it reads the
// camera block can accept any size in this range.
constexpr bool SizeAccepted(uint32_t byteWidth) { return byteWidth >= kMinBytes && byteWidth <= kMaxBytes; }

// Whether the buffer bound at slot 2 is a camera block: its size is in [0x3b0, 0x3b0 + 0xFC50], the bound range
// (`first`, `count` in 16-byte constants) starts inside it and covers at least 59 vectors.
constexpr bool Accept(uint32_t byteWidth, uint32_t first, uint32_t count)
{
    if (!SizeAccepted(byteWidth))
        return false;
    if (count < kVectors)
        return false;
    return static_cast<uint64_t>(byteWidth) >= (static_cast<uint64_t>(first) + kVectors) * 16u;
}

// The copy buffer is reused while the source keeps its size.
constexpr bool CanReuseCopy(uint32_t copyBytes, uint32_t sourceBytes) { return copyBytes != 0 && copyBytes == sourceBytes; }

// The stage the camera block of a pass is asked from, chosen by the draw that starts the pass. One stage only, never a fallback to the
// other: D3D11 keeps a binding from draw to draw, so the other stage may still hold the block of another window, and a pass named after the
// wrong window is shifted for an upscaler that will not run on it.
enum class Stage : uint8_t
{
    Vertex, // a depth prepass draws through the vertex shader that carries the camera block: vertex-shader slot 0
    Pixel,  // a G-buffer draw: pixel-shader slot 2
};

constexpr Stage StageFor(bool prepass) { return prepass ? Stage::Vertex : Stage::Pixel; }
constexpr uint32_t SlotOf(Stage stage) { return stage == Stage::Vertex ? kVertexSlot : kSlot; }

// Which preview window a pass is drawing, as far as the binding can say: the constant buffer object bound at the draw's own stage, its
// first bound constant (a window's block may be a view into a shared per-frame buffer) and the pass's render-target size. Nothing here is
// read from video memory, so a pass is identified without waiting for the GPU. The buffer's byte width is deliberately left out: the same
// window may see its block as the exact 944-byte buffer at one draw and as a view into a larger one at another, and the identity must not
// depend on which binding history it had.
struct BlockKey
{
    uintptr_t buffer = 0; // the bound constant buffer object (0: nothing that can hold the camera block is bound there)
    uint32_t first = 0;   // the first constant of the bound range, in 16-byte units
    uint32_t width = 0;   // the pass's render target
    uint32_t height = 0;

    bool Valid() const { return buffer != 0; }
    bool operator==(const BlockKey& other) const
    {
        return buffer == other.buffer && first == other.first && width == other.width && height == other.height;
    }
};
} // namespace ext::ffxiv::chara::camera
