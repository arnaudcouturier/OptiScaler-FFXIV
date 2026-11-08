// The draw diagnostic's reflection reader: the DXBC RDEF table of a pixel shader compiled in this test with the same compiler the game's
// build uses (D3DCompiler). No device, no GPU: the parser is pure and works on the blob.
#include "check.h"

#include "ext/ffxiv/diag/ShaderBindings.h"

#include <d3dcompiler.h>
#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

using namespace ext::ffxiv::diag;

namespace
{
const char* kSource = R"(
Texture2D<float4> g_SamplerDepth : register(t0);
Texture2D<float4> g_SamplerGBuffer : register(t3);
Texture2DArray<float4> g_SamplerOcclusion : register(t5);
Texture2D<float4> g_SamplerCascade[4] : register(t8);
SamplerState g_SamplerLinear : register(s0);
cbuffer g_CommonParameter : register(b0)
{
    float4 m_Viewport;
    float2 m_Pad;
};
cbuffer g_LightParam : register(b2)
{
    float3 m_Position;
    float3 m_Direction;
    float3 m_DiffuseColor;
    float4 m_Attenuation;
};
struct PSIn
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(PSIn i) : SV_Target
{
    const float4 a = g_SamplerDepth.Sample(g_SamplerLinear, i.uv);
    const float4 b = g_SamplerGBuffer.Sample(g_SamplerLinear, i.uv);
    const float4 c = g_SamplerOcclusion.Sample(g_SamplerLinear, float3(i.uv, 0));
    const float4 d = g_SamplerCascade[1].Sample(g_SamplerLinear, i.uv);
    return a + b + c + d + float4(m_Direction * m_Viewport.x, 1);
}
)";

struct Blob
{
    std::vector<unsigned char> bytes;
    const void* data() const { return bytes.data(); }
    size_t size() const { return bytes.size(); }
};

bool Compile(Blob& out)
{
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile(kSource, std::strlen(kSource), "diag_test.hlsl", nullptr, nullptr, "main", "ps_5_0", 0, 0, &code, &errors);
    if (errors)
    {
        std::printf("%s", static_cast<const char*>(errors->GetBufferPointer()));
        errors->Release();
    }
    if (FAILED(hr) || code == nullptr)
        return false;
    out.bytes.assign(static_cast<const unsigned char*>(code->GetBufferPointer()),
                     static_cast<const unsigned char*>(code->GetBufferPointer()) + code->GetBufferSize());
    code->Release();
    return true;
}

const ShaderBinding* Find(const ShaderBinding* bindings, size_t count, const char* name)
{
    for (size_t i = 0; i < count; ++i)
    {
        const std::string text(bindings[i].name, bindings[i].nameLength);
        if (text == name)
            return &bindings[i];
    }
    return nullptr;
}
} // namespace

int main()
{
    Blob blob;
    CHECK(Compile(blob)); // the test itself is broken if the compiler is not there
    CHECK(blob.size() > 64);
    CHECK(std::memcmp(blob.data(), "DXBC", 4) == 0);

    ShaderBinding bindings[kMaxBindings];
    uint32_t declared = 0;
    const size_t count = ParseBindings(blob.data(), blob.size(), bindings, kMaxBindings, &declared);
    CHECK_EQ(count, size_t(declared)); // a real shader's table fits
    CHECK_EQ(count, size_t(7));        // five resources of the four declarations, two constant buffers

    // Everything the shader declares is found, with its type, its slot and how many slots it covers.
    const ShaderBinding* depth = Find(bindings, count, "g_SamplerDepth");
    CHECK(depth != nullptr);
    if (depth)
    {
        CHECK_EQ(depth->type, kBindTexture);
        CHECK_EQ(depth->bindPoint, 0u);
        CHECK_EQ(depth->bindCount, 1u);
    }
    const ShaderBinding* gbuffer = Find(bindings, count, "g_SamplerGBuffer");
    CHECK(gbuffer != nullptr);
    if (gbuffer)
    {
        CHECK_EQ(gbuffer->type, kBindTexture);
        CHECK_EQ(gbuffer->bindPoint, 3u); // the slot the engine binds its G-buffer at
    }
    const ShaderBinding* occlusion = Find(bindings, count, "g_SamplerOcclusion");
    CHECK(occlusion != nullptr);
    if (occlusion)
    {
        CHECK_EQ(occlusion->type, kBindTexture);
        CHECK_EQ(occlusion->bindPoint, 5u);
    }
    // A texture array indexed by a constant is declared per element and keeps that element's own slot.
    const ShaderBinding* cascade = Find(bindings, count, "g_SamplerCascade[1]");
    CHECK(cascade != nullptr);
    if (cascade)
    {
        CHECK_EQ(cascade->type, kBindTexture);
        CHECK_EQ(cascade->bindPoint, 9u); // base slot 8 + element 1
        CHECK_EQ(cascade->bindCount, 1u);
    }
    const ShaderBinding* sampler = Find(bindings, count, "g_SamplerLinear");
    CHECK(sampler != nullptr);
    if (sampler)
    {
        CHECK_EQ(sampler->type, kBindSampler);
        CHECK_EQ(sampler->bindPoint, 0u);
    }
    // Constant buffers are told apart from textures and carry their own slot (b0 / b2, not t0 / t2).
    const ShaderBinding* common = Find(bindings, count, "g_CommonParameter");
    CHECK(common != nullptr);
    if (common)
    {
        CHECK_EQ(common->type, kBindConstantBuffer);
        CHECK_EQ(common->bindPoint, 0u);
    }
    const ShaderBinding* light = Find(bindings, count, "g_LightParam");
    CHECK(light != nullptr);
    if (light)
    {
        CHECK_EQ(light->type, kBindConstantBuffer);
        CHECK_EQ(light->bindPoint, 2u);
        CHECK_EQ(std::string(light->name), std::string("g_LightParam")); // NUL terminated right after the name
    }
    // The names are the shader's own; a name that is not declared is not invented.
    CHECK(Find(bindings, count, "m_Direction") == nullptr); // a constant-buffer member is not a bound resource
    CHECK(Find(bindings, count, "g_SamplerNothing") == nullptr);

    // A texture array indexed at run time is declared once at its base slot and covers as many slots as it has elements. The rule the draw
    // path matches a bound slot with is checked here on its own, because no shader this compiler emits for a single slot exercises it. The kind
    // is part of the question, the same way it is for the lookup by name: a slot number means nothing without the right table.
    {
        ShaderBinding array;
        array.name = "g_SamplerLightDiffuse";
        array.nameLength = 20;
        array.type = kBindTexture;
        array.bindPoint = 8;
        array.bindCount = 4;
        CHECK(BindingAnswers(array, BindKind::Texture, 8));
        CHECK(BindingAnswers(array, BindKind::Texture, 11));
        CHECK(!BindingAnswers(array, BindKind::Texture, 7));
        CHECK(!BindingAnswers(array, BindKind::Texture, 12));
        CHECK(!BindingAnswers(array, BindKind::ConstantBuffer, 9));
        CHECK(!BindingAnswers(array, BindKind::Sampler, 8));
        ShaderBinding single = array;
        single.bindCount = 1;
        CHECK(!BindingAnswers(single, BindKind::Texture, 9));
        // A binding whose range was never read covers nothing: an empty range is not "every slot".
        single.bindCount = 0;
        CHECK(!BindingAnswers(single, BindKind::Texture, 8));
    }

    // A smaller destination truncates instead of overflowing, and says how many the table declares.
    {
        ShaderBinding few[2];
        uint32_t total = 0;
        CHECK_EQ(ParseBindings(blob.data(), blob.size(), few, 2, &total), size_t(2));
        CHECK_EQ(total, declared);
    }

    // A DXBC blob built by hand from the container header and one RDEF chunk: the body carries the table header, one 32-byte entry and the
    // name it points at. `terminated` writes the name's NUL; without it the name reaches the end of the body, which is what a truncated or
    // hand-edited reflection looks like. These are the blobs no compiler emits, so the reader's own bounds are what is under test.
    const auto put = [](std::vector<unsigned char>& bytes, size_t at, uint32_t value)
    {
        bytes[at] = static_cast<unsigned char>(value & 0xFF);
        bytes[at + 1] = static_cast<unsigned char>((value >> 8) & 0xFF);
        bytes[at + 2] = static_cast<unsigned char>((value >> 16) & 0xFF);
        bytes[at + 3] = static_cast<unsigned char>((value >> 24) & 0xFF);
    };
    const auto handBuilt = [&put](const char* name, bool terminated)
    {
        const size_t nameAt = 48; // the body header (16 B) and one entry (32 B)
        const size_t nameBytes = std::strlen(name);
        const size_t bodySize = nameAt + nameBytes + (terminated ? 1 : 0);
        std::vector<unsigned char> body(bodySize, 0);
        put(body, 8, 1);      // one bound resource
        put(body, 12, 16);    // its table right after the body header
        put(body, 16 + 0, static_cast<uint32_t>(nameAt)); // the name's offset inside the body
        put(body, 16 + 4, kBindTexture);                  // a texture
        put(body, 16 + 20, 5);                            // at t5
        put(body, 16 + 24, 1);                            // one slot
        std::memcpy(body.data() + nameAt, name, nameBytes);

        const size_t chunkAt = 36; // "DXBC", the 16-byte digest, version, size, one chunk offset
        std::vector<unsigned char> code(chunkAt + 8 + bodySize, 0);
        std::memcpy(code.data(), "DXBC", 4);
        put(code, 28, 1); // one chunk
        put(code, 32, static_cast<uint32_t>(chunkAt));
        std::memcpy(code.data() + chunkAt, "RDEF", 4);
        put(code, chunkAt + 4, static_cast<uint32_t>(bodySize));
        std::memcpy(code.data() + chunkAt + 8, body.data(), bodySize);
        return code;
    };

    {
        ShaderBinding one[4];
        uint32_t total = 0;
        const std::vector<unsigned char> named = handBuilt("g_SamplerDepth", true);
        CHECK_EQ(ParseBindings(named.data(), named.size(), one, 4, &total), size_t(1));
        CHECK_EQ(total, 1u);
        CHECK_EQ(std::string(one[0].name, one[0].nameLength), std::string("g_SamplerDepth")); // the name ends at its NUL
        CHECK_EQ(one[0].type, kBindTexture);
        CHECK_EQ(one[0].bindPoint, 5u);

        // A name with no NUL runs to the end of the blob. The entry is dropped rather than taken as a name that spans the rest of the body:
        // a name that long is not a reflected identifier, and everything after it (a second name, another chunk) would be read as its text.
        // The table itself is intact, so the reader still reports what the shader declared.
        const std::vector<unsigned char> unterminated = handBuilt("g_SamplerDepth", false);
        total = 99;
        CHECK_EQ(ParseBindings(unterminated.data(), unterminated.size(), one, 4, &total), size_t(0));
        CHECK_EQ(total, 1u);
        // A name whose NUL is the body's last byte is legitimate: the bound is the end of the body, not the byte before it.
        const std::vector<unsigned char> lastByte = handBuilt("g_SamplerDepth", true);
        CHECK_EQ(ParseBindings(lastByte.data(), lastByte.size(), one, 4), size_t(1));
        CHECK_EQ(std::string(one[0].name, one[0].nameLength), std::string("g_SamplerDepth"));
    }

    // Anything that is not a well-formed DXBC blob yields no bindings rather than a fault: the blobs come from the game.
    {
        ShaderBinding none[4];
        uint32_t total = 99;
        CHECK_EQ(ParseBindings(nullptr, 0, none, 4, &total), size_t(0));
        CHECK_EQ(ParseBindings(blob.data(), 0, none, 4), size_t(0));
        CHECK_EQ(ParseBindings(blob.data(), 4, none, 4), size_t(0)); // four bytes of "DXBC" and nothing else
        CHECK_EQ(ParseBindings(blob.data(), blob.size(), nullptr, 0), size_t(0));
        std::vector<unsigned char> wrong(blob.bytes);
        wrong[0] = 'X';
        CHECK_EQ(ParseBindings(wrong.data(), wrong.size(), none, 4), size_t(0));
        // A truncated blob: the chunk table is intact, the RDEF body is not.
        CHECK_EQ(ParseBindings(blob.data(), 60, none, 4), size_t(0));
        CHECK_EQ(ParseBindings(blob.data(), blob.size() / 2, none, 4), size_t(0));
        // A chunk count too large to be real.
        std::vector<unsigned char> chunks(blob.bytes);
        chunks[28] = 0xFF;
        chunks[29] = 0xFF;
        chunks[30] = 0xFF;
        chunks[31] = 0x7F;
        CHECK_EQ(ParseBindings(chunks.data(), chunks.size(), none, 4), size_t(0));
        // An RDEF table whose entry count runs past the body.
        std::vector<unsigned char> big(blob.bytes);
        size_t rdef = 0;
        for (size_t i = 0; i + 4 <= big.size(); ++i)
            if (std::memcmp(big.data() + i, "RDEF", 4) == 0)
            {
                rdef = i;
                break;
            }
        CHECK(rdef != 0);
        if (rdef != 0)
        {
            big[rdef + 8 + 8] = 0xFF; // the bound-resource count, first byte of the second DWORD of the body header ... see below
            // the count sits at body + 8; body starts at rdef + 8
            big[rdef + 8 + 8] = 0xFF;
            big[rdef + 8 + 9] = 0xFF;
            CHECK_EQ(ParseBindings(big.data(), big.size(), none, 4), size_t(0));
        }
    }

    // ---- whether a reflection declares a name: the shape of a pass is identified by the names it declares, through the base name
    {
        ShaderBinding table[3] {};
        const char* names[] { "g_WetnessParameter", "g_SamplerGBuffer3.T", "g_SamplerViewPosition.T" };
        for (size_t i = 0; i < 3; ++i)
        {
            table[i].name = names[i];
            table[i].nameLength = static_cast<uint32_t>(std::strlen(names[i]));
        }
        CHECK(ReflectionDeclares(table, 3, "g_WetnessParameter"));
        CHECK(ReflectionDeclares(table, 3, "g_SamplerGBuffer3"));
        CHECK(ReflectionDeclares(table, 3, "g_SamplerGBuffer3.T")); // either form may be passed in
        CHECK(ReflectionDeclares(table, 3, "g_SamplerViewPosition"));
        CHECK(!ReflectionDeclares(table, 3, "g_SamplerGBuffer4"));
        CHECK(!ReflectionDeclares(table, 3, "g_SamplerGBuffer")); // no prefix match
        CHECK(!ReflectionDeclares(table, 0, "g_WetnessParameter"));
        CHECK(!ReflectionDeclares(nullptr, 3, "g_WetnessParameter"));
    }

    // ---- the name a binding is compared by: the effect compiler's type suffix is not part of the variable's name
    {
        CHECK(ResourceBaseName("g_SamplerShadow") == "g_SamplerShadow");
        CHECK(ResourceBaseName("g_SamplerShadow.T") == "g_SamplerShadow"); // the texture half of an effect variable
        CHECK(ResourceBaseName("g_SamplerShadow.S") == "g_SamplerShadow"); // its sampler half
        CHECK(ResourceBaseName("g_LightParam") == "g_LightParam");         // constant buffers keep their plain name
        CHECK(ResourceBaseName("g_SamplerShadowMask.T") == "g_SamplerShadowMask"); // the base name is compared, not a prefix
        CHECK(ResourceBaseName("g_SamplerShadow.X") == "g_SamplerShadow.X"); // anything else is left alone
        CHECK(ResourceBaseName("a.T") == "a");                              // a one-letter variable is still a variable
        CHECK(ResourceBaseName(".T") == ".T");                              // nothing before the dot: not a suffix this rule knows
        CHECK(ResourceBaseName("") == "");
    }

    return Finish("shader_bindings");
}
