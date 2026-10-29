// The keys of the per-axis model resolution and of the depth-aware enlargement against the real NrConfig::Bind: the binder only records
// (section, key, field), as in dynamic_mfg_config, and the CustomOptional is the production class (sliced from OptiScaler\Config.h).
// What this pins: each key is bound exactly once, to its OWN member (the vertical key is not bound to the horizontal member and the other way
// round, the depth-aware key is not left unbound), and the defaults are today's behaviour (0 = follow WorkingScale, depth-aware off).
#include "ext/core/ExtConfig.h" // the recording Binder of mfg_cfg_stubs
#include "ext/nr/NrConfig.h"

#include <cstdio>
#include <string>

namespace DlssNrNative
{
void SetCapture(bool) {} // NrConfig::PostLoad calls it; nothing in this test loads a config
}

namespace
{
int failures = 0;
void Check(bool ok, const char* what, int line)
{
    if (!ok)
    {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)
} // namespace

int main()
{
    ext::NrConfig cfg;
    // today's behaviour
    CHECK(cfg.workingScaleX.value_or_default() == 0.0f && cfg.workingScaleY.value_or_default() == 0.0f);
    CHECK(!cfg.depthAwareResolve.value_or_default());
    CHECK(cfg.workingScale.value_or_default() == 1.0f);
    CHECK(!cfg.workingScaleX.has_value() && !cfg.workingScaleY.has_value() && !cfg.depthAwareResolve.has_value());

    ext::config::Binder binder;
    cfg.Bind(binder);
    const auto field = [&](const char* section, const char* key) -> void*
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
    CHECK(field("DlssNr", "WorkingScaleX") == &cfg.workingScaleX);
    CHECK(field("DlssNr", "WorkingScaleY") == &cfg.workingScaleY);
    CHECK(field("DlssNr", "DepthAwareResolve") == &cfg.depthAwareResolve);
    CHECK(field("DlssNr", "WorkingScale") == &cfg.workingScale);
    // four different members, four different bindings
    CHECK(field("DlssNr", "WorkingScaleX") != field("DlssNr", "WorkingScaleY"));
    CHECK(field("DlssNr", "WorkingScaleX") != field("DlssNr", "WorkingScale") && field("DlssNr", "WorkingScaleY") != field("DlssNr", "WorkingScale"));
    CHECK(field("DlssNr", "DepthAwareResolve") != nullptr && field("DlssNr", "WorkingScaleX") != nullptr && field("DlssNr", "WorkingScaleY") != nullptr);
    // the key is bound under no other section, and no member is bound twice
    for (const auto& e : binder.Table())
    {
        const std::string key = e.key;
        if (key == "WorkingScaleX" || key == "WorkingScaleY" || key == "DepthAwareResolve")
            CHECK(std::string(e.section) == "DlssNr");
    }
    int bound = 0;
    for (const auto& e : binder.Table())
        if (e.field == &cfg.workingScaleX || e.field == &cfg.workingScaleY || e.field == &cfg.depthAwareResolve)
            ++bound;
    CHECK(bound == 3);

    // a value written to a member is what the member reads back (the real CustomOptional: the saved value, and the default when unset)
    cfg.workingScaleX = 0.5f;
    cfg.workingScaleY = 0.75f;
    cfg.depthAwareResolve = true;
    CHECK(cfg.workingScaleX.value_or_default() == 0.5f && cfg.workingScaleY.value_or_default() == 0.75f && cfg.depthAwareResolve.value_or_default());
    CHECK(cfg.workingScaleX.has_value() && cfg.workingScaleY.has_value() && cfg.depthAwareResolve.has_value());

    if (failures != 0)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("PASS: WorkingScaleX, WorkingScaleY and DepthAwareResolve are bound once each to their own members, with today's defaults");
    return 0;
}
