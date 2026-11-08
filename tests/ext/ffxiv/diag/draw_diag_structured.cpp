// The draw diagnostic's structured-buffer SRV: the composite declares its per-material shading table as a structured buffer, and the
// reflection reader files that under the kind the slot lookups do not ask about. This test compiles a shader with the same shape and
// pins what the one-frame dump's identification depends on: the parsed type, the kind it is filed under, that the texture question
// never answers it, and that the structured question (the kind the runtime asks) does, at the slot the buffer is bound at.
#include "check.h"

#include "ext/ffxiv/diag/DrawDiagFormat.h"
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
struct ShaderTypeRow
{
    float4 m_Rows[8];
};
StructuredBuffer<ShaderTypeRow> g_ShaderTypeParameter : register(t2);
Texture2D<float4> g_SamplerGBuffer : register(t0);
SamplerState g_SamplerLinear : register(s0);
cbuffer g_CommonParameter : register(b1)
{
    float4 m_Viewport;
};

float4 main(float4 pos : SV_Position) : SV_Target
{
    const uint row = (uint)pos.x;
    return g_ShaderTypeParameter[row].m_Rows[1] + g_SamplerGBuffer.Sample(g_SamplerLinear, pos.xy) + m_Viewport;
}
)";

bool Compile(std::vector<unsigned char>& out)
{
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile(kSource, std::strlen(kSource), "structured_test.hlsl", nullptr, nullptr, "main", "ps_5_0", 0, 0, &code,
                                  &errors);
    if (errors)
    {
        std::printf("%s", static_cast<const char*>(errors->GetBufferPointer()));
        errors->Release();
    }
    if (FAILED(hr) || code == nullptr)
        return false;
    out.assign(static_cast<const unsigned char*>(code->GetBufferPointer()),
               static_cast<const unsigned char*>(code->GetBufferPointer()) + code->GetBufferSize());
    code->Release();
    return true;
}

const ShaderBinding* Find(const ShaderBinding* bindings, size_t count, const char* name)
{
    for (size_t i = 0; i < count; ++i)
        if (std::string(bindings[i].name, bindings[i].nameLength) == name)
            return &bindings[i];
    return nullptr;
}

// The probe the runtime builds from the table it keeps: the kind the parser filed the binding under stands in the type column.
ShaderBinding KeptProbe(const ShaderBinding& parsed)
{
    return ShaderBinding { parsed.name, parsed.nameLength, uint32_t(BindingKindOf(parsed)), parsed.bindPoint, parsed.bindCount };
}
} // namespace

int main()
{
    std::vector<unsigned char> blob;
    CHECK(Compile(blob)); // the test itself is broken if the compiler is not there
    CHECK(blob.size() > 64);

    ShaderBinding bindings[kMaxBindings];
    uint32_t declared = 0;
    const size_t count = ParseBindings(blob.data(), blob.size(), bindings, kMaxBindings, &declared);
    CHECK_EQ(count, size_t(declared));
    CHECK_EQ(count, size_t(4)); // the structured buffer, the texture, the sampler and the constant buffer

    const ShaderBinding* table = Find(bindings, count, "g_ShaderTypeParameter");
    CHECK(table != nullptr);
    if (table)
    {
        // The reflection gives a structured buffer its plain name (no `.T`), the structured type (D3D_SIT_STRUCTURED = 5) and its register.
        CHECK_EQ(table->type, 5u);
        CHECK_EQ(table->bindPoint, 2u);
        CHECK_EQ(table->bindCount, 1u);
        CHECK(IsStructuredDumpName(std::string_view(table->name, table->nameLength)));

        // It is filed under the kind no slot decision asks about; the texture question never answers it, which is why the
        // one-frame dump asks the structured one.
        CHECK(BindingKindOf(*table) == BindKind::Other);
        CHECK(!BindingAnswers(*table, BindKind::Texture, 2));
        CHECK(!BindingAnswers(*table, BindKind::ConstantBuffer, 2));
        const ShaderBinding kept = KeptProbe(*table);
        CHECK(BindingAnswers(kept, BindKind::Other, 2));
        CHECK(!BindingAnswers(kept, BindKind::Other, 1)); // only the register it is bound at
        CHECK(!BindingAnswers(kept, BindKind::Other, 3));
        CHECK(!BindingAnswers(kept, BindKind::Texture, 2));
    }

    // The texture, the sampler and the constant buffer of the same shader keep answering their own kinds and never the structured one.
    const ShaderBinding* gbuffer = Find(bindings, count, "g_SamplerGBuffer");
    CHECK(gbuffer != nullptr);
    if (gbuffer)
    {
        CHECK_EQ(gbuffer->type, kBindTexture);
        CHECK(BindingAnswers(KeptProbe(*gbuffer), BindKind::Texture, 0));
        CHECK(!BindingAnswers(KeptProbe(*gbuffer), BindKind::Other, 0));
    }
    const ShaderBinding* common = Find(bindings, count, "g_CommonParameter");
    CHECK(common != nullptr);
    if (common)
    {
        CHECK_EQ(common->type, kBindConstantBuffer);
        CHECK(!BindingAnswers(KeptProbe(*common), BindKind::Other, 1));
    }

    // The runtime's name test: the table's name answers, a texture's, a near-name's and a constant buffer's do not.
    CHECK(IsStructuredDumpName("g_ShaderTypeParameter"));
    CHECK(!IsStructuredDumpName("g_SamplerGBuffer"));
    CHECK(!IsStructuredDumpName("g_ShaderTypeParameters"));
    CHECK(!IsStructuredDumpName(""));

    return Finish("draw_diag_structured");
}
