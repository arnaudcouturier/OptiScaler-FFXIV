#pragma once

#include <Config.h>

#include <string>

namespace ext
{
namespace config
{
class Binder;
}

// [HudLess] ReShadeProbe (S10 package D0): opt-in diagnostics for the ReShade built-in add-on. Default false.
// false = OptiScaler registers itself with ReShade (one attempt, one log line) and subscribes to nothing.
// true  = it also subscribes to the effect-runtime events, writes <game>\OptiScaler\hudless-diag\reshade-<ts>\
//         reshade-probe.txt and shows the "ReShade built-in add-on (diagnostics)" menu card.
// Package A's [HudLess] Diagnostics key does not exist in this branch; when the branches are stacked the single
// function ext::reshade::ProbeEnabled() (ReShadeAddon.cpp) is where "ReShadeProbe || Diagnostics" is decided.
struct ReShadeConfig
{
    CustomOptional<bool> probe { false };

    // [HudLess] ReShadePresets (S10 package E): auto | on | off, see ext/reshade/ReShadeBoundary.h (PresetsMode). A string key; the
    // default text is "auto", which is also what the ini value "auto" (= unset) reads as. Anything unknown counts as auto.
    CustomOptional<std::string> presets { "auto" };

    void Bind(config::Binder& b);
};
} // namespace ext
