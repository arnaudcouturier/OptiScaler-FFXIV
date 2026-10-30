// [PACE] The [FramePacing] keys through the real ini binding (ExtConfig.cpp): Trace (no default, "auto" = unset) and
// HiddenPresentDoNotWait (default false) bind once under their section, load and save as documented, and the ini fragment
// OptiScaler/ext/ini/FramePacing.ini documents them (the generic hygiene rules of tests/ext/menu/ini_fragments.cpp cover the
// comment/default agreement; this test pins the values this stream decided).
//
// CustomOptional::set_from_config only sets a value that has none yet, so every case reads into a fresh FramePacingConfig.
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

ext::FramePacingConfig LoadFresh(const char* text)
{
    CSimpleIniA ini;
    ini.SetUnicode();
    ini.LoadData(text, std::string(text).size());
    ext::FramePacingConfig cfg;
    ext::config::Binder binder;
    cfg.Bind(binder);
    for (const auto& entry : binder.Table())
        entry.load(entry, ini);
    return cfg;
}

std::string Saved(ext::FramePacingConfig& cfg, const char* key)
{
    CSimpleIniA out;
    ext::config::Binder binder;
    cfg.Bind(binder);
    for (const auto& entry : binder.Table())
        entry.save(entry, out);
    return out.GetValue("FramePacing", key, "missing");
}

void DefaultsAndBinding()
{
    ext::FramePacingConfig cfg;
    CHECK(!cfg.trace.has_value()); // unset = auto
    CHECK(!cfg.TraceKey().has_value());
    CHECK(!cfg.gpuTimers.has_value()); // unset = auto (on only with the trace)
    CHECK(!cfg.GpuTimersKey().has_value());
    CHECK(!cfg.hiddenPresentDoNotWait.has_value());
    CHECK(cfg.hiddenPresentDoNotWait.value_or_default() == false); // todays behaviour
    CHECK(!cfg.tickProbe.has_value()); // unset = auto
    CHECK(!cfg.TickProbeOn());         // the tick probe is not installed with a default ini

    // exactly these four keys are bound in [FramePacing], once each, in the process-wide config too
    for (bool global : { false, true })
    {
        ext::config::Binder b;
        if (global)
            ext::Cfg().Bind(b);
        else
            cfg.Bind(b);
        int count = 0, trace = 0, timers = 0, hidden = 0, tick = 0;
        for (const auto& e : b.Table())
            if (std::string(e.section) == "FramePacing")
            {
                ++count;
                trace += std::string(e.key) == "Trace" ? 1 : 0;
                timers += std::string(e.key) == "GpuTimers" ? 1 : 0;
                hidden += std::string(e.key) == "HiddenPresentDoNotWait" ? 1 : 0;
                tick += std::string(e.key) == "TickProbe" ? 1 : 0;
            }
        CHECK(count == 4 && trace == 1 && timers == 1 && hidden == 1 && tick == 1);
    }
}

void LoadSaveRoundTrip()
{
    // nothing in the ini, "auto" (the shipped template) and unknown words: both unset
    for (const char* text : { "", "[FramePacing]\nTrace=auto\nGpuTimers=auto\nHiddenPresentDoNotWait=auto\nTickProbe=auto\n",
                               "[FramePacing]\nTrace=maybe\nGpuTimers=perhaps\nHiddenPresentDoNotWait=perhaps\nTickProbe=perhaps\n" })
    {
        auto cfg = LoadFresh(text);
        CHECK(!cfg.TraceKey().has_value());
        CHECK(!cfg.GpuTimersKey().has_value());
        CHECK(cfg.hiddenPresentDoNotWait.value_or_default() == false);
        CHECK(!cfg.TickProbeOn());
    }
    {
        auto cfg = LoadFresh("[FramePacing]\nTrace=true\nGpuTimers=true\nHiddenPresentDoNotWait=true\nTickProbe=true\n");
        CHECK(cfg.TraceKey().has_value() && *cfg.TraceKey() == true);
        CHECK(cfg.GpuTimersKey().has_value() && *cfg.GpuTimersKey() == true);
        CHECK(cfg.hiddenPresentDoNotWait.value_or_default() == true);
        CHECK(cfg.TickProbeOn());
        CHECK(Saved(cfg, "Trace") == "true");
        CHECK(Saved(cfg, "GpuTimers") == "true");
        CHECK(Saved(cfg, "HiddenPresentDoNotWait") == "true");
        CHECK(Saved(cfg, "TickProbe") == "true");
    }
    {
        auto cfg = LoadFresh("[FramePacing]\nGpuTimers=false\n");
        CHECK(cfg.GpuTimersKey().has_value() && *cfg.GpuTimersKey() == false); // an explicit false is not "unset"
        CHECK(Saved(cfg, "GpuTimers") == "false");
    }
    {
        auto cfg = LoadFresh("[FramePacing]\nTrace=false\n");
        CHECK(cfg.TraceKey().has_value() && *cfg.TraceKey() == false); // an explicit false is not "unset"
        CHECK(Saved(cfg, "Trace") == "false");
    }
    {
        ext::FramePacingConfig cfg;
        CHECK(Saved(cfg, "Trace") == "auto");
        CHECK(Saved(cfg, "GpuTimers") == "auto");
        CHECK(Saved(cfg, "HiddenPresentDoNotWait") == "auto");
    }
}

void IniFragment(const std::string& repo)
{
    const std::string ini = ReadFile(repo + "/OptiScaler/ext/ini/FramePacing.ini");
    CHECK(!ini.empty());
    CHECK(ini.find("[FramePacing]") == std::string::npos); // the packager adds the section header
    CHECK(ini.find("\nTrace=auto") != std::string::npos);
    CHECK(ini.find("\nGpuTimers=auto") != std::string::npos);
    CHECK(ini.find("\nHiddenPresentDoNotWait=auto") != std::string::npos);
    // what the user needs to know, in words
    CHECK(ini.find("Default (auto) is false") != std::string::npos);  // the switch is off unless asked for
    CHECK(ini.find("Frame pacing CPU [") != std::string::npos);      // the line to look for
    CHECK(ini.find("Add Trace=true") != std::string::npos);          // how to turn it on at the normal log level
    CHECK(ini.find("No menu control") != std::string::npos);         // ini-only keys say so (ini_fragments rule)
    CHECK(ini.find("DXGI_PRESENT_DO_NOT_WAIT") != std::string::npos);
    CHECK(ini.find("Frame pacing trace: ") != std::string::npos);    // the line that says whether the trace is on
    CHECK(ini.find("Frame pacing GPU [") != std::string::npos);      // the lines the timers add when they sample
    CHECK(ini.find("Default (auto) is off unless the trace is on") != std::string::npos); // what auto means here
}

// The live-key mirrors (ext/AddKeysLoadedHook): a module that must not read its key per frame refreshes it at the end of every config load. The
// simulation-tick probe is the only user today; the rule belongs to the config, so it is tested here.
int g_keysLoadedCalls = 0;
void CountKeysLoaded() { ++g_keysLoadedCalls; }

void KeysLoadedHooks()
{
    ext::AddKeysLoadedHook(&CountKeysLoaded);
    const int before = g_keysLoadedCalls;
    ext::Cfg().PostLoad();
    CHECK(g_keysLoadedCalls == before + 1);
    ext::AddKeysLoadedHook(nullptr); // ignored, not a crash
    ext::Cfg().PostLoad();
    CHECK(g_keysLoadedCalls == before + 2);
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    DefaultsAndBinding();
    LoadSaveRoundTrip();
    IniFragment(repo);
    KeysLoadedHooks();
    if (g_failed != 0)
    {
        std::printf("frame_pacing_config: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("frame_pacing_config: all passed\n");
    return 0;
}
