#pragma once
// [Window] config key (docs/FEATURES.md, "Window mode"; docs/SEAMS.md section 3.4). Use: ext::Cfg().window.GetMode(), or
// ext::window::CurrentMode() for the cached copy on the Present path.
//
// Header-only on purpose: several test recipes link ExtConfig.cpp without the feature sources. Bind() is defined in
// ExtConfig.cpp next to the other small config structs.

#include <Config.h>

#include "ext/window/WindowMode.h"
#include "ext/window/WindowSeams.h"

#include <string>

namespace ext::config
{
class Binder;
}

namespace ext
{
struct WindowConfig
{
    // off | probe | on. A string key: anything else reads as off.
    CustomOptional<std::string> mode { "off" }; // Mode

    window::Mode GetMode() const { return window::ParseMode(mode.value_or_default()); }

    void Bind(config::Binder& b);
    void PostLoad() { window::Publish(GetMode()); } // end of config::Load: publishes the cached mode
};
} // namespace ext
