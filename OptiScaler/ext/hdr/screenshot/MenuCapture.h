#pragma once
// "Include OptiScaler menu" (ini `[HDR] ScreenshotMenu`, F4; docs/RECONSTRUCTION-NICKLE.md): when it applies, an HDR PNG
// is taken after the menu was drawn onto the presented image instead of from the clean image. Kept free of any
// engine type so the decision can be tested without a GPU.

namespace ext::hdr::screenshot
{
struct MenuCaptureInputs
{
    bool hdrActive = false;    // OptiHDR is engaged (hdr::Active())
    int format = 0;            // [HDR] ScreenshotFormat: 0 SDR PNG, 1 HDR PNG
    bool includeMenu = false;  // [HDR] ScreenshotMenu
    bool overlayMenu = false;  // [Menu] OverlayMenu: the ImGui overlay is enabled at all
    bool menuVisible = false;  // the menu is open right now
};

// All five have to hold. With the menu hidden, SDR PNG, or the option off, the normal clean capture is used.
constexpr bool WantsMenuCapture(const MenuCaptureInputs& in)
{
    return in.hdrActive && in.format == 1 && in.includeMenu && in.overlayMenu && in.menuVisible;
}
} // namespace ext::hdr::screenshot
