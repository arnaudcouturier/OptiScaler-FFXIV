#include "check.h"
#include "ext/core/ExtConfig.h"

#include <fstream>
#include <sstream>
#include <string>

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
    FfxivConfig fresh;
    CHECK(!fresh.jitterProbe.value_or_default());
    CHECK(!fresh.jitterCorrection.value_or_default());
    CHECK(!fresh.jitterProbe.has_value() && !fresh.jitterCorrection.has_value()); // unset: "auto"

    config::Binder binder;
    fresh.Bind(binder);
    int probe = 0, correction = 0;
    for (const auto& b : binder.Table())
    {
        if (std::string(b.section) == "FFXIV" && std::string(b.key) == "JitterProbe" && b.field == &fresh.jitterProbe)
            ++probe;
        if (std::string(b.section) == "FFXIV" && std::string(b.key) == "JitterCorrection" && b.field == &fresh.jitterCorrection)
            ++correction;
    }
    CHECK(probe == 1 && correction == 1);
}

void TestRoundTrip()
{
    CSimpleIniA ini;
    ini.SetValue("FFXIV", "JitterProbe", "true");
    ini.SetValue("FFXIV", "JitterCorrection", "true");
    config::Load(ini);
    CHECK(Cfg().ffxiv.jitterProbe.value_or_default() && Cfg().ffxiv.jitterCorrection.value_or_default());
    CSimpleIniA out;
    config::Save(out);
    CHECK(std::string(out.GetValue("FFXIV", "JitterProbe", "")) == "true");
    CHECK(std::string(out.GetValue("FFXIV", "JitterCorrection", "")) == "true");

    // "auto" and a missing key mean unset: both stay off, and are written back as auto (a fresh object, as the chara config test does)
    FfxivConfig fresh;
    CSimpleIniA autoIni;
    autoIni.SetValue("FFXIV", "JitterProbe", "auto");
    config::Binder binder;
    fresh.Bind(binder);
    for (const auto& b : binder.Table())
        b.load(b, autoIni);
    CHECK(!fresh.jitterProbe.value_or_default() && !fresh.jitterCorrection.value_or_default());
    CSimpleIniA autoOut;
    for (const auto& b : binder.Table())
        b.save(b, autoOut);
    CHECK(std::string(autoOut.GetValue("FFXIV", "JitterProbe", "")) == "auto");
    CHECK(std::string(autoOut.GetValue("FFXIV", "JitterCorrection", "")) == "auto");
}

void TestFragment(const std::string& repo)
{
    const std::string ffxiv = ReadFile(repo + "/OptiScaler/ext/ini/FFXIV.ini");
    CHECK(HasKeyLine(ffxiv, "JitterProbe") && HasKeyLine(ffxiv, "JitterCorrection"));
    // both keys are documented as default false, the line right above each key
    for (const char* key : { "JitterProbe=auto", "JitterCorrection=auto" })
    {
        const size_t at = ffxiv.find(key);
        CHECK(at != std::string::npos);
        if (at != std::string::npos)
            CHECK(ffxiv.rfind("Default (auto) is false", at) != std::string::npos && at - ffxiv.rfind("Default (auto) is false", at) < 40);
    }
}
} // namespace

int main(int argc, char** argv)
{
    TestDefaultsAndBinding();
    TestRoundTrip();
    TestFragment(argc > 1 ? argv[1] : ".");
    return TEST_RESULT("jitter_config");
}
