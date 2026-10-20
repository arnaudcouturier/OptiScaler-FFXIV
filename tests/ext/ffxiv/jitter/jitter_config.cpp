#include "check.h"
#include "ext/core/ExtConfig.h"

#include <cmath>
#include <cstdlib>
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

// [FFXIV] JitterScaleOverride: a float, default 0 = off ("auto" resolves to 0, which is not an error), read as written.
void TestScaleOverrideKey()
{
    FfxivConfig fresh;
    CHECK(fresh.jitterScaleOverride.value_or_default() == 0.0f && !fresh.jitterScaleOverride.has_value());

    config::Binder binder;
    fresh.Bind(binder);
    int found = 0;
    for (const auto& b : binder.Table())
        if (std::string(b.section) == "FFXIV" && std::string(b.key) == "JitterScaleOverride" && b.field == &fresh.jitterScaleOverride)
            ++found;
    CHECK(found == 1);

    for (const char* text : { "0.8", "0.6", "0.5", "1.0", "0", "-1", "2.5" })
    {
        FfxivConfig loaded;
        config::Binder b;
        loaded.Bind(b);
        CSimpleIniA ini;
        ini.SetValue("FFXIV", "JitterScaleOverride", text);
        for (const auto& entry : b.Table())
            entry.load(entry, ini);
        CHECK_MSG(std::fabs(loaded.jitterScaleOverride.value_or_default() - std::strtof(text, nullptr)) < 1e-6, "value %s", text);
        // saved and loaded again: the same value
        CSimpleIniA out;
        for (const auto& entry : b.Table())
            entry.save(entry, out);
        FfxivConfig again;
        config::Binder b2;
        again.Bind(b2);
        for (const auto& entry : b2.Table())
            entry.load(entry, out);
        CHECK_MSG(std::fabs(again.jitterScaleOverride.value_or_default() - std::strtof(text, nullptr)) < 1e-6, "round trip of %s", text);
    }
    // "auto", an empty value and a missing key mean unset: 0 = off, written back as auto
    for (const char* text : { "auto", "" })
    {
        FfxivConfig loaded;
        config::Binder b;
        loaded.Bind(b);
        CSimpleIniA ini;
        ini.SetValue("FFXIV", "JitterScaleOverride", text);
        for (const auto& entry : b.Table())
            entry.load(entry, ini);
        CHECK(loaded.jitterScaleOverride.value_or_default() == 0.0f && !loaded.jitterScaleOverride.has_value());
        CSimpleIniA out;
        for (const auto& entry : b.Table())
            entry.save(entry, out);
        CHECK(std::string(out.GetValue("FFXIV", "JitterScaleOverride", "")) == "auto");
    }
    FfxivConfig missing;
    config::Binder b;
    missing.Bind(b);
    CSimpleIniA empty;
    for (const auto& entry : b.Table())
        entry.load(entry, empty);
    CHECK(missing.jitterScaleOverride.value_or_default() == 0.0f);
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
    // the correction key: the text describes the calibrate-and-scale behaviour and no longer the per-frame proof
    {
        const size_t at = ffxiv.find("JitterCorrection=auto");
        CHECK(at != std::string::npos);
        if (at != std::string::npos)
        {
            const size_t lf = ffxiv.rfind("\n\n", at), crlf = ffxiv.rfind("\r\n\r\n", at);
            size_t start = lf == std::string::npos ? 0 : lf;
            if (crlf != std::string::npos && crlf > start)
                start = crlf;
            const std::string text = ffxiv.substr(start, at - start);
            CHECK(text.find("learns") != std::string::npos && text.find("60%") != std::string::npos && text.find("EVERY DLSS evaluate") != std::string::npos);
            CHECK(text.find("verification stale") != std::string::npos && text.find("keeps being applied") != std::string::npos); // F2: no over-promise
            CHECK(text.find("generation off first") != std::string::npos && text.find("Test with frame") != std::string::npos); // the comment wraps between the lines
            CHECK(text.find("same-frame") == std::string::npos && text.find("held-back") == std::string::npos &&
                  text.find("frame counter did not increase") == std::string::npos && text.find("replaced by the applied") == std::string::npos);
        }
    }
    // the experiment key: documented as default off (0), the line right above the key, with the valid range and the condition
    CHECK(HasKeyLine(ffxiv, "JitterScaleOverride"));
    {
        const size_t at = ffxiv.find("JitterScaleOverride=auto");
        CHECK(at != std::string::npos);
        if (at != std::string::npos)
        {
            CHECK(ffxiv.rfind("Default (auto) is 0", at) != std::string::npos && at - ffxiv.rfind("Default (auto) is 0", at) < 40);
            // the comment block of the key: from the blank line before it
            const size_t lf = ffxiv.rfind("\n\n", at), crlf = ffxiv.rfind("\r\n\r\n", at);
            size_t start = lf == std::string::npos ? 0 : lf;
            if (crlf != std::string::npos && crlf > start)
                start = crlf;
            const std::string text = ffxiv.substr(start, at - start);
            CHECK(text.find("xperiment") != std::string::npos && text.find("0.25") != std::string::npos && text.find("1.0") != std::string::npos);
            CHECK(text.find("JitterCorrection=true") != std::string::npos && text.find("may be removed") != std::string::npos);
        }
    }
}
} // namespace

int main(int argc, char** argv)
{
    TestDefaultsAndBinding();
    TestRoundTrip();
    TestScaleOverrideKey();
    TestFragment(argc > 1 ? argv[1] : ".");
    return TEST_RESULT("jitter_config");
}
