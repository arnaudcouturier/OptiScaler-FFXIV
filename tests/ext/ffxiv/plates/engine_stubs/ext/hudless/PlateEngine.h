#pragma once
// Stand-in for the plate engine (ext/hudless/PlateEngine.h) in the game-side test: the reservation, the draw count and
// the record serial are the test's, so every answer the engine can give (declined, shadow, hidden, no slot target) can be
// driven without the slots, the pass or a D3D12 device.

#include <cstdint>

struct ID3D11RenderTargetView;

namespace ext::hudless::plates
{
enum class FrameMode : uint8_t
{
    Declined,
    Shadow,
    Hidden,
};

struct FrameReservation
{
    FrameMode mode = FrameMode::Declined;
    int slot = -1;
    unsigned width = 0;
    unsigned height = 0;
    unsigned format = 0;
    ID3D11RenderTargetView* target = nullptr;

    explicit operator bool() const { return mode != FrameMode::Declined; }
};

// ---- the test's engine state ----------------------------------------------------------------------------------------
inline int& AskCount()
{
    static int asks = 0;
    return asks;
}
inline FrameReservation& Answer()
{
    static FrameReservation answer;
    return answer;
}
inline uint64_t& Serial()
{
    static uint64_t serial = 0;
    return serial;
}
inline int& DrawCount()
{
    static int draws = 0;
    return draws;
}
inline void ResetCounters()
{
    AskCount() = 0;
    DrawCount() = 0;
}
inline unsigned& LastFormat()
{
    static unsigned format = 0;
    return format;
}
inline unsigned& LastWidth()
{
    static unsigned width = 0;
    return width;
}
inline unsigned& LastHeight()
{
    static unsigned height = 0;
    return height;
}
inline FrameReservation ReserveFrame(unsigned width, unsigned height, unsigned format)
{
    ++AskCount();
    LastWidth() = width;
    LastHeight() = height;
    LastFormat() = format;
    return Answer();
}
inline void NotePlateDraw() { ++DrawCount(); }
inline bool FrameHidden() { return Answer().mode == FrameMode::Hidden; }
inline uint64_t FrameSerial() { return Serial(); }
} // namespace ext::hudless::plates
