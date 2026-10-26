#pragma once
// Hand-built DXBC containers for the shader-catalog tests: a 32 byte header, the chunk offset table and chunks with the
// layout the compiler writes (8 byte chunk header, 4 byte alignment), signature chunks (ISGN/OSGN: count, key, 24 byte elements,
// then the semantic names) and a SHEX body (version token, length in DWORDs, tokens). No game shader bytes are stored in the repository.
#include <cstdint>
#include <vector>

namespace testdxbc
{
using Bytes = std::vector<uint8_t>;

inline void Put32(Bytes& b, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
inline void Poke32(Bytes& b, size_t at, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}
inline uint32_t Get32(const Bytes& b, size_t at)
{
    return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}
inline uint32_t Cc(const char* s) { return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 | uint32_t(uint8_t(s[3])) << 24; }

struct Chunk
{
    uint32_t fourcc;
    Bytes data;
};

// A DXBC container: 32 byte header, offset table, chunks (8 byte header + data, 4 byte aligned like the compiler writes them).
inline Bytes BuildDxbc(const std::vector<Chunk>& chunks, uint8_t checksumFill = 0xAB)
{
    Bytes out;
    out.insert(out.end(), { 'D', 'X', 'B', 'C' });
    out.insert(out.end(), 16, checksumFill);
    Put32(out, 1);
    Put32(out, 0); // total size, patched below
    Put32(out, uint32_t(chunks.size()));
    const size_t tableAt = out.size();
    for (size_t i = 0; i < chunks.size(); ++i)
        Put32(out, 0);
    for (size_t i = 0; i < chunks.size(); ++i)
    {
        Poke32(out, tableAt + 4 * i, uint32_t(out.size()));
        Put32(out, chunks[i].fourcc);
        Put32(out, uint32_t(chunks[i].data.size()));
        out.insert(out.end(), chunks[i].data.begin(), chunks[i].data.end());
        while (out.size() % 4)
            out.push_back(0);
    }
    Poke32(out, 24, uint32_t(out.size()));
    return out;
}

struct Element
{
    const char* name;
    uint32_t index, sysValue, compType, reg;
    uint8_t mask, rw;
};

// ISGN / OSGN body: count, key (8), 24 byte elements, then the semantic name strings (offsets are relative to the chunk data).
inline Bytes SigChunk(const std::vector<Element>& elements)
{
    Bytes b;
    Put32(b, uint32_t(elements.size()));
    Put32(b, 8);
    const size_t elementsAt = b.size();
    for (size_t i = 0; i < elements.size(); ++i)
        for (int k = 0; k < 6; ++k)
            Put32(b, 0);
    for (size_t i = 0; i < elements.size(); ++i)
    {
        const Element& e = elements[i];
        const size_t at = elementsAt + 24 * i;
        Poke32(b, at, uint32_t(b.size())); // name offset
        for (const char* p = e.name; *p; ++p)
            b.push_back(uint8_t(*p));
        b.push_back(0);
        Poke32(b, at + 4, e.index);
        Poke32(b, at + 8, e.sysValue);
        Poke32(b, at + 12, e.compType);
        Poke32(b, at + 16, e.reg);
        b[at + 20] = e.mask;
        b[at + 21] = e.rw;
    }
    while (b.size() % 4)
        b.push_back(0);
    return b;
}

// SHEX body: version token (type << 16 | major << 4 | minor), length in DWORDs (includes these two), then `bodyTokens` tokens.
inline Bytes ShexChunk(uint32_t type, uint32_t major, uint32_t minor, uint32_t bodyTokens, uint32_t seed = 1)
{
    Bytes b;
    Put32(b, (type << 16) | (major << 4) | minor);
    Put32(b, 2 + bodyTokens);
    uint32_t v = seed;
    for (uint32_t i = 0; i < bodyTokens; ++i)
    {
        v = v * 1664525u + 1013904223u;
        Put32(b, v);
    }
    return b;
}

inline std::vector<Chunk> StrippedPs(uint32_t bodyTokens = 40, uint32_t seed = 1)
{
    return {
        { Cc("ISGN"), SigChunk({ { "SV_Position", 0, 1, 3, 0, 0xF, 0 }, { "TEXCOORD", 0, 0, 3, 1, 0x3, 0x3 } }) },
        { Cc("OSGN"), SigChunk({ { "SV_Target", 0, 0, 3, 0, 0xF, 0 } }) },
        { Cc("SHEX"), ShexChunk(0, 5, 0, bodyTokens, seed) },
    };
}
} // namespace testdxbc
