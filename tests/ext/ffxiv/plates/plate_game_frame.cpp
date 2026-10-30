// The game side of the native nameplates (ext/ffxiv/plates/PlateNative.cpp) on a real WARP D3D11 context: every state a
// nameplate draw needs is a real object, the engine's answers are the test's, and the redirect draws through a recording
// stand-in for the game's own DrawIndexed.
//
// The rules this proves: the classifier refuses a draw that has any one condition out of place *before* the engine is
// asked (so a refused draw never reserves anything); a frame is decided once and the answer holds for all its draws; a
// hidden frame's plates are drawn into the image only; a shadow frame's are drawn both places; and a hidden frame whose
// redirect cannot run falls back to the native draw (one routine: hide never turns into a shadow draw by another path).
#include "pch.h"

#include "plate_game_rig.h"

#include "ext/ExtFeatures.h"
#include "ext/ffxiv/plates/PlateNative.h"
#include "ext/hudless/PlateEngine.h"
#include "ext/hudless/PlateSource.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
using Microsoft::WRL::ComPtr;
using plate_game_test::DrawStateRig;

int failures = 0;
void Check(bool ok, const char* what, int line)
{
    if (!ok)
    {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

// ---- the recording stand-in for the game's own DrawIndexed ------------------------------------------------------------
DrawStateRig* g_rig = nullptr;
int g_nativeDraws = 0; // calls that went to the game's own colour target
int g_slotDraws = 0;   // ... and to the plate image
int g_frameMarks = 0;  // markers painted into whatever target the draw had bound, this frame

// Paints the marker of this draw's ordinal at (ordinal - 1, 0) of the bound colour target. A clear before every redirected
// draw would wipe the earlier markers, so the pixels say whether the image was cleared once.
void PaintMarkers(ID3D11DeviceContext* context)
{
    ++g_frameMarks;
    ID3D11RenderTargetView* targets[2] = {};
    context->OMGetRenderTargets(2, targets, nullptr);
    for (UINT i = 0; i < 2; ++i)
    {
        if (targets[i] == nullptr)
            continue;
        ComPtr<ID3D11DeviceContext1> context1;
        const HRESULT direct = context->QueryInterface(IID_PPV_ARGS(context1.GetAddressOf()));
        if (SUCCEEDED(direct) && context1)
        {
            const FLOAT colour[4] = { g_frameMarks / 255.0f, 0.f, 0.f, 1.f };
            const D3D11_RECT rect { static_cast<LONG>(g_frameMarks - 1), 0, static_cast<LONG>(g_frameMarks), 1 };
            context1->ClearView(targets[i], colour, &rect, 1);
        }
        targets[i]->Release();
    }
    // The first bound target decides where the draw went: the plate image, or the game's own colour target.
    if (targets[0] == g_rig->SlotView())
        ++g_slotDraws;
    else
        ++g_nativeDraws;
}

void STDMETHODCALLTYPE Original(ID3D11DeviceContext* context, UINT /*count*/, UINT /*start*/, INT /*baseVertex*/)
{
    PaintMarkers(context);
}

// One draw of the game, exactly as the hook does it: the plate engine gets the first chance, and when it does not take the
// draw the game's own draw runs (in the product, the whole DrawIndexed path).
bool GameDraw()
{
    if (ext::ffxiv::plates::InterceptIndexed(g_rig->Context(), 3, 0, 0, reinterpret_cast<void*>(&Original)))
        return true;
    Original(g_rig->Context(), 3, 0, 0);
    return false;
}

// ---- the engine's answers ----------------------------------------------------------------------------------------------
void NewFrame(unsigned serial, ext::hudless::plates::FrameMode mode, bool withTarget = true)
{
    ext::hudless::plates::Serial() = serial;
    ext::hudless::plates::FrameReservation& answer = ext::hudless::plates::Answer();
    answer = ext::hudless::plates::FrameReservation {};
    answer.mode = mode;
    answer.width = g_rig->Width();
    answer.height = g_rig->Height();
    answer.format = static_cast<unsigned>(DXGI_FORMAT_R8G8B8A8_UNORM);
    answer.target = withTarget ? g_rig->SlotView() : nullptr;
}

void ResetCounts()
{
    g_nativeDraws = 0;
    g_slotDraws = 0;
    g_frameMarks = 0;
    ext::hudless::plates::ResetCounters();
}
} // namespace

// The engine's frame-finish hook: not part of this test's subject (the product registers it at load).
namespace ext::hudless::plates
{
void SetPlateFinishHook(PlateFinishHook, void*) {}
PlateFinishHook PlateFinish()
{
    return nullptr;
}
void* PlateFinishUser()
{
    return nullptr;
}
} // namespace ext::hudless::plates

int Run()
{
    using ext::hudless::plates::FrameMode;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const HRESULT created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                              &device, nullptr, &context);
    CHECK(SUCCEEDED(created));
    if (!device || !context)
        return 1;
    DrawStateRig rig(device.Get(), context.Get());
    g_rig = &rig;

    // ---- the key off: one atomic load, nothing else ---------------------------------------------------------------
    rig.BindPlateDraw();
    NewFrame(1, FrameMode::Hidden);
    ResetCounts();
    CHECK(!GameDraw());
    CHECK(g_nativeDraws == 1 && g_slotDraws == 0);
    CHECK(ext::hudless::plates::AskCount() == 0);

    // ---- the key on: a declined frame stays native, and its later draws do not read the state or ask again ---------
    ext::SetActive(ext::F::Plate, true);
    NewFrame(1, FrameMode::Declined);
    ResetCounts();
    CHECK(!GameDraw());
    CHECK(g_nativeDraws == 1 && g_slotDraws == 0);
    CHECK(ext::hudless::plates::AskCount() == 1);
    CHECK(!GameDraw());
    CHECK(g_nativeDraws == 2);
    CHECK(ext::hudless::plates::AskCount() == 1); // latched: no second ask in the same frame
    // ... and the later draw of a declined frame costs no state read and no redirect attempt.
    CHECK(ext::ffxiv::plates::Snapshot().drawsFallback == 0);
    // The reservation carried the game's own target size and format.
    CHECK(ext::hudless::plates::LastWidth() == rig.Width() && ext::hudless::plates::LastHeight() == rig.Height());
    CHECK(ext::hudless::plates::LastFormat() == static_cast<unsigned>(DXGI_FORMAT_R8G8B8A8_UNORM));

    // ---- a hidden frame: the plate triangles go into the image and the game's own draw is skipped -------------------
    {
        // The image starts dirty, so the frame's own clear is what has to take the stale pixels away.
        const float stale[4] = { 0.5f, 0.5f, 0.5f, 1.f };
        context->ClearRenderTargetView(rig.SlotView(), stale);
        NewFrame(2, FrameMode::Hidden);
        ResetCounts();
        const uint32_t targetBefore = rig.PixelOf(rig.Target(), 0, 0);
        CHECK(GameDraw()); // hidden: the caller must not draw
        CHECK(GameDraw()); // ... and the frame stays hidden for its later draws
        CHECK(g_nativeDraws == 0 && g_slotDraws == 2);
        CHECK(ext::hudless::plates::AskCount() == 1);
        CHECK(ext::hudless::plates::DrawCount() == 2); // two draws reached the slot
        // Both markers survived: the slot was cleared once, before the frame's first plate draw.
        const uint32_t first = rig.PixelOf(rig.SlotTexture(), 0, 0);
        const uint32_t second = rig.PixelOf(rig.SlotTexture(), 1, 0);
        CHECK((first & 0xFFu) == 1u && (second & 0xFFu) == 2u);
        // ... and the rest of the image is transparent: one clear for the frame, not a stale picture.
        CHECK(rig.PixelOf(rig.SlotTexture(), rig.Width() / 2, rig.Height() / 2) == 0u);
        // The game's own target never saw a draw: its pixels are exactly what the previous frame left.
        CHECK(rig.PixelOf(rig.Target(), 0, 0) == targetBefore);

        // A second hidden frame clears again (a clear that is not keyed to the frame would leave this one dirty).
        context->ClearRenderTargetView(rig.SlotView(), stale);
        NewFrame(3, FrameMode::Hidden);
        ResetCounts();
        CHECK(GameDraw());
        CHECK(rig.PixelOf(rig.SlotTexture(), rig.Width() / 2, rig.Height() / 2) == 0u);
        CHECK((rig.PixelOf(rig.SlotTexture(), 0, 0) & 0xFFu) == 1u);
    }

    // ---- a shadow frame: the image is built and the game draws its own plates too ----------------------------------
    {
        NewFrame(4, FrameMode::Shadow);
        const float clear[4] = { 0.f, 0.f, 0.f, 0.f };
        context->ClearRenderTargetView(rig.SlotView(), clear);
        ResetCounts();
        CHECK(!GameDraw());
        CHECK(g_nativeDraws == 1 && g_slotDraws == 1);
        CHECK(ext::hudless::plates::DrawCount() == 1);
    }

    // ---- a hidden frame whose redirect cannot run: the draw stays native (hide never falls into a shadow path) ------
    {
        NewFrame(5, FrameMode::Hidden, false); // the engine has no render target for the frame
        ResetCounts();
        CHECK(!GameDraw());
        CHECK(g_nativeDraws == 1 && g_slotDraws == 0);
        CHECK(ext::hudless::plates::DrawCount() == 0);
        // no original at all: the same verdict, and nothing reached the image
        NewFrame(6, FrameMode::Hidden);
        ResetCounts();
        CHECK(!ext::ffxiv::plates::InterceptIndexed(rig.Context(), 3, 0, 0, nullptr));
        CHECK(g_slotDraws == 0);
        CHECK(ext::hudless::plates::DrawCount() == 0);
    }

    // ---- the classifier, through the real state: one broken condition keeps the draw native and asks nothing --------
    {
        int index = 0;
        for (const char* broken : DrawStateRig::kBroken)
        {
            ++index;
            rig.BindPlateDraw(broken);
            NewFrame(100 + static_cast<unsigned>(index), FrameMode::Hidden);
            ResetCounts();
            const bool consumed = GameDraw();
            if (consumed)
                std::printf("FAIL: a draw with '%s' out of place was taken for a plate draw\n", broken);
            CHECK(!consumed);
            CHECK(g_nativeDraws == 1 && g_slotDraws == 0);
            CHECK(ext::hudless::plates::AskCount() == 0); // the checks run before the reserve
            CHECK(ext::hudless::plates::DrawCount() == 0);
        }
        CHECK(index == static_cast<int>(DrawStateRig::kBrokenCount));
        // ... and the correct state is accepted on the very next frame, so the loop above did not wedge anything.
        rig.BindPlateDraw();
        NewFrame(400, FrameMode::Hidden);
        ResetCounts();
        CHECK(GameDraw());
        CHECK(g_nativeDraws == 0 && g_slotDraws == 1);
    }

    // ---- a hidden frame does not hide a draw that is not a plate draw -----------------------------------------------
    {
        NewFrame(500, FrameMode::Hidden);
        ResetCounts();
        CHECK(GameDraw()); // first plate draw: the frame is hidden
        rig.BindPlateDraw("shader"); // a UI draw with the same states but another pixel shader
        CHECK(!GameDraw());          // it is native, and the frame stays hidden for the plate draws
        CHECK(g_nativeDraws == 1 && g_slotDraws == 1);
        CHECK(ext::hudless::plates::AskCount() == 1);
    }

    // ---- a later draw of a hidden frame with another target size stays native ---------------------------------------
    {
        // The frame reserved its slot for the first draw's size; a later plate draw onto a differently sized target
        // cannot be written into that slot, so it must be left to the game instead of being bound to a wrong-sized view.
        DrawStateRig other(device.Get(), context.Get(), rig.Width() / 2, rig.Height() / 2);
        rig.BindPlateDraw(); // the previous block left a UI draw bound
        NewFrame(600, FrameMode::Hidden);
        ResetCounts();
        CHECK(GameDraw()); // the first draw reserves the frame at 64x32
        other.BindPlateDraw();
        CHECK(!GameDraw()); // the same state on a 32x16 target: native
        CHECK(g_nativeDraws == 1 && g_slotDraws == 1);
        CHECK(ext::hudless::plates::AskCount() == 1);
    }

    ext::SetActive(ext::F::Plate, false);
    if (failures == 0)
        std::puts("PASS: plate game side: fail-closed classifier before the reserve, one answer per frame, hidden draws "
                  "only in the image, shadow draws in both, a refused redirect stays native, and a later draw of another "
                  "size stays native");
    return failures == 0 ? 0 : 1;
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    try
    {
        return Run();
    }
    catch (const std::exception& error)
    {
        std::puts("EXCEPTION:");
        std::puts(error.what());
        return 2;
    }
}
