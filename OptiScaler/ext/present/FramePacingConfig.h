#pragma once

#include <Config.h>

#include <optional>

namespace ext
{
namespace config
{
class Binder;
}

// [FramePacing] (ext/ini/FramePacing.ini): the frame-pacing CPU trace and the hidden-present switch (ext/present/FramePacingTrace.cpp).
struct FramePacingConfig
{
    // Trace: auto (unset) = on only with file logging at Debug or Trace, true = on at any log level, false = off. Never on outside FFXIV.
    CustomOptional<bool, NoDefault> trace {};

    // GpuTimers: auto (unset) = on only together with the trace above, true = on at any log level, false = off.
    // Never on outside FFXIV. Default auto (= off at the normal log level, like the trace).
    CustomOptional<bool, NoDefault> gpuTimers {};

    // HiddenPresentDoNotWait: the auxiliary present on the DX11 bridge's hidden swapchain gets DXGI_PRESENT_DO_NOT_WAIT and a busy
    // chain skips that present instead of blocking. Off = exactly the old call. Default false.
    CustomOptional<bool> hiddenPresentDoNotWait { false };

    // FgRecorder: the FG event recorder (ext/fgrec): one JSONL file per session in fg-recordings\ next to the DLL.
    // Off = no thread, no allocation, no file. Default false.
    CustomOptional<bool> fgRecorder { false };

    // FgRecorderMarkKey: virtual-key code of the recorder's one-key mark, unbound by default. A press writes a `mark` record into
    // the session file and one log line ("user mark N"), so the user can say "now" while playing. Set from the Keybinds section.
    // Unbound (-1) is today's behaviour: no key, nothing happens.
    CustomOptional<int> fgRecorderMarkKey { UnboundKey }; // FgRecorderMarkKey (hex when positive, like the other key codes)

    // TickProbe: lease the game's simulation tick (the Framework vtable slot, ext/latency) and record its timing to OptiScaler.log every
    // 10 seconds. With it off nothing is installed and no line is written. Default false.
    CustomOptional<bool> tickProbe { false };

    std::optional<bool> TraceKey() const { return trace.has_value() ? std::optional<bool>(trace.value()) : std::nullopt; }
    std::optional<bool> GpuTimersKey() const
    {
        return gpuTimers.has_value() ? std::optional<bool>(gpuTimers.value()) : std::nullopt;
    }
    bool TickProbeOn() const { return tickProbe.value_or_default(); }

    void Bind(config::Binder& b);
};
} // namespace ext
