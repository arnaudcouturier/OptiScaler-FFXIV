// The signed-motion replay of CharaMotion.cpp on D3D11 WARP: a five-target G-buffer draw whose fourth output is a signed
// vector. The replay must put that vector, unclamped, into the private float target without touching the game's targets, and
// must leave every piece of pipeline state as it found it. Skip rules (UAV, open query, unknown shader) are driven end to end.
#include "check.h"
#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/chara/CharaCamera.h"
#include "ext/ffxiv/chara/CharaDispatch.h"
#include "ext/ffxiv/chara/CharaDlssRules.h"
#include "ext/ffxiv/chara/CharaMotion.h"
#include "ext/ffxiv/chara/CharaTags.h"

#include <DirectXPackedVector.h>
#include <d3d11_1.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <d3dcompiler.h>

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::chara;

// The dispatcher's raw full-screen draw (CharaDispatch.cpp is not part of this test).
namespace ext::ffxiv::chara
{
void DrawFullscreenTriangle(ID3D11DeviceContext* context) { context->Draw(3, 0); }
} // namespace ext::ffxiv::chara

namespace
{
constexpr UINT kSize = 4;

ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;

bool Ok(HRESULT hr, const char* what)
{
    if (SUCCEEDED(hr))
        return true;
    std::printf("FAIL: %s (0x%08lX)\n", what, static_cast<unsigned long>(hr));
    ++g_failures;
    return false;
}

const char* kShader = R"(
float4 VS(uint id : SV_VertexID) : SV_Position
{
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.25, 1.0);
}
struct Out { float4 c0 : SV_Target0; float4 c1 : SV_Target1; float4 c2 : SV_Target2; float4 c3 : SV_Target3; float4 c4 : SV_Target4; };
Out PS()
{
    Out o;
    o.c0 = float4(1, 0, 0, 1);
    o.c1 = float4(0, 1, 0, 1);
    o.c2 = float4(0, 0, 1, 1);
    o.c3 = float4(-1.5, 0.75, 0, 0);
    o.c4 = float4(1, 1, 0, 1);
    return o;
}
)";

struct Scene
{
    std::array<ComPtr<ID3D11Texture2D>, 5> targets;
    std::array<ComPtr<ID3D11RenderTargetView>, 5> rtvs;
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> cb944;
    ComPtr<ID3D11DepthStencilState> dss;
    ComPtr<ID3D11BlendState> blend;
    table::Snapshot snapshot;
};

D3D11_DEPTH_WRITE_MASK g_seenWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; // the depth write mask the last draw ran with
BOOL g_seenDepthEnable = FALSE;

void DrawNative(void*)
{
    ComPtr<ID3D11DepthStencilState> st;
    UINT ref = 0;
    g_context->OMGetDepthStencilState(&st, &ref);
    D3D11_DEPTH_STENCIL_DESC dd {};
    if (st)
        st->GetDesc(&dd);
    g_seenWriteMask = dd.DepthWriteMask;
    g_seenDepthEnable = dd.DepthEnable;
    g_context->Draw(3, 0);
}

void Bind(Scene& s)
{
    ID3D11RenderTargetView* rtvs[5];
    for (size_t i = 0; i < 5; ++i)
        rtvs[i] = s.rtvs[i].Get();
    g_context->OMSetRenderTargets(5, rtvs, s.dsv.Get());
    g_context->OMSetDepthStencilState(s.dss.Get(), 7);
    const FLOAT factor[4] { 0.5f, 0.5f, 0.5f, 0.5f };
    g_context->OMSetBlendState(s.blend.Get(), factor, 0xFFFFFFF0u);
    const D3D11_VIEWPORT vp { 0, 0, static_cast<float>(kSize), static_cast<float>(kSize), 0, 1 };
    g_context->RSSetViewports(1, &vp);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(s.vs.Get(), nullptr, 0);
    g_context->PSSetShader(s.ps.Get(), nullptr, 0);
    ID3D11Buffer* cb = s.cb944.Get();
    g_context->VSSetConstantBuffers(0, 1, &cb);
    g_context->PSSetConstantBuffers(2, 1, &cb);
}

void ClearTargets(Scene& s)
{
    const FLOAT clear[4] { 0.2f, 0.2f, 0.2f, 0.2f };
    for (auto& rtv : s.rtvs)
        g_context->ClearRenderTargetView(rtv.Get(), clear);
    g_context->ClearDepthStencilView(s.dsv.Get(), D3D11_CLEAR_DEPTH, 0.5f, 0);
}

Scene MakeScene()
{
    Scene s;
    D3D11_TEXTURE2D_DESC td {};
    td.Width = td.Height = kSize;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    s.snapshot.ready = true;
    for (size_t i = 0; i < 5; ++i)
    {
        Ok(g_device->CreateTexture2D(&td, nullptr, &s.targets[i]), "target");
        Ok(g_device->CreateRenderTargetView(s.targets[i].Get(), nullptr, &s.rtvs[i]), "rtv");
        s.snapshot.entries[8 + i] = { reinterpret_cast<uintptr_t>(s.targets[i].Get()), kSize, kSize };
    }
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    Ok(g_device->CreateTexture2D(&td, nullptr, &s.depth), "depth");
    Ok(g_device->CreateDepthStencilView(s.depth.Get(), nullptr, &s.dsv), "dsv");

    ComPtr<ID3DBlob> code, error;
    Ok(D3DCompile(kShader, std::strlen(kShader), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &code, &error), "compile VS");
    Ok(g_device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &s.vs), "VS");
    Ok(D3DCompile(kShader, std::strlen(kShader), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &code, &error), "compile PS");
    Ok(g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &s.ps), "PS");

    D3D11_BUFFER_DESC bd {};
    bd.ByteWidth = 0x3b0;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    Ok(g_device->CreateBuffer(&bd, nullptr, &s.cb944), "cb");

    D3D11_DEPTH_STENCIL_DESC dd {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS;
    Ok(g_device->CreateDepthStencilState(&dd, &s.dss), "dss");
    D3D11_BLEND_DESC bld {};
    bld.RenderTarget[0].RenderTargetWriteMask = 0x7;
    Ok(g_device->CreateBlendState(&bld, &s.blend), "blend");

    Bind(s);
    ClearTargets(s);
    return s;
}

DrawView ViewOf(Scene& s, uint32_t roles)
{
    DrawView v;
    v.context = g_context.Get();
    v.table = &s.snapshot;
    v.pixelShader = s.ps.Get();
    v.roles = roles;
    v.facts.targetWidth = v.facts.targetHeight = kSize;
    v.facts.samples = 1;
    v.facts.rtv0Entry = 8;
    return v;
}

std::vector<uint8_t> ReadBytes(ID3D11Texture2D* tex, UINT bytesPerPixel)
{
    D3D11_TEXTURE2D_DESC td {};
    tex->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    std::vector<uint8_t> out(kSize * kSize * bytesPerPixel);
    if (!Ok(g_device->CreateTexture2D(&td, nullptr, &staging), "staging"))
        return out;
    g_context->CopyResource(staging.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE m {};
    if (!Ok(g_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m), "Map"))
        return out;
    for (UINT y = 0; y < kSize; ++y)
        std::memcpy(out.data() + y * kSize * bytesPerPixel, static_cast<uint8_t*>(m.pData) + y * m.RowPitch, kSize * bytesPerPixel);
    g_context->Unmap(staging.Get(), 0);
    return out;
}

float Half(const std::vector<uint8_t>& bytes, size_t pixel, size_t component)
{
    uint16_t h;
    std::memcpy(&h, bytes.data() + pixel * 8 + component * 2, 2);
    return DirectX::PackedVector::XMConvertHalfToFloat(h);
}

bool MotionIs(float x, float y)
{
    const auto bytes = ReadBytes(motion::TargetTexture(), 8);
    for (size_t p = 0; p < kSize * kSize; ++p)
    {
        if (std::fabs(Half(bytes, p, 0) - x) > 1e-3f || std::fabs(Half(bytes, p, 1) - y) > 1e-3f)
            return false;
    }
    return true;
}

bool GameTargetsUntouched(Scene& s)
{
    for (auto& t : s.targets)
    {
        const auto b = ReadBytes(t.Get(), 4);
        for (size_t p = 0; p < kSize * kSize; ++p)
        {
            for (int c = 0; c < 4; ++c)
            {
                if (b[p * 4 + c] != 51) // 0.2 * 255
                    return false;
            }
        }
    }
    return true;
}

struct StateSnapshot
{
    std::array<ComPtr<ID3D11RenderTargetView>, 8> rtvs;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11DepthStencilState> dss;
    UINT stencilRef = 0;
    ComPtr<ID3D11BlendState> blend;
    FLOAT factor[4] {};
    UINT mask = 0;
};

StateSnapshot Capture()
{
    StateSnapshot s;
    ID3D11RenderTargetView* rtvs[8] {};
    ID3D11DepthStencilView* dsv = nullptr;
    g_context->OMGetRenderTargets(8, rtvs, &dsv);
    for (size_t i = 0; i < 8; ++i)
        s.rtvs[i].Attach(rtvs[i]);
    s.dsv.Attach(dsv);
    g_context->OMGetDepthStencilState(&s.dss, &s.stencilRef);
    g_context->OMGetBlendState(&s.blend, s.factor, &s.mask);
    return s;
}

bool SameState(const StateSnapshot& a, const StateSnapshot& b)
{
    for (size_t i = 0; i < 8; ++i)
    {
        if (a.rtvs[i].Get() != b.rtvs[i].Get())
            return false;
    }
    return a.dsv.Get() == b.dsv.Get() && a.dss.Get() == b.dss.Get() && a.stencilRef == b.stencilRef && a.blend.Get() == b.blend.Get() &&
           a.mask == b.mask && a.factor[0] == b.factor[0] && a.factor[3] == b.factor[3];
}

void Run()
{
    Scene s = MakeScene();
    CHECK(motion::EnsureTarget(g_device.Get(), kSize, kSize));
    CHECK(motion::TargetTexture() != nullptr && motion::TargetSrv() != nullptr);
    CHECK(MotionIs(0.0f, 0.0f)); // cleared on creation

    DrawCall call { &DrawNative, nullptr };

    // Queries opened before the module started are unknown: no replay until the first frame end.
    motion::MarkQueriesBlind();
    DrawView tagged = ViewOf(s, RoleGeometry);
    CHECK(motion::Replay(tagged, call).skip == motion::Skip::OpenQuery);
    CHECK(MotionIs(0.0f, 0.0f));
    motion::OnFrameEnd();

    // A tagged shader is replayed: the signed vector arrives unclamped, the game's targets and every state are untouched.
    const StateSnapshot before = Capture();
    ClearTargets(s);
    motion::ReplayResult r = motion::Replay(tagged, call);
    CHECK(r.outcome == motion::Outcome::Replayed);
    CHECK(g_seenWriteMask == D3D11_DEPTH_WRITE_MASK_ZERO && g_seenDepthEnable == TRUE); // depth tested, not written
    CHECK(MotionIs(-1.5f, 0.75f));
    CHECK(GameTargetsUntouched(s));
    {
        const auto d = ReadBytes(s.depth.Get(), 4); // 24-bit depth + 8-bit stencil: the replay must not write depth
        uint32_t raw;
        std::memcpy(&raw, d.data(), 4);
        CHECK((raw & 0xFFFFFF) >= 0x7FFFFE && (raw & 0xFFFFFF) <= 0x800001);
    }
    CHECK(SameState(before, Capture()));
    CHECK(motion::ReplayCount() == 1);

    // A second replay of the same state reuses the cached depth state.
    CHECK(motion::Replay(tagged, call).outcome == motion::Outcome::Replayed);

    // Clearing the target resets the motion.
    motion::ClearTarget(g_context.Get());
    CHECK(MotionIs(0.0f, 0.0f));

    // An open occlusion query blocks the replay until it ends.
    D3D11_QUERY_DESC qd {};
    qd.Query = D3D11_QUERY_OCCLUSION;
    ComPtr<ID3D11Query> occlusion;
    Ok(g_device->CreateQuery(&qd, &occlusion), "query");
    motion::OnQueryBegin(occlusion.Get());
    CHECK(motion::Replay(tagged, call).skip == motion::Skip::OpenQuery);
    CHECK(MotionIs(0.0f, 0.0f));
    motion::OnQueryEnd(occlusion.Get());
    CHECK(motion::Replay(tagged, call).outcome == motion::Outcome::Replayed);

    // A timestamp query does not count.
    qd.Query = D3D11_QUERY_EVENT;
    ComPtr<ID3D11Query> event;
    Ok(g_device->CreateQuery(&qd, &event), "event query");
    motion::OnQueryBegin(event.Get());
    CHECK(motion::Replay(tagged, call).outcome == motion::Outcome::Replayed);
    motion::OnQueryEnd(event.Get());

    // A bound unordered access view forbids the second draw.
    motion::ClearTarget(g_context.Get());
    ComPtr<ID3D11UnorderedAccessView> uav;
    D3D11_TEXTURE2D_DESC ud {};
    s.targets[0]->GetDesc(&ud);
    ComPtr<ID3D11Texture2D> uavTexture;
    Ok(g_device->CreateTexture2D(&ud, nullptr, &uavTexture), "uav texture");
    Ok(g_device->CreateUnorderedAccessView(uavTexture.Get(), nullptr, &uav), "uav");
    ID3D11RenderTargetView* rtvs[5];
    for (size_t i = 0; i < 5; ++i)
        rtvs[i] = s.rtvs[i].Get();
    ID3D11UnorderedAccessView* uavs[1] { uav.Get() };
    g_context->OMSetRenderTargetsAndUnorderedAccessViews(5, rtvs, s.dsv.Get(), 5, 1, uavs, nullptr);
    CHECK(motion::Replay(tagged, call).skip == motion::Skip::UnorderedAccess);
    CHECK(MotionIs(0.0f, 0.0f));
    Bind(s);

    // Only the opaque G-buffer: another table entry is left alone.
    DrawView transparent = ViewOf(s, RoleGeometry);
    s.snapshot.entries[8] = {};
    CHECK(motion::Replay(transparent, call).skip == motion::Skip::NotOpaqueGBuffer);
    s.snapshot.entries[8] = { reinterpret_cast<uintptr_t>(s.targets[0].Get()), kSize, kSize };

    // An untagged shader without the exact shape stays native; with the shape it is replayed and tagged on the spot.
    DrawView untagged = ViewOf(s, 0);
    ID3D11Buffer* tiny = nullptr;
    D3D11_BUFFER_DESC bd {};
    bd.ByteWidth = 0x100;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> tinyBuffer;
    Ok(g_device->CreateBuffer(&bd, nullptr, &tinyBuffer), "tiny cb");
    tiny = tinyBuffer.Get();
    g_context->PSSetConstantBuffers(2, 1, &tiny);
    CHECK(motion::Replay(untagged, call).skip == motion::Skip::UnknownShader);
    CHECK((RoleOf(s.ps.Get()) & RoleGeometry) == 0);

    // The wider shape (PreviewReplayWideShape): a camera block that is a view into a larger buffer is refused while the key is off and
    // replayed with it on. The key decides (an always-wide build fails the first check), and a size that only fits the camera range does
    // not tag the shader: the exact block does, later below.
    {
        D3D11_BUFFER_DESC wideDesc {};
        wideDesc.ByteWidth = 1024;
        wideDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> wide;
        Ok(g_device->CreateBuffer(&wideDesc, nullptr, &wide), "wide cb");
        ID3D11Buffer* bound = wide.Get();
        g_context->VSSetConstantBuffers(0, 1, &bound);
        g_context->PSSetConstantBuffers(2, 1, &bound);
        auto& cfg = ext::Cfg();
        cfg.ffxiv.chara.previewReplayWideShape = false;
        CHECK(motion::Replay(untagged, call).skip == motion::Skip::UnknownShader);
        cfg.ffxiv.chara.previewReplayWideShape = true;
        CHECK(motion::Replay(untagged, call).outcome == motion::Outcome::Replayed);
        CHECK((RoleOf(s.ps.Get()) & RoleGeometry) == 0);
        cfg.ffxiv.chara.previewReplayWideShape = false;
        ID3D11Buffer* back = s.cb944.Get();
        g_context->VSSetConstantBuffers(0, 1, &back);
        g_context->PSSetConstantBuffers(2, 1, &back);
    }

    // The wider shape end to end: the replay accepts a camera block that is a view into a larger buffer and does not tag the shader, the guides
    // are taken from that same draw, and the pass reaches the upscaler. Without the capture the same pass would stop at the camera, which is the
    // state the wider shape used to be left in. The camera flag is the real capture's answer, which is what the G-buffer case stores; the depth
    // facts are the only ones this test composes, because the depth view of a real preview draw lives in the dispatcher's view struct.
    {
        D3D11_BUFFER_DESC wideDesc {};
        wideDesc.ByteWidth = 1024;
        wideDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> wide;
        Ok(g_device->CreateBuffer(&wideDesc, nullptr, &wide), "wide cb");
        ID3D11Buffer* bound = wide.Get();
        g_context->VSSetConstantBuffers(0, 1, &bound);
        g_context->PSSetConstantBuffers(2, 1, &bound);
        auto& cfg = ext::Cfg();
        cfg.ffxiv.chara.previewReplayWideShape = true;
        const motion::ReplayResult replay = motion::Replay(untagged, call);
        CHECK(replay.outcome == motion::Outcome::Replayed);
        CHECK((RoleOf(s.ps.Get()) & RoleGeometry) == 0); // replayed, and deliberately not tagged
        const bool capture = motion::CapturesGuides((untagged.roles & RoleGeometry) != 0, replay.outcome == motion::Outcome::Replayed);
        CHECK(capture); // the draw the replay accepted gives the pass its guides
        const camera::Result captured = camera::Capture(g_context.Get());
        CHECK(captured == camera::Result::Captured);
        dlss::FinishFacts f;
        f.width = kSize;
        f.height = kSize;
        f.slotFound = true;
        f.now = 1000;
        f.lastUse = 1000;
        f.guides.replayed = replay.outcome == motion::Outcome::Replayed;
        f.guides.depthTexture = true;
        f.guides.depthView = true;
        f.guides.cameraCaptured = captured == camera::Result::Captured; // what the guide capture of that draw stores
        CHECK(dlss::DecideFinish(f) == dlss::FinishVerdict::Proceed);
        f.guides.cameraCaptured = false;
        CHECK(dlss::DecideFinish(f) == dlss::FinishVerdict::WaitingForCamera); // what the wider shape used to end in
        cfg.ffxiv.chara.previewReplayWideShape = false;
        camera::Release();
        ID3D11Buffer* back = s.cb944.Get();
        g_context->VSSetConstantBuffers(0, 1, &back);
        g_context->PSSetConstantBuffers(2, 1, &back);
    }

    // The camera block binding of a pass, asked from the context alone: the draw's own stage, its first constant and the pass size. Nothing that
    // cannot hold the camera block is accepted, and the other stage is never consulted (a stale binding there would name another window).
    {
        camera::Release();
        ID3D11Buffer* none = nullptr;
        g_context->VSSetConstantBuffers(0, 1, &none);
        g_context->PSSetConstantBuffers(2, 1, &none);
        CHECK(!camera::BoundBlock(g_context.Get(), camera::Stage::Pixel, 576, 960).Valid());
        CHECK(!camera::BoundBlock(g_context.Get(), camera::Stage::Vertex, 576, 960).Valid());
        ID3D11Buffer* tinyBound = tinyBuffer.Get(); // 0x100 bytes: cannot hold the camera block
        g_context->PSSetConstantBuffers(2, 1, &tinyBound);
        CHECK(!camera::BoundBlock(g_context.Get(), camera::Stage::Pixel, 576, 960).Valid());
        // the block at the pixel stage only: the G-buffer draw is named, a prepass of the same pass is not
        ID3D11Buffer* block = s.cb944.Get();
        g_context->PSSetConstantBuffers(2, 1, &block);
        camera::BlockKey key = camera::BoundBlock(g_context.Get(), camera::Stage::Pixel, 576, 960);
        CHECK(key.Valid() && key.buffer == reinterpret_cast<uintptr_t>(block) && key.first == 0 && key.width == 576 && key.height == 960);
        CHECK(!camera::BoundBlock(g_context.Get(), camera::Stage::Vertex, 576, 960).Valid());
        // the same block at the vertex stage: the prepass is named, and the pixel stage is not consulted
        g_context->VSSetConstantBuffers(0, 1, &block);
        g_context->PSSetConstantBuffers(2, 1, &none);
        const camera::BlockKey prepass = camera::BoundBlock(g_context.Get(), camera::Stage::Vertex, 576, 960);
        CHECK(prepass.Valid() && prepass.buffer == reinterpret_cast<uintptr_t>(block));
        CHECK(!camera::BoundBlock(g_context.Get(), camera::Stage::Pixel, 576, 960).Valid());
        // a wider buffer that can hold the block is a valid binding, and it is a different key from the exact one
        D3D11_BUFFER_DESC wideDesc {};
        wideDesc.ByteWidth = 1024;
        wideDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> wide;
        Ok(g_device->CreateBuffer(&wideDesc, nullptr, &wide), "wide cb");
        ID3D11Buffer* wideBuffer = wide.Get();
        g_context->PSSetConstantBuffers(2, 1, &wideBuffer);
        const camera::BlockKey view = camera::BoundBlock(g_context.Get(), camera::Stage::Pixel, 576, 960);
        CHECK(view.Valid() && view.buffer == reinterpret_cast<uintptr_t>(wideBuffer) && !(view == key));
        // A block bound as a view into the buffer: the first constant is part of the key. This software adapter keeps no constant buffer offsets
        // (PSGetConstantBuffers1 answers 0 and the whole buffer), so the key is checked against what the context reports and nothing more: the
        // claim that two windows that share one buffer get different identities is unverified on a runtime that keeps offsets.
        ComPtr<ID3D11DeviceContext1> context1;
        Ok(g_context.As(&context1), "ID3D11DeviceContext1");
        UINT first = 4, count = camera::kVectors;
        context1->PSSetConstantBuffers1(2, 1, &wideBuffer, &first, &count);
        ComPtr<ID3D11Buffer> reported;
        UINT reportedFirst = 0, reportedCount = 0;
        context1->PSGetConstantBuffers1(2, 1, &reported, &reportedFirst, &reportedCount);
        const camera::BlockKey offset = camera::BoundBlock(g_context.Get(), camera::Stage::Pixel, 576, 960);
        CHECK(offset.Valid() && offset.buffer == reinterpret_cast<uintptr_t>(wideBuffer));
        CHECK(offset.first == reportedFirst); // the key carries the first constant the context reports
        if (reportedFirst == first)
            CHECK(!(offset == view)); // a runtime that keeps the offset makes the view a different identity
        // the same binding asked again is the same key
        CHECK(camera::BoundBlock(g_context.Get(), camera::Stage::Pixel, 576, 960) == offset);
        g_context->PSSetConstantBuffers(2, 1, &none);
        camera::Release();
    }

    ID3D11Buffer* exact = s.cb944.Get();    g_context->PSSetConstantBuffers(2, 1, &exact);
    CHECK(motion::Replay(untagged, call).outcome == motion::Outcome::Replayed);
    CHECK((RoleOf(s.ps.Get()) & RoleGeometry) != 0);

    // Own draws are never replayed.
    {
        ReplayScope scope;
        CHECK(motion::Replay(tagged, call).skip == motion::Skip::OwnDraw);
    }

    // A size that is not the target's: no replay.
    DrawView other = ViewOf(s, RoleGeometry);
    other.facts.targetWidth = 8;
    CHECK(motion::Replay(other, call).skip == motion::Skip::NoTarget);

    // Releasing and asking again for the target works.
    motion::Release();
    CHECK(motion::TargetTexture() == nullptr);
    CHECK(motion::EnsureTarget(g_device.Get(), kSize, kSize));
    CHECK(MotionIs(0.0f, 0.0f));
    CHECK(motion::EnsureTarget(g_device.Get(), kSize * 2, kSize * 2)); // size change recreates
    D3D11_TEXTURE2D_DESC td {};
    motion::TargetTexture()->GetDesc(&td);
    CHECK(td.Width == kSize * 2 && td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
    motion::Release();
}
} // namespace

int main()
{
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    if (!Ok(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &g_device, nullptr, &g_context),
            "D3D11CreateDevice(WARP)"))
        return 1;
    Run();
    return TEST_RESULT("chara_motion_replay");
}
