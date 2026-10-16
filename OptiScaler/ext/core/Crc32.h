#pragma once
#include <cstddef>
#include <cstdint>

// Shared CRC-32 (polynomial 0xedb88320, bit-reversed/reflected, byte-at-a-time, initial/final
// complement). The same loop was independently hand-written in four places:
// ext::hdr::screenshot::image::Crc (Image.h), ext::ffxiv::companion::Gpu::Identify
// (CompanionGpu.h), ext::ffxiv::companion::replay::FfxivNameplateRenderer::Crc
// (FfxivNameplateRenderer.h) and ext::ffxiv::lighting::FfxivLightingCapture::Crc
// (FfxivLightingCapture.h). All four now forward to this function; this is the only place left
// with the actual loop body. Deliberately dependency-free (no project includes) so it can be
// pulled into any header, including ones a tests/ext recipe builds standalone without pch.h.
namespace ext::core
{
inline uint32_t Crc32(const void* data, size_t size)
{
    uint32_t crc = ~0u;
    for (auto p = static_cast<const unsigned char*>(data); size--; ++p)
    {
        crc ^= *p;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
} // namespace ext::core
