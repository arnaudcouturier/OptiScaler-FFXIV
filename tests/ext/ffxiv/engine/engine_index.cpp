// Engine pass overrides: the shader index (PassLayoutOfShader in ext/ffxiv/engine/PassIndex.cpp) on shaders compiled in the test.
//
// The index must find the names a draw is keyed on at the slots its bytecode really uses, must keep a texture out of a buffer's slot, must
// answer "not mine" for an ordinary geometry pass, and must refuse a truncated or foreign blob instead of reading it - the bytecode comes
// from the game, so a patch that changes it has to fail open rather than fault.
#include "check.h"

#include "ext/ffxiv/engine/PassIndex.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <string>
#pragma comment(lib, "d3dcompiler.lib")

using namespace ext::ffxiv::engine;

namespace
{
// Compiles one pixel shader and returns its bytecode (empty on failure).
std::string Compile(const char* source)
{
    Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(source, std::strlen(source), "engine_pass_test", nullptr, nullptr, "PS", "ps_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr) || code == nullptr)
    {
        if (errors)
            std::printf("    compile failed: %s\n", static_cast<const char*>(errors->GetBufferPointer()));
        return {};
    }
    return std::string(static_cast<const char*>(code->GetBufferPointer()), code->GetBufferSize());
}

// The composite's declarations at the slots a capture of the deferred composite recorded: the ambient table at b2, the diffuse light at t4.
constexpr const char* kComposite = R"(
Texture2D<float4> g_SamplerLightDiffuse : register(t4);
Texture2D<float4> g_SamplerReflectionArray : register(t0);
cbuffer g_AmbientParamArray : register(b2) { float4 ambientA; float4 ambientB; };
cbuffer g_CommonParameter : register(b0) { float4 common; };
float4 PS() : SV_Target
{
    return g_SamplerLightDiffuse.Load(int3(0, 0, 0)) + g_SamplerReflectionArray.Load(int3(0, 0, 0)) + ambientA + ambientB + common;
}
)";

// The same names at other slots: the layout follows the bytecode, never a constant in this fork.
constexpr const char* kOtherSlots = R"(
Texture2D<float4> g_SamplerLightDiffuse : register(t0);
cbuffer g_AmbientParamArray : register(b0) { float4 ambientA; };
float4 PS() : SV_Target { return g_SamplerLightDiffuse.Load(int3(0, 0, 0)) + ambientA; }
)";

// A buffer of the index at its own slot, two render targets: the output count is part of what the index remembers.
constexpr const char* kTwoTargets = R"(
cbuffer cBrightPassParam : register(b1) { float4 bright; };
struct Out { float4 a : SV_Target0; float4 b : SV_Target1; };
Out PS() { Out o; o.a = bright; o.b = bright; return o; }
)";

// Every name of the table except the knob's two anchors: the wetness buffer, the tone map's and the bright pass's parameters.
constexpr const char* kLaterKnobs = R"(
cbuffer g_WetnessParameter : register(b1) { float4 wetness; };
cbuffer cCommonTexParam : register(b0) { float4 common; };
float4 PS() : SV_Target { return wetness + common; }
)";

// An ordinary geometry draw: none of the names.
constexpr const char* kGeometry = R"(
Texture2D<float4> sAlbedo : register(t0);
cbuffer g_PerObject : register(b0) { float4 tint; };
float4 PS() : SV_Target { return sAlbedo.Load(int3(0, 0, 0)) * tint; }
)";

// A texture that carries a buffer's name: the kind is part of the match, so it must not take the buffer's slot.
constexpr const char* kTextureNamedLikeBuffer = R"(
Texture2D<float4> g_AmbientParamArray : register(t5);
float4 PS() : SV_Target { return g_AmbientParamArray.Load(int3(0, 0, 0)); }
)";

// The sky's own sun sprite, at the slot a capture recorded it (b1): cSunParam alone, and no light parameter name beside it.
constexpr const char* kSunParam = R"(
cbuffer cSunParam : register(b1) { float4 sunRow0; float4 sunRow1; };
float4 PS() : SV_Target { return sunRow0 + sunRow1; }
)";
} // namespace

int main()
{
    // ---- the composite: both names, at the slots the bytecode declares, one output
    {
        const std::string code = Compile(kComposite);
        CHECK(!code.empty());
        const PassLayout layout = PassLayoutOfShader(code.data(), code.size());
        CHECK(layout.ok);
        CHECK(layout.Has(0) && layout.Has(1));
        CHECK_EQ(layout.SlotOf(0), 2u); // g_AmbientParamArray
        CHECK_EQ(layout.SlotOf(1), 4u); // g_SamplerLightDiffuse (a texture: the name is matched after the .T suffix is stripped)
        CHECK(!layout.Has(3) && !layout.Has(4)); // the tone map's and the bright pass's buffers are not declared
        CHECK_EQ(layout.outputs, 1u);
    }

    // ---- the sky's own sun: one name, its own anchors, and no light pass's
    {
        const std::string code = Compile(kSunParam);
        CHECK(!code.empty());
        const PassLayout layout = PassLayoutOfShader(code.data(), code.size());
        CHECK(layout.ok);
        CHECK(layout.Has(12));
        CHECK_EQ(layout.SlotOf(12), 1u);
        CHECK(!layout.Has(6)); // the shadow-casting light's parameters belong to another draw
        CHECK(IsSunParamDraw(layout));
        const Anchors anchors = DrawAnchors(layout);
        CHECK(IsShaftsSunParamAnchors(anchors) && !IsShaftsLightAnchors(anchors));
        CHECK_EQ(ShaftsSlotOf(anchors), 1u);
    }

    // ---- the same names at other slots: the layout follows the shader
    {
        const std::string code = Compile(kOtherSlots);
        CHECK(!code.empty());
        const PassLayout layout = PassLayoutOfShader(code.data(), code.size());
        CHECK(layout.ok);
        CHECK_EQ(layout.SlotOf(0), 0u);
        CHECK_EQ(layout.SlotOf(1), 0u);
    }

    // ---- two render targets, and a buffer the later knobs patch
    {
        const std::string code = Compile(kTwoTargets);
        CHECK(!code.empty());
        const PassLayout layout = PassLayoutOfShader(code.data(), code.size());
        CHECK(layout.ok);
        CHECK(!layout.Has(0));
        CHECK(layout.Has(4)); // cBrightPassParam
        CHECK_EQ(layout.SlotOf(4), 1u);
        CHECK_EQ(layout.outputs, 2u);
    }
    {
        const std::string code = Compile(kLaterKnobs);
        CHECK(!code.empty());
        const PassLayout layout = PassLayoutOfShader(code.data(), code.size());
        CHECK(layout.ok);
        CHECK(layout.Has(2) && layout.Has(3));
        CHECK_EQ(layout.SlotOf(2), 1u);
        CHECK_EQ(layout.SlotOf(3), 0u);
        CHECK_EQ(layout.outputs, 1u);
    }

    // ---- a geometry pass is not in the index at all
    {
        const std::string code = Compile(kGeometry);
        CHECK(!code.empty());
        const PassLayout layout = PassLayoutOfShader(code.data(), code.size());
        CHECK(!layout.ok);
        CHECK_EQ(layout.found, 0u);
        CHECK_EQ(layout.outputs, 1u); // the output count is still read: it costs one chunk walk
    }

    // ---- a texture named like the ambient buffer does not take its slot
    {
        const std::string code = Compile(kTextureNamedLikeBuffer);
        CHECK(!code.empty());
        const PassLayout layout = PassLayoutOfShader(code.data(), code.size());
        CHECK(!layout.ok);
        CHECK_EQ(layout.found, 0u);
    }

    // ---- a truncated or foreign blob is refused rather than read
    {
        const std::string code = Compile(kComposite);
        CHECK(!code.empty());
        CHECK(!PassLayoutOfShader(nullptr, 0).ok);
        CHECK(!PassLayoutOfShader(code.data(), 16).ok);
        // every prefix is read without a fault: the readers bounds-check what they walk
        for (size_t cut = 0; cut < code.size(); cut += 97)
        {
            (void) PassLayoutOfShader(code.data(), cut);
            (void) OutputCountOfShader(code.data(), cut);
        }
        const char junk[64] { 'N', 'O', 'T', 'D', 'X', 'B', 'C' };
        CHECK(!PassLayoutOfShader(junk, sizeof junk).ok);
        CHECK_EQ(OutputCountOfShader(junk, sizeof junk), 0u);
        CHECK_EQ(OutputCountOfShader(nullptr, 0), 0u);
        std::string magicless = code;
        magicless[0] = 'X';
        CHECK(!PassLayoutOfShader(magicless.data(), magicless.size()).ok);
        CHECK_EQ(OutputCountOfShader(magicless.data(), magicless.size()), 0u);
    }

    // ---- the output count on its own
    {
        const std::string two = Compile(kTwoTargets);
        CHECK(!two.empty());
        CHECK_EQ(OutputCountOfShader(two.data(), two.size()), 2u);
    }

    return Finish("engine_pass_index");
}
