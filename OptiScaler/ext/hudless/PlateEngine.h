#pragma once
// What the game side asks of the plate engine: the frame's reservation (taken once, at the first accepted plate draw),
// the count of plate draws that reached the slot, and the record serial that tells one frame from the next. The engine
// (UiPlates.h) implements these; the game side includes only this header, so it never depends on the pass, the slot
// table or the settings, and a test can drive it through a stand-in.
//
// Render thread only, like the draws and the present seam they are called from.

#include <cstdint>

struct ID3D11RenderTargetView;

namespace ext::hudless::plates
{
// What the game side does with this frame's plates, decided once at the first accepted plate draw.
enum class FrameMode : uint8_t
{
    Declined, // the frame stays native, nothing is reserved: no slot, no image
    Shadow,   // the image is built and the frame is also drawn natively (the warm-up proof)
    Hidden,   // only the image is drawn: the frame's plate draws are not drawn by the game
};

struct FrameReservation
{
    FrameMode mode = FrameMode::Declined;
    int slot = -1;
    unsigned width = 0;
    unsigned height = 0;
    // The slot's DXGI format (as an unsigned; the game side compares it with its own target's format).
    unsigned format = 0;
    // The slot's render target view on the game's D3D11 device (null unless the mode is Shadow or Hidden): clear it
    // once, bind it together with the game's depth-stencil view and draw the plate triangles into it.
    ID3D11RenderTargetView* target = nullptr;

    explicit operator bool() const { return mode != FrameMode::Declined; }
};

// The frame's answer. The first accepted plate draw of a frame asks once, after every draw-state check passed; a slot
// is reserved before the answer, so nothing that can fail runs after the frame may be hidden.
//
// `format` is the DXGI format of the game's colour target (the cast is the caller's): a frame whose target format is not
// the slot's format is refused, because drawing it into the slot would swap its channels. Fail closed.
FrameReservation ReserveFrame(unsigned width, unsigned height, unsigned format);
// One plate draw reached the slot (clears nothing, draws nothing: the count is what the alive proof needs).
void NotePlateDraw();
// The frame has a reservation and must not be drawn natively. False for a Declined frame and between frames.
bool FrameHidden();
// The frame's identity: it advances at every present the bridge starts and again at the frame finish, so the game side
// can tell one frame from the next without a seam of its own. It is not the record serial: a frame whose present was
// abandoned is still a different frame from the next one, and its reservation is dropped by the engine.
uint64_t FrameSerial();
} // namespace ext::hudless::plates
