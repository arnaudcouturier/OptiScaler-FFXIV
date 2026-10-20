#pragma once

#include <Config.h>

namespace ext
{
namespace config
{
class Binder;
}

// [NgxRelease] (ext/ini/NgxRelease.ini): the quality change without the stall.
struct NgxReleaseConfig
{
    // SkipReleaseSleep: when the DX11 upscaler is released (every quality change), upstream sleeps 500 ms before the
    // reset. With this on, the sleep is skipped when the release has proven the work is finished: the GPU queues the
    // feature could still be running on are drained and no Evaluate of the handle is in flight. Anything else keeps the
    // sleep. Off = exactly the old flow (sleep, no handle closing, no wait for in-flight evaluates). Default true.
    CustomOptional<bool> skipReleaseSleep { true };

    void Bind(config::Binder& b);
};
} // namespace ext
