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
    CHECK(!defaults.previewNrTrace.value_or_default());

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
        { "HDR", "PreviewNRTrace", &defaults.previewNrTrace },
    };
    CHECK(binder.Table().size() == 5);
    for (size_t i = 0; i < 5 && i < binder.Table().size(); ++i)
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
    ini.SetValue("HDR", "PreviewNRTrace", "true");
    config::Load(ini);

    const auto& c = Cfg().ffxiv.chara;
    CHECK(c.previewDlss.value_or_default());
    CHECK(c.previewAA.value_or_default());
    CHECK(!c.previewUpscale.value_or_default());
    CHECK(c.previewGuideCapture.value_or_default());
    CHECK(c.previewNrTrace.value_or_default());

    // saving writes the non-default values back under the same section and key names
    CSimpleIniA out;
    config::Save(out);
    CHECK(std::string(out.GetValue("FFXIV", "PreviewDLSS", "")) == "true");
    CHECK(std::string(out.GetValue("FFXIV", "PreviewAA", "")) == "true");
    CHECK(std::string(out.GetValue("FFXIV", "PreviewUpscale", "")) == "false");
    CHECK(std::string(out.GetValue("FFXIV", "PreviewGuideCapture", "")) == "true");
    CHECK(std::string(out.GetValue("HDR", "PreviewNRTrace", "")) == "true");

    // the pre-existing FFXIV-companion keys keep working next to them
    CHECK(Cfg().ffxiv.companionHudReplacement.value_or_default() == false);
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

    CSimpleIniA out;
    for (const auto& b : binder.Table())
        b.save(b, out);
    CHECK(std::string(out.GetValue("FFXIV", "PreviewUpscale", "")) == "auto");
    CHECK(std::string(out.GetValue("FFXIV", "PreviewAA", "")) == "auto");
}

void TestFragments(const std::string& repo)
{
    const std::string ffxiv = ReadFile(repo + "/OptiScaler/ext/ini/FFXIV.ini");
    CHECK(!ffxiv.empty());
    for (const char* key : { "PreviewDLSS", "PreviewAA", "PreviewUpscale", "PreviewGuideCapture" })
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
