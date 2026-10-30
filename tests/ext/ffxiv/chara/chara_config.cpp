#include "check.h"
#include "ext/core/ExtConfig.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace ext;

namespace
{
std::string ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool HasKeyLine(const std::string& text, const std::string& key)
{
    // a line "Key=value" (not a comment)
    size_t pos = 0;
    while ((pos = text.find(key + "=", pos)) != std::string::npos)
    {
        if (pos == 0 || text[pos - 1] == '\n')
            return true;
        pos += key.size();
    }
    return false;
}

void TestDefaultsAndBinding()
{
    ffxiv::chara::CharaConfig defaults;
    CHECK(!defaults.previewDlss.value_or_default());
    CHECK(!defaults.previewAA.value_or_default());
    CHECK(defaults.previewUpscale.value_or_default()); // the one key that defaults to true
    CHECK(!defaults.previewGuideCapture.value_or_default());
    CHECK(!defaults.previewDebugTint.value_or_default());
    CHECK(!defaults.previewNrTrace.value_or_default());
    CHECK(!defaults.previewNr.value_or_default());
    CHECK(defaults.previewNrLingerSeconds.value_or_default() == 5u);
    CHECK(!defaults.previewNrLate.value_or_default());
    CHECK(!defaults.previewNrBeforeSr.value_or_default()); // experimental, off unless asked for
    CHECK(!defaults.previewProbe.value_or_default());
    CHECK(!defaults.previewDebugFlood.value_or_default());
    CHECK(!defaults.previewReplayWideShape.value_or_default());
    CHECK(!defaults.previewBudget.value_or_default());           // the budget is off by default
    CHECK(defaults.previewMaxWindows.value_or_default() == 2u);  // ... and its count is inert while it is off

    config::Binder binder;
    defaults.Bind(binder);
    const struct
    {
        const char* section;
        const char* key;
        void* field;
    } expected[] = {
        { "FFXIV", "PreviewDLSS", &defaults.previewDlss },
        { "FFXIV", "PreviewAA", &defaults.previewAA },
        { "FFXIV", "PreviewUpscale", &defaults.previewUpscale },
        { "FFXIV", "PreviewGuideCapture", &defaults.previewGuideCapture },
        { "FFXIV", "PreviewDebugTint", &defaults.previewDebugTint },
        { "HDR", "PreviewNRTrace", &defaults.previewNrTrace },
        { "FFXIV", "PreviewNR", &defaults.previewNr },
        { "FFXIV", "PreviewNRLingerSeconds", &defaults.previewNrLingerSeconds },
        { "FFXIV", "PreviewNRLate", &defaults.previewNrLate },
        { "FFXIV", "PreviewNRBeforeSR", &defaults.previewNrBeforeSr },
        { "FFXIV", "PreviewProbe", &defaults.previewProbe },
        { "FFXIV", "PreviewDebugFlood", &defaults.previewDebugFlood },
        { "FFXIV", "PreviewReplayWideShape", &defaults.previewReplayWideShape },
        { "FFXIV", "PreviewBudget", &defaults.previewBudget },
        { "FFXIV", "PreviewMaxWindows", &defaults.previewMaxWindows },
    };
    CHECK(binder.Table().size() == 15);
    for (size_t i = 0; i < 15 && i < binder.Table().size(); ++i)
    {
        CHECK(std::string(binder.Table()[i].section) == expected[i].section);
        CHECK(std::string(binder.Table()[i].key) == expected[i].key);
        CHECK(binder.Table()[i].field == expected[i].field);
    }
}

void TestRoundTrip()
{
    // the feature is wired into the fork config and read from the ini
    CSimpleIniA ini;
    ini.SetValue("FFXIV", "PreviewDLSS", "true");
    ini.SetValue("FFXIV", "PreviewAA", "true");
    ini.SetValue("FFXIV", "PreviewUpscale", "false");
    ini.SetValue("FFXIV", "PreviewGuideCapture", "true");
    ini.SetValue("FFXIV", "PreviewDebugTint", "true");
    ini.SetValue("HDR", "PreviewNRTrace", "true");
    ini.SetValue("FFXIV", "PreviewNR", "true");
    ini.SetValue("FFXIV", "PreviewNRLingerSeconds", "12");
    ini.SetValue("FFXIV", "PreviewNRLate", "true");
    ini.SetValue("FFXIV", "PreviewNRBeforeSR", "true");
    ini.SetValue("FFXIV", "PreviewProbe", "true");
    ini.SetValue("FFXIV", "PreviewDebugFlood", "true");
    ini.SetValue("FFXIV", "PreviewReplayWideShape", "true");
    ini.SetValue("FFXIV", "PreviewBudget", "true");
    ini.SetValue("FFXIV", "PreviewMaxWindows", "5");
    config::Load(ini);

    const auto& c = Cfg().ffxiv.chara;
    CHECK(c.previewDlss.value_or_default());
    CHECK(c.previewAA.value_or_default());
    CHECK(!c.previewUpscale.value_or_default());
    CHECK(c.previewGuideCapture.value_or_default());
    CHECK(c.previewNrTrace.value_or_default());
    CHECK(c.previewNr.value_or_default() && c.previewNrLate.value_or_default() && c.previewNrLingerSeconds.value_or_default() == 12u);
    CHECK(c.previewNrBeforeSr.value_or_default());
    CHECK(c.previewDebugTint.value_or_default());
    CHECK(c.previewProbe.value_or_default());
    CHECK(c.previewDebugFlood.value_or_default());
    CHECK(c.previewReplayWideShape.value_or_default());
    CHECK(c.previewBudget.value_or_default());
    CHECK(c.previewMaxWindows.value_or_default() == 5u);

    // saving writes the non-default values back under the same section and key names
    CSimpleIniA out;
    config::Save(out);
    CHECK(std::string(out.GetValue("FFXIV", "PreviewDLSS", "")) == "true");
    CHECK(std::string(out.GetValue("FFXIV", "PreviewAA", "")) == "true");
    CHECK(std::string(out.GetValue("FFXIV", "PreviewUpscale", "")) == "false");
    CHECK(std::string(out.GetValue("FFXIV", "PreviewGuideCapture", "")) == "true");
    CHECK(std::string(out.GetValue("FFXIV", "PreviewDebugTint", "")) == "true");
    CHECK(std::string(out.GetValue("HDR", "PreviewNRTrace", "")) == "true");
    CHECK(std::string(out.GetValue("FFXIV", "PreviewNR", "")) == "true");
    CHECK(std::string(out.GetValue("FFXIV", "PreviewNRLingerSeconds", "")) == "12");
    CHECK(std::string(out.GetValue("FFXIV", "PreviewNRLate", "")) == "true");
    CHECK(std::string(out.GetValue("FFXIV", "PreviewNRBeforeSR", "")) == "true");
    CHECK(std::string(out.GetValue("FFXIV", "PreviewProbe", "")) == "true");
    CHECK(std::string(out.GetValue("FFXIV", "PreviewDebugFlood", "")) == "true");
    CHECK(std::string(out.GetValue("FFXIV", "PreviewReplayWideShape", "")) == "true");
    CHECK(std::string(out.GetValue("FFXIV", "PreviewBudget", "")) == "true");
    CHECK(std::string(out.GetValue("FFXIV", "PreviewMaxWindows", "")) == "5");
}

void TestUnsetKeysStayDefault()
{
    // "auto" and a missing key mean unset: the defaults apply and nothing but the non-default values is written
    ffxiv::chara::CharaConfig fresh;
    CSimpleIniA ini;
    ini.SetValue("FFXIV", "PreviewUpscale", "auto");
    config::Binder binder;
    fresh.Bind(binder);
    for (const auto& b : binder.Table())
        b.load(b, ini);
    CHECK(fresh.previewUpscale.value_or_default());
    CHECK(!fresh.previewAA.value_or_default());
    CHECK(!fresh.previewProbe.value_or_default());
    CHECK(!fresh.previewNrBeforeSr.value_or_default());
    CHECK(!fresh.previewDebugFlood.value_or_default());
    CHECK(!fresh.previewReplayWideShape.value_or_default());
    CHECK(!fresh.previewBudget.value_or_default());
    CHECK(fresh.previewMaxWindows.value_or_default() == 2u);

    CSimpleIniA out;
    for (const auto& b : binder.Table())
        b.save(b, out);
    CHECK(std::string(out.GetValue("FFXIV", "PreviewUpscale", "")) == "auto");
    CHECK(std::string(out.GetValue("FFXIV", "PreviewAA", "")) == "auto");
    CHECK(std::string(out.GetValue("FFXIV", "PreviewProbe", "")) == "auto");
    CHECK(std::string(out.GetValue("FFXIV", "PreviewNRBeforeSR", "")) == "auto");
    CHECK(std::string(out.GetValue("FFXIV", "PreviewDebugFlood", "")) == "auto");
    CHECK(std::string(out.GetValue("FFXIV", "PreviewReplayWideShape", "")) == "auto");
    CHECK(std::string(out.GetValue("FFXIV", "PreviewBudget", "")) == "auto");
    CHECK(std::string(out.GetValue("FFXIV", "PreviewMaxWindows", "")) == "auto");
}

void TestFragments(const std::string& repo)
{
    const std::string ffxiv = ReadFile(repo + "/OptiScaler/ext/ini/FFXIV.ini");
    CHECK(!ffxiv.empty());
    for (const char* key : { "PreviewDLSS", "PreviewAA", "PreviewUpscale", "PreviewGuideCapture", "PreviewDebugTint", "PreviewNR", "PreviewNRLingerSeconds", "PreviewNRLate", "PreviewNRBeforeSR", "PreviewProbe", "PreviewDebugFlood", "PreviewReplayWideShape", "PreviewBudget", "PreviewMaxWindows" })
        CHECK(HasKeyLine(ffxiv, key));
    CHECK(!HasKeyLine(ffxiv, "PreviewNRTrace"));
    CHECK(ffxiv.find("[FFXIV]") == std::string::npos || ffxiv.find("[FFXIV]") == 0); // a leading header would be dropped by the packager

    const std::string hdr = ReadFile(repo + "/OptiScaler/ext/ini/HDR.ini");
    CHECK(HasKeyLine(hdr, "PreviewNRTrace"));
    // the HDR key was appended, the others are still there
    CHECK(HasKeyLine(hdr, "ScreenshotMenu") && HasKeyLine(hdr, "FfxivHDR"));

    // documented defaults match the code: PreviewUpscale is the only true one
    CHECK(ffxiv.find("Default (auto) is true") != std::string::npos);
}
} // namespace

int main(int argc, char** argv)
{
    TestDefaultsAndBinding();
    TestRoundTrip();
    TestUnsetKeysStayDefault();
    TestFragments(argc > 1 ? argv[1] : ".");
    return TEST_RESULT("chara_config");
}
