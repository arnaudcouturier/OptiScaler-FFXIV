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

    // HiddenPresentDoNotWait: the auxiliary present on the DX11 bridge's hidden swapchain gets DXGI_PRESENT_DO_NOT_WAIT and a busy
    // chain skips that present instead of blocking. Off = exactly the old call. Default false.
    CustomOptional<bool> hiddenPresentDoNotWait { false };

    std::optional<bool> TraceKey() const { return trace.has_value() ? std::optional<bool>(trace.value()) : std::nullopt; }

    void Bind(config::Binder& b);
};
} // namespace ext
