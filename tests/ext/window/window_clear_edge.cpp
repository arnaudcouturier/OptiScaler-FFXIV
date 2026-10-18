// B1-a: the pure border rule. A window with neither a caption nor a sizing frame must not carry a window edge or a client
// edge; a real windowed mode (a caption or a frame is present) is left alone.
#include "window_test.h"

using namespace ext::window;

namespace
{
constexpr uint32_t kPopup = 0x80000000u;            // WS_POPUP
constexpr uint32_t kVisible = 0x10000000u;          // WS_VISIBLE
constexpr uint32_t kOverlapped = 0x00CF0000u;       // WS_OVERLAPPEDWINDOW (caption, sysmenu, thickframe, min, max)
constexpr uint32_t kBorder = 0x00800000u;           // WS_BORDER
constexpr uint32_t kDlgFrame = 0x00400000u;         // WS_DLGFRAME
constexpr uint32_t kThickFrame = 0x00040000u;       // WS_THICKFRAME
constexpr uint32_t kExAppWindow = 0x00040000u;      // WS_EX_APPWINDOW
constexpr uint32_t kExTopmost = 0x00000008u;        // WS_EX_TOPMOST
constexpr uint32_t kExWindowEdge = 0x00000100u;     // WS_EX_WINDOWEDGE
constexpr uint32_t kExClientEdge = 0x00000200u;     // WS_EX_CLIENTEDGE
constexpr uint32_t kExOverlapped = 0x00000300u;     // WS_EX_OVERLAPPEDWINDOW (window edge + client edge)
} // namespace

int main()
{
    // The bit values the module relies on.
    CHECK(kStyleCaption == (kBorder | kDlgFrame));
    CHECK(kStyleThickFrame == kThickFrame);
    CHECK(kExStyleClientEdge == kExClientEdge && kExStyleWindowEdge == kExWindowEdge);

    // The bug itself: the fork's leave branch wrote WS_EX_OVERLAPPEDWINDOW; the game kept its own frame-less style. The
    // exstyle 0x200 seen live (client edge only) is cleared too.
    {
        const auto fixed = ClearEdge(kPopup | kVisible, kExOverlapped);
        CHECK(fixed.has_value() && *fixed == 0u);
        const auto client = ClearEdge(kPopup | kVisible, kExClientEdge);
        CHECK(client.has_value() && *client == 0u);
        const auto window = ClearEdge(kPopup | kVisible, kExWindowEdge);
        CHECK(window.has_value() && *window == 0u);
    }

    // Only the edge bits are cleared: every other extended style bit stays.
    {
        const auto fixed = ClearEdge(kPopup | kVisible, kExOverlapped | kExAppWindow | kExTopmost);
        CHECK(fixed.has_value() && *fixed == (kExAppWindow | kExTopmost));
    }

    // A style of zero has no frame either.
    {
        const auto fixed = ClearEdge(0u, kExClientEdge);
        CHECK(fixed.has_value() && *fixed == 0u);
    }

    // A real windowed mode is left alone: caption and sizing frame, with the edges a normal window carries.
    CHECK(!ClearEdge(kOverlapped | kVisible, kExOverlapped).has_value());
    CHECK(!ClearEdge(kOverlapped, kExClientEdge).has_value());
    // Any one frame bit is enough to leave it alone (the rule is conservative).
    CHECK(!ClearEdge(kPopup | kThickFrame, kExClientEdge).has_value());
    CHECK(!ClearEdge(kPopup | kBorder, kExClientEdge).has_value());
    CHECK(!ClearEdge(kPopup | kDlgFrame, kExClientEdge).has_value());

    // Nothing to clear: no edge bit.
    CHECK(!ClearEdge(kPopup | kVisible, 0u).has_value());
    CHECK(!ClearEdge(kPopup | kVisible, kExAppWindow).has_value());

    // Idempotent: the result needs no second fix.
    {
        const auto fixed = ClearEdge(kPopup | kVisible, kExOverlapped | kExAppWindow);
        CHECK(fixed.has_value());
        CHECK(!ClearEdge(kPopup | kVisible, *fixed).has_value());
    }

    return wt::g_failed ? 1 : 0;
}
