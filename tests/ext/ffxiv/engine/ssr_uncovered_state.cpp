// Exercise the real SSR pass while the immediate context contains a deliberately distinctive game pipeline.
#define main OriginalSsrWarpMain
#include "ssr_warp.cpp"
#undef main

#include <array>

namespace
{
struct Fingerprint
{
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> vb, ib, cb;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    UINT stride = 0, offset = 0, indexOffset = 0, stencilRef = 0, sampleMask = 0;
    DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    float blendFactor[4] {};
    D3D11_VIEWPORT viewport {};
    D3D11_RECT scissor {};
    UINT viewportCount = 1, scissorCount = 1;

    explicit Fingerprint(ID3D11DeviceContext* c)
    {
        ID3D11VertexShader* v = nullptr; c->VSGetShader(&v, nullptr, nullptr); vs.Attach(v);
        ID3D11PixelShader* p = nullptr; c->PSGetShader(&p, nullptr, nullptr); ps.Attach(p);
        ID3D11GeometryShader* g = nullptr; c->GSGetShader(&g, nullptr, nullptr); gs.Attach(g);
        ID3D11InputLayout* l = nullptr; c->IAGetInputLayout(&l); layout.Attach(l);
        ID3D11Buffer* b = nullptr; c->IAGetVertexBuffers(0, 1, &b, &stride, &offset); vb.Attach(b);
        b = nullptr; c->IAGetIndexBuffer(&b, &indexFormat, &indexOffset); ib.Attach(b);
        c->IAGetPrimitiveTopology(&topology);
        b = nullptr; c->PSGetConstantBuffers(7, 1, &b); cb.Attach(b);
        ID3D11ShaderResourceView* s = nullptr; c->PSGetShaderResources(7, 1, &s); srv.Attach(s);
        ID3D11SamplerState* sm = nullptr; c->PSGetSamplers(7, 1, &sm); sampler.Attach(sm);
        ID3D11RasterizerState* rs = nullptr; c->RSGetState(&rs); raster.Attach(rs);
        ID3D11BlendState* bs = nullptr; c->OMGetBlendState(&bs, blendFactor, &sampleMask); blend.Attach(bs);
        ID3D11DepthStencilState* ds = nullptr; c->OMGetDepthStencilState(&ds, &stencilRef); depth.Attach(ds);
        ID3D11RenderTargetView* r = nullptr; ID3D11DepthStencilView* d = nullptr;
        c->OMGetRenderTargets(1, &r, &d); rtv.Attach(r); dsv.Attach(d);
        c->RSGetViewports(&viewportCount, &viewport); c->RSGetScissorRects(&scissorCount, &scissor);
    }

    bool Matches(ID3D11DeviceContext* c)
    {
        Fingerprint now(c);
        return now.vs.Get() == vs.Get() && now.ps.Get() == ps.Get() && now.gs.Get() == gs.Get() &&
            now.layout.Get() == layout.Get() && now.vb.Get() == vb.Get() && now.ib.Get() == ib.Get() &&
            now.cb.Get() == cb.Get() && now.srv.Get() == srv.Get() && now.sampler.Get() == sampler.Get() &&
            now.raster.Get() == raster.Get() && now.blend.Get() == blend.Get() && now.depth.Get() == depth.Get() &&
            now.rtv.Get() == rtv.Get() && now.dsv.Get() == dsv.Get() && now.stride == stride && now.offset == offset &&
            now.indexFormat == indexFormat && now.indexOffset == indexOffset && now.topology == topology &&
            now.stencilRef == stencilRef && now.sampleMask == sampleMask && now.viewportCount == viewportCount &&
            now.scissorCount == scissorCount && std::memcmp(now.blendFactor, blendFactor, sizeof blendFactor) == 0 &&
            std::memcmp(&now.viewport, &viewport, sizeof viewport) == 0 && std::memcmp(&now.scissor, &scissor, sizeof scissor) == 0;
    }
};

bool MakeBuffer(ID3D11Device* d, UINT bind, UINT bytes, ID3D11Buffer** out)
{
    D3D11_BUFFER_DESC desc{}; desc.ByteWidth = bytes; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = bind;
    return SUCCEEDED(d->CreateBuffer(&desc, nullptr, out));
}
}

int main(int argc, char** argv)
{
    Harness h;
    // Request the debug layer first so the output can report whether a debug audit was possible.
    HRESULT deviceHr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0,
                                         D3D11_SDK_VERSION, &h.device, nullptr, &h.context);
    const bool debugLayerAvailable = SUCCEEDED(deviceHr);
    if (!debugLayerAvailable)
        CHECK(h.Create());
    std::printf("[ssr_uncovered_state] D3D11 debug layer: %s\n", debugLayerAvailable ? "available" : "unavailable; no debug-layer audit");
    if (!h.device) return Finish("ssr_pipeline_restore");
    Pass pass;
    Frame frame = BuildScene(true, true, 16, 16);
    CHECK(h.Upload(frame));

    const char* shader = "struct V{float4 p:SV_Position;}; V VS(uint i:SV_VertexID){V o;o.p=float4((i==1)?1:-1,(i==2)?1:-1,0.25,1);return o;} float4 PS():SV_Target{return float4(.1,.3,.7,.9);} [maxvertexcount(3)] void GS(triangle V a[3],inout TriangleStream<V> s){s.Append(a[0]);s.Append(a[1]);s.Append(a[2]);}";
    ComPtr<ID3DBlob> vsBlob, psBlob, gsBlob, errors;
    CHECK(SUCCEEDED(D3DCompile(shader, strlen(shader), "restore", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vsBlob, &errors)));
    CHECK(SUCCEEDED(D3DCompile(shader, strlen(shader), "restore", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &psBlob, &errors)));
    CHECK(SUCCEEDED(D3DCompile(shader, strlen(shader), "restore", nullptr, nullptr, "GS", "gs_5_0", 0, 0, &gsBlob, &errors)));
    if (!vsBlob || !psBlob || !gsBlob) return Finish("ssr_pipeline_restore");
    ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps; ComPtr<ID3D11GeometryShader> gs;
    CHECK(SUCCEEDED(h.device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs)));
    CHECK(SUCCEEDED(h.device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps)));
    CHECK(SUCCEEDED(h.device->CreateGeometryShader(gsBlob->GetBufferPointer(), gsBlob->GetBufferSize(), nullptr, &gs)));
    ComPtr<ID3D11Buffer> vb, ib, cb;
    CHECK(MakeBuffer(h.device.Get(), D3D11_BIND_VERTEX_BUFFER, 64, &vb));
    CHECK(MakeBuffer(h.device.Get(), D3D11_BIND_INDEX_BUFFER, 64, &ib));
    CHECK(MakeBuffer(h.device.Get(), D3D11_BIND_CONSTANT_BUFFER, 16, &cb));
    ComPtr<ID3D11Texture2D> marker;
    D3D11_TEXTURE2D_DESC td{}; td.Width=4; td.Height=4; td.MipLevels=1; td.ArraySize=1; td.Format=DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count=1; td.Usage=D3D11_USAGE_DEFAULT; td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    CHECK(SUCCEEDED(h.device->CreateTexture2D(&td, nullptr, &marker)));
    ComPtr<ID3D11ShaderResourceView> srv; CHECK(SUCCEEDED(h.device->CreateShaderResourceView(marker.Get(), nullptr, &srv)));
    D3D11_SAMPLER_DESC sd{}; sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT; sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler; CHECK(SUCCEEDED(h.device->CreateSamplerState(&sd, &sampler)));
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode=D3D11_FILL_WIREFRAME; rd.CullMode=D3D11_CULL_FRONT; rd.DepthClipEnable=FALSE; rd.ScissorEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster; CHECK(SUCCEEDED(h.device->CreateRasterizerState(&rd, &raster)));
    D3D11_BLEND_DESC bd{}; bd.RenderTarget[0].BlendEnable=TRUE; bd.RenderTarget[0].SrcBlend=D3D11_BLEND_BLEND_FACTOR; bd.RenderTarget[0].DestBlend=D3D11_BLEND_INV_BLEND_FACTOR; bd.RenderTarget[0].BlendOp=D3D11_BLEND_OP_ADD; bd.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_ZERO; bd.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD; bd.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> blend; CHECK(SUCCEEDED(h.device->CreateBlendState(&bd, &blend)));
    D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthEnable=TRUE; dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO; dd.DepthFunc=D3D11_COMPARISON_GREATER; dd.StencilEnable=TRUE; dd.StencilReadMask=D3D11_DEFAULT_STENCIL_READ_MASK; dd.StencilWriteMask=D3D11_DEFAULT_STENCIL_WRITE_MASK;
    dd.FrontFace.StencilFailOp=dd.BackFace.StencilFailOp=D3D11_STENCIL_OP_KEEP; dd.FrontFace.StencilDepthFailOp=dd.BackFace.StencilDepthFailOp=D3D11_STENCIL_OP_INCR; dd.FrontFace.StencilPassOp=dd.BackFace.StencilPassOp=D3D11_STENCIL_OP_REPLACE; dd.FrontFace.StencilFunc=dd.BackFace.StencilFunc=D3D11_COMPARISON_EQUAL;
    ComPtr<ID3D11DepthStencilState> depth; CHECK(SUCCEEDED(h.device->CreateDepthStencilState(&dd, &depth)));
    ComPtr<ID3D11Texture2D> depthTex; td.Width=frame.facts.sceneWidth; td.Height=frame.facts.sceneHeight; td.Format=DXGI_FORMAT_D24_UNORM_S8_UINT; td.BindFlags=D3D11_BIND_DEPTH_STENCIL; CHECK(SUCCEEDED(h.device->CreateTexture2D(&td,nullptr,&depthTex)));
    ComPtr<ID3D11DepthStencilView> dsv; CHECK(SUCCEEDED(h.device->CreateDepthStencilView(depthTex.Get(),nullptr,&dsv)));
    D3D11_INPUT_ELEMENT_DESC elem{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0}; ComPtr<ID3D11InputLayout> layout;
    CHECK(SUCCEEDED(h.device->CreateInputLayout(&elem,1,vsBlob->GetBufferPointer(),vsBlob->GetBufferSize(),&layout)));

    // Separate resources keep this legal: RTV slot 0 and an OM UAV in uncovered slot 1.
    D3D11_TEXTURE2D_DESC ud{}; ud.Width=4; ud.Height=4; ud.MipLevels=1; ud.ArraySize=1; ud.Format=DXGI_FORMAT_R32_UINT; ud.SampleDesc.Count=1; ud.Usage=D3D11_USAGE_DEFAULT; ud.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> omTexture; CHECK(SUCCEEDED(h.device->CreateTexture2D(&ud,nullptr,&omTexture)));
    ComPtr<ID3D11UnorderedAccessView> omUav; CHECK(SUCCEEDED(h.device->CreateUnorderedAccessView(omTexture.Get(),nullptr,&omUav)));
    ComPtr<ID3D11Texture2D> csTexture; CHECK(SUCCEEDED(h.device->CreateTexture2D(&ud,nullptr,&csTexture)));
    ComPtr<ID3D11UnorderedAccessView> csUav; CHECK(SUCCEEDED(h.device->CreateUnorderedAccessView(csTexture.Get(),nullptr,&csUav)));
    ComPtr<ID3D11Buffer> soBuffer; D3D11_BUFFER_DESC soDesc{}; soDesc.ByteWidth=256; soDesc.Usage=D3D11_USAGE_DEFAULT; soDesc.BindFlags=D3D11_BIND_STREAM_OUTPUT; CHECK(SUCCEEDED(h.device->CreateBuffer(&soDesc,nullptr,&soBuffer)));
    D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION_PREDICATE,0}; ComPtr<ID3D11Predicate> predicate; CHECK(SUCCEEDED(h.device->CreatePredicate(&qd,&predicate)));
    auto* c=h.context.Get();
    c->Begin(predicate.Get()); c->End(predicate.Get());
    BOOL predicateReady=FALSE; HRESULT predicateHr=S_FALSE;
    for (unsigned i=0; i<1000 && predicateHr==S_FALSE; ++i) predicateHr=c->GetData(predicate.Get(),&predicateReady,sizeof predicateReady,0);
    CHECK(predicateHr==S_OK);

    ID3D11RenderTargetView* target=h.sceneRtv.Get(); ID3D11UnorderedAccessView* ou=omUav.Get(); UINT initial=0xffffffffu; c->OMSetRenderTargetsAndUnorderedAccessViews(1,&target,dsv.Get(),1,1,&ou,&initial);
    c->VSSetShader(vs.Get(),nullptr,0); c->PSSetShader(ps.Get(),nullptr,0); c->GSSetShader(gs.Get(),nullptr,0);
    c->IASetInputLayout(layout.Get()); UINT stride=16, offset=4; ID3D11Buffer* vertex=vb.Get(); c->IASetVertexBuffers(0,1,&vertex,&stride,&offset); c->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R16_UINT,2); c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11Buffer* constant=cb.Get(); c->PSSetConstantBuffers(7,1,&constant); ID3D11ShaderResourceView* resource=srv.Get(); c->PSSetShaderResources(7,1,&resource); ID3D11SamplerState* samp=sampler.Get(); c->PSSetSamplers(7,1,&samp);
    c->RSSetState(raster.Get()); D3D11_VIEWPORT vp{3,5,31,29,.2f,.8f}; c->RSSetViewports(1,&vp); D3D11_RECT sc{2,4,35,37}; c->RSSetScissorRects(1,&sc);
    ID3D11UnorderedAccessView* cu=csUav.Get(); c->CSSetUnorderedAccessViews(3,1,&cu,&initial);
    ID3D11Buffer* so=soBuffer.Get(); UINT soOffset=16; c->SOSetTargets(1,&so,&soOffset);
    c->SetPredication(predicate.Get(),TRUE);
    float bf[4]{.17f,.31f,.53f,.79f}; c->OMSetBlendState(blend.Get(),bf,0x12345678); c->OMSetDepthStencilState(depth.Get(),0x4d);
    Fingerprint before(c);
    PassInputs in; in.planar=h.planarView.Get(); in.planarTexture=h.planarTexture.Get(); in.vposition=h.positionView.Get(); in.scene=h.sceneTexture.Get(); in.camera=h.camera.Get(); in.facts=frame.facts; in.params=frame.params;
    auto checkSpecial = [&] {
        ID3D11UnorderedAccessView* got=nullptr; c->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,1,1,&got); CHECK(got==omUav.Get()); if(got)got->Release();
        got=nullptr; c->CSGetUnorderedAccessViews(3,1,&got); CHECK(got==csUav.Get()); if(got)got->Release();
        ID3D11Buffer* soGot=nullptr; c->SOGetTargets(1,&soGot); CHECK(soGot==soBuffer.Get()); if(soGot)soGot->Release();
        ID3D11Predicate* predGot=nullptr; BOOL predValue=FALSE; c->GetPredication(&predGot,&predValue); CHECK(predGot==predicate.Get()); CHECK(predValue==TRUE); if(predGot)predGot->Release();
    };
    auto checkOm = [&](ID3D11RenderTargetView* expectedRtv, ID3D11DepthStencilView* expectedDsv, ID3D11UnorderedAccessView* expectedUav) {
        ID3D11RenderTargetView* gotRtv=nullptr; ID3D11DepthStencilView* gotDsv=nullptr;
        c->OMGetRenderTargets(1,&gotRtv,&gotDsv); CHECK(gotRtv==expectedRtv); CHECK(gotDsv==expectedDsv);
        if(gotRtv)gotRtv->Release(); if(gotDsv)gotDsv->Release();
        ID3D11UnorderedAccessView* gotUav=nullptr; c->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,1,1,&gotUav);
        CHECK(gotUav==expectedUav); if(gotUav)gotUav->Release();
    };
    // Control recipe: seed and read back the legal uncovered OM UAV before touching it.
    checkOm(target,dsv.Get(),omUav.Get());
    ID3D11RenderTargetView* gameViews[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* gameDepth=nullptr;
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,gameViews,&gameDepth);
    c->OMSetRenderTargets(0,nullptr,nullptr);
    D3D11_TEXTURE2D_DESC sceneDesc{}; h.sceneTexture->GetDesc(&sceneDesc);
    ComPtr<ID3D11Texture2D> privateCopy; CHECK(SUCCEEDED(h.device->CreateTexture2D(&sceneDesc,nullptr,&privateCopy)));
    c->CopySubresourceRegion(privateCopy.Get(),0,0,0,0,h.sceneTexture.Get(),0,nullptr);
    c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,gameViews,gameDepth);
    for(auto* view:gameViews) if(view)view->Release(); if(gameDepth)gameDepth->Release();
    ID3D11UnorderedAccessView* sequenceUav=nullptr; c->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,1,1,&sequenceUav);
    const bool sequencePreservedUav = sequenceUav==omUav.Get(); if(sequenceUav)sequenceUav->Release();
    std::printf("[ssr_uncovered_state] exact OM sequence preserved slot-1 UAV: %s\n",sequencePreservedUav?"yes":"no");
    // Direct-copy control: no OM unbind/rebind. Check bindings and byte equality through staging resources.
    c->OMSetRenderTargetsAndUnorderedAccessViews(1,&target,dsv.Get(),1,1,&ou,&initial);
    checkOm(target,dsv.Get(),omUav.Get());
    ComPtr<ID3D11Texture2D> directCopy; CHECK(SUCCEEDED(h.device->CreateTexture2D(&sceneDesc,nullptr,&directCopy)));
    checkOm(target,dsv.Get(),omUav.Get());
    c->CopySubresourceRegion(directCopy.Get(),0,0,0,0,h.sceneTexture.Get(),0,nullptr);
    checkOm(target,dsv.Get(),omUav.Get());
    D3D11_TEXTURE2D_DESC stagingDesc=sceneDesc; stagingDesc.Usage=D3D11_USAGE_STAGING; stagingDesc.BindFlags=0; stagingDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> sceneReadback, copyReadback;
    CHECK(SUCCEEDED(h.device->CreateTexture2D(&stagingDesc,nullptr,&sceneReadback)));
    CHECK(SUCCEEDED(h.device->CreateTexture2D(&stagingDesc,nullptr,&copyReadback)));
    c->CopyResource(sceneReadback.Get(),h.sceneTexture.Get()); c->CopyResource(copyReadback.Get(),directCopy.Get());
    D3D11_MAPPED_SUBRESOURCE sceneMap{},copyMap{};
    CHECK(SUCCEEDED(c->Map(sceneReadback.Get(),0,D3D11_MAP_READ,0,&sceneMap)));
    CHECK(SUCCEEDED(c->Map(copyReadback.Get(),0,D3D11_MAP_READ,0,&copyMap)));
    if(sceneMap.pData&&copyMap.pData) for(UINT y=0;y<sceneDesc.Height;++y)
        CHECK(std::memcmp(static_cast<const BYTE*>(sceneMap.pData)+size_t(y)*sceneMap.RowPitch,
                          static_cast<const BYTE*>(copyMap.pData)+size_t(y)*copyMap.RowPitch,sceneDesc.Width*8)==0);
    if(sceneMap.pData)c->Unmap(sceneReadback.Get(),0); if(copyMap.pData)c->Unmap(copyReadback.Get(),0);
    if (argc > 2 && std::strcmp(argv[2], "control") == 0)
        return Finish("ssr_uncovered_state_control");
    // Validate the seed before blaming the pass, and restore it for each independent trial.
    checkSpecial();
    CHECK(pass.Run(c,in,false,true)!=nullptr); CHECK(before.Matches(c)); checkSpecial();
    c->OMSetRenderTargetsAndUnorderedAccessViews(1,&target,dsv.Get(),1,1,&ou,&initial);
    CHECK(before.Matches(c)); checkSpecial();
    CHECK(pass.Run(c,in,false,false)!=nullptr); CHECK(before.Matches(c)); checkSpecial();
    return Finish("ssr_uncovered_state");
}
