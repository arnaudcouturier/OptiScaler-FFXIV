#include "pch.h"

#include "ext/ffxiv/engine/PassIndex.h"

#include "ext/ffxiv/diag/ShaderBindings.h" // the DXBC RDEF reader the developer draw diagnostic already uses

#include <string_view>
#include <cstring>

// Engine pass overrides: the shader index (ext/ffxiv/engine/PassIndex.h). It reads a pixel shader's DXBC bytecode and remembers the slots
// the names of EnginePassRules.h's table are declared at, plus the number of pixels the shader writes; the shader-creation hook fills a
// bounded table of the results and the draw path copies a layout out of it.
//
// The names arrive as the engine's compiler writes them: a texture of an effect variable carries the effect compiler's type suffix
// (`g_SamplerLightDiffuse.T`), while a constant buffer keeps its plain name. The comparison strips that suffix (diag::ResourceBaseName),
// otherwise the knob's second anchor would never match and every composite draw would look like one without it.
namespace ext::ffxiv::engine
{
namespace
{
// The chunk ids of the output signature: OSGN is what the shader model 5 compiler writes for a pixel shader, OSG5 the variant that carries
// a stream index in every element. Both start with the element count.
constexpr uint32_t kOsgn = 0x4E47534Fu; // 'OSGN'
constexpr uint32_t kOsg5 = 0x3547534Fu; // 'OSG5'

uint32_t Dword(const unsigned char* bytes, size_t at)
{
    return uint32_t(bytes[at]) | (uint32_t(bytes[at + 1]) << 8) | (uint32_t(bytes[at + 2]) << 16) | (uint32_t(bytes[at + 3]) << 24);
}

bool Readable(size_t size, size_t at, size_t bytes) { return at <= size && bytes <= size - at; }
} // namespace

uint32_t OutputCountOfShader(const void* code, size_t size)
{
    if (code == nullptr)
        return 0;
    const auto* bytes = static_cast<const unsigned char*>(code);
    constexpr size_t kChunkTableAt = 4 + 16 + 4 + 4; // "DXBC", digest, version, size
    if (size < kChunkTableAt + 4 || std::memcmp(bytes, "DXBC", 4) != 0)
        return 0;
    const uint32_t chunks = Dword(bytes, kChunkTableAt);
    if (chunks == 0 || chunks > 64)
        return 0;
    for (uint32_t i = 0; i < chunks; ++i)
    {
        const size_t entryAt = kChunkTableAt + 4 + size_t(i) * 4;
        if (!Readable(size, entryAt, 4))
            return 0;
        const size_t at = Dword(bytes, entryAt);
        if (!Readable(size, at, 8))
            continue;
        const uint32_t fourcc = Dword(bytes, at);
        if (fourcc != kOsgn && fourcc != kOsg5)
            continue;
        const size_t bodyAt = at + 8;
        const size_t bodySize = Dword(bytes, at + 4);
        if (!Readable(size, bodyAt, 4) || bodySize < 4 || bodyAt + bodySize > size)
            return 0;
        const uint32_t count = Dword(bytes, bodyAt);
        // The element table has to fit: an element is 24 bytes in OSGN and 32 in OSG5. A count that does not fit is a blob this reader does
        // not understand, and 0 is the answer that means "not declared" rather than a wrong number.
        const size_t elementBytes = fourcc == kOsg5 ? 32 : 24;
        if (uint64_t(count) * elementBytes + 4 > bodySize)
            return 0;
        return count;
    }
    return 0;
}

bool MentionsAnyName(const void* code, size_t size) noexcept
{
    if (code == nullptr)
        return false;
    const auto* bytes = static_cast<const unsigned char*>(code);
    for (const NameSpec& spec : kNames)
    {
        const size_t length = std::strlen(spec.name);
        if (size < length)
            continue;
        for (size_t i = 0; i + length <= size; ++i)
            if (bytes[i] == static_cast<unsigned char>(spec.name[0]) && std::memcmp(bytes + i, spec.name, length) == 0)
                return true;
    }
    return false;
}

// The kind a name's slot is looked up with: a buffer row asks for a constant buffer, a texture row for a texture. The lookup itself
// (diag::BindPointOf) compares the binding's type, so a sampler that carries a texture's name - the effect compiler writes `<name>.S` for it
// before the texture's `<name>.T` - never hands a patch a slot no buffer or texture is bound at.
constexpr ext::ffxiv::diag::BindKind BindKindOf(NameKind kind) noexcept
{
    return kind == NameKind::ConstantBuffer ? ext::ffxiv::diag::BindKind::ConstantBuffer : ext::ffxiv::diag::BindKind::Texture;
}

PassLayout PassLayoutOfShader(const void* code, size_t size)
{
    PassLayout layout;
    if (code == nullptr || size < 32)
        return layout;
    ext::ffxiv::diag::ShaderBinding parsed[ext::ffxiv::diag::kMaxBindings];
    const size_t count = ext::ffxiv::diag::ParseBindings(code, size, parsed, ext::ffxiv::diag::kMaxBindings);
    for (size_t i = 0; i < count; ++i)
    {
        const std::string_view name =
            ext::ffxiv::diag::ResourceBaseName(std::string_view(parsed[i].name, parsed[i].nameLength));
        const int32_t index = IndexOfName(name);
        if (index < 0)
            continue;
        // The kind is part of the match. Before the lookup asked for it explicitly, a texture row took the LAST binding whose base name
        // matched whatever its type was: for the engine's own order (the sampler before the texture) that answer was the texture's register,
        // which is why the patch worked by accident. Asking for the texture's kind answers the same register for that order and refuses a
        // sampler-only table instead of patching a slot no buffer is bound at.
        const uint32_t slot = ext::ffxiv::diag::BindPointOf(parsed, count, BindKindOf(kNames[index].kind), kNames[index].name);
        if (slot == ext::ffxiv::diag::kNoBindPoint)
            continue;
        layout.slots[index] = slot;
        layout.found |= 1u << index;
    }
    layout.ok = layout.found != 0;
    layout.outputs = OutputCountOfShader(code, size);
    return layout;
}
} // namespace ext::ffxiv::engine
