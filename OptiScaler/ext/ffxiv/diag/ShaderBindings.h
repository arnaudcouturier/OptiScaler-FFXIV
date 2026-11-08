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
#include <string_view>

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

// The name a binding is compared by. The effect compiler this game's shaders go through declares a texture and its sampler under ONE
// variable name and writes the pair into the reflection as "<name>.T" (the texture) and "<name>.S" (the sampler state); a constant
// buffer keeps its plain name. So the reflected name of the sun mask is `g_SamplerShadow.T`, not `g_SamplerShadow`. The binding's own
// type already tells a texture from a sampler, so the suffix carries nothing a caller needs: compare the variable's name.
inline constexpr std::string_view ResourceBaseName(std::string_view name)
{
    if (name.size() > 2 && name[name.size() - 2] == '.' && (name.back() == 'T' || name.back() == 'S'))
        return name.substr(0, name.size() - 2);
    return name;
}

// The kind of resource a slot is looked up for. The effect compiler this game's shaders go through writes a texture and its sampler under one
// variable name, as "<name>.T" (the texture) and "<name>.S" (the sampler state), and the two are bound at registers that need not agree (the
// ambient occlusion shader declares g_SamplerDepth.S at s0 and .T at t1). A lookup that compared names alone would therefore answer the
// sampler's slot for a texture - the bug that cost two sessions - so every slot decision asks for the kind it binds and this is the one place
// the type is compared, for a lookup by name and for a lookup by slot.
enum class BindKind : uint32_t
{
    ConstantBuffer = kBindConstantBuffer,
    Texture = kBindTexture,
    Sampler = kBindSampler,
    Other = 0xFFFFFFFFu, // a kind no slot decision here asks for (a UAV, a structured buffer): kept, and never an answer
};

// The kind a parsed binding declares, for a reader that keeps the parsed table in a shape of its own. A binding of a type no slot decision here
// asks about answers `Other`, so it can neither be looked up nor answer a lookup by accident.
constexpr BindKind BindingKindOf(const ShaderBinding& binding)
{
    switch (binding.type)
    {
    case kBindConstantBuffer: return BindKind::ConstantBuffer;
    case kBindTexture: return BindKind::Texture;
    case kBindSampler: return BindKind::Sampler;
    default: return BindKind::Other;
    }
}

// The answer of a lookup for a name and kind the shader does not declare, in eight bits apart from every slot D3D11 can bind.
inline constexpr uint32_t kNoBindPoint = 0xFFFFFFFFu;

// True when a binding answers the slot a draw has bound: it is of the kind asked for and covers bindPoint .. bindPoint + bindCount - 1 (a
// texture array declares one entry for its whole range). This is BindPointOf read the other way, for a reader that walks a draw's slots and asks
// which name it carries (a capture names the buffer it samples), and it is the second and last place a binding's type is compared: a reader that
// compared the type itself could answer a name for the wrong kind of resource, which is the same defect the slot lookup exists for.
constexpr bool BindingAnswers(const ShaderBinding& binding, BindKind kind, uint32_t slot)
{
    return binding.type == uint32_t(kind) && binding.bindCount != 0 && slot >= binding.bindPoint &&
           slot < binding.bindPoint + binding.bindCount;
}

// The slot `name` is bound at as `kind`, or kNoBindPoint. The name is compared by the variable's name (ResourceBaseName) and the binding has to
// be of the kind asked for - a binding of another kind is never a candidate, whatever its name - and the first binding that carries both wins,
// so a shader that declares one name twice is read at the slot its reflection lists first.
constexpr uint32_t BindPointOf(const ShaderBinding* bindings, size_t count, BindKind kind, std::string_view name)
{
    if (bindings == nullptr)
        return kNoBindPoint;
    const std::string_view wanted = ResourceBaseName(name);
    for (size_t i = 0; i < count; ++i)
    {
        const ShaderBinding& bind = bindings[i];
        if (bind.name == nullptr || !BindingAnswers(bind, kind, bind.bindPoint))
            continue;
        if (ResourceBaseName(std::string_view(bind.name, bind.nameLength)) == wanted)
            return bind.bindPoint;
    }
    return kNoBindPoint;
}

// True when a parsed table declares `name`, which is what a draw's shape is identified by (the wetness pass declares its own constant
// buffer and two named textures, for example). Compared by the variable's name for the same reason as ResourceBaseName: a raw comparison
// finds nothing for a texture, because the reflection calls it `g_SamplerGBuffer3.T`. Either form may be passed in.
inline bool ReflectionDeclares(const ShaderBinding* bindings, size_t count, std::string_view name)
{
    const std::string_view wanted = ResourceBaseName(name);
    if (bindings == nullptr)
        return false;
    for (size_t i = 0; i < count; ++i)
        if (ResourceBaseName(std::string_view(bindings[i].name, bindings[i].nameLength)) == wanted)
            return true;
    return false;
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
