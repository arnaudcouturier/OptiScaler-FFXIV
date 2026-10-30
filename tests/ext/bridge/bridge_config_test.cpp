// [BRIDGE] The [Bridge] keys through the real ini binding (ExtConfig.cpp): FlushAfterSignal is one bool key, default
// false (today's hand-over), loads and saves as documented, and publishes into the cached copy the D3D11 -> D3D12
// hand-over reads. The ini fragment OptiScaler/ext/ini/Bridge.ini documents it (the generic hygiene rules of
// tests/ext/menu/ini_fragments.cpp cover the comment/default agreement; this test pins the values and the seam).
//
// CustomOptional::set_from_config only sets a value that has none yet, so every case reads into a fresh BridgeConfig.
#include "ext/bridge/BridgeConfig.h"
#include "ext/core/ExtConfig.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int g_failed = 0;
#define CHECK(cond)                                                                                              \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                             \
        {                                                                                                        \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                  \
            ++g_failed;                                                                                          \
        }                                                                                                        \
    } while (0)

namespace
{
std::string ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// CRLF -> LF, so a checkout setting cannot decide what the fragment checks see
std::string Normalized(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text)
        if (c != '\r')
            out += c;
    return out;
}

ext::BridgeConfig LoadFresh(const char* text)
{
    CSimpleIniA ini;
    ini.SetUnicode();
    ini.LoadData(text, std::string(text).size());
    ext::BridgeConfig cfg;
    ext::config::Binder binder;
    cfg.Bind(binder);
    for (const auto& entry : binder.Table())
        entry.load(entry, ini);
    return cfg;
}

std::string Saved(ext::BridgeConfig& cfg)
{
    CSimpleIniA out;
    ext::config::Binder binder;
    cfg.Bind(binder);
    for (const auto& entry : binder.Table())
        entry.save(entry, out);
    return out.GetValue("Bridge", "FlushAfterSignal", "missing");
}

void DefaultsAndBinding()
{
    ext::BridgeConfig cfg;
    CHECK(!cfg.flushAfterSignal.has_value());                       // unset = auto
    CHECK(cfg.flushAfterSignal.value_or_default() == false);        // today's hand-over: no flush

    // exactly one key is bound in [Bridge], once, and the process-wide config binds it too
    for (bool global : { false, true })
    {
        ext::config::Binder b;
        if (global)
            ext::Cfg().Bind(b);
        else
            cfg.Bind(b);
        int count = 0, flush = 0;
        for (const auto& e : b.Table())
            if (std::string(e.section) == "Bridge")
            {
                ++count;
                flush += std::string(e.key) == "FlushAfterSignal" ? 1 : 0;
            }
        CHECK(count == 1 && flush == 1);
    }
}

void LoadSaveRoundTrip()
{
    // nothing in the ini, "auto" (the shipped template) and unknown words: unset, and the default answers off
    for (const char* text :
         { "", "[Bridge]\nFlushAfterSignal=auto\n", "[Bridge]\nFlushAfterSignal=perhaps\n", "[Bridge]\nFlushAfterSignal=1\n" })
    {
        auto cfg = LoadFresh(text);
        CHECK(cfg.flushAfterSignal.value_or_default() == false);
        CHECK(Saved(cfg) == "auto");
    }
    {
        auto cfg = LoadFresh("[Bridge]\nFlushAfterSignal=true\n");
        CHECK(cfg.flushAfterSignal.value_or_default() == true);
        CHECK(Saved(cfg) == "true");
    }
    {
        auto cfg = LoadFresh("[Bridge]\nFlushAfterSignal=false\n");
        CHECK(cfg.flushAfterSignal.value_or_default() == false);
        CHECK(Saved(cfg) == "auto"); // an explicit false is the default, and a default is not written back
    }
    {
        // a value set by hand in the ini survives a save, and dropping the line again means off
        auto cfg = LoadFresh("[Bridge]\nFlushAfterSignal=TRUE\n");
        CHECK(cfg.flushAfterSignal.value_or_default() == true);
        CHECK(Saved(cfg) == "true");
        auto cleared = LoadFresh("[Bridge]\n");
        CHECK(cleared.flushAfterSignal.value_or_default() == false);
    }
}

void PublishesTheCachedCopy()
{
    // the seam reads the cached copy, so PostLoad must be what moves it: on, then off again
    const bool before = ext::bridge::FlushAfterSignal();

    auto on = LoadFresh("[Bridge]\nFlushAfterSignal=true\n");
    on.PostLoad();
    CHECK(ext::bridge::FlushAfterSignal() == true);

    auto off = LoadFresh("[Bridge]\nFlushAfterSignal=auto\n");
    off.PostLoad();
    CHECK(ext::bridge::FlushAfterSignal() == false);

    ext::bridge::PublishFlushAfterSignal(before); // leave the process as it was found
    CHECK(ext::bridge::FlushAfterSignal() == before);
}

void IniFragment(const std::string& repo)
{
    const std::string ini = Normalized(ReadFile(repo + "/OptiScaler/ext/ini/Bridge.ini"));
    CHECK(!ini.empty());
    CHECK(ini.find("[Bridge]") == std::string::npos); // the packager adds the section header
    CHECK(ini.find("\nFlushAfterSignal=auto\n") != std::string::npos);
    // what the user needs to know, in words
    CHECK(ini.find("Default (auto) is false") != std::string::npos); // the switch is off unless asked for
    CHECK(ini.find("No menu control") != std::string::npos);         // ini-only keys say so (ini_fragments rule)
    CHECK(ini.find("Bridge FlushAfterSignal on:") != std::string::npos); // the startup line to look for
    CHECK(ini.find("D3D11 -> D3D12") != std::string::npos);          // where in the frame the experiment sits
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    DefaultsAndBinding();
    LoadSaveRoundTrip();
    PublishesTheCachedCopy();
    IniFragment(repo);
    if (g_failed != 0)
    {
        std::printf("bridge_config: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("bridge_config: all passed\n");
    return 0;
}
