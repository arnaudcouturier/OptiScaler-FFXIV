#include "pch.h"

#include "ext/ffxiv/ui/FfxivUiBoundary.h"

#include "ext/core/Crc32.h"
#include "ext/core/HooksBypass.h"
#include "ext/ffxiv/lighting/FfxivLightingCapture.h"
#include "ext/ffxiv/ui/UiBoundaryCore.h"
#include "ext/ffxiv/ui/UiDiag.h"
#include "ext/ffxiv/ui/UiSnapshot.h"
#include "ext/hdr/scene/FfxivSceneHdr.h"
#include "ext/hudless/HudlessSource.h"

#include <State.h>

// S10 package A (docs/HUDLESS.md section 10): wiring of the boundary detector core to the game. D1 is observation only:
// Take() never delivers a frame, no draw, copy or Present is changed.
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

class UiBoundaryProvider final : public ext::hudless::IHudlessSource
{
  public:
    bool Active() override { return Armed(); }

    // D1 does not deliver frames (docs/HUDLESS.md section 12): armed means "observing".
    ext::hudless::TakeResult Take(ext::hudless::FrameImage& out) override
    {
        out = {};
        return Armed() ? ext::hudless::TakeResult::NotThisFrame : ext::hudless::TakeResult::NoSource;
    }

    const char* LastReason() override
    {
        return Armed() ? "diagnostic build: boundary observed, no frame delivered" : "boundary detector not armed";
    }

    // Bridge Present exit: closes the frame (EndFrame runs at every Present, armed or not).
    void EndFrame() override
    {
        if (!Armed())
            return;
        try
        {
            FrameOutcome outcome = C().CompleteFrame();
            if (DiagnosticsOn())
                diag::OnFrameComplete(C(), std::move(outcome), Others().Get());
        }
        catch (...)
        {
        }
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
        if (res.change != 0 && snapshot::Armed())
            snapshot::OnCandidate(context, core, res); // before the candidate draw runs
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
