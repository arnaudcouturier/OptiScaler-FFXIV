#pragma once

#include <Config.h>

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

    void Bind(config::Binder& b);
};
} // namespace ext
