#pragma once
// A structural fingerprint of a compiled DXBC shader.
//
// A game patch that recompiles a shader changes its CRC-32 (the catalog identity) while the shader keeps its role. The fingerprint is
// the first step towards recognising such a shader: it is RECORDED and LOGGED for every catalog shader, so one real game run produces
// the reference table that can later be baked into the catalogs (CatalogTable.h: Entry::refFingerprint). It is never used to tag or
// match a shader by itself: no CRC changes and nothing is adopted on a fingerprint match (logging only, see CatalogLedger.h).
//
// What is recorded (everything below is also in the hash):
//   * the container's declared total size, its chunk count and every chunk's FourCC + size, in container order;
//   * the input (ISGN) and output (OSGN) signature elements: semantic name, semantic index, system value, component type, register,
//     write mask and read/write mask (the first kMaxSigElements are kept in the struct, all of them are in the hash);
//   * the program chunk (SHEX, or SHDR for shader model 4): program type, shader model major/minor and the program length token in
//     DWORDs ("token count"). The program BODY is not decoded and not hashed: the fingerprint says "same shape", not "same code".
//
// Works on the stripped shaders the game ships (ISGN, OSGN, SHEX only) and on any container with fewer or more chunks. Signature chunks
// other than ISGN/OSGN (ISG1, OSG1, OSG5, PCSG...) are recorded as FourCC + size only.
//
// Safety: the input is untrusted bytes. Nothing is read at or past `size`; all offset and size arithmetic is done in 64 bits; any
// count, offset or length that does not fit the buffer rejects the input (valid = false, a reason in `error`, no hash). The function
// does not allocate and does not throw.
//
// Hash: FNV-1a 64 over this canonical little-endian byte stream (NOT over the struct's memory, which has padding):
//   "FP1" | u32 totalSize | u32 chunkCount | chunkCount x { u32 fourcc, u32 size } |
//   input: u8 present [, u32 count, count x element] | output: u8 present [, u32 count, count x element] |
//   u8 hasProgram [, u16 programType, u8 major, u8 minor, u32 tokenCount]
//   element = semantic name bytes, 0x00, u32 semanticIndex, u32 systemValue, u32 componentType, u32 register, u8 mask, u8 rwMask
// The format is pinned by a golden value in tests/ext/ffxiv/catalog/shader_fingerprint.cpp: changing it invalidates every reference
// fingerprint that has been baked into a catalog, so it must be a decision.
//
// Pure and dependency free (no pch, no D3D) so tests/ext/ffxiv/catalog builds it standalone.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace ext::ffxiv::catalog
{
enum class FpError : uint8_t
{
    None,
    TooSmall,      // null, or shorter than the 32 byte DXBC header
    BadMagic,      // does not start with "DXBC"
    BadHeader,     // the constant is not 1, the total size or the chunk count is not plausible
    Truncated,     // the header says the container is longer than the bytes handed over
    BadChunkTable, // a chunk offset or size does not fit the container
    BadSignature,  // an ISGN/OSGN chunk is inconsistent (count, offsets, unterminated or over-long semantic name)
    BadProgram,    // a SHEX/SHDR chunk is inconsistent (shorter than its header, length token out of range)
};

inline const char* FpErrorName(FpError e)
{
    switch (e)
    {
    case FpError::None: return "none";
    case FpError::TooSmall: return "too small";
    case FpError::BadMagic: return "not DXBC";
    case FpError::BadHeader: return "bad header";
    case FpError::Truncated: return "truncated";
    case FpError::BadChunkTable: return "bad chunk table";
    case FpError::BadSignature: return "bad signature chunk";
    case FpError::BadProgram: return "bad program chunk";
    }
    return "?";
}

inline constexpr size_t kMaxChunks = 16;        // chunks kept in the summary (all of them are hashed)
inline constexpr size_t kMaxSigElements = 32;   // signature elements kept per signature (all of them are hashed)
inline constexpr size_t kSemanticChars = 12;    // semantic name characters kept for display (the hash covers the whole name)
inline constexpr uint32_t kMaxContainerChunks = 64; // a container with more chunks is not a shader
inline constexpr uint32_t kMaxSemanticName = 63;    // longest accepted semantic name (a longer one is not a semantic)

struct FpChunk
{
    uint32_t fourcc = 0; // little-endian: 'I' | 'S' << 8 | 'G' << 16 | 'N' << 24
    uint32_t size = 0;   // the chunk's declared data size
};

struct FpElement
{
    char semantic[kSemanticChars] {}; // cut to kSemanticChars - 1 characters, NUL terminated
    uint16_t semanticIndex = 0;       // clamped to 16 bits for display
    uint8_t systemValue = 0;          // clamped to 8 bits for display
    uint8_t componentType = 0;
    uint8_t mask = 0;
    uint8_t rwMask = 0;
    uint32_t reg = 0;
};

struct FpSignature
{
    bool present = false;
    uint32_t count = 0;  // elements declared by the chunk
    uint32_t stored = 0; // elements kept in `e` (min(count, kMaxSigElements))
    FpElement e[kMaxSigElements] {};
};

struct ShaderFingerprint
{
    bool valid = false;
    FpError error = FpError::None;
    uint32_t totalSize = 0;   // the container's own declared size (the buffer may be longer)
    uint32_t chunkCount = 0;  // chunks declared by the container
    uint32_t chunksStored = 0; // min(chunkCount, kMaxChunks)
    FpChunk chunks[kMaxChunks] {};
    FpSignature input, output;
    bool hasProgram = false;
    uint16_t programType = 0; // 0 pixel, 1 vertex, 2 geometry, 3 hull, 4 domain, 5 compute
    uint8_t versionMajor = 0, versionMinor = 0;
    uint32_t tokenCount = 0; // the program length token: DWORDs including the version and length tokens
    uint64_t hash = 0;       // 0 when invalid
};

namespace detail
{
constexpr uint32_t FourCc(char a, char b, char c, char d)
{
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}
inline constexpr uint32_t kIsgn = FourCc('I', 'S', 'G', 'N');
inline constexpr uint32_t kOsgn = FourCc('O', 'S', 'G', 'N');
inline constexpr uint32_t kShex = FourCc('S', 'H', 'E', 'X');
inline constexpr uint32_t kShdr = FourCc('S', 'H', 'D', 'R');

inline uint32_t Rd32(const uint8_t* p, uint64_t at)
{
    uint32_t v;
    std::memcpy(&v, p + at, 4); // the caller has checked at + 4 <= the buffer
    return v;                   // the fork only runs on little-endian x64; the hash writes explicit bytes either way
}

struct Fnv
{
    uint64_t h = 0xcbf29ce484222325ull;
    void Byte(uint8_t b)
    {
        h ^= b;
        h *= 0x100000001b3ull;
    }
    void U8(uint32_t v) { Byte(static_cast<uint8_t>(v)); }
    void U16(uint32_t v)
    {
        Byte(static_cast<uint8_t>(v));
        Byte(static_cast<uint8_t>(v >> 8));
    }
    void U32(uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            Byte(static_cast<uint8_t>(v >> (8 * i)));
    }
};

struct ChunkRef
{
    uint32_t fourcc, offset, size; // offset of the chunk's DATA from the container start
};

// A signature chunk: `data` points at the chunk data, `size` is its declared size (already known to lie inside the buffer).
inline FpError ParseSignature(const uint8_t* data, uint32_t size, FpSignature& sig, Fnv& hash)
{
    if (size < 8)
        return FpError::BadSignature;
    const uint32_t count = Rd32(data, 0);
    const uint64_t elementsEnd = 8ull + 24ull * count;
    if (elementsEnd > size)
        return FpError::BadSignature;
    sig.present = true;
    sig.count = count;
    hash.U8(1);
    hash.U32(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint64_t at = 8ull + 24ull * i;
        const uint32_t nameOffset = Rd32(data, at);
        // The names follow the element table; an offset inside the table is not a name. An offset at or past the chunk's end leaves
        // the scan below empty (limit <= nameOffset), which is rejected as "no terminator".
        if (nameOffset < elementsEnd)
            return FpError::BadSignature;
        const uint64_t limit = nameOffset + uint64_t(kMaxSemanticName) + 1 < size ? nameOffset + uint64_t(kMaxSemanticName) + 1 : size;
        uint64_t end = nameOffset;
        while (end < limit && data[end] != 0)
            ++end;
        if (end >= limit) // no terminator inside the chunk within the length limit
            return FpError::BadSignature;
        const uint32_t semanticIndex = Rd32(data, at + 4), systemValue = Rd32(data, at + 8), componentType = Rd32(data, at + 12);
        const uint32_t reg = Rd32(data, at + 16);
        const uint8_t mask = data[at + 20], rwMask = data[at + 21];
        for (uint64_t k = nameOffset; k < end; ++k)
            hash.Byte(data[k]);
        hash.Byte(0);
        hash.U32(semanticIndex);
        hash.U32(systemValue);
        hash.U32(componentType);
        hash.U32(reg);
        hash.U8(mask);
        hash.U8(rwMask);
        if (i < kMaxSigElements)
        {
            FpElement& e = sig.e[i];
            const uint64_t shown = (end - nameOffset) < kSemanticChars - 1 ? (end - nameOffset) : kSemanticChars - 1;
            std::memcpy(e.semantic, data + nameOffset, static_cast<size_t>(shown));
            e.semantic[shown] = '\0';
            e.semanticIndex = static_cast<uint16_t>(semanticIndex > 0xFFFFu ? 0xFFFFu : semanticIndex);
            e.systemValue = static_cast<uint8_t>(systemValue > 0xFFu ? 0xFFu : systemValue);
            e.componentType = static_cast<uint8_t>(componentType > 0xFFu ? 0xFFu : componentType);
            e.mask = mask;
            e.rwMask = rwMask;
            e.reg = reg;
            sig.stored = i + 1;
        }
    }
    return FpError::None;
}
} // namespace detail

// The fingerprint of the DXBC container at `dxbc` (`size` bytes). Never reads outside [dxbc, dxbc + size).
inline ShaderFingerprint Fingerprint(const void* dxbc, size_t size)
{
    using namespace detail;
    ShaderFingerprint fp;
    const auto reject = [&fp](FpError e)
    {
        fp = ShaderFingerprint {};
        fp.error = e;
        return fp;
    };

    if (dxbc == nullptr || size < 32)
        return reject(FpError::TooSmall);
    const auto* p = static_cast<const uint8_t*>(dxbc);
    if (std::memcmp(p, "DXBC", 4) != 0)
        return reject(FpError::BadMagic);

    const uint32_t one = Rd32(p, 20), total = Rd32(p, 24), count = Rd32(p, 28);
    if (one != 1 || total < 32 || count == 0 || count > kMaxContainerChunks)
        return reject(FpError::BadHeader);
    if (total > size)
        return reject(FpError::Truncated);
    const uint64_t tableEnd = 32ull + 4ull * count;
    if (tableEnd > total)
        return reject(FpError::BadChunkTable);

    // Chunk table. A chunk is a u32 FourCC, a u32 size and `size` bytes of data, all inside [tableEnd, total).
    ChunkRef refs[kMaxContainerChunks];
    Fnv hash;
    hash.Byte('F');
    hash.Byte('P');
    hash.Byte('1');
    hash.U32(total);
    hash.U32(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t offset = Rd32(p, 32ull + 4ull * i);
        if (offset < tableEnd || uint64_t(offset) + 8ull > total)
            return reject(FpError::BadChunkTable);
        const uint32_t fourcc = Rd32(p, offset), chunkSize = Rd32(p, uint64_t(offset) + 4);
        if (uint64_t(offset) + 8ull + chunkSize > total)
            return reject(FpError::BadChunkTable);
        refs[i] = { fourcc, offset + 8, chunkSize };
        hash.U32(fourcc);
        hash.U32(chunkSize);
        if (i < kMaxChunks)
        {
            fp.chunks[i] = { fourcc, chunkSize };
            fp.chunksStored = i + 1;
        }
    }
    fp.totalSize = total;
    fp.chunkCount = count;

    // First ISGN, first OSGN, first SHEX/SHDR.
    const ChunkRef *isgn = nullptr, *osgn = nullptr, *program = nullptr;
    for (uint32_t i = 0; i < count; ++i)
    {
        if (refs[i].fourcc == kIsgn && !isgn)
            isgn = &refs[i];
        else if (refs[i].fourcc == kOsgn && !osgn)
            osgn = &refs[i];
        else if ((refs[i].fourcc == kShex || refs[i].fourcc == kShdr) && !program)
            program = &refs[i];
    }

    for (int which = 0; which < 2; ++which)
    {
        const ChunkRef* c = which == 0 ? isgn : osgn;
        FpSignature& sig = which == 0 ? fp.input : fp.output;
        if (!c)
        {
            hash.U8(0);
            continue;
        }
        if (const FpError e = ParseSignature(p + c->offset, c->size, sig, hash); e != FpError::None)
            return reject(e);
    }

    if (program)
    {
        if (program->size < 8)
            return reject(FpError::BadProgram);
        const uint32_t version = Rd32(p, program->offset), length = Rd32(p, uint64_t(program->offset) + 4);
        if (length < 2 || uint64_t(length) * 4ull > program->size)
            return reject(FpError::BadProgram);
        fp.hasProgram = true;
        fp.programType = static_cast<uint16_t>(version >> 16);
        fp.versionMajor = static_cast<uint8_t>((version >> 4) & 0xF);
        fp.versionMinor = static_cast<uint8_t>(version & 0xF);
        fp.tokenCount = length;
        hash.U8(1);
        hash.U16(fp.programType);
        hash.U8(fp.versionMajor);
        hash.U8(fp.versionMinor);
        hash.U32(length);
    }
    else
        hash.U8(0);

    fp.hash = hash.h;
    fp.valid = true;
    return fp;
}

// ---- log text -------------------------------------------------------------------------------------------------------------------
inline std::string HashText(uint64_t h)
{
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%016llx", static_cast<unsigned long long>(h));
    return buf;
}

inline std::string FourCcText(uint32_t fourcc)
{
    std::string s(4, '?');
    for (int i = 0; i < 4; ++i)
    {
        const char c = static_cast<char>((fourcc >> (8 * i)) & 0xFF);
        s[i] = (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    return s;
}

inline const char* ProgramTypeName(uint16_t type)
{
    switch (type)
    {
    case 0: return "ps";
    case 1: return "vs";
    case 2: return "gs";
    case 3: return "hs";
    case 4: return "ds";
    case 5: return "cs";
    default: return "p?";
    }
}

inline std::string SignatureText(const FpSignature& sig)
{
    std::string s = "[";
    for (uint32_t i = 0; i < sig.stored; ++i)
    {
        const FpElement& e = sig.e[i];
        if (i)
            s += ',';
        s += e.semantic;
        s += std::to_string(e.semanticIndex);
        s += " r";
        s += e.reg == 0xFFFFFFFFu ? std::string("-") : std::to_string(e.reg);
        s += ' ';
        const char* letters = "xyzw";
        for (int bit = 0; bit < 4; ++bit)
            if (e.mask & (1 << bit))
                s += letters[bit];
    }
    if (sig.count > sig.stored)
        s += ",+" + std::to_string(sig.count - sig.stored);
    s += ']';
    return s;
}

// `fp=0x... size=1512 chunks=ISGN:60/OSGN:44/SHEX:900 model=ps_5_0 tokens=222 in=[SV_Position0 r0 xyzw,...] out=[...]`
inline std::string FormatFingerprint(const ShaderFingerprint& fp)
{
    if (!fp.valid)
        return std::string("fp=invalid(") + FpErrorName(fp.error) + ")";
    std::string s = "fp=" + HashText(fp.hash) + " size=" + std::to_string(fp.totalSize) + " chunks=";
    for (uint32_t i = 0; i < fp.chunksStored; ++i)
    {
        if (i)
            s += '/';
        s += FourCcText(fp.chunks[i].fourcc) + ":" + std::to_string(fp.chunks[i].size);
    }
    if (fp.chunkCount > fp.chunksStored)
        s += "/+" + std::to_string(fp.chunkCount - fp.chunksStored);
    if (fp.hasProgram)
        s += std::string(" model=") + ProgramTypeName(fp.programType) + "_" + std::to_string(fp.versionMajor) + "_" + std::to_string(fp.versionMinor) +
             " tokens=" + std::to_string(fp.tokenCount);
    else
        s += " model=none";
    if (fp.input.present)
        s += " in=" + SignatureText(fp.input);
    if (fp.output.present)
        s += " out=" + SignatureText(fp.output);
    return s;
}
} // namespace ext::ffxiv::catalog
