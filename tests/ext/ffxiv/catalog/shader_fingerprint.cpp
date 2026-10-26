// The structural shader fingerprint (ShaderFingerprint.h).
//
// The fingerprint is a pure function over DXBC bytes: total size, chunk list (FourCC + size), ISGN/OSGN element summaries, the SHEX/SHDR
// version token and length, and a stable 64-bit hash of all of that. It is recorded and logged for every catalog shader and never used
// to tag anything, so the checks here are about two properties:
//   * it is SAFE: no read past `size` whatever the bytes say. Every input below is placed flush against a PAGE_NOACCESS page, so one
//     byte read past the end is an access violation (the test crashes and the recipe fails). Every prefix of a valid shader, and every
//     header/chunk/signature field set to hostile values, is fed through it.
//   * it is STABLE and discriminating: the hash is over a canonical little-endian serialisation (golden value pinned below, cross-checked
//     with an independent implementation), identical for identical structure, different when any recorded field differs.
// A real compiler output (D3DCompile + D3DStripShader, the same stripped shape the game's shaders have) and a hand-built container
// (for hostile variants) are both used; no game shader bytes are stored in the repository.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "check.h"
#include "dxbc_builder.h"

#include "ext/core/Crc32.h"
#include "ext/ffxiv/catalog/ShaderFingerprint.h"

#include <windows.h>

#include <d3dcompiler.h>

#include <chrono>
#include <cstring>
#include <vector>

using namespace ext::ffxiv::catalog;

namespace
{
using namespace testdxbc;

// ---- an input flush against a PAGE_NOACCESS page -------------------------------------------------------------------------------
struct Guarded
{
    uint8_t* region = nullptr;
    size_t usable = 0;
    explicit Guarded(size_t maxBytes)
    {
        SYSTEM_INFO si {};
        GetSystemInfo(&si);
        const size_t page = si.dwPageSize;
        usable = ((maxBytes + page - 1) / page) * page;
        region = static_cast<uint8_t*>(VirtualAlloc(nullptr, usable + page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        DWORD old = 0;
        if (region)
            VirtualProtect(region + usable, page, PAGE_NOACCESS, &old);
    }
    ~Guarded() { VirtualFree(region, 0, MEM_RELEASE); }
    // Copies `n` bytes so that the last one is the last byte before the guard page.
    const uint8_t* Place(const uint8_t* bytes, size_t n) const
    {
        uint8_t* at = region + usable - n;
        if (n)
            std::memcpy(at, bytes, n);
        return at;
    }
};

ShaderFingerprint FpOf(const Bytes& b, const Guarded& g) { return Fingerprint(g.Place(b.data(), b.size()), b.size()); }

// ---- tests -----------------------------------------------------------------------------------------------------------------------
void Fields(const Guarded& g)
{
    const Bytes blob = BuildDxbc(StrippedPs());
    const ShaderFingerprint fp = FpOf(blob, g);
    CHECK(fp.valid);
    CHECK_EQ(int(fp.error), int(FpError::None));
    CHECK_EQ(fp.totalSize, blob.size());
    CHECK_EQ(fp.chunkCount, 3u);
    CHECK_EQ(fp.chunksStored, 3u);
    CHECK_EQ(fp.chunks[0].fourcc, Cc("ISGN"));
    CHECK_EQ(fp.chunks[1].fourcc, Cc("OSGN"));
    CHECK_EQ(fp.chunks[2].fourcc, Cc("SHEX"));
    CHECK_EQ(fp.chunks[2].size, 8u + 40u * 4u);
    CHECK_EQ(fp.chunks[1].size, Get32(blob, Get32(blob, 32 + 4) + 4));

    CHECK(fp.input.present && fp.output.present);
    CHECK_EQ(fp.input.count, 2u);
    CHECK_EQ(fp.input.stored, 2u);
    CHECK(std::string(fp.input.e[0].semantic) == "SV_Position");
    CHECK_EQ(fp.input.e[0].semanticIndex, 0u);
    CHECK_EQ(fp.input.e[0].systemValue, 1u);
    CHECK_EQ(fp.input.e[0].reg, 0u);
    CHECK_EQ(fp.input.e[0].mask, 0xFu);
    CHECK(std::string(fp.input.e[1].semantic) == "TEXCOORD");
    CHECK_EQ(fp.input.e[1].reg, 1u);
    CHECK_EQ(fp.input.e[1].mask, 3u);
    CHECK_EQ(fp.input.e[1].rwMask, 3u);
    CHECK_EQ(fp.output.count, 1u);
    CHECK(std::string(fp.output.e[0].semantic) == "SV_Target");

    CHECK(fp.hasProgram);
    CHECK_EQ(fp.programType, 0u); // pixel shader
    CHECK_EQ(fp.versionMajor, 5u);
    CHECK_EQ(fp.versionMinor, 0u);
    CHECK_EQ(fp.tokenCount, 42u); // the length token: includes the version and length tokens
    CHECK(fp.hash != 0);
}

// Works on the three-chunk stripped shape and on less: signatures and program are all optional, only the container has to be sound.
void Shapes(const Guarded& g)
{
    const ShaderFingerprint onlyProgram = FpOf(BuildDxbc({ { Cc("SHEX"), ShexChunk(1, 5, 0, 10) } }), g);
    CHECK(onlyProgram.valid);
    CHECK(!onlyProgram.input.present && !onlyProgram.output.present);
    CHECK_EQ(onlyProgram.programType, 1u); // vertex
    CHECK_EQ(onlyProgram.tokenCount, 12u);

    const ShaderFingerprint shdr = FpOf(BuildDxbc({ { Cc("SHDR"), ShexChunk(0, 4, 0, 10) } }), g); // shader model 4 chunk name
    CHECK(shdr.valid && shdr.hasProgram);
    CHECK_EQ(shdr.versionMajor, 4u);

    const ShaderFingerprint noProgram = FpOf(BuildDxbc({ { Cc("ISGN"), SigChunk({ { "POSITION", 0, 0, 3, 0, 0xF, 0 } }) } }), g);
    CHECK(noProgram.valid);
    CHECK(!noProgram.hasProgram);
    CHECK_EQ(noProgram.tokenCount, 0u);

    // A chunk the fingerprint does not know (DXIL, STAT, RDEF...) is recorded as FourCC + size only.
    const ShaderFingerprint dxil = FpOf(BuildDxbc({ { Cc("DXIL"), Bytes(64, 7) }, { Cc("SFI0"), Bytes(8, 1) } }), g);
    CHECK(dxil.valid);
    CHECK_EQ(dxil.chunkCount, 2u);
    CHECK(!dxil.hasProgram);
    CHECK_EQ(dxil.chunks[0].size, 64u);

    // An empty signature is a signature with zero elements.
    const ShaderFingerprint emptySig = FpOf(BuildDxbc({ { Cc("ISGN"), SigChunk({}) }, { Cc("SHEX"), ShexChunk(0, 5, 0, 4) } }), g);
    CHECK(emptySig.valid && emptySig.input.present && emptySig.input.count == 0);
}

// Golden: the hash recipe is a format that gets baked into catalogs later, so any change to it has to be a decision. The value was
// computed with an independent implementation (Python, FNV-1a 64 over the canonical byte stream described in ShaderFingerprint.h).
constexpr uint64_t kGoldenHash = 0xB34D234DD176B8E8ull;
void Golden(const Guarded& g)
{
    const ShaderFingerprint fp = FpOf(BuildDxbc(StrippedPs()), g);
    CHECK_EQ(fp.hash, kGoldenHash);
}

void Stability(const Guarded& g)
{
    const Bytes base = BuildDxbc(StrippedPs());
    const ShaderFingerprint a = FpOf(base, g);
    CHECK_EQ(FpOf(base, g).hash, a.hash); // deterministic

    // Not part of the structure: the DXBC checksum bytes, the body tokens (only their count), trailing bytes after the container.
    CHECK_EQ(FpOf(BuildDxbc(StrippedPs(), 0x11), g).hash, a.hash);
    CHECK_EQ(FpOf(BuildDxbc(StrippedPs(40, 99)), g).hash, a.hash);
    Bytes trailing = base;
    trailing.insert(trailing.end(), 37, 0xEE);
    const ShaderFingerprint t = FpOf(trailing, g);
    CHECK(t.valid);
    CHECK_EQ(t.hash, a.hash);
    CHECK_EQ(t.totalSize, base.size()); // the container's own size, not the buffer's

    // Everything recorded is part of the hash.
    auto differs = [&](const std::vector<Chunk>& chunks) { return FpOf(BuildDxbc(chunks), g).hash != a.hash; };
    CHECK(differs(StrippedPs(41)));  // token count (and with it the chunk and total size)
    {
        auto c = StrippedPs();
        c[0].data = SigChunk({ { "SV_Position", 0, 1, 3, 0, 0xF, 0 }, { "TEXCOORD", 1, 0, 3, 1, 0x3, 0x3 } }); // semantic index
        CHECK(differs(c));
        c[0].data = SigChunk({ { "SV_Position", 0, 1, 3, 0, 0xF, 0 }, { "TEXCOORD", 0, 0, 3, 2, 0x3, 0x3 } }); // register
        CHECK(differs(c));
        c[0].data = SigChunk({ { "SV_Position", 0, 1, 3, 0, 0xF, 0 }, { "TEXCOORD", 0, 0, 3, 1, 0x7, 0x3 } }); // mask
        CHECK(differs(c));
        c[0].data = SigChunk({ { "SV_Position", 0, 1, 3, 0, 0xF, 0 }, { "TEXCOORD", 0, 0, 3, 1, 0x3, 0x1 } }); // read/write mask
        CHECK(differs(c));
        c[0].data = SigChunk({ { "SV_Position", 0, 1, 3, 0, 0xF, 0 }, { "TEXCOORD", 0, 0, 2, 1, 0x3, 0x3 } }); // component type
        CHECK(differs(c));
        c[0].data = SigChunk({ { "SV_Position", 0, 2, 3, 0, 0xF, 0 }, { "TEXCOORD", 0, 0, 3, 1, 0x3, 0x3 } }); // system value
        CHECK(differs(c));
        c[0].data = SigChunk({ { "SV_Position", 0, 1, 3, 0, 0xF, 0 }, { "TEXCOORE", 0, 0, 3, 1, 0x3, 0x3 } }); // one letter of a name
        CHECK(differs(c));
        c[0].data = SigChunk({ { "SV_Position", 0, 1, 3, 0, 0xF, 0 } }); // an element fewer
        CHECK(differs(c));
    }
    {
        auto c = StrippedPs();
        c[1].data = SigChunk({ { "SV_Target", 1, 0, 3, 0, 0xF, 0 } }); // output element changed
        CHECK(differs(c));
        c = StrippedPs();
        std::swap(c[0], c[1]); // chunk order is part of the structure
        CHECK(differs(c));
        c = StrippedPs();
        c.push_back({ Cc("STAT"), Bytes(16, 0) }); // an extra chunk
        CHECK(differs(c));
        c = StrippedPs();
        c[2].data = ShexChunk(0, 5, 1, 40); // shader model minor
        CHECK(differs(c));
        c[2].data = ShexChunk(0, 4, 0, 40); // major
        CHECK(differs(c));
        c[2].data = ShexChunk(1, 5, 0, 40); // program type
        CHECK(differs(c));
        c = StrippedPs();
        c[2].fourcc = Cc("SHDR"); // the chunk name
        CHECK(differs(c));
    }
    // Input and output signature are distinguished (the same element list as input or as output is a different shader).
    {
        const Bytes sig = SigChunk({ { "COLOR", 0, 0, 3, 0, 0xF, 0 } });
        const ShaderFingerprint asInput = FpOf(BuildDxbc({ { Cc("ISGN"), sig } }), g);
        const ShaderFingerprint asOutput = FpOf(BuildDxbc({ { Cc("OSGN"), sig } }), g);
        CHECK(asInput.valid && asOutput.valid);
        CHECK(asInput.hash != asOutput.hash);
    }
}

// More chunks or elements than the summary stores: the summary is cut, the hash still covers everything and the count says so.
void Overflow(const Guarded& g)
{
    std::vector<Element> many;
    std::vector<std::string> names;
    for (int i = 0; i < 40; ++i)
        names.push_back("ATTR" + std::to_string(i));
    for (int i = 0; i < 40; ++i)
        many.push_back({ names[i].c_str(), 0, 0, 3, uint32_t(i), 0xF, 0 });
    const ShaderFingerprint a = FpOf(BuildDxbc({ { Cc("ISGN"), SigChunk(many) }, { Cc("SHEX"), ShexChunk(0, 5, 0, 8) } }), g);
    CHECK(a.valid);
    CHECK_EQ(a.input.count, 40u);
    CHECK_EQ(a.input.stored, uint32_t(kMaxSigElements));
    many[39].mask = 0x3; // an element beyond the stored ones
    const ShaderFingerprint b = FpOf(BuildDxbc({ { Cc("ISGN"), SigChunk(many) }, { Cc("SHEX"), ShexChunk(0, 5, 0, 8) } }), g);
    CHECK(b.valid);
    CHECK(a.hash != b.hash);

    std::vector<Chunk> chunks;
    for (int i = 0; i < 20; ++i)
        chunks.push_back({ Cc("PADD"), Bytes(4 + 4 * size_t(i), 0) });
    const ShaderFingerprint c = FpOf(BuildDxbc(chunks), g);
    CHECK(c.valid);
    CHECK_EQ(c.chunkCount, 20u);
    CHECK_EQ(c.chunksStored, uint32_t(kMaxChunks));
    chunks[19].data.push_back(0);
    chunks[19].data.push_back(0);
    chunks[19].data.push_back(0);
    chunks[19].data.push_back(0);
    CHECK(FpOf(BuildDxbc(chunks), g).hash != c.hash);
}

// Every prefix of a valid shader (down to zero bytes) is rejected: the header's own total size is longer than what was handed over.
void Truncation(const Guarded& g)
{
    const Bytes blob = BuildDxbc(StrippedPs());
    for (size_t n = 0; n < blob.size(); ++n)
    {
        const ShaderFingerprint fp = Fingerprint(g.Place(blob.data(), n), n);
        CHECK(!fp.valid);
        CHECK(fp.error != FpError::None);
        CHECK_EQ(fp.hash, 0u); // a rejected input has no hash
    }
    CHECK(FpOf(blob, g).valid); // the full length is accepted

    // Same with a stripped real shape that has many chunks and elements.
    std::vector<Chunk> big = StrippedPs(300);
    big.push_back({ Cc("STAT"), Bytes(116, 5) });
    const Bytes bigBlob = BuildDxbc(big);
    for (size_t n = 0; n < bigBlob.size(); ++n)
        CHECK(!Fingerprint(g.Place(bigBlob.data(), n), n).valid);

    // Null and empty.
    CHECK(!Fingerprint(nullptr, 0).valid);
    CHECK(!Fingerprint(nullptr, 100).valid);
    CHECK(!Fingerprint(blob.data(), 0).valid);
    CHECK(!Fingerprint(blob.data(), 31).valid);
}

// Hostile bytes: each field of the header, the offset table, a chunk header, a signature and the program header is set to values that
// point past the end, wrap 32 bits or lie about a count. None may read outside the buffer (guard page) and the ones that cannot be
// parsed are rejected with a specific reason.
void Hostile(const Guarded& g)
{
    const Bytes base = BuildDxbc(StrippedPs());
    const uint32_t off0 = Get32(base, 32), off1 = Get32(base, 36), off2 = Get32(base, 40);
    const uint32_t values[] = { 0u, 1u, 2u, 3u, 7u, 8u, 0x7Fu, 0x100u, 0xFFFFu, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFF0u, 0xFFFFFFFCu, 0xFFFFFFFFu };

    // header: version-ish word, total size, chunk count, offset table, chunk headers, signature header + element, program header
    const size_t words[] = { 20, 24, 28, 32, 36, 40, off0 + 4, off1 + 4, off2 + 4, off0 + 8, off0 + 12, off0 + 16, off0 + 20, off0 + 24, off0 + 28,
                             off0 + 32, off0 + 36, off1 + 8, off1 + 12, off2 + 8, off2 + 12 };
    size_t runs = 0, accepted = 0;
    for (size_t w : words)
        for (uint32_t v : values)
        {
            Bytes b = base;
            Poke32(b, w, v);
            const ShaderFingerprint fp = FpOf(b, g); // must not crash
            ++runs;
            if (fp.valid)
                ++accepted;
            else
                CHECK(fp.error != FpError::None);
            // ...and the same hostile bytes cut short anywhere (every 5th length, to keep the run quick)
            for (size_t n = 0; n < b.size(); n += 5)
                (void)Fingerprint(g.Place(b.data(), n), n);
        }
    CHECK(runs > 200);
    CHECK(accepted < runs); // many are rejected

    auto reject = [&](size_t word, uint32_t value, FpError expected, const char* what)
    {
        Bytes b = base;
        Poke32(b, word, value);
        const ShaderFingerprint fp = FpOf(b, g);
        if (fp.valid || fp.error != expected)
        {
            std::printf("hostile case \"%s\": valid=%d error=%s (expected %s)\n", what, fp.valid ? 1 : 0, FpErrorName(fp.error), FpErrorName(expected));
            ++g_failures;
        }
    };
    reject(20, 2, FpError::BadHeader, "the constant one is not 1");
    reject(24, uint32_t(base.size()) + 1, FpError::Truncated, "total size longer than the buffer");
    reject(24, 0xFFFFFFFFu, FpError::Truncated, "total size 4 GiB");
    reject(24, 31, FpError::BadHeader, "total size smaller than the header");
    reject(28, 0, FpError::BadHeader, "no chunks");
    reject(28, 0xFFFFFFFFu, FpError::BadHeader, "chunk count wraps 32 bits");
    reject(28, 0x40000000u, FpError::BadHeader, "chunk count x 4 wraps 32 bits");
    reject(28, 4000, FpError::BadHeader, "chunk table longer than the container");
    reject(32, 0xFFFFFFFFu, FpError::BadChunkTable, "chunk offset 4 GiB");
    reject(32, uint32_t(base.size()) - 4, FpError::BadChunkTable, "chunk header past the end");
    reject(32, 8, FpError::BadChunkTable, "chunk inside the header");
    reject(off0 + 4, 0xFFFFFFFFu, FpError::BadChunkTable, "chunk size 4 GiB");
    reject(off0 + 4, 0xFFFFFFFCu, FpError::BadChunkTable, "chunk size wraps with the offset");
    reject(off0 + 4, uint32_t(base.size()), FpError::BadChunkTable, "chunk size longer than the container");
    reject(off0 + 8, 0xFFFFFFFFu, FpError::BadSignature, "signature element count 4 G");
    reject(off0 + 8, 100000, FpError::BadSignature, "signature element count beyond the chunk");
    reject(off0 + 16, 0xFFFFFFFFu, FpError::BadSignature, "semantic name offset 4 GiB");
    reject(off0 + 16, 1, FpError::BadSignature, "semantic name offset inside the element table is not a name end");
    reject(off2 + 12, 0xFFFFFFFFu, FpError::BadProgram, "program length 4 G dwords");
    reject(off2 + 12, 1, FpError::BadProgram, "program length below the two header tokens");
    reject(off2 + 12, 1000, FpError::BadProgram, "program longer than its chunk");

    // A signature chunk that claims an element it has no room for, as the LAST chunk: the bytes after it are the guard page.
    {
        Bytes sig;
        Put32(sig, 1); // one element declared
        Put32(sig, 8);
        const Bytes b = BuildDxbc({ { Cc("SHEX"), ShexChunk(0, 5, 0, 4) }, { Cc("ISGN"), sig } });
        const ShaderFingerprint fp = FpOf(b, g);
        CHECK(!fp.valid);
        CHECK_EQ(int(fp.error), int(FpError::BadSignature));
        Bytes shortSig; // not even the count and key
        Put32(shortSig, 1);
        CHECK(!FpOf(BuildDxbc({ { Cc("SHEX"), ShexChunk(0, 5, 0, 4) }, { Cc("OSGN"), shortSig } }), g).valid);
    }
    // A semantic name with no terminator inside the chunk.
    {
        Bytes b = BuildDxbc({ { Cc("ISGN"), SigChunk({ { "ABCDEFGH", 0, 0, 3, 0, 0xF, 0 } }) } });
        const uint32_t chunkAt = Get32(b, 32);
        const uint32_t chunkSize = Get32(b, chunkAt + 4);
        for (uint32_t i = 0; i < chunkSize; ++i) // overwrite every byte of the string area with non-NULs
            if (chunkAt + 8 + 8 + 24 <= chunkAt + 8 + i)
                b[chunkAt + 8 + i] = 'Z';
        const ShaderFingerprint fp = FpOf(b, g);
        CHECK(!fp.valid);
        CHECK_EQ(int(fp.error), int(FpError::BadSignature));
    }
    // A semantic name longer than the limit (but terminated) is rejected, not truncated silently.
    {
        const std::string longName(200, 'N');
        const ShaderFingerprint fp = FpOf(BuildDxbc({ { Cc("ISGN"), SigChunk({ { longName.c_str(), 0, 0, 3, 0, 0xF, 0 } }) } }), g);
        CHECK(!fp.valid);
        CHECK_EQ(int(fp.error), int(FpError::BadSignature));
    }
    // Wrong magic.
    {
        Bytes b = base;
        b[0] = 'X';
        CHECK_EQ(int(FpOf(b, g).error), int(FpError::BadMagic));
    }
}

// A real compiler output, stripped the way the game's shaders are (ISGN, OSGN, SHEX only).
Bytes Compile(const char* hlsl)
{
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile(hlsl, std::strlen(hlsl), "t.hlsl", nullptr, nullptr, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (errors)
        errors->Release();
    if (FAILED(hr) || !code)
        return {};
    ID3DBlob* stripped = nullptr;
    const UINT flags = D3DCOMPILER_STRIP_REFLECTION_DATA | D3DCOMPILER_STRIP_DEBUG_INFO | D3DCOMPILER_STRIP_TEST_BLOBS | D3DCOMPILER_STRIP_PRIVATE_DATA;
    Bytes out;
    if (SUCCEEDED(D3DStripShader(code->GetBufferPointer(), code->GetBufferSize(), flags, &stripped)) && stripped)
    {
        const auto* p = static_cast<const uint8_t*>(stripped->GetBufferPointer());
        out.assign(p, p + stripped->GetBufferSize());
        stripped->Release();
    }
    code->Release();
    return out;
}

void Real(const Guarded& g)
{
    const Bytes ps = Compile("Texture2D t : register(t0); SamplerState s : register(s0);"
                             "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 { return t.Sample(s, uv) * 0.5f + p.z; }");
    CHECK(!ps.empty());
    if (ps.empty())
        return;
    const ShaderFingerprint fp = FpOf(ps, g);
    CHECK(fp.valid);
    CHECK_EQ(fp.totalSize, ps.size());
    CHECK_EQ(fp.chunkCount, 3u); // stripped: ISGN OSGN SHEX
    CHECK_EQ(fp.chunks[0].fourcc, Cc("ISGN"));
    CHECK_EQ(fp.chunks[1].fourcc, Cc("OSGN"));
    CHECK_EQ(fp.chunks[2].fourcc, Cc("SHEX"));
    CHECK(fp.input.present && fp.output.present);
    CHECK_EQ(fp.input.count, 2u);
    CHECK(std::string(fp.input.e[0].semantic) == "SV_Position");
    CHECK(std::string(fp.input.e[1].semantic) == "TEXCOORD");
    CHECK_EQ(fp.output.count, 1u);
    CHECK(std::string(fp.output.e[0].semantic) == "SV_TARGET" || std::string(fp.output.e[0].semantic) == "SV_Target");
    CHECK(fp.hasProgram);
    CHECK_EQ(fp.programType, 0u);
    CHECK_EQ(fp.versionMajor, 5u);
    CHECK_EQ(fp.versionMinor, 0u);
    CHECK(uint64_t(fp.tokenCount) * 4 <= fp.chunks[2].size && fp.tokenCount >= 10);

    // Same source compiles to the same fingerprint; a different program does not.
    CHECK_EQ(FpOf(Compile("Texture2D t : register(t0); SamplerState s : register(s0);"
                          "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 { return t.Sample(s, uv) * 0.5f + p.z; }"), g).hash,
             fp.hash);
    const Bytes other = Compile("float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 { return float4(uv, p.z, 1); }");
    CHECK(!other.empty());
    CHECK(FpOf(other, g).hash != fp.hash);

    // Every prefix of the real shader is rejected too, and a flipped byte anywhere never reads out of bounds.
    for (size_t n = 0; n < ps.size(); ++n)
        CHECK(!Fingerprint(g.Place(ps.data(), n), n).valid);
    for (size_t i = 0; i < ps.size(); ++i)
        for (uint8_t flip : { uint8_t(0x01), uint8_t(0x80), uint8_t(0xFF) })
        {
            Bytes b = ps;
            b[i] ^= flip;
            (void)FpOf(b, g);
        }
}

// The log text.
void Text(const Guarded& g)
{
    const ShaderFingerprint fp = FpOf(BuildDxbc(StrippedPs()), g);
    const std::string text = FormatFingerprint(fp);
    CHECK_HAS(text, "fp=0x");
    CHECK_HAS(text, "size=");
    CHECK_HAS(text, "chunks=ISGN:");
    CHECK_HAS(text, "/OSGN:");
    CHECK_HAS(text, "/SHEX:");
    CHECK_HAS(text, "model=ps_5_0");
    CHECK_HAS(text, "tokens=42");
    CHECK_HAS(text, "in=[SV_Position0 r0 xyzw,TEXCOORD0 r1 xy]");
    CHECK_HAS(text, "out=[SV_Target0 r0 xyzw]");
    char hex[32];
    std::snprintf(hex, sizeof hex, "fp=0x%016llx", static_cast<unsigned long long>(fp.hash));
    CHECK_HAS(text, hex);

    const ShaderFingerprint bad = Fingerprint(nullptr, 0);
    CHECK_HAS(FormatFingerprint(bad), "invalid");
    CHECK(FormatFingerprint(bad).find("fp=0x") == std::string::npos);
    CHECK_EQ(HashText(0x1234abcdull).size(), size_t(18)); // "0x" + 16 hex digits
}

// Cost: the fingerprint walks the chunk table and the two signatures and reads two program tokens; it never touches the program
// body. Printed for the report (not asserted tightly: machines differ), next to the CRC the hook already computes for every shader.
void Cost(const Guarded& g)
{
    struct Case
    {
        const char* label;
        uint32_t bodyTokens;
    };
    for (Case c : { Case { "small (about 0.4 KB)", 40 }, Case { "medium (about 2 KB)", 400 }, Case { "large (about 17 KB)", 4200 } })
    {
        const Bytes blob = BuildDxbc(StrippedPs(c.bodyTokens));
        const uint8_t* at = g.Place(blob.data(), blob.size());
        constexpr int kRuns = 20000;
        volatile uint64_t sink = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kRuns; ++i)
            sink = sink + Fingerprint(at, blob.size()).hash;
        const auto t1 = std::chrono::steady_clock::now();
        const double fpUs = std::chrono::duration<double, std::micro>(t1 - t0).count() / kRuns;

        constexpr int kCrcRuns = 200;
        const auto c0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kCrcRuns; ++i)
            sink = sink + ext::core::Crc32(at, blob.size());
        const auto c1 = std::chrono::steady_clock::now();
        const double crcUs = std::chrono::duration<double, std::micro>(c1 - c0).count() / kCrcRuns;
        std::printf("cost %-22s %6zu bytes: fingerprint %.2f us, Crc32 (already paid per shader) %.2f us\n", c.label, blob.size(), fpUs, crcUs);
        CHECK(fpUs < 1000.0); // generous: a few microseconds on any machine, this only catches an accidental O(n^2) or a body walk
    }
}
} // namespace

int main()
{
    const Guarded g(64 * 1024);
    CHECK(g.region != nullptr);
    if (!g.region)
        return 2;
    Fields(g);
    Shapes(g);
    Golden(g);
    Stability(g);
    Overflow(g);
    Truncation(g);
    Hostile(g);
    Real(g);
    Text(g);
    Cost(g);
    return Finish("shader_fingerprint");
}
