// One typed binding lookup for every module: the reflection a pixel shader carries lists a texture and its sampler under one variable name -
// the effect compiler writes the pair as `<name>.T` (the texture) and `<name>.S` (the sampler state), the sampler FIRST, at its own register.
// A scan that compared names alone therefore answered the sampler's slot for a texture, which is a different texture at the draw. The lookup
// that reads a slot asks for the kind it binds (diag::BindPointOf, ext/ffxiv/diag/ShaderBindings.h) and every module's slot function goes
// through it: the reflections, the ambient occlusion, the ground wetness, the shader index and the contact shadow index.
//
// The fixture below is a real HLSL shader compiled in this test, with explicit `register(sN)` and `register(tM)` declarations whose two numbers
// differ, so the two halves of every name land at different bind points. fxc writes the samplers first (checked by hand with `fxc -dumpbin`:
// type 3 entries, ascending s register, precede every type 2 entry) and cannot produce the `.T`/`.S` names themselves, so this test gives the
// compiled entries the effect compiler's names by rewriting the name strings of the reflection in place. Nothing else of the blob is touched.
//
// The text pin beside it takes the parser's callers from the tree (every source under ext/ that calls diag::ParseBindings) and holds each of them
// to the typed lookup: none of them reads a binding's type itself, whichever way the comparison is spelled.
#include "check.h"

#include "ext/ffxiv/diag/ShaderBindings.h"
#include "ext/ffxiv/engine/AoRules.h"
#include "ext/ffxiv/engine/GlossPuddlesRules.h"
#include "ext/ffxiv/engine/PassIndex.h"
#include "ext/ffxiv/engine/SsrRules.h"
#include "ext/ffxiv/relight/Inject.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>
#pragma comment(lib, "d3dcompiler.lib")

using namespace ext::ffxiv;
namespace ao = ext::ffxiv::engine::ao;
namespace gloss = ext::ffxiv::engine::gloss;
namespace engine = ext::ffxiv::engine;
namespace ssr = ext::ffxiv::engine::ssr;
namespace relight = ext::ffxiv::relight;

namespace
{
// The fixture: every name the modules read, each declared as a sampler and as a texture at explicit and DIFFERENT registers, the samplers
// first as the game's compiler writes them, plus the seven constant buffers the modules look up. Every resource is read in the body, so no
// compiler pass can drop a declaration out of the reflection.
constexpr const char* kFixture = R"(
SamplerState g_SamplerDepthState : register(s0);
SamplerState g_SamplerReflectionMapState : register(s1);
SamplerState g_SamplerGBuffer1State : register(s2);
SamplerState g_SamplerVPositionState : register(s3);
SamplerState g_SamplerGBuffer2State : register(s4);
SamplerState g_SamplerGBufferState : register(s5);
SamplerState g_SamplerWaveMapState : register(s6);
SamplerState g_SamplerGBuffer3State : register(s7);
SamplerState g_SamplerShadowState : register(s8);
SamplerState g_SamplerOcclusionState : register(s9);
SamplerState g_SamplerLightDiffuseState : register(s10);

Texture2D<float4> g_SamplerDepthTexture : register(t1);
Texture2D<float4> g_SamplerGBuffer1Texture : register(t2);
Texture2D<float4> g_SamplerGBuffer2Texture : register(t3);
Texture2D<float4> g_SamplerReflectionMapTexture : register(t4);
Texture2D<float4> g_SamplerGBuffer3Texture : register(t5);
Texture2D<float4> g_SamplerVPositionTexture : register(t6);
Texture2D<float4> g_SamplerGBufferTexture : register(t7);
Texture2D<float4> g_SamplerWaveMapTexture : register(t9);
Texture2D<float4> g_SamplerShadowTexture : register(t10);
Texture2D<float4> g_SamplerOcclusionTexture : register(t11);
Texture2D<float4> g_SamplerLightDiffuseTexture : register(t12);

cbuffer cCommonTexParam : register(b0) { float4 commonTexParam; };
cbuffer g_CameraParameter : register(b1) { float4 cameraRows[16]; };
cbuffer g_AmbientParamArray : register(b2) { float4 ambientRows[4]; };
cbuffer g_WetnessParameter : register(b3) { float4 wetnessParam; };
cbuffer cBrightPassParam : register(b4) { float4 brightPassParam; };
cbuffer g_LightParam : register(b5) { float4 lightParam[4]; };
cbuffer cSunParam : register(b6) { float4 sunParam[8]; };

float4 PS(float4 position : SV_Position) : SV_Target0
{
    const int2 texel = int2(position.xy);
    const float2 uv = position.xy * 0.001;
    float4 sum = 0;
    sum += g_SamplerDepthTexture.Load(int3(texel, 0)) + g_SamplerDepthTexture.SampleLevel(g_SamplerDepthState, uv, 0);
    sum += g_SamplerGBuffer1Texture.Load(int3(texel, 0)) + g_SamplerGBuffer1Texture.SampleLevel(g_SamplerGBuffer1State, uv, 0);
    sum += g_SamplerGBuffer2Texture.Load(int3(texel, 0)) + g_SamplerGBuffer2Texture.SampleLevel(g_SamplerGBuffer2State, uv, 0);
    sum += g_SamplerGBuffer3Texture.Load(int3(texel, 0)) + g_SamplerGBuffer3Texture.SampleLevel(g_SamplerGBuffer3State, uv, 0);
    sum += g_SamplerGBufferTexture.Load(int3(texel, 0)) + g_SamplerGBufferTexture.SampleLevel(g_SamplerGBufferState, uv, 0);
    sum += g_SamplerReflectionMapTexture.Load(int3(texel, 0)) + g_SamplerReflectionMapTexture.SampleLevel(g_SamplerReflectionMapState, uv, 0);
    sum += g_SamplerVPositionTexture.Load(int3(texel, 0)) + g_SamplerVPositionTexture.SampleLevel(g_SamplerVPositionState, uv, 0);
    sum += g_SamplerWaveMapTexture.Load(int3(texel, 0)) + g_SamplerWaveMapTexture.SampleLevel(g_SamplerWaveMapState, uv, 0);
    sum += g_SamplerShadowTexture.Load(int3(texel, 0)) + g_SamplerShadowTexture.SampleLevel(g_SamplerShadowState, uv, 0);
    sum += g_SamplerOcclusionTexture.Load(int3(texel, 0)) + g_SamplerOcclusionTexture.SampleLevel(g_SamplerOcclusionState, uv, 0);
    sum += g_SamplerLightDiffuseTexture.Load(int3(texel, 0)) + g_SamplerLightDiffuseTexture.SampleLevel(g_SamplerLightDiffuseState, uv, 0);
    sum += commonTexParam + cameraRows[0] + ambientRows[0] + wetnessParam + brightPassParam + lightParam[0] + sunParam[0];
    return sum;
}
)";

std::string Compile(const char* source)
{
    Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(source, std::strlen(source), "slot_kind_fixture", nullptr, nullptr, "PS", "ps_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr) || code == nullptr)
    {
        if (errors)
            std::printf("    compile failed: %s\n", static_cast<const char*>(errors->GetBufferPointer()));
        return {};
    }
    return std::string(static_cast<const char*>(code->GetBufferPointer()), code->GetBufferSize());
}

uint32_t Dword(const unsigned char* bytes, size_t at) { return uint32_t(bytes[at]) | (uint32_t(bytes[at + 1]) << 8) | (uint32_t(bytes[at + 2]) << 16) | (uint32_t(bytes[at + 3]) << 24); }

// The RDEF body of the compiled blob, or null: the same container layout ext/ffxiv/diag/ShaderBindings.h documents.
const unsigned char* RdefBody(std::string& blob, size_t* bodySize)
{
    auto* bytes = reinterpret_cast<const unsigned char*>(blob.data());
    if (blob.size() < 36 || std::memcmp(bytes, "DXBC", 4) != 0)
        return nullptr;
    const uint32_t chunks = Dword(bytes, 28);
    for (uint32_t i = 0; i < chunks; ++i)
    {
        const size_t at = Dword(bytes, 32 + size_t(i) * 4);
        if (at + 8 > blob.size() || std::memcmp(bytes + at, "RDEF", 4) != 0)
            continue;
        const size_t size = Dword(bytes, at + 4);
        if (at + 8 + size > blob.size())
            return nullptr;
        *bodySize = size;
        return bytes + at + 8;
    }
    return nullptr;
}

// Gives one compiled entry the effect compiler's name (`g_SamplerDepth` -> `g_SamplerDepth.S`). HLSL cannot declare a name with a dot, so the
// compiled name string is rewritten in place; a new name has to fit the old one's bytes, which the fixture's names do.
bool RenameBinding(std::string& blob, std::string_view from, std::string_view to)
{
    size_t bodySize = 0;
    unsigned char* body = const_cast<unsigned char*>(RdefBody(blob, &bodySize));
    if (body == nullptr || from.size() < to.size())
        return false;
    const uint32_t count = Dword(body, 8);
    const uint32_t tableAt = Dword(body, 12);
    size_t renamed = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        const size_t entry = tableAt + size_t(i) * 32;
        if (entry + 32 > bodySize)
            return false;
        const uint32_t nameAt = Dword(body, entry);
        if (nameAt >= bodySize)
            continue;
        const std::string_view name(reinterpret_cast<const char*>(body + nameAt));
        if (name != from)
            continue;
        if (nameAt + to.size() + 1 > bodySize)
            return false;
        std::memset(body + nameAt, 0, from.size() + 1);
        std::memcpy(body + nameAt, to.data(), to.size());
        ++renamed;
    }
    return renamed == 1;
}

// The effect compiler's names for every half of the fixture, from the compiled name to the reflected one.
struct Rename
{
    const char* from;
    const char* to;
};
constexpr Rename kRenames[] {
    { "g_SamplerDepthState", "g_SamplerDepth.S" },                 { "g_SamplerDepthTexture", "g_SamplerDepth.T" },
    { "g_SamplerGBuffer1State", "g_SamplerGBuffer1.S" },           { "g_SamplerGBuffer1Texture", "g_SamplerGBuffer1.T" },
    { "g_SamplerGBuffer2State", "g_SamplerGBuffer2.S" },           { "g_SamplerGBuffer2Texture", "g_SamplerGBuffer2.T" },
    { "g_SamplerGBuffer3State", "g_SamplerGBuffer3.S" },           { "g_SamplerGBuffer3Texture", "g_SamplerGBuffer3.T" },
    { "g_SamplerGBufferState", "g_SamplerGBuffer.S" },           { "g_SamplerGBufferTexture", "g_SamplerGBuffer.T" },
    { "g_SamplerReflectionMapState", "g_SamplerReflectionMap.S" }, { "g_SamplerReflectionMapTexture", "g_SamplerReflectionMap.T" },
    { "g_SamplerVPositionState", "g_SamplerVPosition.S" },         { "g_SamplerVPositionTexture", "g_SamplerVPosition.T" },
    { "g_SamplerWaveMapState", "g_SamplerWaveMap.S" },             { "g_SamplerWaveMapTexture", "g_SamplerWaveMap.T" },
    { "g_SamplerShadowState", "g_SamplerShadow.S" },               { "g_SamplerShadowTexture", "g_SamplerShadow.T" },
    { "g_SamplerOcclusionState", "g_SamplerOcclusion.S" },         { "g_SamplerOcclusionTexture", "g_SamplerOcclusion.T" },
    { "g_SamplerLightDiffuseState", "g_SamplerLightDiffuse.S" },   { "g_SamplerLightDiffuseTexture", "g_SamplerLightDiffuse.T" },
};

// ---- the text pins: where the slot decisions live ------------------------------------------------------------

std::string ReadFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// Comments and whitespace removed, so a pin is about the code and not about its formatting.
std::string Code(const std::string& text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text.compare(i, 2, "//") == 0)
        {
            while (i < text.size() && text[i] != '\n')
                ++i;
            continue;
        }
        if (text.compare(i, 2, "/*") == 0)
        {
            const size_t end = text.find("*/", i + 2);
            i = end == std::string::npos ? text.size() : end + 1;
            continue;
        }
        const char c = text[i];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            out += c;
    }
    return out;
}

bool Has(const std::string& text, std::string_view needle) { return text.find(needle) != std::string::npos; }
} // namespace

int main(int, char** argv)
{
    const std::string repo = argv[1];

    // ---- the compiled fixture: the samplers come first, at their own registers
    {
        std::string blob = Compile(kFixture);
        CHECK(!blob.empty());

        diag::ShaderBinding parsed[diag::kMaxBindings];
        const size_t count = diag::ParseBindings(blob.data(), blob.size(), parsed, diag::kMaxBindings);
        CHECK_EQ(count, size_t(29)); // 11 samplers, 11 textures and 7 constant buffers

        // The game's compiler writes the samplers before the textures. This is the order the whole class of defects came from: a name-only scan
        // takes the first half of a pair, which is the sampler's slot. The registers of the two halves differ here on purpose.
        size_t samplers = 0, textures = 0, buffers = 0;
        for (size_t i = 0; i < count; ++i)
        {
            const uint32_t type = parsed[i].type;
            if (type == diag::kBindSampler)
            {
                CHECK_EQ(samplers + textures + buffers, samplers); // no texture or buffer before a sampler
                ++samplers;
            }
            else if (type == diag::kBindTexture)
            {
                CHECK_EQ(samplers + textures + buffers, samplers + textures); // and no buffer before a texture
                ++textures;
            }
            else if (type == diag::kBindConstantBuffer)
                ++buffers;
        }
        CHECK_EQ(samplers, size_t(11));
        CHECK_EQ(textures, size_t(11));
        CHECK_EQ(buffers, size_t(7));

        // Before the rename the halves are named for this fixture (`...State` / `...Texture`), which is not what the game's reflection carries:
        // the effect compiler's `.S`/`.T` names are what a slot lookup sees in a session, so the compiled table answers nothing under those names
        // yet, and only the constant buffer (which keeps its plain name) is found.
        const ssr::ReaderSlots plain = ssr::SlotsOfBindings(parsed, count);
        CHECK_EQ(plain.reflection, uint32_t(ssr::kSlotMissing));
        CHECK_EQ(plain.vposition, uint32_t(ssr::kSlotMissing));
        CHECK_EQ(plain.camera, 1u);
        CHECK(!plain.water);

        for (const Rename& rename : kRenames)
        {
            if (!RenameBinding(blob, rename.from, rename.to))
            {
                std::printf("    rename failed: %s -> %s\n", rename.from, rename.to);
                CHECK(false);
            }
        }
        CHECK_EQ(diag::ParseBindings(blob.data(), blob.size(), parsed, diag::kMaxBindings), count);

        // ---- the reflections
        const ssr::ReaderSlots slots = ssr::SlotsOfBindings(parsed, count);
        CHECK_EQ(slots.reflection, 4u); // `g_SamplerReflectionMap.T`; a name-only scan answers the sampler at 1
        CHECK_EQ(slots.vposition, 6u);  // `g_SamplerVPosition.T`; the sampler is at 3
        CHECK_EQ(slots.camera, 1u);
        CHECK(slots.water); // the wave map at t9 and its sampler at s6
        CHECK(slots.Complete());
        CHECK(ssr::IsWaterReader(slots));

        // The same answers from the bytecode, the entry point the product's shader-creation hook calls.
        const ssr::ReaderSlots fromBytes = ssr::SlotsOfShader(blob.data(), blob.size());
        CHECK_EQ(fromBytes.reflection, slots.reflection);
        CHECK_EQ(fromBytes.vposition, slots.vposition);
        CHECK_EQ(fromBytes.camera, slots.camera);
        CHECK_EQ(fromBytes.water, slots.water);

        // The lookup itself, on the table the effect compiler would have written: the two halves of one name sit at their own registers, which
        // is what makes the kind necessary; and a buffer name asked for as a texture is nothing, so the two spaces cannot be crossed.
        CHECK_EQ(diag::BindPointOf(parsed, count, diag::BindKind::Sampler, "g_SamplerReflectionMap"), 1u);
        CHECK_EQ(diag::BindPointOf(parsed, count, diag::BindKind::Texture, "g_SamplerReflectionMap"), 4u);
        CHECK_EQ(diag::BindPointOf(parsed, count, diag::BindKind::Sampler, "g_SamplerGBuffer3"), 7u);
        CHECK_EQ(diag::BindPointOf(parsed, count, diag::BindKind::Texture, "g_SamplerGBuffer3"), 5u);
        CHECK_EQ(diag::BindPointOf(parsed, count, diag::BindKind::Texture, "g_CameraParameter"), diag::kNoBindPoint);
        CHECK_EQ(diag::BindPointOf(parsed, count, diag::BindKind::ConstantBuffer, "g_CameraParameter"), 1u);
        CHECK_EQ(diag::BindPointOf(parsed, count, diag::BindKind::Texture, "g_SamplerNotThere"), diag::kNoBindPoint);
        CHECK_EQ(diag::BindPointOf(nullptr, 0, diag::BindKind::Texture, "g_SamplerDepth"), diag::kNoBindPoint);

        // ---- the ambient occlusion
        const ao::Inputs aoInputs = ao::InputsOfBindings(parsed, count);
        CHECK_EQ(uint32_t(aoInputs.depth), 1u);  // `g_SamplerDepth.T`; the sampler is at 0
        CHECK_EQ(uint32_t(aoInputs.normal), 7u); // `g_SamplerGBuffer.T`; the sampler is at 5
        CHECK_EQ(uint32_t(aoInputs.camera), 1u);
        CHECK(aoInputs.Complete());

        // ---- the ground wetness
        const gloss::Inputs glossInputs = gloss::InputsFromBindings(parsed, count);
        CHECK_EQ(uint32_t(glossInputs.depth), 1u);
        CHECK_EQ(uint32_t(glossInputs.camera), 1u);
        CHECK_EQ(uint32_t(glossInputs.targets[0]), 2u); // g_SamplerGBuffer1: t2, s2 - the sampler's register is a texture's here
        CHECK_EQ(uint32_t(glossInputs.targets[1]), 3u);
        CHECK_EQ(uint32_t(glossInputs.targets[2]), 5u); // g_SamplerGBuffer3: t5, s7 - a name-only scan would answer 7
        CHECK_EQ(uint32_t(glossInputs.targets[3]), 7u); // g_SamplerGBuffer: t7, s5
        CHECK(glossInputs.Complete(3));

        // ---- the shader index (engine pass overrides)
        const engine::PassLayout layout = engine::PassLayoutOfShader(blob.data(), blob.size());
        CHECK(layout.ok);
        CHECK_EQ(layout.found, 0x1FFFu); // every row of the shared name table is in this fixture
        CHECK_EQ(layout.SlotOf(0), 2u);   // g_AmbientParamArray, a buffer
        CHECK_EQ(layout.SlotOf(1), 12u);  // g_SamplerLightDiffuse, the texture's register; its sampler is at 10
        CHECK_EQ(layout.SlotOf(2), 3u);   // g_WetnessParameter
        CHECK_EQ(layout.SlotOf(3), 0u);   // cCommonTexParam
        CHECK_EQ(layout.SlotOf(4), 4u);   // cBrightPassParam
        CHECK_EQ(layout.SlotOf(5), 11u);  // g_SamplerOcclusion, t11
        CHECK_EQ(layout.SlotOf(6), 5u);   // g_LightParam
        CHECK_EQ(layout.SlotOf(7), 2u);   // g_SamplerGBuffer1
        CHECK_EQ(layout.SlotOf(8), 3u);   // g_SamplerGBuffer2
        CHECK_EQ(layout.SlotOf(9), 5u);   // g_SamplerGBuffer3
        CHECK_EQ(layout.SlotOf(10), 7u);  // g_SamplerGBuffer
        CHECK_EQ(layout.SlotOf(11), 4u);  // g_SamplerReflectionMap, the texture's register; its sampler is at 1
        CHECK_EQ(layout.SlotOf(12), 6u);  // cSunParam, the sky's visual sun
        CHECK(engine::IsReflectionDraw(layout));
        CHECK(engine::IsSunParamDraw(layout));

        // ---- the contact shadow index
        const relight::Layout contact = relight::LayoutOfShader(blob.data(), blob.size());
        CHECK(contact.ok);
        CHECK_EQ(contact.found, 15u);
        CHECK_EQ(contact.maskSlot, 10u);      // g_SamplerShadow.T; the sampler is at 8
        CHECK_EQ(contact.depthSlot, 1u);      // g_SamplerDepth.T
        CHECK_EQ(contact.lightParamSlot, 5u); // g_LightParam, a buffer
        CHECK_EQ(contact.cameraParamSlot, 1u);
    }

    // ---- the text pins: every file that reads the parsed table goes through the typed lookup, and none of them compares a binding's type by hand
    //
    // The files are taken from the tree rather than from a list here: every header or source under ext/ that calls diag::ParseBindings is a caller,
    // and every one that names the parsed row type is a reader of the table even when it does not parse it - a consumer that receives the table
    // from a caller. `.hpp` is scanned beside `.h` and `.cpp`, because a header is as much a caller as a source file. The rules below then apply
    // to every one of them, so a module that starts reading the game's reflection tomorrow is pinned the day it is written. The pin is on the
    // token (`.type`, `->type`, `BindingCovers(`, `BindingCovers<`), not on one spelling of a comparison: `switch (bind.type)`, `auto t =
    // b.type;`, `bind.type < kBindTexture` and a hand-fed `BindingCovers(bind, type, slot)` all read the type, and all of them are the defect
    // this pin exists for.
    {
        const std::string ext = repo + "/OptiScaler/ext";
        constexpr const char* kExtensions[] { ".h", ".cpp", ".hpp" };
        std::vector<std::filesystem::path> callers; // files that parse a shader's bytecode
        std::vector<std::filesystem::path> readers; // those, and the files that name the parsed row without parsing
        for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(ext))
        {
            if (!entry.is_regular_file())
                continue;
            const std::string extension = entry.path().extension().string();
            if (std::find(std::begin(kExtensions), std::end(kExtensions), extension) == std::end(kExtensions))
                continue;
            const std::string code = Code(ReadFile(entry.path().string()));
            const bool parses = Has(code, "ParseBindings(");
            if (parses)
                callers.push_back(entry.path());
            if (parses || Has(code, "ShaderBinding"))
                readers.push_back(entry.path());
        }
        // A scan that matched nothing would pass every check below, so the set is held to the files that exist: the parser's own header, the
        // reflections' rules header, the two wrappers that hand a parsed table to the occlusion and wetness rules, the shader index, the contact
        // shadow index and the draw diagnostic call the parser; five more name the row without calling it (the format header, the two rules
        // headers above, and the two apply files that only include the reader).
        CHECK(callers.size() >= size_t(7));
        CHECK(readers.size() >= size_t(12));
        std::vector<std::string> found;
        for (const std::filesystem::path& caller : callers)
            found.push_back(caller.filename().string());
        for (const char* expected : { "ShaderBindings.h", "SsrRules.h", "PassIndex.cpp", "ReflectionIndex.cpp", "AoApply.cpp",
                                      "GlossPuddlesApply.cpp", "FfxivDrawDiag.cpp" })
            CHECK(std::find(found.begin(), found.end(), std::string(expected)) != found.end());

        for (const std::filesystem::path& reader : readers)
        {
            const std::string name = reader.filename().string();
            const std::string code = Code(ReadFile(reader.string()));
            // The parser's own header is where the one type comparison lives; it is the only file allowed to read the field.
            if (name == "ShaderBindings.h")
            {
                size_t comparisons = 0;
                for (size_t at = code.find("binding.type==uint32_t(kind)"); at != std::string::npos;
                     at = code.find("binding.type==uint32_t(kind)", at + 1))
                    ++comparisons;
                CHECK_EQ(comparisons, size_t(1)); // the one place the kind is compared, for both directions of the lookup
                CHECK(Has(code, "boolBindingAnswers("));
                CHECK(Has(code, "uint32_tBindPointOf("));
                continue;
            }
            const bool readsType = Has(code, ".type") || Has(code, "->type") || Has(code, "BindingCovers");
            if (readsType)
                std::printf("    %s reads a binding's type itself\n", name.c_str());
            CHECK(!readsType);
        }
        // Every parser caller hands the parsed table it holds to the typed lookup, never reading it for slots here.
        for (const std::filesystem::path& caller : callers)
        {
            const std::string name = caller.filename().string();
            if (name == "ShaderBindings.h")
                continue; // the lookup itself
            const std::string code = Code(ReadFile(caller.string()));
            const bool throughTyped = Has(code, "BindPointOf(") || Has(code, "BindingAnswers(") || Has(code, "InputsOfBindings(") ||
                                      Has(code, "InputsFromBindings(");
            if (!throughTyped)
                std::printf("    %s keeps the parsed table without a typed lookup\n", name.c_str());
            CHECK(throughTyped);
        }
    }

    return Finish("slot_kind");
}
