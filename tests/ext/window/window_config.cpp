// [Window] Mode through the real ini binding: default off, one bound key, load and save round trip, the cached mode the
// Present path reads is published by config::Load, and unknown values read as off.
//
// CustomOptional::set_from_config only sets a value that has none yet (a config is read once per process), so the
// process-wide Cfg() is loaded once and every other value is read into a fresh WindowConfig.
#include "ext/core/ExtConfig.h"
#include "ext/window/WindowSeams.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_failed = 0;
#define CHECK(cond)                                                                                                         \
    do                                                                                                                      \
    {                                                                                                                       \
        if (!(cond))                                                                                                        \
        {                                                                                                                   \
            std::printf("CHECK failed line %d: %s\n", __LINE__, #cond);                                                     \
            ++g_failed;                                                                                                     \
        }                                                                                                                   \
    } while (0)

namespace
{
// Reads [Window] Mode from `ini` into a fresh config exactly like config::Load does, and publishes it like PostLoad.
ext::WindowConfig LoadFresh(CSimpleIniA& ini)
{
    ext::WindowConfig cfg;
    ext::config::Binder binder;
    cfg.Bind(binder);
    for (const auto& entry : binder.Table())
        entry.load(entry, ini);
    cfg.PostLoad();
    return cfg;
}

std::string SavedValue(ext::WindowConfig& cfg)
{
    CSimpleIniA out;
    ext::config::Binder binder;
    cfg.Bind(binder);
    for (const auto& entry : binder.Table())
        entry.save(entry, out);
    return out.GetValue("Window", "Mode", "missing");
}
} // namespace

int main()
{
    using namespace ext;
    using ext::window::Mode;

    // Defaults: off, in the struct and in the published copy.
    WindowConfig defaults;
    CHECK(defaults.GetMode() == Mode::Off);
    CHECK(window::CurrentMode() == Mode::Off);

    // Exactly one key is bound: [Window] Mode.
    {
        config::Binder binder;
        Cfg().Bind(binder);
        int windowKeys = 0;
        for (const auto& entry : binder.Table())
        {
            if (std::strcmp(entry.section, "Window") == 0)
            {
                ++windowKeys;
                CHECK(std::strcmp(entry.key, "Mode") == 0);
                CHECK(entry.field == &Cfg().window.mode);
            }
        }
        CHECK(windowKeys == 1);
    }

    // The real path: config::Load reads the ini and PostLoad publishes the cached mode the PresentScope guard reads.
    {
        CSimpleIniA ini;
        ini.SetValue("Window", "Mode", "probe");
        config::Load(ini);
        CHECK(Cfg().window.GetMode() == Mode::Probe);
        CHECK(window::CurrentMode() == Mode::Probe);

        // And config::Save writes it back as text.
        CSimpleIniA out;
        config::Save(out);
        CHECK(std::string(out.GetValue("Window", "Mode", "missing")) == "probe");
    }

    // The other values, read into fresh structs.
    {
        CSimpleIniA ini;
        ini.SetValue("Window", "Mode", "ON");
        WindowConfig cfg = LoadFresh(ini);
        CHECK(cfg.GetMode() == Mode::On);
        CHECK(window::CurrentMode() == Mode::On);
        CHECK(SavedValue(cfg) == "ON" || SavedValue(cfg) == "on");
    }
    {
        // "auto" is what the shipped template carries: unset, so off, and saved back as auto.
        CSimpleIniA ini;
        ini.SetValue("Window", "Mode", "auto");
        WindowConfig cfg = LoadFresh(ini);
        CHECK(cfg.GetMode() == Mode::Off);
        CHECK(window::CurrentMode() == Mode::Off);
        CHECK(SavedValue(cfg) == "auto");
    }
    {
        // A missing key is the default.
        CSimpleIniA ini;
        window::Publish(Mode::On);
        WindowConfig cfg = LoadFresh(ini);
        CHECK(cfg.GetMode() == Mode::Off);
        CHECK(window::CurrentMode() == Mode::Off);
        CHECK(SavedValue(cfg) == "auto");
    }
    {
        CSimpleIniA ini;
        ini.SetValue("Window", "Mode", "yes please");
        WindowConfig cfg = LoadFresh(ini);
        CHECK(cfg.GetMode() == Mode::Off);
        CHECK(window::CurrentMode() == Mode::Off);
    }

    return g_failed ? 1 : 0;
}
