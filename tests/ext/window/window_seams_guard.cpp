// The cost of [Window] Mode = off: every entry point of the module is an inline guard on one cached atomic. With the mode off
// the out-of-line bodies (which take a lock, read the window and log) are never reached; with probe or on they are. The slow
// bodies are replaced here by counters, so this checks the header exactly as the seam bodies include it.
#include "ext/window/WindowSeams.h"

#include <cstdio>

using namespace ext::window;

static int g_failed = 0;
#define CHECK(cond)                                                                                                         \
    do                                                                                                                      \
    {                                                                                                                       \
        if (!(cond))                                                                                                        \
        {                                                                                                                   \
            std::printf("CHECK failed line %d: %s\n", __LINE__, #cond);                                                     \
            ++g_failed;                                                                                                     \
        }                                                                                                                   \
    } while (0)

namespace
{
int g_resize = 0, g_begin = 0, g_end = 0, g_event = 0, g_destroy = 0;
} // namespace

namespace ext::window
{
void ResizeDoneSlow(Dx11wDx12SC&) noexcept { ++g_resize; }
void DestroySlow(Dx11wDx12SC&) noexcept { ++g_destroy; }
void PresentBeginSlow() noexcept { ++g_begin; }
void PresentEndSlow() noexcept { ++g_end; }
bool SetFullscreenStateSlow(uint64_t, uint64_t, bool, uint64_t) noexcept
{
    ++g_event;
    return true;
}
} // namespace ext::window

int main()
{
    alignas(8) char storage[8] {};
    Dx11wDx12SC& sc = *reinterpret_cast<Dx11wDx12SC*>(storage); // never touched: the stub does not look at it

    // The default is off, and off reaches nothing.
    CHECK(CurrentMode() == Mode::Off);
    OnResizeDone(sc);
    OnDestroy(sc);
    OnPresentBegin();
    OnPresentEnd();
    CHECK(!OnSetFullscreenState(1, 2, true, 0)); // and never prevents a call
    CHECK(g_resize == 0 && g_begin == 0 && g_end == 0 && g_event == 0 && g_destroy == 0);

    // Probe and on reach the bodies.
    for (Mode mode : { Mode::Probe, Mode::On })
    {
        g_resize = g_begin = g_end = g_event = g_destroy = 0;
        Publish(mode);
        OnResizeDone(sc);
        OnDestroy(sc);
        OnPresentBegin();
        OnPresentEnd();
        const bool prevented = OnSetFullscreenState(1, 2, true, 0);
        CHECK(g_resize == 1 && g_begin == 1 && g_end == 1 && g_event == 1 && g_destroy == 1);
        CHECK(prevented); // the body's answer is passed on
    }

    // Back to off: silent again.
    g_resize = g_begin = g_end = g_event = g_destroy = 0;
    Publish(Mode::Off);
    OnResizeDone(sc);
    OnDestroy(sc);
    OnPresentBegin();
    OnPresentEnd();
    CHECK(!OnSetFullscreenState(1, 2, true, 0));
    CHECK(g_resize == 0 && g_begin == 0 && g_end == 0 && g_event == 0 && g_destroy == 0);

    // The guards are noexcept (the seam bodies and the ReShade callback must never throw into the game).
    static_assert(noexcept(OnResizeDone(sc)) && noexcept(OnDestroy(sc)) && noexcept(OnPresentBegin()) && noexcept(OnPresentEnd()) &&
                  noexcept(OnSetFullscreenState(1, 2, true, 0)));

    return g_failed ? 1 : 0;
}
