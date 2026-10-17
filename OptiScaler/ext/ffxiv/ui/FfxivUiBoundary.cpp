#include "pch.h"

#include "ext/ffxiv/ui/FfxivUiBoundary.h"

#include "ext/core/Crc32.h"
#include "ext/core/HooksBypass.h"
#include "ext/ffxiv/lighting/FfxivLightingCapture.h"
#include "ext/ffxiv/ui/UiBoundaryCore.h"
#include "ext/ffxiv/ui/UiDiag.h"
#include "ext/ffxiv/ui/UiSnapshot.h"
#include "ext/hdr/scene/FfxivSceneHdr.h"
#include "ext/ffxiv/ui/UiLive.h"
#include "ext/hudless/HudlessSource.h"
#include "ext/hudless/HudlessState.h"
#include "ext/reshade/ReShadeAddon.h"
#include "ext/reshade/ReShadeBoundary.h"

#include <State.h>

#include <atomic>

// S10 package A (docs/HUDLESS.md section 10): wiring of the boundary detector core to the game. With Diagnostics alone (D1) it is
// observation only. With [HudLess] Mode on (Wanted) the live source (UiLive.h, package E) applies the ReShade presets in place at the R1b
// boundary (first UI draw into the backbuffer after the last scene stage) and delivers the UI-free frame through Take().
namespace ext::ffxiv::ui
{
namespace
{
inline int64_t Qpc()
{
    LARGE_INTEGER v {};
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

// The buffer of `chain` the bridge copies from (Dx11wDx12SC::_CopyDx11BackBufferToShared uses GetBuffer(index) with
// index = GetCurrentBackBufferIndex() when IDXGISwapChain3 exists). `device` (may be null) restricts the choice to a chain
// on the game's device: another component's helper chain (State::currentRealSwapchain can be replaced by a composition
// swapchain of an overlay) must not become "the backbuffer". Only the pointer value and the description are kept: a
// retained reference would make ResizeBuffers fail.
bool BufferOf(IDXGISwapChain* chain, ID3D11Device* device, const void** texture, dfmt::RtShape* shape)
{
    if (!chain)
        return false;
    if (device)
    {
        Microsoft::WRL::ComPtr<ID3D11Device> owner;
        if (FAILED(chain->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get() != device)
            return false;
    }

    UINT index = 0;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> chain3;
    if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain3))) && chain3)
        index = chain3->GetCurrentBackBufferIndex();

    Microsoft::WRL::ComPtr<ID3D11Texture2D> buffer;
    if ((FAILED(chain->GetBuffer(index, IID_PPV_ARGS(&buffer))) || !buffer) &&
        (index == 0 || FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&buffer))) || !buffer))
        return false;

    D3D11_TEXTURE2D_DESC d {};
    buffer->GetDesc(&d);
    *texture = buffer.Get();
    *shape = { d.Width, d.Height, static_cast<uint32_t>(d.Format) };
    return true; // `buffer` is released on return; the pointer stays valid as an identity for the frame
}

bool Backbuffer(const void** texture, dfmt::RtShape* shape)
{
    auto& st = State::Instance();
    Microsoft::WRL::ComPtr<ID3D11Device> game;
    if (auto* context = ext::ffxiv::lighting::FfxivLightingCapture::context)
        context->GetDevice(&game);
    // Prefer a chain on the game's device; fall back to the bridge's real chain whatever its device (a proxied device
    // object may not compare equal).
    return BufferOf(st.currentRealSwapchain, game.Get(), texture, shape) || BufferOf(st.currentSwapchain, game.Get(), texture, shape) ||
           BufferOf(st.currentRealSwapchain, nullptr, texture, shape);
}
Core& C()
{
    static Core* core = []
    {
        auto* c = new Core; // never destroyed (no COM or lock teardown under the loader lock)
        c->SetGameStageTag(&ext::hdr::FfxivSceneHdr::shaderTag);
        c->SetBackbufferProvider(&Backbuffer);
        return c;
    }();
    return *core;
}

ContextTable& Others()
{
    static auto* table = new ContextTable;
    return *table;
}

// ---- S10 E: the live source (docs/HUDLESS.md section 16 and S10-E-LOG.md) -------------------------------------------------------
// The lighting hook's un-hooked CopyResource trampoline, read at call time (it is assigned when the hooks are installed); our
// snapshot copy must not reach OnCopy (FfxivLightingScan::ColorCopy / the scene-HDR alias state would take it for a game copy).
void STDMETHODCALLTYPE SnapshotCopy(ID3D11DeviceContext* c, ID3D11Resource* dest, ID3D11Resource* source)
{
    if (auto* fn = ext::ffxiv::lighting::FfxivLightingCapture::copy)
        fn(c, dest, source);
    else
        c->CopyResource(dest, source);
}

std::string EffectsDetail() { return ext::reshade::Stats().lastDetail; }

std::atomic<live::LiveSource*> g_live { nullptr };

live::LiveSource& Live()
{
    static live::LiveSource* source = []
    {
        live::Env env;
        env.effects = &ext::reshade::ApplyPresetsInPlace; // returns NotEnabled by itself with [HudLess] ReShadePresets = off
        env.copy = &SnapshotCopy;
        env.effectsDetail = &EffectsDetail;
        auto* s = new live::LiveSource(env); // never destroyed
        g_live.store(s, std::memory_order_release);
        return s;
    }();
    return *source;
}

void LogLines(const std::string& lines)
{
    size_t start = 0;
    while (start < lines.size())
    {
        size_t end = lines.find('\n', start);
        if (end == std::string::npos)
            end = lines.size();
        LOG_INFO("{}", lines.substr(start, end - start));
        start = end + 1;
    }
}

class UiBoundaryProvider final : public ext::hudless::IHudlessSource
{
  public:
    bool Active() override { return Armed(); }

    // Armed by [HudLess] Mode (Wanted): the live source delivers the UI-free frame. Armed by Diagnostics alone (D1) it only
    // observes: no frame is delivered and nothing is applied, copied or changed.
    ext::hudless::TakeResult Take(ext::hudless::FrameImage& out) override
    {
        out = {};
        if (!Armed())
            return ext::hudless::TakeResult::NoSource;
        if (!ext::hudless::Wanted())
            return ext::hudless::TakeResult::NotThisFrame;
        try
        {
            return Live().Take(out);
        }
        catch (...)
        {
            out = {};
            return ext::hudless::TakeResult::NotThisFrame;
        }
    }

    const char* LastReason() override
    {
        if (!Armed())
            return "boundary detector not armed";
        if (!ext::hudless::Wanted())
            return "diagnostic build: boundary observed, no frame delivered";
        try
        {
            return Live().LastReason();
        }
        catch (...)
        {
            return "boundary detector failed";
        }
    }

    // Bridge Present exit: closes the frame (EndFrame runs at every Present, armed or not).
    void EndFrame() override
    {
        if (!Armed())
        {
            ReleaseLive();
            return;
        }
        try
        {
            FrameOutcome outcome = C().CompleteFrame();
            const live::FrameNote note = live::NoteOf(outcome); // before the outcome is moved into the D1 recorder
            if (DiagnosticsOn())
                diag::OnFrameComplete(C(), std::move(outcome), Others().Get());
            if (ext::hudless::Wanted())
            {
                LogLines(Live().EndFrame(note)); // summary and state lines are written here, never from the draw path
                ext::reshade::FrameEnded();
            }
            else
                ReleaseLive();
        }
        catch (...)
        {
        }
    }

  private:
    static void ReleaseLive()
    {
        if (auto* s = g_live.load(std::memory_order_acquire))
            s->Release();
    }
};

UiBoundaryProvider provider;
} // namespace

ext::hudless::IHudlessSource& Provider() { return provider; }

void OnDraw(ID3D11DeviceContext* context, bool latched)
{
    try
    {
        if (ext::core::HooksBypassed())
            return;

        if (!latched)
        {
            Others().Note(context, C().Frame());
            return;
        }

        Core& core = C();
        const int64_t start = Qpc();
        const Core::DrawResult res = core.OnDraw(context);
        const bool live = ext::hudless::Wanted();
        // S10 D0: the ReShade feasibility probe runs at the earliest candidate boundary (R3 moved), before that draw runs.
        // One atomic load and a failed compare unless the user armed it from its menu card. Not while the live path is on: it
        // calls render_effects itself and a second call in the same frame would apply the effects twice.
        if (res.moved && !live)
            ext::reshade::ProbeAtBoundary(context);
        if (res.change != 0 && snapshot::Armed())
            snapshot::OnCandidate(context, core, res); // before the candidate draw runs (and before the live effects: D1 shows the raw boundary)

        if (live && res.r1bChanged)
        {
            // S10 E2: R1b only (R1/R2/R3 are tracked for D1 but never acted on). R1b set by this draw = the first UI draw into the
            // backbuffer after the last scene stage = capture opportunity (UI draws into other render targets, e.g. the character
            // preview, are skipped); R1b dropped by this draw = a later scene/post stage superseded it.
            const live::DrawEvent ev = live::EventOf(core, res);
            if (ev.r1bSet || ev.r1bCleared)
            {
                ext::core::BypassScope bypass; // ReShade's own draws and copies must not look like game draws to other hooks
                Live().OnDraw(context, ev);
            }
        }
        core.AddHookTicks(Qpc() - start);
    }
    catch (...)
    {
    }
}

void TagShader(ID3D11PixelShader* shader, const void* code, size_t size)
{
    if (!Armed() || shader == nullptr || code == nullptr || size == 0 || size > 0xFFFFFFFFull)
        return;

    try
    {
        C().TagShader(shader, ext::core::Crc32(code, size), static_cast<uint32_t>(size));
    }
    catch (...)
    {
    }
}

void OnPresentBegin()
{
    try
    {
        RefreshArmed();
        if (!Armed())
            return;

        Core& core = C();
        if (DiagnosticsOn())
        {
            diag::OnPresentEntry(core);
            snapshot::OnPresentEntry(core);
        }
        core.FreezeFrame();
    }
    catch (...)
    {
    }
}
} // namespace ext::ffxiv::ui
