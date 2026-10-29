#include "pch.h"

#include "ext/ffxiv/ui/PlateDraws.h"

#include "ext/core/Crc32.h"
#include "ext/ffxiv/companion/replay/FfxivNameplateRenderer.h"
#include "ext/ffxiv/ui/PlatePackets.h"
#include "ext/ffxiv/ui/UiBoundaryCore.h"
#include "ext/hudless/PlateAssocCounters.h"
#include "ext/hudless/PlateProbeFormat.h"
#include "ext/hudless/PlateProbePackets.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <string>
#include <string.h>
#include <unordered_map>
#include <vector>

// What the draw probe reads per draw is the D3D11 state of the immediate context (shaders, depth-stencil, blend, render target, viewport,
// bound textures) through the Get* calls: CPU side only, no map, no readback, nothing bound or drawn.
//
// Threads: OnGameDraw, InterceptedDraw and OnFrameEnd share the state in `Probe` (rows, row cap, overlay boxes, shader table) WITHOUT a lock.
// That is sound only because the draw hooks and the frame end (the bridge Present) run on the same thread, which is what the boundary detector
// that calls them already assumes for its own state. The writer thread never touches `Probe`: rows reach it as text through AppendFile, and
// the counters it prints are atomics.
namespace ext::ffxiv::ui::plate
{
namespace
{
using Microsoft::WRL::ComPtr;
namespace pf = ext::hudless::plate;

constexpr GUID kVsTag { 0x6b0d3a52, 0x41c7, 0x4f09, { 0x8c, 0x1e, 0x5d, 0x7a, 0x93, 0x20, 0xb1, 0x66 } };

struct Probe
{
    pf::ShaderTable table;
    std::unordered_map<const void*, size_t> psIndex;
    std::unordered_map<const void*, uint32_t> textureIds;
    pf::DrawBudget budget;
    std::string rows;
    pf::RowCap cap; // the row cap of the frame in progress (draws.csv)
    std::vector<OverlayBox> overlay; // boxes of the candidate rows of the frame in progress, for the overlay
    uint64_t frame = 1;
    bool headerSent = false;
    bool truncatedSent[2] = { false, false };
    Core* core = nullptr;
};

Probe& S()
{
    static Probe* probe = new Probe; // never destroyed
    return *probe;
}

std::string Hex32(uint32_t v)
{
    char b[16];
    snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

pf::RtClass ClassOf(dfmt::RtKind kind)
{
    switch (kind)
    {
    case dfmt::RtKind::Backbuffer:
        return pf::RtClass::Backbuffer;
    case dfmt::RtKind::SameAsBackbuffer:
        return pf::RtClass::BackbufferLike;
    case dfmt::RtKind::Other:
        return pf::RtClass::Other;
    default:
        return pf::RtClass::None;
    }
}

// No depth-stencil state object bound means the D3D11 default (LESS, writes on), which is never a candidate.
pf::DepthState ReadDepth(ID3D11DeviceContext* c, bool dsvBound)
{
    ComPtr<ID3D11DepthStencilState> state;
    UINT ref = 0;
    c->OMGetDepthStencilState(&state, &ref);
    pf::DepthState d = pf::DefaultDepthState(dsvBound);
    if (state)
    {
        D3D11_DEPTH_STENCIL_DESC desc {};
        state->GetDesc(&desc);
        d.depthEnable = desc.DepthEnable != FALSE;
        d.depthWrite = desc.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO;
        d.depthFunc = static_cast<uint32_t>(desc.DepthFunc);
        d.stencilEnable = desc.StencilEnable != FALSE;
    }
    return d;
}

// Identity of a texture inside the run: the order of first sight (a pointer is not stable between runs).
uint32_t TextureId(const void* resource)
{
    Probe& p = S();
    if (p.textureIds.size() > 16384)
        p.textureIds.clear(); // bounded; the ids restart
    auto it = p.textureIds.find(resource);
    if (it != p.textureIds.end())
        return it->second;
    const uint32_t id = static_cast<uint32_t>(p.textureIds.size()) + 1;
    p.textureIds[resource] = id;
    return id;
}

// The rest of a row, beyond the cheap depth test.
void FillRow(ID3D11DeviceContext* c, pf::DrawRow& row)
{
    ComPtr<ID3D11VertexShader> vs;
    c->VSGetShader(&vs, nullptr, nullptr);
    if (vs)
    {
        uint32_t crc = 0;
        UINT size = sizeof crc;
        if (SUCCEEDED(vs->GetPrivateData(kVsTag, &size, &crc)) && size == sizeof crc)
            row.vs = Hex32(crc);
    }
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    c->IAGetPrimitiveTopology(&topology);
    row.topology = static_cast<uint32_t>(topology);

    ID3D11Buffer* vertexBuffer = nullptr;
    UINT stride = 0, offset = 0;
    c->IAGetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
    if (vertexBuffer)
        vertexBuffer->Release();
    row.stride = stride;

    UINT viewports = 1;
    D3D11_VIEWPORT vp {};
    c->RSGetViewports(&viewports, &vp);
    if (viewports > 0)
    {
        row.vpWidth = static_cast<uint32_t>(vp.Width);
        row.vpHeight = static_cast<uint32_t>(vp.Height);
    }

    ComPtr<ID3D11BlendState> blend;
    FLOAT factor[4] {};
    UINT mask = 0;
    c->OMGetBlendState(&blend, factor, &mask);
    if (blend)
    {
        D3D11_BLEND_DESC desc {};
        blend->GetDesc(&desc);
        const auto& t = desc.RenderTarget[0];
        row.blend = t.BlendEnable != FALSE;
        row.srcBlend = static_cast<uint32_t>(t.SrcBlend);
        row.dstBlend = static_cast<uint32_t>(t.DestBlend);
        row.blendOp = static_cast<uint32_t>(t.BlendOp);
        row.srcAlpha = static_cast<uint32_t>(t.SrcBlendAlpha);
        row.dstAlpha = static_cast<uint32_t>(t.DestBlendAlpha);
    }

    ID3D11ShaderResourceView* views[4] {};
    c->PSGetShaderResources(0, 4, views);
    std::string srvs;
    for (UINT i = 0; i < 4; ++i)
    {
        if (!views[i])
            continue;
        ComPtr<ID3D11Resource> resource;
        views[i]->GetResource(&resource);
        std::string token = std::to_string(i) + ":" + std::to_string(TextureId(resource.Get()));
        D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        if (resource)
            resource->GetType(&dimension);
        if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        {
            D3D11_TEXTURE2D_DESC desc {};
            static_cast<ID3D11Texture2D*>(resource.Get())->GetDesc(&desc);
            token += "/" + std::to_string(desc.Width) + "x" + std::to_string(desc.Height) + "/" +
                     std::to_string(static_cast<uint32_t>(desc.Format));
        }
        srvs += (srvs.empty() ? "" : "|") + token;
        views[i]->Release();
    }
    if (!srvs.empty())
        row.srvs = srvs;
}

// The POSITION element of the input layout the draw is bound with. Only the layouts of the three nameplate vertex shaders carry the tag that
// says where it is (the nameplate renderer puts it on them when they are created, FfxivNameplateRenderer.h); any other layout reads as unknown.
bool PositionOfBoundLayout(ID3D11DeviceContext* c, pf::VertexLayout& out)
{
    namespace rend = ext::ffxiv::companion::replay::FfxivNameplateRenderer;
    ComPtr<ID3D11InputLayout> input;
    c->IAGetInputLayout(&input);
    if (!input)
        return false;
    rend::Layout layout;
    UINT size = sizeof layout;
    if (FAILED(input->GetPrivateData(rend::layoutTag, &size, &layout)) || size != sizeof layout || layout.count == 0 || layout.count > 16)
        return false;
    for (UINT i = 0; i < layout.count; ++i)
    {
        const auto& e = layout.elements[i];
        if (e.index != 0 || _stricmp(e.semantic, "POSITION") != 0)
            continue;
        const uint32_t floats = e.format == DXGI_FORMAT_R32G32_FLOAT         ? 2
                                : e.format == DXGI_FORMAT_R32G32B32_FLOAT    ? 3
                                : e.format == DXGI_FORMAT_R32G32B32A32_FLOAT ? 4
                                                                              : 0;
        if (floats == 0)
            return false;
        out.posOffset = e.offset;
        out.posFloats = floats;
        return true;
    }
    return false;
}

// Stride of the first vertex buffer bound at the input assembler. The game binds the buffer of the layout it draws, but which input slot it
// uses is not assumed here.
uint32_t BoundStride(ID3D11DeviceContext* c)
{
    ID3D11Buffer* buffers[4] {};
    UINT strides[4] {}, offsets[4] {};
    c->IAGetVertexBuffers(0, 4, buffers, strides, offsets);
    uint32_t stride = 0;
    for (UINT i = 0; i < 4; ++i)
    {
        if (buffers[i] == nullptr)
            continue;
        if (stride == 0)
            stride = strides[i];
        buffers[i]->Release();
    }
    return stride;
}

// What the packet side found for a row, and, for an associated row, why it has no box.
struct Applied
{
    pf::RowOutcome outcome = pf::RowOutcome::NoPacket;
    pf::BoxReason box = pf::BoxReason::Ok;
};

// Fills the packet columns of a row from what the packet side found for the draw. The box is read from the vertices the record copied at
// enqueue time, with the position offset of the input layout the draw is bound with, and only when the strides of the packets, the renderer's
// slot and the bound vertex buffer agree. Returns what happened to the row (for the counters).
Applied ApplyPacket(ID3D11DeviceContext* c, pf::DrawRow& row, const PacketInfo& packet)
{
    const pf::DrawRecord* record = packet.record;
    if (record == nullptr)
        return { packet.outcome };
    row.batch = record->batch;
    row.batchDraw = record->batchDraw;
    if (packet.outcome != pf::RowOutcome::Associated)
        return { packet.outcome };
    row.qkey = pf::detail::Hex(record->qkey);
    row.entries = record->entries;
    pf::VertexLayout layout;
    const bool tagged = !record->run.empty() && PositionOfBoundLayout(c, layout);
    pf::Box box;
    const pf::BoxReason reason = pf::ResolveBox(record->run, record->stride, tagged, layout, tagged ? BoundStride(c) : 0, record->used, box);
    if (reason == pf::BoxReason::Ok)
    {
        row.hasBox = true;
        row.box[0] = box.x0;
        row.box[1] = box.y0;
        row.box[2] = box.x1;
        row.box[3] = box.y1;
        row.boxSource = "vtx";
    }
    return { pf::RowOutcome::Associated, reason };
}

// The row has been admitted by the frame's row cap (RowCap::Admit) before it was built.
void AddRow(Probe& p, const pf::DrawRow& row, const Applied& applied)
{
    p.rows += pf::DrawsRow(row);
    pf::Counters().NoteRow(applied.outcome, applied.box);
    if (row.hasBox && pf::IsCandidate(row.depth, row.rt))
    {
        OverlayBox box;
        std::copy(row.box, row.box + 4, box.box);
        box.rtWidth = row.rtWidth;
        box.rtHeight = row.rtHeight;
        box.vpWidth = row.vpWidth;
        box.vpHeight = row.vpHeight;
        p.overlay.push_back(box);
    }
}

void SetTarget(pf::DrawRow& row, const RtInfo& rt)
{
    row.rt = ClassOf(rt.kind);
    row.rtWidth = rt.shape.width;
    row.rtHeight = rt.shape.height;
    row.rtFormat = rt.shape.format;
}

void SetShader(pf::DrawRow& row, const ShaderRec* rec)
{
    if (!rec)
        return;
    row.ps = rec->Id();
    row.stage = rec->stage != kStageUnknown ? int(rec->stage) : -1;
}

void SetArgs(pf::DrawRow& row, const DrawArgs& args)
{
    row.kind = args.kind;
    row.indexCount = args.count;
    row.instanceCount = args.instances;
    row.startIndex = args.start;
    row.baseVertex = args.baseVertex;
}
} // namespace

// Runs on the thread of the draw hooks. It shares the unlocked state in `Probe` with OnFrameEnd and InterceptedDraw, which is sound because the draw
// hooks and the frame end (the bridge Present) are on the same thread, as the boundary detector that calls them already assumes.
void OnGameDraw(ID3D11DeviceContext* c, Core& core, uint32_t ordinal, uint8_t stage, uint32_t firstUiOrdinal)
{
    try
    {
        Probe& p = S();
        p.core = &core;
        p.frame = core.Frame();
        const DrawArgs args = g_args;

        // UI phase: a UI-stage shader, or any draw from the first UI draw of the frame on
        const bool uiPhase = stage == kUiStage || (firstUiOrdinal != 0 && ordinal >= firstUiOrdinal);

        // the cheap part, for every draw of every frame: the depth-stencil state and whether a depth view is bound
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11DepthStencilView> dsv;
        c->OMGetRenderTargets(1, &rtv, &dsv);
        const pf::DepthState depth = ReadDepth(c, dsv != nullptr);
        const bool depthPasses = depth.dsvBound && depth.depthEnable && depth.depthFunc == pf::kCompareGreaterEqual && !depth.depthWrite &&
                                 !depth.stencilEnable;

        // the render target is looked up for UI-phase draws and for draws that pass the depth part of r0; the others count as not looked up
        RtInfo rt;
        if (uiPhase || depthPasses)
            rt = core.ClassifyRt(c);
        const pf::RtClass rtClass = ClassOf(rt.kind);
        const bool r0 = pf::IsCandidate(depth, rtClass);

        ShaderRec* rec = core.PixelShaderRec(c);
        auto it = p.psIndex.find(rec);
        size_t index;
        if (it == p.psIndex.end())
        {
            index = p.table.Find(rec ? rec->Id() : std::string("-"), rec && rec->stage != kStageUnknown ? int(rec->stage) : -1);
            p.psIndex[rec] = index;
        }
        else
            index = it->second;
        p.table.Add(index, p.frame, r0, rtClass);

        // full rows only on the logged frames, for UI-phase draws and for any draw that satisfies r0
        if (!pf::LogThisFrame(p.frame) || !(uiPhase || r0))
            return;
        // the row cap of the frame, asked before the row is built (a refused row costs nothing more); an r0 draw is a priority row, which has a
        // second, higher limit, so the rows the probe is about are not the ones that get dropped
        if (!p.cap.Admit(r0))
            return;
        pf::DrawRow row;
        row.frame = p.frame;
        row.draw = ordinal;
        SetShader(row, rec);
        SetArgs(row, args);
        SetTarget(row, rt);
        row.depth = depth;
        FillRow(c, row);
        const Applied applied = ApplyPacket(c, row, CurrentPacket());
        p.table.NoteVertexShader(index, row.vs);
        AddRow(p, row, applied);
    }
    catch (...)
    {
    }
}

// Runs on the Present thread, which is the thread of the draw hooks (see OnGameDraw): the rows, the row cap and the overlay boxes of the frame
// are read and reset here without a lock. The writer thread only ever sees the text this hands to AppendFile and the atomic counters.
void OnFrameEnd(uint64_t frame)
{
    try
    {
        Probe& p = S();
        // a frame that lost rows to the row cap says so in the file (comment rows an analysis skips) and in the run's counters
        if (p.cap.Capped())
        {
            p.rows += pf::CapMarker(frame, p.cap);
            pf::Counters().NoteCappedFrame(p.cap.dropped, p.cap.priorityDropped);
        }
        p.cap.NextFrame();
        if (!p.rows.empty())
        {
            std::string text;
            if (!p.headerSent)
            {
                text = pf::DrawsHeaderText();
                p.headerSent = true;
            }
            text += p.rows;
            const int which = frame <= pf::kEarlyFrames ? 0 : 1;
            if (p.budget.Take(frame, text.size()))
                pf::AppendFile("draws.csv", std::move(text));
            else if (!p.truncatedSent[which])
            {
                p.truncatedSent[which] = true;
                pf::AppendFile("draws.csv", pf::kTruncatedLine);
            }
        }
        p.rows.clear();
        // the packet side reads game memory only around the frames this probe logs: the frame being drawn next is frame + 1
        SetFrameHint(frame + 1);
        if (pf::LogThisFrame(frame))
        {
            OverlayFrame shown;
            shown.frame = frame;
            shown.boxes = std::move(p.overlay);
            PublishOverlay(std::move(shown));
        }
        p.overlay.clear();
        if (ext::hudless::plate::OverlayWanted())
            CheckOverlaySlot();
        if (pf::RewriteShadersThisFrame(frame))
            pf::RewriteFile("shaders-plate.csv", p.table.Csv());
    }
    catch (...)
    {
    }
}

namespace detail
{
void VertexShaderCreated(ID3D11VertexShader* shader, const void* code, size_t size)
{
    if (shader == nullptr || code == nullptr || size < 32 || size > 0xFFFFFFFFull)
        return;
    const uint32_t crc = ext::core::Crc32(code, size);
    shader->SetPrivateData(kVsTag, sizeof crc, &crc);
}

void InterceptedDraw(ID3D11DeviceContext* c)
{
    try
    {
        Probe& p = S();
        if (p.core == nullptr || !pf::LogThisFrame(p.frame))
            return;
        if (!p.cap.Admit(true)) // an intercepted draw is a priority row
            return;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11DepthStencilView> dsv;
        c->OMGetRenderTargets(1, &rtv, &dsv);
        pf::DrawRow row;
        row.frame = p.frame;
        row.intercepted = true;
        SetArgs(row, g_args);
        row.depth = ReadDepth(c, dsv != nullptr);
        SetTarget(row, p.core->ClassifyRt(c));
        SetShader(row, p.core->PixelShaderRec(c));
        FillRow(c, row);
        const Applied applied = ApplyPacket(c, row, CurrentPacket());
        AddRow(p, row, applied);
    }
    catch (...)
    {
    }
}
} // namespace detail
} // namespace ext::ffxiv::ui::plate
