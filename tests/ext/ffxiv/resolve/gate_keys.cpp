// The three persistent keys of the game build handling: [FFXIV] FfxivLegacyGameBuild (rollback), FfxivUnknownBuildHooks, FfxivForceUnknownBuild.
// Default OFF ("auto" and a missing key mean unset = false), bound exactly once in section FFXIV, round trip, documented in the ini fragment that
// tools/package.ps1 merges into the shipped OptiScaler.ini (the upstream template is never edited), listed as keys without a menu control, and
// read by the startup into the GateSettings the gate uses (the default settings are all false: nothing changes with a default ini).
#include "../chara/check.h"
#include "ext/core/ExtConfig.h"
#include "ext/core/MenuManifest.h"
#include "ext/ffxiv/resolve/Gate.h"
#include "ext/ffxiv/resolve/StartupDecision.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

using namespace ext;

namespace
{
const char* const kKeys[] = { "FfxivLegacyGameBuild", "FfxivUnknownBuildHooks", "FfxivForceUnknownBuild" };

CustomOptional<bool>& Field(FfxivConfig& c, const std::string& key)
{
    if (key == "FfxivLegacyGameBuild")
        return c.legacyGameBuild;
    if (key == "FfxivUnknownBuildHooks")
        return c.unknownBuildHooks;
    return c.forceUnknownBuild;
}

std::string ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void TestDefaultsAndBinding()
{
    FfxivConfig fresh;
    for (const char* key : kKeys)
    {
        CHECK(!Field(fresh, key).value_or_default());
        CHECK(!Field(fresh, key).has_value()); // unset: "auto"
    }
    config::Binder binder;
    fresh.Bind(binder);
    for (const char* key : kKeys)
    {
        int found = 0;
        for (const auto& b : binder.Table())
            if (std::string(b.section) == "FFXIV" && std::string(b.key) == key && b.field == &Field(fresh, key))
                ++found;
        CHECK(found == 1);
    }
    // the settings the startup builds from a fresh config are all false: a default ini changes nothing
    ext::ffxiv::resolve::GateSettings s;
    CHECK(!s.legacyGameBuild && !s.unknownBuildHooks && !s.forceUnknownBuild);
}

void TestRoundTrip()
{
    for (const char* key : kKeys)
    {
        for (const char* text : { "true", "false" })
        {
            FfxivConfig loaded;
            config::Binder b;
            loaded.Bind(b);
            CSimpleIniA ini;
            ini.SetValue("FFXIV", key, text);
            for (const auto& entry : b.Table())
                entry.load(entry, ini);
            CHECK(Field(loaded, key).value_or_default() == (std::string(text) == "true"));
            // only this key moved
            for (const char* other : kKeys)
                if (std::string(other) != key)
                    CHECK(!Field(loaded, other).value_or_default());
            CSimpleIniA out;
            for (const auto& entry : b.Table())
                entry.save(entry, out);
            if (std::string(text) == "true")
                CHECK(std::string(out.GetValue("FFXIV", key, "")) == "true");
            FfxivConfig again;
            config::Binder b2;
            again.Bind(b2);
            for (const auto& entry : b2.Table())
                entry.load(entry, out);
            CHECK(Field(again, key).value_or_default() == (std::string(text) == "true"));
        }
        for (const char* text : { "auto", "" })
        {
            FfxivConfig loaded;
            config::Binder b;
            loaded.Bind(b);
            CSimpleIniA ini;
            ini.SetValue("FFXIV", key, text);
            for (const auto& entry : b.Table())
                entry.load(entry, ini);
            CHECK(!Field(loaded, key).value_or_default() && !Field(loaded, key).has_value());
            CSimpleIniA out;
            for (const auto& entry : b.Table())
                entry.save(entry, out);
            CHECK(std::string(out.GetValue("FFXIV", key, "")) == "auto");
        }
        FfxivConfig missing;
        config::Binder b;
        missing.Bind(b);
        CSimpleIniA empty;
        for (const auto& entry : b.Table())
            entry.load(entry, empty);
        CHECK(!Field(missing, key).value_or_default());
    }
}

// The mapping from the ini keys to the GateSettings the gate uses is a pure function (ReadSettings), tested on the REAL config type: each
// key, set alone through the ini loader, moves exactly its own flag (a swapped pair of keys or a wrong member would flip a rollback or an
// unknown-build decision without any other test noticing).
void TestReadSettingsMapsEachKeyToItsOwnFlag()
{
    using ext::ffxiv::resolve::GateSettings;
    using ext::ffxiv::resolve::ReadSettings;
    {
        const FfxivConfig fresh;
        const GateSettings s = ReadSettings(fresh);
        CHECK(!s.legacyGameBuild && !s.unknownBuildHooks && !s.forceUnknownBuild); // a default ini changes nothing
    }
    for (const char* key : kKeys)
    {
        FfxivConfig loaded;
        config::Binder b;
        loaded.Bind(b);
        CSimpleIniA ini;
        ini.SetValue("FFXIV", key, "true");
        for (const auto& entry : b.Table())
            entry.load(entry, ini);
        const GateSettings s = ReadSettings(loaded);
        const std::string k = key;
        if (s.legacyGameBuild != (k == "FfxivLegacyGameBuild") || s.unknownBuildHooks != (k == "FfxivUnknownBuildHooks") ||
            s.forceUnknownBuild != (k == "FfxivForceUnknownBuild"))
            std::printf("FAIL: key %s maps to legacy=%d unknownHooks=%d force=%d\n", key, s.legacyGameBuild, s.unknownBuildHooks, s.forceUnknownBuild);
        CHECK(s.legacyGameBuild == (k == "FfxivLegacyGameBuild"));
        CHECK(s.unknownBuildHooks == (k == "FfxivUnknownBuildHooks"));
        CHECK(s.forceUnknownBuild == (k == "FfxivForceUnknownBuild"));
    }
    // all three on, and the process-wide config (the one the startup reads) maps the same way
    {
        FfxivConfig all;
        all.legacyGameBuild = true;
        all.unknownBuildHooks = true;
        all.forceUnknownBuild = true;
        const GateSettings s = ReadSettings(all);
        CHECK(s.legacyGameBuild && s.unknownBuildHooks && s.forceUnknownBuild);
    }
    {
        ext::Cfg().ffxiv.legacyGameBuild = true;
        const GateSettings s = ReadSettings(ext::Cfg().ffxiv);
        CHECK(s.legacyGameBuild && !s.unknownBuildHooks && !s.forceUnknownBuild);
        ext::Cfg().ffxiv.legacyGameBuild.reset();
    }
}

void TestFragmentAndManifest(const std::string& repo)
{
    std::string fragment = ReadFile(repo + "/OptiScaler/ext/ini/FFXIV.ini");
    fragment.erase(std::remove(fragment.begin(), fragment.end(), '\r'), fragment.end()); // the working copy may be CRLF
    CHECK(!fragment.empty());
    for (const char* key : kKeys)
    {
        // the documented line `Key=auto`, with a comment block right above it that gives the default and says there is no menu control
        const std::string line = std::string(key) + "=auto";
        const size_t at = fragment.find("\n" + line);
        CHECK(at != std::string::npos);
        if (at == std::string::npos)
            continue;
        const size_t start = fragment.rfind("\n\n", at); // the comment block of this key
        const std::string block = fragment.substr(start == std::string::npos ? 0 : start, at - (start == std::string::npos ? 0 : start));
        CHECK(block.find("true or false - Default (auto) is false") != std::string::npos);
        CHECK(block.find("No menu control") != std::string::npos);
        // the key is listed once as an ini-only key
        int listed = 0;
        for (const char* entry : ext::menu::manifest::kIniOnlyKeys)
            if (std::string(entry) == "[FFXIV] " + std::string(key))
                ++listed;
        CHECK(listed == 1);
    }
    // the rollback key says what it restores
    CHECK(fragment.find("exactly the behaviour before the address search existed") != std::string::npos);
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    TestDefaultsAndBinding();
    TestRoundTrip();
    TestReadSettingsMapsEachKeyToItsOwnFlag();
    TestFragmentAndManifest(repo);
    return TEST_RESULT("gate_keys");
}
