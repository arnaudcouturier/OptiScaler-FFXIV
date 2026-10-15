#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/hdr/HdrConfig.h"

namespace ext
{
void HdrConfig::Bind(config::Binder& b)
{
    b.Add("HDR", "FfxivHDR", ffxivHdr);
    b.Add("HDR", "Mode", mode);
    b.Add("HDR", "ReShadeHighlights", reshadeHighlights);
    b.Add("HDR", "NRSceneInput", nrSceneInput);
    b.Add("HDR", "DLSSSceneInput", dlssSceneInput);
    b.Add("HDR", "PeakNits", peak);
    b.Add("HDR", "PaperWhiteNits", paper);
    b.Add("HDR", "HighlightExpansion", expansion);
    b.Add("HDR", "Contrast", contrast);
    b.Add("HDR", "Saturation", saturation);
    b.Add("HDR", "Vibrance", vibrance);
    b.Add("HDR", "ScreenshotKey", screenshotKey, true); // hex when positive (a key code), like the reference build
    b.Add("HDR", "ScreenshotFormat", screenshotFormat);
    b.Add("HDR", "ScreenshotIccOnly", screenshotIccOnly);
}

void HdrConfig::Retire(CSimpleIniA& ini)
{
    ini.Delete("HDR", "MenuNits");          // Retired: menu HDR mapping is automatic.
    ini.Delete("HDR", "ScreenshotHotkey");  // Retire the old fixed Ctrl+Alt+F12 toggle.
}
} // namespace ext
