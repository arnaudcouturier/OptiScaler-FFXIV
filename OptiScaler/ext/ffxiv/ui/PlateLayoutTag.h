#pragma once
// Input-layout tags for the game's nameplate vertex shaders. Writing a tag is also what counts these layouts for the shader
// catalog: every created layout is counted as seen, and the three vertex shaders this file recognises are reported as the
// nameplate group (ext/ffxiv/catalog, by bytecode CRC). Nothing reads the tag payload back: the layout table and the bytecode
// copy stay written for a future consumer, and no consumer in this tree resolves a layout through them.
#include <d3d11.h>

#include <array>
#include <cstring>

#include "../../core/Crc32.h"
#include "../catalog/FfxivCatalog.h" // ext: catalog health accounting

namespace ext::ffxiv::ui::plate
{
inline constexpr GUID vertexCodeTag {0x2ddb1677,0x8cc9,0x4116,{0xb8,0x01,0x34,0x95,0x80,0x11,0x27,0x19}};
inline constexpr GUID layoutTag {0xb8b5a74c,0x6319,0x46fe,{0xbb,0xf3,0x1d,0xad,0x36,0x50,0xc4,0x84}};
struct Element { char semantic[32]{}; UINT index=0; DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN; UINT offset=0; };
struct Layout { UINT count=0; std::array<Element,16> elements{}; };
inline UINT FormatBytes(DXGI_FORMAT f)
{
    switch(f)
    {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:return 16;
    case DXGI_FORMAT_R32G32B32_FLOAT:return 12;
    case DXGI_FORMAT_R32G32_FLOAT:case DXGI_FORMAT_R16G16B16A16_SINT:case DXGI_FORMAT_R16G16B16A16_UINT:return 8;
    case DXGI_FORMAT_R32_FLOAT:case DXGI_FORMAT_R16G16_SINT:case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_SINT:case DXGI_FORMAT_R8G8B8A8_UINT:return 4;
    default:return 0;
    }
}
inline void TagLayout(ID3D11InputLayout* object,const D3D11_INPUT_ELEMENT_DESC* elements,UINT count,const void* code,SIZE_T size)
{
    ext::ffxiv::catalog::OnLayoutSeen(); // ext: the nameplate catalog counts as measured once any layout was created
    if(!object || !elements || !count || count>16 || !code || !size || size>256*1024) return;
    const auto crc=ext::core::Crc32(code,size);
    if(crc!=2964264238u && crc!=1445359818u && crc!=3153679471u) return;
    ext::ffxiv::catalog::OnNameplateVertexShader(code,size,crc); // ext: catalog health (before the element validation below can return)
    Layout layout; layout.count=count;
    std::array<UINT,32> ends{};
    for(UINT i=0;i<count;++i)
    {
        const auto& in=elements[i]; auto& out=layout.elements[i]; UINT bytes=FormatBytes(in.Format);
        if(!in.SemanticName || strlen(in.SemanticName)>=sizeof(out.semantic) || in.InputSlot>=ends.size() ||
           in.InputSlotClass!=D3D11_INPUT_PER_VERTEX_DATA || in.InstanceDataStepRate || !bytes) return;
        strcpy_s(out.semantic,in.SemanticName); out.index=in.SemanticIndex; out.format=in.Format;
        out.offset=in.AlignedByteOffset==D3D11_APPEND_ALIGNED_ELEMENT?ends[in.InputSlot]:in.AlignedByteOffset;
        if(out.offset>128 || out.offset+bytes>128) return;
        ends[in.InputSlot]=out.offset+bytes;
    }
    object->SetPrivateData(layoutTag,sizeof(layout),&layout);
    object->SetPrivateData(vertexCodeTag,static_cast<UINT>(size),code);
}
} // namespace ext::ffxiv::ui::plate
