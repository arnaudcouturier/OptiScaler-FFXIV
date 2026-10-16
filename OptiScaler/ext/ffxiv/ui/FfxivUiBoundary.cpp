#include "pch.h"

#include "ext/ffxiv/ui/FfxivUiBoundary.h"

#include "ext/core/Crc32.h"
#include "ext/core/HooksBypass.h"
#include "ext/ffxiv/lighting/FfxivLightingCapture.h"
#include "ext/ffxiv/ui/UiBoundaryCore.h"
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

// The swapchain backbuffer the bridge copies from (Dx11wDx12SC::_CopyDx11BackBufferToShared uses GetBuffer(index) with
// index = GetCurrentBackBufferIndex() when IDXGISwapChain3 exists). Only the pointer value and the description are kept:
// a retained reference would make ResizeBuffers fail.
bool Backbuffer(const void** texture, fmt::RtShape* shape)
{
    IDXGISwapChain* chain = State::Instance().currentRealSwapchain;
    if (!chain)
        return false;

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
            (void) outcome;
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
        core.OnDraw(context);
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
        core.BeginRenderThread(GetCurrentThreadId());
        core.FreezeFrame();
    }
    catch (...)
    {
    }
}
} // namespace ext::ffxiv::ui
