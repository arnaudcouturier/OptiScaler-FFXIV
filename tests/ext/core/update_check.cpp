// The upstream update check ([Hotfix] CheckForUpdate): this fork leaves it off unless the player turned it on.
//
// What runs here is the production code: the real CustomOptional (sliced out of OptiScaler\Config.h by the Prepare script, volatile
// values included), the real ExtConfig.cpp (config::Load, config::Save, the two hook pointers) and the real glue UpdateCheck.cpp
// (its start-up registration and its two hook bodies). Only upstream's own Config::Reload / Config::SaveIni statements are
// repeated by hand (Session below), and the text pins at the end tie each of them to the line in Config.cpp / dllmain.cpp.
//
// Why the save side matters: upstream declares the key with the default true, and CustomOptional::value_for_config() answers
// "nothing to save" for a value equal to the default, so Save Settings writes `auto` for an explicit true. In this fork `auto` means
// off, so without the save hook a player who turned the check on (ini or menu) would lose it at the next Save Settings.
#include "ext/core/ExtConfig.h"
#include "ext/core/UpdateCheck.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
int g_failed = 0;

#define CHECK(cond)                                                                                              \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                             \
        {                                                                                                        \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                  \
            ++g_failed;                                                                                          \
        }                                                                                                        \
    } while (0)

std::string ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end()); // the checkout may be CRLF
    return text;
}

// Config.cpp: GetBoolValue (pinned below)
std::string GetBoolValue(std::optional<bool> value)
{
    if (!value.has_value())
        return "auto";

    return value.value() ? "true" : "false";
}

// Config::readBool: the lower-cased text of the key, "true" or "false"; anything else (the key absent, "auto") is unset
std::optional<bool> ReadBool(CSimpleIniA& ini)
{
    std::string value = ini.GetValue("Hotfix", "CheckForUpdate", "auto");
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "true")
        return true;
    if (value == "false")
        return false;
    return std::nullopt;
}

CustomOptional<bool>& Key() { return Config::Instance()->CheckForUpdate; }

// One launch of the game: Config::Reload reads the ini text, then ext::config::Load runs the post-load step.
// Save() is a click on "Save Settings" (Config::SaveIni) and returns what the ini file would hold for the key.
struct Session
{
    CSimpleIniA ini;

    // iniText nullptr: the key is not in the file. iniReadable false: OptiScaler.ini is missing or unreadable, Config::Reload
    // returns false before it reaches ext::config::Load, so nothing is read and no post-load hook runs.
    explicit Session(const char* iniText, bool iniReadable = true)
    {
        Config::Fresh();
        if (iniText != nullptr)
            ini.SetValue("Hotfix", "CheckForUpdate", iniText);
        if (iniReadable)
            Reload();
    }

    void Reload()
    {
        Key().set_from_config(ReadBool(ini));
        ext::config::Load(ini);
    }

    std::string Save()
    {
        ini.SetValue("Hotfix", "CheckForUpdate", GetBoolValue(Key().value_for_config()).c_str());
        ext::config::Save(ini);
        return ini.GetValue("Hotfix", "CheckForUpdate", "<missing>");
    }

    // The next launch reads what Save() left in the file
    std::string Saved() const { return ini.GetValue("Hotfix", "CheckForUpdate", "<missing>"); }
};

bool CheckRuns() { return Key().value_or_default(); } // dllmain.cpp: if (CheckForUpdate.value_or_default()) VersionCheck::Start()

using Failures = std::vector<std::string>;

#define EXPECT(failures, name, cond)                                                                             \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                             \
            (failures).push_back(name);                                                                          \
    } while (0)

// Every rule of the feature, against whatever hooks are installed in ExtConfig right now. Returns the names of the rules that failed.
Failures RunScenarios()
{
    Failures f;

    { // not in the ini, or "auto": off, and the key is saved as it was found
        for (const char* text : { static_cast<const char*>(nullptr), "auto", "AUTO" })
        {
            Session s(text);
            EXPECT(f, "unset: check is off", !CheckRuns());
            EXPECT(f, "unset: nothing to save for the key", !Key().value_for_config().has_value());
            EXPECT(f, "unset: Save Settings writes auto", s.Save() == "auto");
            EXPECT(f, "unset: still off after Save Settings", !CheckRuns());
            s.Reload(); // a second load (LoadFromPath): upstream's set_from_config does not overwrite, the override stays
            EXPECT(f, "unset: still off after a reload", !CheckRuns());
            Session next(s.Saved().c_str()); // the next launch reads the saved file
            EXPECT(f, "unset: off at the next launch", !CheckRuns());
        }
    }

    { // explicit true in the ini
        Session s("true");
        EXPECT(f, "ini true: check is on", CheckRuns());
        EXPECT(f, "ini true: Save Settings keeps true", s.Save() == "true");
        Session next(s.Saved().c_str());
        EXPECT(f, "ini true: on at the next launch", CheckRuns());
    }
    {
        Session s("TRUE"); // readBool lower-cases
        EXPECT(f, "ini TRUE: check is on", CheckRuns());
    }

    { // explicit false in the ini
        Session s("false");
        EXPECT(f, "ini false: check is off", !CheckRuns());
        EXPECT(f, "ini false: Save Settings keeps false", s.Save() == "false");
        Session next(s.Saved().c_str());
        EXPECT(f, "ini false: off at the next launch", !CheckRuns());
    }

    { // the menu's "Check for Update" checkbox on a launch that had nothing in the ini (ConfigCheckbox: option = value)
        Session s(nullptr);
        Key() = true;
        EXPECT(f, "menu on: check is on", CheckRuns());
        EXPECT(f, "menu on: Save Settings writes true", s.Save() == "true");
        Session next(s.Saved().c_str());
        EXPECT(f, "menu on: on at the next launch", CheckRuns());
    }
    { // ticked, then unticked again
        Session s(nullptr);
        Key() = true;
        Key() = false;
        EXPECT(f, "menu off again: check is off", !CheckRuns());
        EXPECT(f, "menu off again: Save Settings writes false", s.Save() == "false");
        Session next(s.Saved().c_str());
        EXPECT(f, "menu off again: off at the next launch", !CheckRuns());
    }
    { // true in the ini, unticked in the menu
        Session s("true");
        Key() = false;
        EXPECT(f, "ini true, menu off: Save Settings writes false", s.Save() == "false");
    }

    // The banner step: ext::core::Banner() (dllmain.cpp, after Config::Instance() and before CheckWorkingMode starts the check) calls
    // ApplyDefault(). It changes nothing after a normal load, and it is the only step that runs when no ini could be read.
    {
        Session s(nullptr, false);
        ext::updatecheck::ApplyDefault();
        EXPECT(f, "no ini: check is off after the banner step", !CheckRuns());
        EXPECT(f, "no ini: Save Settings writes auto", s.Save() == "auto");
        EXPECT(f, "no ini: still off after Save Settings", !CheckRuns());
    }
    {
        Session s(nullptr);
        ext::updatecheck::ApplyDefault();
        EXPECT(f, "unset: off after the banner step too", !CheckRuns());
        EXPECT(f, "unset: the banner step does not change what is saved", s.Save() == "auto");
    }
    for (const char* text : { "true", "false" })
    {
        Session s(text);
        const bool before = CheckRuns();
        ext::updatecheck::ApplyDefault();
        EXPECT(f, "explicit value: the banner step leaves it alone", CheckRuns() == before);
        EXPECT(f, "explicit value: the banner step leaves what is saved alone", s.Save() == text);
    }

    return f;
}

bool Has(const Failures& f, const char* name) { return std::find(f.begin(), f.end(), name) != f.end(); }

// ---- deliberately broken hooks: each rule above has to notice them (mutation checks by construction)
void LoadHookWithoutUnsetGuard() { Key().set_volatile_value(false); } // overrides an explicit true as well
void SaveHookAlwaysTrue(CSimpleIniA& ini) { ini.SetValue("Hotfix", "CheckForUpdate", "true"); }
void LoadHookWritesThrough() { Key() = false; } // a plain assignment is the value to save, not a volatile override

void WithHooks(void (*onLoaded)(), void (*onSaved)(CSimpleIniA&), const std::function<void()>& body)
{
    auto savedLoad = ext::ExtConfig::updateCheckOnLoaded;
    auto savedSave = ext::ExtConfig::updateCheckOnSaved;
    ext::ExtConfig::updateCheckOnLoaded = onLoaded;
    ext::ExtConfig::updateCheckOnSaved = onSaved;
    body();
    ext::ExtConfig::updateCheckOnLoaded = savedLoad;
    ext::ExtConfig::updateCheckOnSaved = savedSave;
}

// ---- the statements of upstream's files that the design stands on
void UpstreamFactsArePinned(const std::string& repo)
{
    const std::string config = ReadFile(repo + "/OptiScaler/Config.cpp");
    const std::string configH = ReadFile(repo + "/OptiScaler/Config.h");
    const std::string dllmain = ReadFile(repo + "/OptiScaler/dllmain.cpp");
    CHECK(!config.empty() && !configH.empty() && !dllmain.empty());

    // the default of the key is true: that is what makes an explicit true "equal to the default" and saved as auto
    CHECK(configH.find("CustomOptional<bool> CheckForUpdate { true };") != std::string::npos);

    // load: the key is read before ext::config::Load runs the post-load step (so the step sees the ini's answer)
    const size_t read = config.find("CheckForUpdate.set_from_config(readBool(\"Hotfix\", \"CheckForUpdate\"));");
    const size_t load = config.find("ext::config::Load(ini);");
    CHECK(read != std::string::npos && load != std::string::npos && read < load);
    CHECK(config.find("ext::config::Load(ini);") == config.rfind("ext::config::Load(ini);")); // the only call

    // ... and only when an ini was read: Reload returns false before ext::config::Load when OptiScaler.ini cannot be loaded, which is why
    // the banner step exists. DllMain itself never reads the ini again (Config::Instance() does it once; LoadFromPath has no caller).
    CHECK(config.find("if (ini.LoadFile(iniPath.c_str()) == SI_OK)") != std::string::npos);
    CHECK(config.find("ext::config::Load(ini);\n\n        return true;\n    }\n\n    return false;\n}") != std::string::npos);
    CHECK(config.find("_config->Reload(_config->absoluteFileName);") != std::string::npos);
    CHECK(dllmain.find("Reload(") == std::string::npos && dllmain.find("LoadFromPath(") == std::string::npos);

    // save: upstream writes the key, then ext::config::Save runs (the last write wins)
    const size_t write = config.find(
        "ini.SetValue(\"Hotfix\", \"CheckForUpdate\", GetBoolValue(Instance()->CheckForUpdate.value_for_config()).c_str());");
    const size_t save = config.find("ext::config::Save(ini);");
    CHECK(write != std::string::npos && save != std::string::npos && write < save);
    CHECK(config.find("ext::config::Save(ini);") == config.rfind("ext::config::Save(ini);"));

    // GetBoolValue is the one repeated above
    CHECK(config.find("std::string GetBoolValue(std::optional<bool> value)\n{\n    if (!value.has_value())\n        return \"auto\";\n\n"
                      "    return value.value() ? \"true\" : \"false\";\n}") != std::string::npos);

    // readBool: lower-cased text, true / false, everything else unset
    CHECK(config.find("std::optional<bool> Config::readBool(std::string section, std::string key)\n{\n"
                      "    auto value = readString(section, key, true);\n    if (value == \"true\")\n        return true;\n"
                      "    else if (value == \"false\")\n        return false;\n\n    return std::nullopt;\n}") != std::string::npos);

    // the check starts in exactly one place and is gated by the key (the volatile value is what value_or_default() answers)
    const std::string gate = "    if (Config::Instance()->CheckForUpdate.value_or_default())\n        VersionCheck::Start();";
    CHECK(dllmain.find(gate) != std::string::npos);
    CHECK(dllmain.find("VersionCheck::Start()") == dllmain.rfind("VersionCheck::Start()"));
    // ... inside CheckWorkingMode, which DllMain calls after Config::Instance() was first used (MainDllPath)
    const size_t fn = dllmain.find("static void CheckWorkingMode()");
    const size_t gateAt = dllmain.find(gate);
    const size_t after = dllmain.find("static void printQuirks(");
    CHECK(fn != std::string::npos && gateAt != std::string::npos && after != std::string::npos && fn < gateAt && gateAt < after);
    const size_t firstConfig = dllmain.find("Config::Instance()->MainDllPath.has_value()");
    const size_t dllMain = dllmain.find("BOOL APIENTRY DllMain(");
    CHECK(dllMain != std::string::npos && firstConfig != std::string::npos && dllMain < firstConfig);

    // the banner step: ext::core::Banner() runs on the normal path after PrepareLogger (so after the first Config::Instance()) and before
    // CheckWorkingMode; the passthrough path returns from CheckWorkingMode before the version gate and never calls Banner
    const size_t banner = dllmain.find("ext::core::Banner();");
    const size_t lastCheck = dllmain.rfind("CheckWorkingMode();");
    CHECK(banner != std::string::npos && lastCheck != std::string::npos && dllMain < banner && banner < lastCheck);
    CHECK(dllmain.find("ext::core::Banner();") == dllmain.rfind("ext::core::Banner();"));
    CHECK(dllmain.find("    if (_passThruMode)\n        return;\n\n    if (!modeFound)", fn) != std::string::npos);
    CHECK(dllmain.find("    if (_passThruMode)\n        return;\n\n    if (!modeFound)", fn) < gateAt);
    {
        const size_t passThru = dllmain.find("        if (_passThruMode)\n        {", dllMain);
        CHECK(passThru != std::string::npos && passThru < banner); // the passthrough branch of DllMain returns before Banner
    }
    const std::string bannerCpp = ReadFile(repo + "/OptiScaler/ext/core/Banner.cpp");
    CHECK(bannerCpp.find("#include \"ext/core/UpdateCheck.h\"") != std::string::npos);
    // a line of code of its own: a call that was commented out (`// ext::updatecheck::ApplyDefault();`) must not pass
    CHECK(bannerCpp.find("\n    ext::updatecheck::ApplyDefault();\n") != std::string::npos);
    const std::string catalog = ReadFile(repo + "/docs/SEAMS-catalog.md");
    CHECK(catalog.find("`ext::core::Banner()` | hook |") != std::string::npos);
    CHECK(catalog.find("applies the default of the upstream update check") != std::string::npos);

    // the ConfigCheckbox of the menu assigns (which makes the value the one to save), it does not call set_volatile_value
    const std::string menu = ReadFile(repo + "/OptiScaler/menu/menu_common.cpp");
    CHECK(menu.find("ConfigCheckbox(\"Check for Update\", config->CheckForUpdate,") != std::string::npos);
    CHECK(menu.find("if (changed)\n        option = value;") != std::string::npos);
}

void FragmentAndDocsSayIt(const std::string& repo)
{
    // The shipped template (upstream's file, never edited) says "Default (auto) is true"; the fork's fragment, merged at the end
    // of [Hotfix], says what auto means here. It must not add a key line of its own (a second CheckForUpdate= would be read as the key).
    const std::string frag = ReadFile(repo + "/OptiScaler/ext/ini/Hotfix.ini");
    CHECK(frag.find("\nCheckForUpdate=") == std::string::npos && frag.rfind("CheckForUpdate=", 0) != 0);
    CHECK(frag.find("CheckForUpdate") != std::string::npos);
    CHECK(frag.find("auto means false in this fork") != std::string::npos);

    const std::string features = ReadFile(repo + "/docs/FEATURES.md");
    CHECK(features.find("does not check for updates") != std::string::npos);
    CHECK(features.find("upstream OptiScaler's GitHub project, not this fork's") != std::string::npos);
    CHECK(features.find("[Update check off by default](#update-check-off-by-default)") != std::string::npos); // the summary row
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";

    // The glue registered both hooks when its translation unit was initialised, before main (before DllMain in the product).
    void (*realLoad)() = ext::ExtConfig::updateCheckOnLoaded;
    void (*realSave)(CSimpleIniA&) = ext::ExtConfig::updateCheckOnSaved;
    CHECK(realLoad != nullptr);
    CHECK(realSave != nullptr);

    // 1. production hooks: every rule holds
    {
        const Failures f = RunScenarios();
        for (const auto& name : f)
            std::printf("FAIL scenario: %s\n", name.c_str());
        g_failed += static_cast<int>(f.size());
    }

    // 2. negative controls: without the glue, or with a broken one, the rules notice
    WithHooks(nullptr, nullptr,
              []
              {
                  const Failures f = RunScenarios();
                  CHECK(Has(f, "unset: check is off")); // the old default: the check runs
                  CHECK(Has(f, "menu on: Save Settings writes true"));
                  CHECK(Has(f, "ini true: Save Settings keeps true")); // upstream alone loses an explicit true at Save Settings
                  CHECK(!Has(f, "ini true: check is on"));
                  CHECK(!Has(f, "ini false: check is off"));
              });

    WithHooks(realLoad, nullptr,
              [&]
              {
                  const Failures f = RunScenarios(); // load side only: off by default, but an explicit true does not survive a save
                  CHECK(!Has(f, "unset: check is off"));
                  CHECK(Has(f, "ini true: Save Settings keeps true"));
                  CHECK(Has(f, "menu on: Save Settings writes true"));
                  CHECK(Has(f, "ini true: on at the next launch"));
              });

    WithHooks(&LoadHookWithoutUnsetGuard, realSave,
              []
              {
                  const Failures f = RunScenarios(); // no "unset" guard: the explicit true of the ini is overridden
                  CHECK(Has(f, "ini true: check is on"));
                  CHECK(Has(f, "ini TRUE: check is on"));
              });

    WithHooks(&LoadHookWritesThrough, realSave,
              []
              {
                  const Failures f = RunScenarios(); // a plain assignment would be saved as an explicit false
                  CHECK(Has(f, "unset: Save Settings writes auto"));
              });

    WithHooks(realLoad, &SaveHookAlwaysTrue,
              []
              {
                  const Failures f = RunScenarios(); // a save hook that does not look at the value
                  CHECK(Has(f, "unset: Save Settings writes auto"));
                  CHECK(Has(f, "ini false: Save Settings keeps false"));
              });

    // The gap the banner step closes: with no readable ini nothing loads, the key stays unset and upstream's default (true) runs the check
    {
        Session s(nullptr, false);
        CHECK(CheckRuns());
        CHECK(!Key().has_value());
    }

    // 3. the templates the hooks are made of, on their own
    {
        CustomOptional<bool> k { true };
        ext::updatecheck::DefaultOff(k);
        CHECK(!k.value_or_default() && !k.value_for_config().has_value());
        ext::updatecheck::DefaultOff(k); // idempotent
        CHECK(!k.value_or_default() && !k.value_for_config().has_value());
        CSimpleIniA ini;
        ext::updatecheck::KeepOn(k, ini); // off: nothing written
        CHECK(ini.GetValue("Hotfix", "CheckForUpdate", nullptr) == nullptr);
        k = true;
        ext::updatecheck::KeepOn(k, ini);
        CHECK(std::string(ini.GetValue("Hotfix", "CheckForUpdate", "")) == "true");
    }

    // KeepOn on an option nobody set (a launch where no load hook ran) is not "the player turned it on": nothing is written. The compiled
    // default of the key is true, so value_or_default() would answer true here, and an unchecked value() throws.
    {
        CustomOptional<bool> unset { true };
        CSimpleIniA ini;
        try
        {
            ext::updatecheck::KeepOn(unset, ini);
        }
        catch (...)
        {
            std::printf("FAIL: KeepOn threw on an unset option\n");
            ++g_failed;
        }
        CHECK(ini.GetValue("Hotfix", "CheckForUpdate", nullptr) == nullptr);
    }

    UpstreamFactsArePinned(repo);
    FragmentAndDocsSayIt(repo);

    if (g_failed != 0)
    {
        std::printf("update_check: %d failure(s)\n", g_failed);
        return 1;
    }
    std::puts("update_check: unset/auto off, explicit true on and kept by Save Settings, explicit false off, the override is never saved; negative controls fire");
    return 0;
}
