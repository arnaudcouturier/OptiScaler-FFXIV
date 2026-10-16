#include "pch.h"

#include "ext/ffxiv/ui/UiSnapshot.h"

#include "ext/ffxiv/lighting/FfxivLightingCapture.h"
#include "ext/ffxiv/ui/UiCapture.h"
#include "ext/ffxiv/ui/UiDiag.h"

#include <State.h>

#include <atomic>

namespace ext::ffxiv::ui::snapshot
{
using Microsoft::WRL::ComPtr;

namespace
{
constexpr uint64_t kWindowFrames = 60;  // frames after the press in which a boundary candidate is waited for
constexpr uint32_t kMaxTries = 240;     // frames a readback may stay "still drawing" before it is given up

enum Slot : size_t { SlotR1, SlotR2, SlotBridge, SlotCount };

struct SlotState
{
    ComPtr<ID3D11Texture2D> texture; // staging, reused across presses
    bool filled = false;             // a copy was issued this press
    bool pending = false;            // waiting for readback
    uint32_t tries = 0;
    uint32_t ordinal = 0;            // candidate draw ordinal (0 for the bridge copy)
    uint64_t frame = 0;
    uint32_t width = 0, height = 0, format = 0;
    dfmt::RtKind kind = dfmt::RtKind::None;
    dfmt::RtShape shape {};
    std::string note;                // why a slot is empty
};

struct State_
{
    std::atomic<uint32_t> requests { 0 };
    std::atomic<bool> armed { false };
    uint32_t handled = 0;
    uint32_t press = 0;
    uint32_t images = 0;
    uint64_t armedFrom = 0, armedUntil = 0;
    uint64_t captureFrame = 0; // frame whose draws produced the current captures
    std::array<SlotState, SlotCount> slots {};
};

State_& S()
{
    static auto* s = new State_; // never destroyed
    return *s;
}

const char* SlotName(size_t i) { return i == SlotR1 ? "R1" : (i == SlotR2 ? "R2" : "bridge"); }

capture::CopyFn Trampoline()
{
    // The un-hooked CopyResource of the lighting hook: our copy must not reach OnCopy (FfxivLightingScan::ColorCopy and
    // the scene-HDR alias state would take it for a game copy). Null before the hooks exist: the plain context call is used.
    return ext::ffxiv::lighting::FfxivLightingCapture::copy;
}

uint8_t DescribeAndCopy(ID3D11DeviceContext* c, Core& core, SlotState& slot, uint32_t ordinal)
{
    ComPtr<ID3D11Device> device;
    c->GetDevice(&device);
    capture::Bound bound;
    const auto rt = core.ClassifyRt(c);
    const auto r = capture::CopyBound(device.Get(), c, true, 0, 0, slot.texture, Trampoline(), bound);
    slot.filled = r == capture::Result::Ok;
    slot.pending = slot.filled;
    slot.tries = 0;
    slot.ordinal = ordinal;
    slot.frame = core.Frame();
    slot.kind = rt.kind;
    slot.shape = rt.shape;
    slot.note = slot.filled ? "" : capture::ResultText(r);
    if (slot.filled)
    {
        slot.width = bound.desc.Width;
        slot.height = bound.desc.Height;
        slot.format = bound.desc.Format;
    }
    return slot.filled ? 1 : 0;
}

void ResetPress(State_& s)
{
    for (auto& slot : s.slots)
    {
        slot.filled = false;
        slot.pending = false;
        slot.ordinal = 0;
        slot.note.clear();
    }
}

bool AnyPending(const State_& s)
{
    for (const auto& slot : s.slots)
        if (slot.pending)
            return true;
    return false;
}

void Pump(State_& s, ID3D11DeviceContext* c, uint64_t frame)
{
    for (size_t i = 0; i < SlotCount; ++i)
    {
        SlotState& slot = s.slots[i];
        if (!slot.pending || !slot.texture || slot.frame >= frame)
            continue; // a copy issued this frame is still only queued

        D3D11_MAPPED_SUBRESOURCE mapped {};
        const HRESULT hr = c->Map(slot.texture.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
        {
            if (++slot.tries > kMaxTries)
            {
                slot.pending = false;
                slot.note = "readback never completed";
            }
            continue;
        }
        slot.pending = false;
        if (FAILED(hr))
        {
            slot.note = "Map failed";
            continue;
        }

        const uint64_t bytes = uint64_t(mapped.RowPitch) * slot.height;
        if (!diag::ImageBudgetLeft(bytes))
        {
            c->Unmap(slot.texture.Get(), 0);
            slot.note = "image byte budget of this run used up";
            continue;
        }
        std::vector<uint8_t> pixels(static_cast<size_t>(bytes));
        std::memcpy(pixels.data(), mapped.pData, pixels.size());
        c->Unmap(slot.texture.Get(), 0);

        char name[96];
        if (i == SlotBridge)
            snprintf(name, sizeof name, "snap-%02u-f%llu-bridge", s.press, (unsigned long long) slot.frame);
        else
            snprintf(name, sizeof name, "snap-%02u-f%llu-%s-o%u", s.press, (unsigned long long) slot.frame, SlotName(i),
                     slot.ordinal);
        diag::SubmitImage(name, std::move(pixels), slot.width, slot.height, slot.format, mapped.RowPitch,
                          static_cast<uint32_t>(slot.frame));
        ++s.images;
        diag::SetSnapshotStatus(s.press, s.images, AnyPending(s) ? "reading back" : "idle");
    }
    if (!AnyPending(s)) // a readback that failed or timed out must not leave the status at "reading back"
        diag::SetSnapshotStatus(s.press, s.images, "idle");
}

void WriteInfo(State_& s, Core& core)
{
    std::string t = "press " + std::to_string(s.press) + ": frame " + std::to_string(s.captureFrame) + "\n";
    for (size_t i = 0; i < SlotCount; ++i)
    {
        const SlotState& slot = s.slots[i];
        char line[320];
        if (slot.filled)
            snprintf(line, sizeof line, "  %-6s ordinal %u  render target %s %s  (%ux%u format %u)%s\n", SlotName(i), slot.ordinal,
                     dfmt::RtKindName(slot.kind), dfmt::ShapeText(slot.shape).c_str(), slot.width, slot.height, slot.format,
                     i == SlotBridge ? "  = swapchain buffer at Present entry" : "");
        else
            snprintf(line, sizeof line, "  %-6s none (%s)\n", SlotName(i), slot.note.empty() ? "no such candidate in the frame" : slot.note.c_str());
        t += line;
    }
    t += "  Toggle the game's hide-UI key between two presses to get a UI-hidden / UI-shown pair of the same scene.\n";
    (void) core;
    diag::AppendText("snap-info.txt", std::move(t));
}
} // namespace

void Request() { S().requests.fetch_add(1, std::memory_order_relaxed); }

bool Armed() { return S().armed.load(std::memory_order_relaxed); }

void OnPresentEntry(Core& core)
{
    auto& s = S();
    ID3D11DeviceContext* ctx = ext::ffxiv::lighting::FfxivLightingCapture::context;
    if (!ctx)
        return;

    const uint64_t frame = core.Frame();

    if (s.armed.load(std::memory_order_relaxed) && s.captureFrame == frame)
    {
        // A capture was taken in the draws of the frame that is ending. A candidate that a later stage draw dropped again
        // is stale: its picture is not the boundary of this frame.
        const Candidates& last = core.CurrentCandidates();
        SlotState& r1 = s.slots[SlotR1];
        SlotState& r2 = s.slots[SlotR2];
        if (r1.filled && r1.ordinal != last.r1)
        {
            r1.filled = r1.pending = false;
            r1.note = "stale: the candidate was superseded, the final R1 is ordinal " + std::to_string(last.r1);
        }
        if (r2.filled && r2.ordinal != last.r2)
        {
            r2.filled = r2.pending = false;
            r2.note = "stale: the candidate was superseded, the final R2 is ordinal " + std::to_string(last.r2);
        }

        if (r1.filled && r2.filled && r1.ordinal == r2.ordinal)
        {
            // Same draw, same picture: keep the R1 file only.
            r2.filled = r2.pending = false;
            r2.note = "same draw as R1 (see the R1 image)";
        }

        if (!r1.filled && !r2.filled)
        {
            s.captureFrame = 0; // nothing usable: wait for another frame inside the window
            for (auto& slot : s.slots)
                slot.pending = false;
        }
        else
        {
            // Copy the backbuffer as the bridge will see it a moment later (same frame number).
            IDXGISwapChain* chain = State::Instance().currentRealSwapchain;
            SlotState& bridge = s.slots[SlotBridge];
            if (!chain)
                bridge.note = "no real swapchain";
            else
            {
                UINT index = 0;
                ComPtr<IDXGISwapChain3> chain3;
                if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain3))) && chain3)
                    index = chain3->GetCurrentBackBufferIndex();
                ComPtr<ID3D11Texture2D> buffer;
                ComPtr<ID3D11Device> device;
                ctx->GetDevice(&device);
                capture::Bound bound;
                if (FAILED(chain->GetBuffer(index, IID_PPV_ARGS(&buffer))) || !buffer)
                    bridge.note = "GetBuffer failed";
                else
                {
                    bound.texture = buffer;
                    buffer->GetDesc(&bound.desc);
                    if (!capture::EnsureTexture(device.Get(), bound, true, bridge.texture))
                        bridge.note = "could not create the staging texture";
                    else
                    {
                        if (auto copy = Trampoline())
                            copy(ctx, bridge.texture.Get(), buffer.Get());
                        else
                            ctx->CopyResource(bridge.texture.Get(), buffer.Get());
                        bridge.filled = bridge.pending = true;
                        bridge.tries = 0;
                        bridge.ordinal = 0;
                        bridge.frame = frame;
                        bridge.width = bound.desc.Width;
                        bridge.height = bound.desc.Height;
                        bridge.format = bound.desc.Format;
                        bridge.kind = dfmt::RtKind::Backbuffer;
                        bridge.shape = { bridge.width, bridge.height, bridge.format };
                    }
                }
            }
            WriteInfo(s, core);
            s.armed.store(false, std::memory_order_relaxed);
            diag::SetSnapshotStatus(s.press, s.images, "reading back");
        }
    }

    // Readbacks of copies issued in earlier frames (the GPU needs a moment; Map never waits).
    if (AnyPending(s))
        Pump(s, ctx, frame);

    if (s.armed.load(std::memory_order_relaxed))
    {
        if (frame > s.armedUntil)
        {
            diag::AppendText("snap-info.txt", "press " + std::to_string(s.press) + ": no boundary candidate in " +
                                                  std::to_string(kWindowFrames) + " frames, nothing captured\n");
            s.armed.store(false, std::memory_order_relaxed);
            diag::SetSnapshotStatus(s.press, s.images, "no boundary seen (press again in a scene)");
        }
        return;
    }

    if (s.handled < s.requests.load(std::memory_order_relaxed) && !AnyPending(s))
    {
        ++s.handled;
        if (s.press >= kMaxPresses)
        {
            diag::SetSnapshotStatus(s.press, s.images, "press limit of this run reached");
            return;
        }
        ++s.press;
        ResetPress(s);
        s.captureFrame = 0;
        s.armedFrom = frame + 1;
        s.armedUntil = frame + kWindowFrames;
        s.armed.store(true, std::memory_order_relaxed);
        diag::SetSnapshotStatus(s.press, s.images, "armed: waiting for the next boundary");
    }
}

void OnCandidate(ID3D11DeviceContext* context, Core& core, const Core::DrawResult& draw)
{
    auto& s = S();
    if (!s.armed.load(std::memory_order_relaxed))
        return;
    const uint64_t frame = core.Frame();
    if (frame < s.armedFrom || frame > s.armedUntil)
        return;
    if (s.captureFrame != 0 && s.captureFrame != frame)
        return; // one frame per press

    const uint32_t ordinal = draw.ordinal;
    const Candidates& c = draw.cand;
    bool took = false;
    // A candidate that was set (not cleared) by this very draw: copy BEFORE the draw runs.
    if ((draw.change & 1) && c.r1 == ordinal)
        took |= DescribeAndCopy(context, core, s.slots[SlotR1], ordinal) != 0;
    if ((draw.change & 2) && c.r2 == ordinal)
        took |= DescribeAndCopy(context, core, s.slots[SlotR2], ordinal) != 0;
    if (took)
        s.captureFrame = frame;
}
} // namespace ext::ffxiv::ui::snapshot
