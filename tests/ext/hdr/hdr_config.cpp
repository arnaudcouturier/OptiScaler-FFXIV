#include "ext/core/ExtConfig.h"
#include "ext/hdr/HdrConfig.h"
#include "ext/hdr/HdrScene.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

int main()
{
    using namespace ext;

    HdrConfig defaults;
    assert(!defaults.ffxivHdr.value_or_default());
    assert(defaults.mode.value_or_default() == 0);
    assert(!defaults.reshadeHighlights.value_or_default());
    assert(!defaults.nrSceneInput.value_or_default());
    assert(!defaults.dlssSceneInput.value_or_default());
    assert(defaults.peak.value_or_default() == 1100.0f);
    assert(defaults.paper.value_or_default() == 203.0f);
    assert(defaults.expansion.value_or_default() == 0.5f);
    assert(defaults.contrast.value_or_default() == 1.0f);
    assert(defaults.saturation.value_or_default() == 1.0f);
    assert(defaults.vibrance.value_or_default() == 0.0f);
    assert(defaults.screenshotKey.value_or_default() == UnboundKey);
    assert(defaults.screenshotFormat.value_or_default() == 0);
    assert(!defaults.screenshotIccOnly.value_or_default());
    assert(!defaults.screenshotMenu.value_or_default());

    config::Binder binder;
    defaults.Bind(binder);
    const std::vector<std::string> expectedKeys {
        "FfxivHDR", "Mode", "ReShadeHighlights", "NRSceneInput", "DLSSSceneInput", "PeakNits", "PaperWhiteNits",
        "HighlightExpansion", "Contrast", "Saturation", "Vibrance", "ScreenshotKey", "ScreenshotFormat",
        "ScreenshotIccOnly", "ScreenshotMenu"
    };
    assert(binder.Table().size() == expectedKeys.size());
    for (size_t i = 0; i < expectedKeys.size(); ++i)
    {
        const auto& entry = binder.Table()[i];
        assert(std::string(entry.section) == "HDR");
        assert(std::string(entry.key) == expectedKeys[i]);
    }
    assert(binder.Table()[11].field == &defaults.screenshotKey);
    assert(binder.Table()[11].hex);

    CSimpleIniA ini;
    ini.SetValue("HDR", "FfxivHDR", "true");
    ini.SetValue("HDR", "Mode", "1");
    ini.SetValue("HDR", "ReShadeHighlights", "true");
    ini.SetValue("HDR", "NRSceneInput", "true");
    ini.SetValue("HDR", "DLSSSceneInput", "true");
    ini.SetValue("HDR", "PeakNits", "1500");
    ini.SetValue("HDR", "PaperWhiteNits", "180");
    ini.SetValue("HDR", "HighlightExpansion", "0.75");
    ini.SetValue("HDR", "Contrast", "1.2");
    ini.SetValue("HDR", "Saturation", "1.1");
    ini.SetValue("HDR", "Vibrance", "-0.2");
    ini.SetValue("HDR", "ScreenshotKey", "0x79");
    ini.SetValue("HDR", "ScreenshotFormat", "1");
    ini.SetValue("HDR", "ScreenshotIccOnly", "true");
    ini.SetValue("HDR", "ScreenshotMenu", "true");
    ini.SetValue("HDR", "MenuNits", "203");
    ini.SetValue("HDR", "ScreenshotHotkey", "0x7b");

    config::Load(ini);
    auto& hdr = Cfg().hdr;
    assert(hdr.ffxivHdr.value_or_default());
    assert(hdr.mode.value_or_default() == 1);
    assert(hdr.reshadeHighlights.value_or_default());
    assert(hdr.nrSceneInput.value_or_default());
    assert(hdr.dlssSceneInput.value_or_default());
    assert(hdr.peak.value_or_default() == 1500.0f);
    assert(hdr.paper.value_or_default() == 180.0f);
    assert(hdr.expansion.value_or_default() == 0.75f);
    assert(std::abs(hdr.contrast.value_or_default() - 1.2f) < 0.001f);
    assert(std::abs(hdr.saturation.value_or_default() - 1.1f) < 0.001f);
    assert(std::abs(hdr.vibrance.value_or_default() + 0.2f) < 0.001f);
    assert(hdr.screenshotKey.value_or_default() == 0x79);
    assert(hdr.screenshotFormat.value_or_default() == 1);
    assert(hdr.screenshotIccOnly.value_or_default());
    assert(hdr.screenshotMenu.value_or_default());
    assert(hdr::sceneHookInstallCalls == 1);

    config::Save(ini);
    assert(std::string(ini.GetValue("HDR", "ScreenshotKey", "")) == "0x79");
    assert(std::string(ini.GetValue("HDR", "ScreenshotMenu", "")) == "true");
    assert(ini.GetValue("HDR", "FfxivHDR", nullptr) != nullptr);
    assert(ini.GetValue("HDR", "MenuNits", nullptr) == nullptr);
    assert(ini.GetValue("HDR", "ScreenshotHotkey", nullptr) == nullptr);

    hdr.PostLoad();
    assert(hdr::sceneHookInstallCalls == 1);

    std::puts("PASS: HDR defaults and 15 INI bindings (incl. ScreenshotMenu), round-trip values, hex screenshot key, retired keys, one-time scene hook");
    return 0;
}
