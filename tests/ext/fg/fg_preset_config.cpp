// [DLSSG] RenderPreset: the key binds once with default 0 (Auto), PostLoad drops values outside 0..3, and the ini fragment
// DLSSG.ini documents it (the ini hygiene rules of tests/ext/menu/ini_fragments.cpp cover the generic parts).
#include "fg_preset_harness.h"

#include "ext/core/ExtConfig.h" // the recording Binder of mfg_cfg_stubs
#include "ext/fg/FgConfig.h"

#include <fstream>
#include <initializer_list>
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

void KeyBindsWithDefault()
{
    ext::FgConfig cfg;
    CHECK(cfg.renderPreset.value_or_default() == 0);
    CHECK(!cfg.renderPreset.has_value());

    ext::config::Binder binder;
    cfg.Bind(binder);
    int count = 0;
    void* field = nullptr;
    for (const auto& e : binder.Table())
        if (std::string(e.section) == "DLSSG" && std::string(e.key) == "RenderPreset")
        {
            ++count;
            field = e.field;
        }
    CHECK(count == 1);
    CHECK(field == &cfg.renderPreset);
}

void PostLoadRange()
{
    for (int bad : { -1, 4, 5, 13, 99, -100 })
    {
        ext::FgConfig cfg;
        cfg.renderPreset = bad;
        cfg.PostLoad();
        CHECK_MSG(!cfg.renderPreset.has_value() && cfg.renderPreset.value_or_default() == 0, "RenderPreset %d kept", bad);
    }
    for (int good : { 0, 1, 2, 3 })
    {
        ext::FgConfig cfg;
        cfg.renderPreset = good;
        cfg.PostLoad();
        CHECK(cfg.renderPreset.has_value() && cfg.renderPreset.value_or_default() == good);
    }
}

void IniFragment(const std::string& repo)
{
    const std::string ini = ReadFile(repo + "/OptiScaler/ext/ini/DLSSG.ini");
    CHECK(!ini.empty());
    CHECK(ini.find("\nRenderPreset=auto") != std::string::npos);
    CHECK(ini.find("Default (auto) is 0") != std::string::npos);
    CHECK(ini.find("[DLSSG]") == std::string::npos); // the packager adds the section header
    // what the key does and does not do, in words the user can act on
    CHECK(ini.find("0 = Auto (the driver and the runtime decide), 1 = Preset A, 2 = Preset B, 3 = Preset C") != std::string::npos);
    CHECK(ini.find("No NVIDIA profile is written") != std::string::npos);
    CHECK(ini.find("restart needed") != std::string::npos);
    // Preset C is not handled by every runtime build; the text must not promise it
    CHECK(ini.find("not every runtime build") != std::string::npos);
    // the recomposition sentence follows the rule (UiRecompositionRule.h): Preset A and C are not asked for it from the first option call,
    // nothing is "paused" afterwards (the old mechanism); Preset B and Auto keep asking
    CHECK(ini.find("pause") == std::string::npos);
    CHECK(ini.find("In HUD-less recomposition mode Preset A and C are not asked for NVIDIA's UI recomposition") != std::string::npos);
    CHECK(ini.find("Preset B and Auto ask for it") != std::string::npos);
}
} // namespace

int main(int argc, char** argv)
{
    KeyBindsWithDefault();
    PostLoadRange();
    IniFragment(argc > 1 ? argv[1] : ".");
    if (g_failures)
    {
        std::printf("fg_preset_config: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("fg_preset_config: key binds once with default 0, PostLoad range 0..3, ini fragment documents it");
    return 0;
}
