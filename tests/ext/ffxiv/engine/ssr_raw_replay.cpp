#include "check.h"
#include "ext/ffxiv/engine/SsrPass.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::engine::ssr;

namespace {
std::vector<unsigned char> Read(const std::string& p) {
    std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}};
}
float F(const unsigned char* p) { float v; std::memcpy(&v,p,4); return v; }
bool ManifestRow(const std::vector<unsigned char>& bytes,const std::string& file,unsigned w,unsigned h,unsigned format,unsigned pitch) {
    std::istringstream input(std::string(bytes.begin(),bytes.end())); std::string line;
    while(std::getline(input,line)) { std::istringstream row(line); std::string field; std::vector<std::string> f; while(std::getline(row,field,',')) f.push_back(field);
        if(f.size()==7 && f[0]==file) return std::stoul(f[3])==w && std::stoul(f[4])==h && std::stoul(f[5])==format && std::stoul(f[6])==pitch;
    } return false;
}
ComPtr<ID3D11Texture2D> Texture(ID3D11Device* d, UINT w, UINT h, DXGI_FORMAT fmt, UINT pitch, const void* bytes, UINT bind) {
    D3D11_TEXTURE2D_DESC x{}; x.Width=w; x.Height=h; x.MipLevels=x.ArraySize=1; x.Format=fmt; x.SampleDesc.Count=1; x.Usage=D3D11_USAGE_DEFAULT; x.BindFlags=bind;
    D3D11_SUBRESOURCE_DATA data{}; data.pSysMem=bytes; data.SysMemPitch=pitch; ComPtr<ID3D11Texture2D> t;
    if (FAILED(d->CreateTexture2D(&x,&data,&t))) return {}; return t;
}
}
int main(int argc,char** argv) {
    CHECK(argc==3 || argc==4 || argc==5); const std::string dir=argv[1], stem=argv[2];
    const std::string mode=argc==3?"compare":(argc==4?"diag":argv[3]); const bool diagnostic=mode=="diag", save=mode=="save"||diagnostic;
    CHECK(mode=="compare"||mode=="save"||mode=="diag");
    const auto manifest=Read(dir+"/manifest.csv"); CHECK(!manifest.empty());
    CHECK(ManifestRow(manifest,stem+"_scene.raw",3840,2160,10,30720)); CHECK(ManifestRow(manifest,stem+"_view-position.raw",3840,2160,10,30720));
    CHECK(ManifestRow(manifest,stem+"_planar-source.raw",1920,1080,87,7680)); CHECK(ManifestRow(manifest,stem+"_output.raw",1920,1080,87,7680));
    CHECK(ManifestRow(manifest,stem+"_camera.raw",0,0,0,352)); CHECK(ManifestRow(manifest,stem+"_constants.raw",0,0,0,80));
    auto sceneBytes=Read(dir+"/"+stem+"_scene.raw"), posBytes=Read(dir+"/"+stem+"_view-position.raw"), planarBytes=Read(dir+"/"+stem+"_planar-source.raw"), expected=Read(dir+"/"+stem+"_output.raw"), cam=Read(dir+"/"+stem+"_camera.raw"), con=Read(dir+"/"+stem+"_constants.raw");
    CHECK(sceneBytes.size()==3840ull*2160*8 && posBytes.size()==sceneBytes.size()); CHECK(planarBytes.size()==1920ull*1080*4 && expected.size()==planarBytes.size()); CHECK(cam.size()==352 && con.size()==80);
    ReaderFacts facts{}; facts.hasPlanar=facts.hasScene=facts.hasVPosition=facts.hasCamera=true;
    facts.planarFormat=87; facts.planarWidth=1920; facts.planarHeight=1080; facts.planarSamples=1;
    facts.sceneFormat=facts.vpositionFormat=10; facts.sceneWidth=facts.vpositionWidth=3840; facts.sceneHeight=facts.vpositionHeight=2160; facts.sceneSamples=facts.vpositionSamples=1; facts.cameraBytes=352;
    facts.window[0]=F(con.data()); facts.window[1]=F(con.data()+4); facts.window[2]=F(con.data()+8); facts.window[3]=F(con.data()+12);
    Params params{}; params.on=true; params.steps=UINT(F(con.data()+40)); params.thickness=F(con.data()+44); params.blend=F(con.data()+48); params.fade=F(con.data()+52);
    const Constants rebuilt=ConstantsOf(facts,params); CHECK(std::memcmp(&rebuilt,con.data(),80)==0);
    D3D11_TEXTURE2D_DESC dd{}; dd.Width=1920; dd.Height=1080; dd.MipLevels=dd.ArraySize=1; dd.Format=DXGI_FORMAT_B8G8R8A8_UNORM; dd.SampleDesc.Count=1; dd.Usage=D3D11_USAGE_DEFAULT; dd.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA pd{}; pd.pSysMem=planarBytes.data(); pd.SysMemPitch=7680; ComPtr<ID3D11Texture2D> planar;
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx; CHECK(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,nullptr,&ctx)));
    CHECK(SUCCEEDED(dev->CreateTexture2D(&dd,&pd,&planar)));
    auto scene=Texture(dev.Get(),3840,2160,DXGI_FORMAT_R16G16B16A16_FLOAT,30720,sceneBytes.data(),D3D11_BIND_SHADER_RESOURCE);
    auto position=Texture(dev.Get(),3840,2160,DXGI_FORMAT_R16G16B16A16_FLOAT,30720,posBytes.data(),D3D11_BIND_SHADER_RESOURCE); CHECK(scene&&position);
    ComPtr<ID3D11ShaderResourceView> pv,sv,vv; CHECK(SUCCEEDED(dev->CreateShaderResourceView(planar.Get(),nullptr,&pv))); CHECK(SUCCEEDED(dev->CreateShaderResourceView(scene.Get(),nullptr,&sv))); CHECK(SUCCEEDED(dev->CreateShaderResourceView(position.Get(),nullptr,&vv)));
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth=352; bd.Usage=D3D11_USAGE_DEFAULT; bd.BindFlags=D3D11_BIND_SHADER_RESOURCE; D3D11_SUBRESOURCE_DATA cbd{}; cbd.pSysMem=cam.data(); ComPtr<ID3D11Buffer> cb; CHECK(SUCCEEDED(dev->CreateBuffer(&bd,&cbd,&cb)));
    PassInputs in{}; in.planar=pv.Get(); in.planarTexture=planar.Get(); in.vposition=vv.Get(); in.scene=scene.Get(); in.camera=cb.Get(); in.facts=facts; in.params=params;
    Pass pass; CHECK(pass.Run(ctx.Get(),in)!=nullptr);
    D3D11_TEXTURE2D_DESC sd{}; sd.Width=1920; sd.Height=1080; sd.MipLevels=sd.ArraySize=1; sd.Format=DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count=1; sd.Usage=D3D11_USAGE_STAGING; sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ; ComPtr<ID3D11Texture2D> stage; CHECK(SUCCEEDED(dev->CreateTexture2D(&sd,nullptr,&stage))); ctx->CopyResource(stage.Get(),pass.OutputTexture());
    D3D11_MAPPED_SUBRESOURCE m{}; CHECK(SUCCEEDED(ctx->Map(stage.Get(),0,D3D11_MAP_READ,0,&m))); uint64_t changed=0, over1=0, over4=0; unsigned maxd=0; uint64_t sum=0;
    std::vector<unsigned char> actual(size_t(1920)*1080*4); for(UINT y=0;y<1080;y++) { const auto* src=static_cast<const unsigned char*>(m.pData)+size_t(y)*m.RowPitch; std::memcpy(actual.data()+size_t(y)*1920*4,src,size_t(1920)*4); }
    if (save) { std::ofstream out(argc==4?argv[3]:argv[4],std::ios::binary); out.write(reinterpret_cast<const char*>(actual.data()),actual.size()); CHECK(bool(out)); }
    else for(UINT y=0;y<1080;y++) for(UINT x=0;x<1920;x++) for(UINT c=0;c<4;c++) { unsigned a=actual[(size_t(y)*1920+x)*4+c], b=expected[(size_t(y)*1920+x)*4+c]; unsigned delta=a>b?a-b:b-a; sum+=delta; maxd=std::max(maxd,delta); if(delta) {changed++; if(delta>1) over1++; if(delta>4) over4++;} }
    ctx->Unmap(stage.Get(),0); const uint64_t n=expected.size(); if (diagnostic) { uint64_t codes[256]{}; for(size_t i=0;i<actual.size();i+=4) codes[actual[i+2]]++; std::printf("%s: diagnostic map saved; constants exact;",stem.c_str()); for(unsigned i=0;i<256;i++) if(codes[i]) std::printf(" %u=%llu",i,(unsigned long long)codes[i]); std::puts(""); return Finish("ssr_raw_replay"); }
    if (mode=="save") { std::printf("%s: normal output saved; constants exact; bytes=%llu\n",stem.c_str(),(unsigned long long)n); return Finish("ssr_raw_replay"); }
    std::printf("%s: constants exact; output bytes=%llu differing=%llu (>1=%llu, >4=%llu), max=%u, mean_abs=%.6f\n",stem.c_str(),(unsigned long long)n,(unsigned long long)changed,(unsigned long long)over1,(unsigned long long)over4,maxd,double(sum)/n);
    // WARP and the game driver's shader execution can round half/UNORM conversions differently. Keep a strict bounded-error gate and print the
    // full histogram above; this is deliberately byte level so a capture with a wrong matrix or mapping cannot pass on a visually similar image.
    CHECK(changed < n/100 && over4 < n/10000 && maxd <= 64); return Finish("ssr_raw_replay");
}
