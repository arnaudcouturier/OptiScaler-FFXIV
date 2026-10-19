// Dynamic MFG config: the real FgConfig.cpp (Bind, PostLoad) against a recording Binder, and the real CustomOptional
// (sliced from OptiScaler\Config.h by the Prepare script) for the volatile write the selector uses.
#include "dynamic_mfg_harness.h"

#include "ext/core/ExtConfig.h" // the recording Binder of mfg_cfg_stubs
#include "ext/fg/FgConfig.h"

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

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

void KeysBindWithDefaults()
{
    ext::FgConfig cfg;
    CHECK(!cfg.autoMfg.value_or_default());
    CHECK(cfg.autoMfgMax.value_or_default() == 4);
    CHECK(cfg.autoMfgTarget.value_or_default() == 0.0f);
    CHECK(!cfg.autoMfgSelfTest.value_or_default());

    ext::config::Binder binder;
    cfg.Bind(binder);
    auto field = [&](const char* section, const char* key) -> void*
    {
        void* found = nullptr;
        int count = 0;
        for (const auto& e : binder.Table())
            if (std::string(e.section) == section && std::string(e.key) == key)
            {
                found = e.field;
                ++count;
            }
        return count == 1 ? found : nullptr; // present exactly once
    };
    CHECK(field("DLSSG", "AutoMFG") == &cfg.autoMfg);
    CHECK(field("DLSSG", "AutoMFGMax") == &cfg.autoMfgMax);
    CHECK(field("DLSSG", "AutoMFGTarget") == &cfg.autoMfgTarget);
    CHECK(field("DLSSG", "AutoMFGSelfTest") == &cfg.autoMfgSelfTest);
    CHECK(field("DLSSG", "AdaMfgUnlock") == &cfg.adaMfgUnlock); // the neighbours are still bound
}

void PostLoadRangeChecks()
{
    for (int bad : { 1, 0, -3, 5, 7, 100 })
    {
        ext::FgConfig cfg;
        cfg.autoMfgMax = bad;
        cfg.PostLoad();
        CHECK_MSG(!cfg.autoMfgMax.has_value() && cfg.autoMfgMax.value_or_default() == 4, "AutoMFGMax %d kept", bad);
    }
    for (int good : { 2, 3, 4 })
    {
        ext::FgConfig cfg;
        cfg.autoMfgMax = good;
        cfg.PostLoad();
        CHECK(cfg.autoMfgMax.has_value() && cfg.autoMfgMax.value_or_default() == good);
    }
    for (float bad : { -1.0f, -0.5f, 1001.0f, 1e9f, std::nanf(""), INFINITY })
    {
        ext::FgConfig cfg;
        cfg.autoMfgTarget = bad;
        cfg.PostLoad();
        CHECK_MSG(!cfg.autoMfgTarget.has_value(), "AutoMFGTarget %g kept", (double) bad);
    }
    for (float good : { 0.0f, 144.0f, 240.5f, 500.0f, 1000.0f })
    {
        ext::FgConfig cfg;
        cfg.autoMfgTarget = good;
        cfg.PostLoad();
        CHECK(cfg.autoMfgTarget.has_value() && cfg.autoMfgTarget.value_or_default() == good);
    }
}

void IniFragmentDocumentsTheKeys(const std::string& repo)
{
    const std::string ini = ReadFile(repo + "/OptiScaler/ext/ini/DLSSG.ini");
    CHECK(!ini.empty());
    for (const char* key : { "AutoMFG", "AutoMFGMax", "AutoMFGTarget", "AutoMFGSelfTest" })
        CHECK_MSG(HasKeyLine(ini, key), "DLSSG.ini lacks %s", key);
    CHECK(HasKeyLine(ini, "AdaMfgUnlock") && HasKeyLine(ini, "UIRecomposition")); // the old ones are still there
    CHECK(ini.find("[DLSSG]") == std::string::npos); // a header would be dropped by the packager
    CHECK(ini.find("Default (auto) is 4") != std::string::npos); // AutoMFGMax
    CHECK(ini.find("tested on one RTX 4090 setup only") != std::string::npos);
    // the kill-switch rule as the controller has it: two long frames in a row or three in all (not "a second one")
    CHECK(ini.find("stops after two such changes in a row, or three in all. An earlier one is forgotten only") != std::string::npos);
    // strikes are forgotten by the number of clean changes in a row, not by time (the code: kForgetAfterClean)
    CHECK(ini.find("after ten clean changes in a row, counted in changes and not in time") != std::string::npos);
    CHECK(ini.find("ten clean changes forget") == std::string::npos);
    // the stall rule (the code: DynamicMfgControl.h kStallMs 30 ms = about 33 fps, kStallGiveUpMs 5 s, kSelfTestCalmMs 20 s)
    CHECK(ini.find("A game stall (a loading screen, a menu change that re-creates DLSS) is never held against the mode") !=
          std::string::npos);
    CHECK(ini.find("rate is below about 33 fps (an average above 30 ms) no change is made and the last one is not judged") !=
          std::string::npos);
    CHECK(ini.find("\"judging the change at Nx skipped, the game was stalled\") when that is not") != std::string::npos);
    CHECK(ini.find("; possible within 5 seconds. On a setup that always runs below about 33 real fps") != std::string::npos);
    CHECK(ini.find("Starts after 20 seconds without a stall (a stall during it ends it") != std::string::npos);
    CHECK(ini.find("Takes about 40 seconds in the game world") == std::string::npos);
    {
        const std::string code = ReadFile(repo + "/OptiScaler/ext/fg/DynamicMfgControl.h");
        CHECK(code.find("kStallMs = 30.0;") != std::string::npos);
        CHECK(code.find("kStallCalmMs = 1000;") != std::string::npos);
        CHECK(code.find("kStallGiveUpMs = 5000;") != std::string::npos);
        CHECK(code.find("kSelfTestCalmMs = 20000;") != std::string::npos);
        CHECK(code.find("judging the change at {}x skipped, the game was stalled") != std::string::npos);
    }
    CHECK(ini.find("stops after a second such change") == std::string::npos);
    // the one side effect of the Ada clamp (the frame limiter still paces for the slider count) is told next to the key
    CHECK(ini.find("with a frame limit keep the slider on 4X") != std::string::npos);
}

// The menu card cannot be run here (ImGui); its registration and the honest caveat of its tooltip are pinned by text.
void MenuCardKeepsTheCaveat(const std::string& repo)
{
    const std::string card = ReadFile(repo + "/OptiScaler/ext/fg/MenuAutoMfg.cpp");
    CHECK(!card.empty());
    CHECK(card.find("\"fg_auto_mfg\", \"Frame Gen\"") != std::string::npos);
    CHECK(card.find("\"fg_dlssg\"") != std::string::npos); // right after the DLSSG card
    CHECK(card.find("can still cost one long frame (about 0.2 s)") != std::string::npos);
    // the kill-switch rule as the controller has it (DynamicMfgControl.h kStrikesToStop, kStrikesInTotalToStop)
    CHECK(card.find("waits 12 s between changes and stops after two in a row or three in all.") != std::string::npos);
    CHECK(card.find("An earlier one is forgotten only after ten clean changes in a row, not after a time.") != std::string::npos);
    CHECK(card.find("A game stall (below about 33 real fps) is never held against it: nothing changes while it lasts.") !=
          std::string::npos);
    CHECK(card.find("stops after a second one") == std::string::npos);
    CHECK(card.find("Tested on one RTX 4090 setup only") != std::string::npos);
    // the self-test key is a checkbox of the card: read from the key, written back to the key, nothing else writes it
    CHECK(card.find("bool selfTest = cfg.autoMfgSelfTest.value_or_default();") != std::string::npos);
    CHECK(card.find("if (ImGui::Checkbox(\"Run the self-test once##AutoMfg\", &selfTest))") != std::string::npos);
    CHECK(card.find("cfg.autoMfgSelfTest = selfTest;") != std::string::npos);
    CHECK(card.find("cfg.autoMfgSelfTest = ") == card.rfind("cfg.autoMfgSelfTest = ")); // exactly one write
    CHECK(card.find("It starts after 20 calm seconds") != std::string::npos);
    // the visibility rule itself is tested in dynamic_mfg_gate_unit (AutoMfgCardVisible); here only that the card asks it
    // the GPU is asked only after the cheap output check (a lambda, so a card that is not shown never enumerates adapters)
    CHECK(card.find("AutoMfgCardVisibleAsking(ctx.state.activeFgOutput == FGOutput::DLSSG, [] { return IsBlackwell(); })") !=
          std::string::npos);
    CHECK(card.find("AutoMfgCardVisible(ctx.state.activeFgOutput == FGOutput::DLSSG, IsBlackwell())") == std::string::npos);
    CHECK(card.find("GpuAsk ask;") != std::string::npos); // the retries are capped and spaced by the tested class
    CHECK(card.find("IsBlackwellArchitecture(") != std::string::npos); // the card and the gate share the RTX 50 test
}

// The selector writes the count with set_volatile_value: Save Settings must keep storing what the user chose.
void VolatileWriteKeepsTheSavedValue()
{
    CustomOptional<int> count { 1 };
    count.set_from_config(2); // the ini says 3X
    CHECK(count.value_for_config() == std::optional<int>(2));
    count.set_volatile_value(3);
    CHECK(count.value_or_default() == 3);
    CHECK(count.value_for_config() == std::optional<int>(2));
    count.set_volatile_value(1); // a later automatic change does not overwrite the saved value either
    CHECK(count.value_or_default() == 1);
    CHECK(count.value_for_config() == std::optional<int>(2));
    count = 3; // the user drags the slider: now it is the value to save
    CHECK(count.value_or_default() == 3 && count.value_for_config() == std::optional<int>(3));
    count.set_volatile_value(2);
    CHECK(count.value_for_config() == std::optional<int>(3));

    CustomOptional<int> unset { 1 }; // nothing in the ini: default stays the saved state
    unset.set_volatile_value(3);
    CHECK(unset.value_or_default() == 3 && !unset.value_for_config().has_value());

    CustomOptional<int> asDefault { 1 }; // the ini holds the default value itself
    asDefault.set_from_config(1);
    asDefault.set_volatile_value(2);
    CHECK(!asDefault.value_for_config().has_value());
}
} // namespace

int main(int argc, char** argv)
{
    KeysBindWithDefaults();
    PostLoadRangeChecks();
    IniFragmentDocumentsTheKeys(argc > 1 ? argv[1] : ".");
    VolatileWriteKeepsTheSavedValue();
    MenuCardKeepsTheCaveat(argc > 1 ? argv[1] : ".");
    if (g_failures)
    {
        std::printf("dynamic_mfg_config: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("dynamic_mfg_config: all passed\n");
    return 0;
}
