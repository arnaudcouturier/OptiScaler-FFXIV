// [Menu] Style through the real ini binding: default classic, one bound key in [Menu], load and save round trip, "auto" (the
// shipped template) and unknown values read as classic.
//
// CustomOptional::set_from_config only sets a value that has none yet (a config is read once per process), so the
// process-wide Cfg() is loaded once and every other value is read into a fresh MenuStyleConfig.
#include "ext/core/ExtConfig.h"

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
ext::MenuStyleConfig LoadFresh(CSimpleIniA& ini)
{
    ext::MenuStyleConfig cfg;
    ext::config::Binder binder;
    cfg.Bind(binder);
    for (const auto& entry : binder.Table())
        entry.load(entry, ini);
    return cfg;
}

std::string SavedValue(ext::MenuStyleConfig& cfg)
{
    CSimpleIniA out;
    ext::config::Binder binder;
    cfg.Bind(binder);
    for (const auto& entry : binder.Table())
        entry.save(entry, out);
    return out.GetValue("Menu", "Style", "missing");
}
} // namespace

int main()
{
    using namespace ext;
    using ext::menustyle::Style;

    MenuStyleConfig defaults;
    CHECK(defaults.GetStyle() == Style::Classic);

    // Exactly one key is bound in [Menu]: Style (the other [Menu] keys belong to upstream's Config).
    {
        config::Binder binder;
        Cfg().Bind(binder);
        int menuKeys = 0;
        for (const auto& entry : binder.Table())
        {
            if (std::strcmp(entry.section, "Menu") == 0)
            {
                ++menuKeys;
                CHECK(std::strcmp(entry.key, "Style") == 0);
                CHECK(entry.field == &Cfg().menuStyle.look);
            }
        }
        CHECK(menuKeys == 1);
    }

    // The real path: config::Load reads the ini, config::Save writes it back as text.
    {
        CSimpleIniA ini;
        ini.SetValue("Menu", "Style", "ember");
        config::Load(ini);
        CHECK(Cfg().menuStyle.GetStyle() == Style::Ember);

        CSimpleIniA out;
        config::Save(out);
        CHECK(std::string(out.GetValue("Menu", "Style", "missing")) == "ember");
    }

    {
        CSimpleIniA ini;
        ini.SetValue("Menu", "Style", "EMBER");
        MenuStyleConfig cfg = LoadFresh(ini);
        CHECK(cfg.GetStyle() == Style::Ember);
    }
    {
        // "auto" is what the shipped template carries: unset, so classic, and saved back as auto.
        CSimpleIniA ini;
        ini.SetValue("Menu", "Style", "auto");
        MenuStyleConfig cfg = LoadFresh(ini);
        CHECK(cfg.GetStyle() == Style::Classic);
        CHECK(SavedValue(cfg) == "auto");
    }
    {
        // A missing key is the default.
        CSimpleIniA ini;
        MenuStyleConfig cfg = LoadFresh(ini);
        CHECK(cfg.GetStyle() == Style::Classic);
        CHECK(SavedValue(cfg) == "auto");
    }
    {
        CSimpleIniA ini;
        ini.SetValue("Menu", "Style", "sleek");
        MenuStyleConfig cfg = LoadFresh(ini);
        CHECK(cfg.GetStyle() == Style::Classic);
    }

    return g_failed ? 1 : 0;
}
