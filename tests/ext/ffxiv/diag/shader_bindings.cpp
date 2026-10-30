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
    // path matches a bound slot with is checked here on its own, because no shader this compiler emits for a single slot exercises it.
    {
        ShaderBinding array;
        array.name = "g_SamplerLightDiffuse";
        array.nameLength = 20;
        array.type = kBindTexture;
        array.bindPoint = 8;
        array.bindCount = 4;
        CHECK(BindingCovers(array, kBindTexture, 8));
        CHECK(BindingCovers(array, kBindTexture, 11));
        CHECK(!BindingCovers(array, kBindTexture, 7));
        CHECK(!BindingCovers(array, kBindTexture, 12));
        CHECK(!BindingCovers(array, kBindConstantBuffer, 9)); // a slot number means nothing without the right table
        ShaderBinding single = array;
        single.bindCount = 1;
        CHECK(!BindingCovers(single, kBindTexture, 9));
    }

    // A smaller destination truncates instead of overflowing, and says how many the table declares.
    {
        ShaderBinding few[2];
        uint32_t total = 0;
        CHECK_EQ(ParseBindings(blob.data(), blob.size(), few, 2, &total), size_t(2));
        CHECK_EQ(total, declared);
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

    return Finish("shader_bindings");
}
