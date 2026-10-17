#pragma once
// What the dispatcher hands to a preview module for one draw call.

#include <d3d11.h>

#include "ext/ffxiv/chara/CharaDecision.h"
#include "ext/ffxiv/chara/CharaTable.h"

namespace ext::ffxiv::chara
{
enum class DrawApi : uint8_t
{
    Draw,
    DrawIndexed,
    DrawInstanced,
    DrawIndexedInstanced,
    DrawInstancedIndirect,
    DrawIndexedInstancedIndirect,
};

inline constexpr const char* DrawApiName(DrawApi a)
{
    switch (a)
    {
    case DrawApi::Draw: return "Draw";
    case DrawApi::DrawIndexed: return "DrawIndexed";
    case DrawApi::DrawInstanced: return "DrawInstanced";
    case DrawApi::DrawIndexedInstanced: return "DrawIndexedInstanced";
    case DrawApi::DrawInstancedIndirect: return "DrawInstancedIndirect";
    default: return "DrawIndexedInstancedIndirect";
    }
}

// The native draw. Calling it runs the game's own draw (and, outside the feature, the other modules' work around it).
// A preview module that handles a draw must call it exactly once.
struct DrawCall
{
    void (*function)(void*) = nullptr;
    void* user = nullptr;
    void operator()() const { function(user); }
};

// One draw as seen by the dispatcher. Every pointer is borrowed: valid only for the duration of the call.
struct DrawView
{
    ID3D11DeviceContext* context = nullptr; // the immediate context
    DrawApi api = DrawApi::Draw;
    DrawKind kind = DrawKind::Ignore;
    DrawFacts facts {};
    uint32_t roles = 0; // ext::ffxiv::chara::Role bits of the bound pixel shader
    uint64_t now = 0;   // GetTickCount64() when the draw started
    const table::Snapshot* table = nullptr;

    ID3D11PixelShader* pixelShader = nullptr;
    ID3D11RenderTargetView* rtv0 = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11Texture2D* rtvTexture = nullptr; // null when there is no render target or it is not a 2D texture
    ID3D11Texture2D* dsvTexture = nullptr;
    D3D11_VIEWPORT viewport {};            // viewport 0, valid when haveViewport
    bool haveViewport = false;

    PassState* pass = nullptr; // the render this draw belongs to (shared by the modules)
    bool passStarted = false;  // this draw is the first accepted geometry draw of a new pass (view.pass->counter was just bumped)
    bool finisherAccepted = false; // kind == Finisher and the finisher check passed (evaluated before the draw, see FinisherCheck)
};
} // namespace ext::ffxiv::chara
