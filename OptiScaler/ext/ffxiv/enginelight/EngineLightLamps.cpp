#include "pch.h"

#include "ext/ffxiv/enginelight/EngineLightLamps.h"

#include "ext/ffxiv/diag/ShaderCrc.h"  // the CRC the diagnostic stamped on the pixel shader, for the capture frame's rows
#include "ext/ffxiv/diag/StageDump.h"  // StageOpen: the capture frame is the only time anything below formats a string

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

// Engine light controls, the lamp arm (EngineLightLamps.h). The cost while the factor is 1: nothing (the draw path never builds a wrapper). While it is not: on a light draw
// (about 46 a frame) one blend-state query, one description read, two render-target queries; on a volume draw (34 a frame) one cached-state set and one set back.
namespace ext::ffxiv::enginelight
{
using Microsoft::WRL::ComPtr;

namespace
{
std::atomic<float> g_factor { 1.0f };
std::atomic<uint64_t> g_scaled { 0 };
std::atomic<uint64_t> g_considered { 0 };
std::atomic<uint64_t> g_notVolume { 0 };
std::atomic<uint32_t> g_statesCreated { 0 };
std::atomic<bool> g_failed { false };
LampRowFn g_sinkForTest = nullptr;

// The derived blend states, keyed by the CONTENT of the game's state (never by its pointer: the game may release and re-create a state at the same address). A handful of
// distinct additive states exist in a session, so the table is tiny and a miss is rare.
struct Derived
{
    D3D11_BLEND_DESC original {};
    ComPtr<ID3D11BlendState> state;
};
std::mutex g_cacheMutex;
std::vector<Derived> g_cache;
ID3D11Device* g_cacheDevice = nullptr; // compared, never dereferenced
constexpr size_t kMaxCached = 8;

BlendRt RtOf(const D3D11_RENDER_TARGET_BLEND_DESC& desc)
{
    BlendRt rt;
    rt.enable = desc.BlendEnable != FALSE;
    rt.src = int(desc.SrcBlend);
    rt.dst = int(desc.DestBlend);
    rt.op = int(desc.BlendOp);
    return rt;
}

// The derived state for the game's blend description: SrcBlend ONE -> BLEND_FACTOR on the targets the draw writes. Null when the state could not be created.
ID3D11BlendState* DerivedFor(ID3D11Device* device, const D3D11_BLEND_DESC& original)
{
    std::lock_guard lock(g_cacheMutex);
    if (g_cacheDevice != device)
    {
        g_cache.clear();
        g_cacheDevice = device;
    }
    for (const Derived& entry : g_cache)
        if (std::memcmp(&entry.original, &original, sizeof original) == 0)
            return entry.state.Get();
    D3D11_BLEND_DESC desc = original;
    const int targets = original.IndependentBlendEnable ? 2 : 1;
    for (int i = 0; i < targets; ++i)
        if (desc.RenderTarget[i].BlendEnable && desc.RenderTarget[i].SrcBlend == D3D11_BLEND_ONE)
            desc.RenderTarget[i].SrcBlend = D3D11_BLEND_BLEND_FACTOR;
    Derived entry;
    entry.original = original;
    if (FAILED(device->CreateBlendState(&desc, &entry.state)) || !entry.state)
    {
        g_failed.store(true, std::memory_order_relaxed);
        return nullptr;
    }
    if (g_cache.size() >= kMaxCached)
        g_cache.clear();
    g_cache.push_back(entry);
    g_statesCreated.fetch_add(1, std::memory_order_relaxed);
    return g_cache.back().state.Get();
}

uint32_t FormatOf(ID3D11RenderTargetView* view)
{
    if (view == nullptr)
        return 0;
    D3D11_RENDER_TARGET_VIEW_DESC desc {};
    view->GetDesc(&desc);
    return uint32_t(desc.Format);
}

std::string Hex(const unsigned char* bytes, size_t count)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    text.reserve(count * 2);
    for (size_t i = 0; i < count; ++i)
    {
        text += kDigits[bytes[i] >> 4];
        text += kDigits[bytes[i] & 0x0F];
    }
    return text;
}

std::string PointerText(const void* p)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p)));
    return buf;
}

// One row of the capture frame's lamp file: every volume draw's whole g_LightParam as bound, with the shader, the buffer and the targets it wrote. The radius (the WVP rows
// at byte 320 and m_Attenuation at byte 64 of the same struct, enginemap section 2 row 2) and the colour (byte 32 / 48) read off these bytes in a later session.
// The capture frame only: the caller tested StageOpen, so no other frame builds a string, and the single staging copy + map is a diagnostic's stall.
void DumpLight(ID3D11DeviceContext* context, uint32_t lightSlot, float factor, ID3D11RenderTargetView* rt0, ID3D11RenderTargetView* rt1)
{
    static std::atomic<uint32_t> serial { 0 };
    LampRowFn sink = g_sinkForTest != nullptr ? g_sinkForTest : ext::ffxiv::diag::detail::g_lampRow;
    if (sink == nullptr)
        return;
    ComPtr<ID3D11Buffer> bound;
    context->PSGetConstantBuffers(lightSlot, 1, &bound);
    if (!bound)
        return;
    D3D11_BUFFER_DESC desc {};
    bound->GetDesc(&desc);
    if (desc.ByteWidth == 0 || desc.ByteWidth > 4096)
        return;
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    D3D11_BUFFER_DESC stagingDesc {};
    stagingDesc.ByteWidth = desc.ByteWidth;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> staging;
    if (device == nullptr || FAILED(device->CreateBuffer(&stagingDesc, nullptr, &staging)))
        return;
    context->CopyResource(staging.Get(), bound.Get());
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)) || mapped.pData == nullptr)
        return;
    const std::string hex = Hex(static_cast<const unsigned char*>(mapped.pData), desc.ByteWidth);
    context->Unmap(staging.Get(), 0);
    ComPtr<ID3D11PixelShader> shader;
    context->PSGetShader(&shader, nullptr, nullptr);
    char crc[16] = "-";
    if (const uint32_t value = ext::ffxiv::diag::ReadShaderCrc(shader.Get()); value != 0)
        snprintf(crc, sizeof crc, "0x%08X", value);
    ComPtr<ID3D11Resource> r0, r1;
    if (rt0 != nullptr)
        rt0->GetResource(&r0);
    if (rt1 != nullptr)
        rt1->GetResource(&r1);
    char head[256];
    snprintf(head, sizeof head, "%u,%s,volume,%.4f,%u,%s,%s,%s,", serial.fetch_add(1, std::memory_order_relaxed), crc, double(factor), desc.ByteWidth,
             PointerText(bound.Get()).c_str(), PointerText(r0.Get()).c_str(), PointerText(r1.Get()).c_str());
    sink(std::string(head) + hex + "\n");
}

// The set-and-restore of one volume draw. Built on the stack around the game's draw call; its destructor puts the game's blend state, factor and sample mask back whatever
// happened in between (a throw out of the draw included).
class Scope
{
  public:
    Scope(const LampDraw& call)
    {
        try
        {
            g_considered.fetch_add(1, std::memory_order_relaxed);
            ID3D11DeviceContext* context = call.context;
            ComPtr<ID3D11BlendState> original;
            float originalFactor[4] {};
            UINT originalMask = 0;
            context->OMGetBlendState(&original, originalFactor, &originalMask);
            if (!original)
            {
                NotVolume();
                return;
            }
            D3D11_BLEND_DESC desc {};
            original->GetDesc(&desc);
            if (!IsAdditive(RtOf(desc.RenderTarget[0])))
            {
                NotVolume();
                return;
            }
            ID3D11RenderTargetView* views[2] { nullptr, nullptr };
            context->OMGetRenderTargets(2, views, nullptr);
            const bool shape = IsVolumeShape(RtOf(desc.RenderTarget[0]), RtOf(desc.RenderTarget[1]), desc.IndependentBlendEnable != FALSE, FormatOf(views[0]), FormatOf(views[1]));
            if (shape && ext::ffxiv::diag::StageOpen(ext::ffxiv::diag::StageId::NightGrade))
                DumpLight(context, call.lightSlot, call.factor, views[0], views[1]);
            for (ID3D11RenderTargetView*& view : views)
                if (view != nullptr)
                {
                    view->Release();
                    view = nullptr;
                }
            if (!shape)
            {
                NotVolume();
                return;
            }
            ComPtr<ID3D11Device> device;
            context->GetDevice(&device);
            ID3D11BlendState* derived = device != nullptr ? DerivedFor(device.Get(), desc) : nullptr;
            if (derived == nullptr)
                return; // fail open: the game's own state, the game's own picture
            const float factor[4] { call.factor, call.factor, call.factor, 1.0f };
            _context = context;
            _original = original;
            std::memcpy(_originalFactor, originalFactor, sizeof _originalFactor);
            _originalMask = originalMask;
            _armed = true; // set BEFORE the state call: the restore must run even if that call throws half way
            context->OMSetBlendState(derived, factor, originalMask);
            g_scaled.fetch_add(1, std::memory_order_relaxed);
        }
        catch (...)
        {
            // Fail open. If the set had already been armed the destructor still restores.
        }
    }

    ~Scope()
    {
        if (!_armed)
            return;
        try
        {
            _context->OMSetBlendState(_original.Get(), _originalFactor, _originalMask);
        }
        catch (...)
        {
        }
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

  private:
    void NotVolume() { g_notVolume.fetch_add(1, std::memory_order_relaxed); }

    ID3D11DeviceContext* _context = nullptr;
    ComPtr<ID3D11BlendState> _original;
    float _originalFactor[4] { 1.0f, 1.0f, 1.0f, 1.0f };
    UINT _originalMask = 0xffffffffu;
    bool _armed = false;
};
} // namespace

void LampDraw::Invoke(void* opaque)
{
    LampDraw& self = *static_cast<LampDraw*>(opaque);
    Scope scope(self); // sets the derived state for a volume draw; the destructor restores it, also when the draw throws
    self.draw(self.user);
}

void SetLampFactor(float factor) { g_factor.store(factor == factor ? factor : 1.0f, std::memory_order_relaxed); }
float LampFactorNow() { return g_factor.load(std::memory_order_relaxed); }
bool LampsWanted() { return !NearOne(LampFactorNow()); }

bool PrepareLampDraw(ID3D11DeviceContext* context, engine::Anchors anchors, DrawFn& draw, void*& user, LampDraw& out)
{
    if (!engine::IsShaftsLightAnchors(anchors) || draw == nullptr || context == nullptr)
        return false;
    const float factor = g_factor.load(std::memory_order_relaxed);
    if (NearOne(factor))
        return false; // the engine's own value: no wrapper, no state, nothing
    out.context = context;
    out.draw = draw;
    out.user = user;
    out.factor = factor;
    out.lightSlot = engine::ShaftsSlotOf(anchors);
    draw = &LampDraw::Invoke;
    user = &out;
    return true;
}

LampStatus CurrentLampStatus()
{
    LampStatus status;
    status.factor = LampFactorNow();
    status.scaled = g_scaled.load(std::memory_order_relaxed);
    status.considered = g_considered.load(std::memory_order_relaxed);
    status.notVolume = g_notVolume.load(std::memory_order_relaxed);
    status.statesCreated = g_statesCreated.load(std::memory_order_relaxed);
    status.failed = g_failed.load(std::memory_order_relaxed);
    return status;
}

void ReleaseLamps()
{
    std::lock_guard lock(g_cacheMutex);
    g_cache.clear();
    g_cacheDevice = nullptr;
}

void SetLampRowSinkForTest(LampRowFn sink) { g_sinkForTest = sink; }

std::string LampRowHeader() { return "serial,ps_crc,class,factor,bytes,ptr,rt0,rt1,hex\n"; }
} // namespace ext::ffxiv::enginelight
