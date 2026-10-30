#pragma once
// Reads the bound-resource table of a pixel shader's DXBC bytecode: for every texture, sampler and constant buffer the shader
// declares, its reflected name, its type and the slot it is bound at. The names are the engine's own (`g_SamplerDepth`,
// `g_LightParam`, ...), so a draw can be described by what it reads instead of by a fingerprint alone.
//
// Pure and D3D free (no d3d11.h, no pch), so a unit test parses a shader it compiled itself and the draw path only ever sees
// plain structs.
//
// The container this reads (little endian, Windows DXBC):
//   "DXBC" | 16-byte digest | DWORD version | DWORD size | DWORD chunkCount | chunkCount * DWORD chunk offsets (from byte 0)
//   each chunk: FOURCC | DWORD bodySize | body
//   RDEF body:  DWORD cbCount | DWORD cbOffset | DWORD bindCount | DWORD bindOffset | ... then `bindCount` entries of 32 bytes:
//               DWORD nameOffset | DWORD type | DWORD returnType | DWORD dimension | DWORD sampleCount | DWORD bindPoint
//               | DWORD bindCount | DWORD flags
//   Name and variable offsets are relative to the start of the RDEF body. Every read is bounds checked: a truncated or
//   unfamiliar blob yields no bindings rather than a fault, because the blob comes from the game.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ext::ffxiv::diag
{
// The D3D_SIT_* values this diagnostic distinguishes; other values are passed through untouched.
inline constexpr uint32_t kBindConstantBuffer = 0;
inline constexpr uint32_t kBindTexture = 2;
inline constexpr uint32_t kBindSampler = 3;

// One bound resource. `name` points into the blob that was parsed and is only valid while that blob is: the caller copies
// what it keeps.
struct ShaderBinding
{
    const char* name = nullptr;
    uint32_t nameLength = 0;
    uint32_t type = 0;
    uint32_t bindPoint = 0;
    uint32_t bindCount = 0;
};

// True when a binding serves the slot a draw has bound: a binding covers bindPoint .. bindPoint + bindCount - 1 (a texture array
// declares one entry for its whole range).
inline bool BindingCovers(const ShaderBinding& binding, uint32_t type, uint32_t slot)
{
    return binding.type == type && slot >= binding.bindPoint && slot < binding.bindPoint + binding.bindCount;
}

inline constexpr size_t kMaxBindings = 48; // the table of a real shader is a dozen to three dozen entries

namespace detail
{
inline bool Read(size_t size, size_t at, size_t bytes) { return at <= size && bytes <= size - at; }

inline uint32_t Dword(const unsigned char* body, size_t at) { return uint32_t(body[at]) | (uint32_t(body[at + 1]) << 8) | (uint32_t(body[at + 2]) << 16) | (uint32_t(body[at + 3]) << 24); }

// The RDEF chunk body of a DXBC blob, or null. `outSize` receives the body size.
inline const unsigned char* FindRdef(const unsigned char* code, size_t size, size_t* outSize)
{
    constexpr size_t kDigestAndVersion = 4 + 16 + 4 + 4; // "DXBC", digest, version, size
    if (!code || size < kDigestAndVersion + 4 || std::memcmp(code, "DXBC", 4) != 0)
        return nullptr;
    const uint32_t chunks = Dword(code, kDigestAndVersion);
    if (chunks == 0 || chunks > 64)
        return nullptr;
    for (uint32_t i = 0; i < chunks; ++i)
    {
        const size_t headerAt = kDigestAndVersion + 4 + size_t(i) * 4;
        if (!Read(size, headerAt, 4))
            return nullptr;
        const size_t at = Dword(code, headerAt);
        if (!Read(size, at, 8) || std::memcmp(code + at, "RDEF", 4) != 0)
            continue;
        const size_t body = at + 8;
        const size_t bodySize = Dword(code, at + 4);
        if (!Read(size, body, bodySize))
            return nullptr;
        *outSize = bodySize;
        return code + body;
    }
    return nullptr;
}
} // namespace detail

// Writes up to `capacity` entries into `out` and returns how many. `declared` (optional) receives the resource count the
// table declares, so a caller can report a table it had to shorten.
inline size_t ParseBindings(const void* code, size_t size, ShaderBinding* out, size_t capacity, uint32_t* declared = nullptr)
{
    if (declared)
        *declared = 0;
    if (!code || !out || capacity == 0)
        return 0;

    const auto* bytes = static_cast<const unsigned char*>(code);
    size_t bodySize = 0;
    const unsigned char* body = detail::FindRdef(bytes, size, &bodySize);
    if (body == nullptr || !detail::Read(bodySize, 0, 16))
        return 0;

    const uint32_t count = detail::Dword(body, 8);
    const uint32_t tableAt = detail::Dword(body, 12);
    if (declared)
        *declared = count;
    if (count == 0 || !detail::Read(bodySize, tableAt, size_t(count) * 32))
        return 0;

    size_t written = 0;
    for (uint32_t i = 0; i < count && written < capacity; ++i)
    {
        const size_t entry = tableAt + size_t(i) * 32;
        const uint32_t nameAt = detail::Dword(body, entry);
        if (nameAt >= bodySize)
            continue;
        // A name is a NUL terminated identifier at an offset inside the body; a missing terminator drops the entry.
        const void* nul = std::memchr(body + nameAt, 0, bodySize - nameAt);
        if (!nul)
            continue;
        ShaderBinding& b = out[written];
        b.name = reinterpret_cast<const char*>(body + nameAt);
        b.nameLength = static_cast<uint32_t>(static_cast<const unsigned char*>(nul) - (body + nameAt));
        if (b.nameLength == 0)
            continue;
        b.type = detail::Dword(body, entry + 4);
        b.bindPoint = detail::Dword(body, entry + 20);
        b.bindCount = detail::Dword(body, entry + 24);
        if (b.bindCount == 0)
            b.bindCount = 1;
        ++written;
    }
    return written;
}
} // namespace ext::ffxiv::diag
