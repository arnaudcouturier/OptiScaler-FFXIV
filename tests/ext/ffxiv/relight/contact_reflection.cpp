// Contact shadows: the reflection index (LayoutOfShader in ext/ffxiv/relight/ReflectionIndex.cpp) on shaders compiled in the test, on the
// shape the engine's own bytecode has built by hand, and on two blobs the game itself handed to CreatePixelShader.
//
// The classifier must find the four names a sun light draw declares, at the slots the bytecode really uses, and must refuse every shader
// that does not carry all four - otherwise the pass would read a cascade atlas where the screen-space mask is expected, or bind a copy into
// a draw that never samples it. The D3DCompile blobs here are compiled by the test, so their RDEF chunk has the shape this compiler writes;
// the effect-style blob is built by hand from the name table an in-game capture of the sun light draw recorded, and the two files under
// tests/ext/ffxiv/relight/fixtures are the real blobs that capture kept (see the block that reads them).
#include "check.h"

#include "ext/ffxiv/diag/ShaderBindings.h"
#include "ext/ffxiv/relight/Inject.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace ext::ffxiv::relight;

namespace
{
// Compiles one pixel shader and returns its bytecode (empty on failure).
std::string Compile(const char* source)
{
    Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(source, std::strlen(source), "contact_shadows_test", nullptr, nullptr, "PS", "ps_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr) || code == nullptr)
    {
        if (errors)
            std::printf("    compile failed: %s\n", static_cast<const char*>(errors->GetBufferPointer()));
        return {};
    }
    return std::string(static_cast<const char*>(code->GetBufferPointer()), code->GetBufferSize());
}

// The declarations of the capture's sun light draw, at the slots the capture recorded (the mask at 6, the depth and the view position
// wherever the engine put them, the two constant buffers).
constexpr const char* kSunLight = R"(
Texture2D<float4> g_SamplerGBuffer : register(t0);
Texture2D<float4> g_SamplerShadow : register(t6);
Texture2D<float> g_SamplerDepth : register(t7);
cbuffer g_LightParam : register(b2) { float4 lightA; float4 lightB; };
cbuffer g_CameraParameter : register(b3) { float4 cameraA; };
float4 PS() : SV_Target
{
    return g_SamplerGBuffer.Load(int3(0, 0, 0)) + g_SamplerShadow.Load(int3(0, 0, 0)) +
           g_SamplerDepth.Load(int3(0, 0, 0)) + lightA + lightB + cameraA;
}
)";

// The same shader at different slots: the layout follows the bytecode, never a constant.
constexpr const char* kOtherSlots = R"(
Texture2D<float4> g_SamplerShadow : register(t1);
Texture2D<float> g_SamplerDepth : register(t0);
cbuffer g_LightParam : register(b0) { float4 lightA; };
cbuffer g_CameraParameter : register(b5) { float4 cameraA; };
float4 PS() : SV_Target
{
    return g_SamplerShadow.Load(int3(0, 0, 0)) + g_SamplerDepth.Load(int3(0, 0, 0)) + lightA + cameraA;
}
)";

// A mask but no depth: the mask writer's own shader shape. It must not be classified as a sun light draw.
constexpr const char* kNoDepth = R"(
Texture2D<float4> g_SamplerShadow : register(t6);
cbuffer g_LightParam : register(b2) { float4 lightA; };
cbuffer g_CameraParameter : register(b3) { float4 cameraA; };
float4 PS() : SV_Target
{
    return g_SamplerShadow.Load(int3(0, 0, 0)) + lightA + cameraA;
}
)";

// The mask writer binds the cascade atlas under the same name and has no camera buffer.
constexpr const char* kAtlasWriter = R"(
Texture2D<float> g_SamplerShadow : register(t2);
Texture2D<float> g_SamplerDepth : register(t0);
cbuffer g_LightParam : register(b2) { float4 lightA; };
float4 PS() : SV_Target
{
    return g_SamplerShadow.Load(int3(0, 0, 0)) + g_SamplerDepth.Load(int3(0, 0, 0)) + lightA;
}
)";

// A draw with none of the names: an ordinary geometry pass.
constexpr const char* kGeometry = R"(
Texture2D<float4> sAlbedo : register(t0);
cbuffer g_PerObject : register(b0) { float4 tint; };
float4 PS() : SV_Target { return sAlbedo.Load(int3(0, 0, 0)) * tint; }
)";

// ---- the engine's own shape ----------------------------------------------------------------------------------------------

// The game's shaders are effect-style: one effect variable declares a texture and its sampler together, and the compiler writes the pair
// into the reflection as "<name>.T" (the texture) and "<name>.S" (the sampler state), while a constant buffer keeps its plain name. That
// is what the in-game capture of the sun light draw shows: `0:g_SamplerDepth.T`, `6:g_SamplerShadow.T`, `1:g_CameraParameter`,
// `3:g_LightParam`. The container below is built by hand from that name table, so the rule is checked against the shape the game has
// without storing a byte of the game's own shaders.
struct Bound
{
    const char* name;
    uint32_t type;
    uint32_t bindPoint;
};

void Put32(std::vector<unsigned char>& bytes, size_t at, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        bytes[at + i] = static_cast<unsigned char>(value >> (8 * i));
}

// A DXBC container with one RDEF chunk: the header (no constant-buffer table of its own), the bound-resource table, then the names.
std::vector<unsigned char> EffectStyleBlob(const std::vector<Bound>& entries)
{
    std::vector<unsigned char> rdef(16, 0); // cbCount, cbOffset, bindCount, bindOffset
    Put32(rdef, 8, static_cast<uint32_t>(entries.size()));
    Put32(rdef, 12, 16);
    rdef.resize(16 + entries.size() * 32, 0);
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const size_t at = 16 + i * 32;
        Put32(rdef, at, static_cast<uint32_t>(rdef.size())); // the name follows the table
        Put32(rdef, at + 4, entries[i].type);
        Put32(rdef, at + 20, entries[i].bindPoint);
        Put32(rdef, at + 24, 1);
        const size_t length = std::strlen(entries[i].name) + 1;
        rdef.insert(rdef.end(), entries[i].name, entries[i].name + length);
    }

    std::vector<unsigned char> out { 'D', 'X', 'B', 'C' };
    out.insert(out.end(), 16, 0xAB); // digest, not read
    out.resize(36, 0);               // version (20), total size (24), chunk count (28), the chunk-offset table (32)
    Put32(out, 20, 1);
    Put32(out, 28, 1);
    Put32(out, 32, 36);              // one chunk, right after the offset table
    out.insert(out.end(), { 'R', 'D', 'E', 'F' });
    const size_t bodySizeAt = out.size();
    out.resize(out.size() + 4, 0);
    out.insert(out.end(), rdef.begin(), rdef.end());
    while (out.size() % 4)
        out.push_back(0);
    Put32(out, bodySizeAt, static_cast<uint32_t>(rdef.size()));
    Put32(out, 24, static_cast<uint32_t>(out.size()));
    return out;
}
} // namespace

int main(int argc, char** argv)
{
    // The repository root comes from the test recipe ('{repo}'), so the two real blobs under tests/ext/ffxiv/relight/fixtures are found
    // whatever folder the test was built in.
    const std::string repo = argc > 1 ? argv[1] : ".";

    // ---- the shape of a sun light draw: all four names, at the slots the bytecode declares
    {
        const std::string code = Compile(kSunLight);
        CHECK(!code.empty());
        const Layout layout = LayoutOfShader(code.data(), code.size());
        CHECK(layout.ok);
        CHECK_EQ(layout.maskSlot, 6u);
        CHECK_EQ(layout.depthSlot, 7u);
        CHECK_EQ(layout.lightParamSlot, 2u);
        CHECK_EQ(layout.cameraParamSlot, 3u);
    }

    // ---- the same four names at other slots: the layout follows the shader, not a number in this fork
    {
        const std::string code = Compile(kOtherSlots);
        CHECK(!code.empty());
        const Layout layout = LayoutOfShader(code.data(), code.size());
        CHECK(layout.ok);
        CHECK_EQ(layout.maskSlot, 1u);
        CHECK_EQ(layout.depthSlot, 0u);
        CHECK_EQ(layout.lightParamSlot, 0u);
        CHECK_EQ(layout.cameraParamSlot, 5u);
    }

    // ---- a shader with the mask but no depth, one with no camera buffer (the mask writer), and one with none of the names
    {
        for (const char* source : { kNoDepth, kAtlasWriter, kGeometry })
        {
            const std::string code = Compile(source);
            CHECK(!code.empty());
            const Layout layout = LayoutOfShader(code.data(), code.size());
            CHECK(!layout.ok);
        }
    }

    // ---- a truncated or foreign blob is refused rather than read
    {
        const std::string code = Compile(kSunLight);
        CHECK(!code.empty());
        CHECK(!LayoutOfShader(nullptr, 0).ok);
        CHECK(!LayoutOfShader(code.data(), 16).ok);
        // Every prefix is read without a fault: the reader bounds-checks what it walks, so a cut blob gives some answer instead of a crash.
        // (A cut that still holds the whole reflection table does answer with it, which is why only "does not fault" is asserted here.)
        for (size_t cut = 0; cut < code.size(); cut += 97)
            (void) LayoutOfShader(code.data(), cut);
        const char junk[64] { 'N', 'O', 'T', 'D', 'X', 'B', 'C' };
        CHECK(!LayoutOfShader(junk, sizeof junk).ok);
        // a valid container with a wrong magic is not a DXBC blob the engine could have produced either
        std::string magicless = code;
        magicless[0] = 'X';
        CHECK(!LayoutOfShader(magicless.data(), magicless.size()).ok);
    }

    // ---- the engine's own shape: an effect-style shader whose textures are named `<name>.T` and whose samplers are `<name>.S`
    {
        using ext::ffxiv::diag::kBindConstantBuffer;
        using ext::ffxiv::diag::kBindSampler;
        using ext::ffxiv::diag::kBindTexture;
        const std::vector<Bound> entries {
            { "g_SamplerDepth.T", kBindTexture, 0 },
            { "g_SamplerDepth.S", kBindSampler, 0 },
            { "g_SamplerSubsurfaceKernel.T", kBindTexture, 1 },
            { "g_SamplerGBuffer.T", kBindTexture, 2 },
            { "g_SamplerShadow.T", kBindTexture, 6 },
            { "g_SamplerShadow.S", kBindSampler, 6 },
            { "g_CommonParameter", kBindConstantBuffer, 0 },
            { "g_CameraParameter", kBindConstantBuffer, 1 },
            { "g_ShaderTypeParameter", kBindConstantBuffer, 2 },
            { "g_LightParam", kBindConstantBuffer, 3 },
        };
        const std::vector<unsigned char> blob = EffectStyleBlob(entries);
        const Layout layout = LayoutOfShader(blob.data(), blob.size());
        CHECK(layout.ok);
        CHECK_EQ(layout.maskSlot, 6u);
        CHECK_EQ(layout.depthSlot, 0u);
        CHECK_EQ(layout.lightParamSlot, 3u);
        CHECK_EQ(layout.cameraParamSlot, 1u);
    }

    // ---- the same shape without the mask's texture half: the sampler state alone is not the mask, so the shader is refused
    {
        using ext::ffxiv::diag::kBindConstantBuffer;
        using ext::ffxiv::diag::kBindSampler;
        using ext::ffxiv::diag::kBindTexture;
        const std::vector<Bound> entries {
            { "g_SamplerDepth.T", kBindTexture, 0 },
            { "g_SamplerShadow.S", kBindSampler, 6 },
            { "g_CameraParameter", kBindConstantBuffer, 1 },
            { "g_LightParam", kBindConstantBuffer, 3 },
        };
        const std::vector<unsigned char> blob = EffectStyleBlob(entries);
        const Layout layout = LayoutOfShader(blob.data(), blob.size());
        CHECK(!layout.ok);
        CHECK_EQ(layout.found, 2u | 4u | 8u); // the depth and both buffers, no mask
    }

    // ---- the real bytecode: two pixel shaders a benchmark session handed to CreatePixelShader, kept by the draw diagnostic's fixture dump
    // (the engine's effect-style names, `<name>.T` for a texture and `<name>.S` for its sampler). The sun light draw's shape - the mask at
    // t1, the depth at t0, the light parameters at b5 and the camera buffer at b1, the slots the module's own "shader indexed" line reported -
    // has to be recognised. The mask writer's shape, which declares the mask, the depth and the camera buffer but no light parameters, has
    // to be refused.
    {
        const auto read = [&repo](const char* name) {
            std::ifstream file(repo + "/tests/ext/ffxiv/relight/fixtures/" + name, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        };
        const std::string sun = read("sun_light_0x31882942.dxbc");
        CHECK(!sun.empty());
        const Layout layout = LayoutOfShader(sun.data(), sun.size());
        CHECK(layout.ok);
        CHECK_EQ(layout.found, 15u);
        CHECK_EQ(layout.maskSlot, 1u);
        CHECK_EQ(layout.depthSlot, 0u);
        CHECK_EQ(layout.lightParamSlot, 5u);
        CHECK_EQ(layout.cameraParamSlot, 1u);

        const std::string writer = read("mask_writer_0x8D03841E.dxbc");
        CHECK(!writer.empty());
        const Layout refused = LayoutOfShader(writer.data(), writer.size());
        CHECK(!refused.ok);
        CHECK_EQ(refused.found, 1u | 2u | 8u); // the mask, the depth and the camera buffer, no g_LightParam
    }

    return Finish("contact_shadows_reflection");
}
