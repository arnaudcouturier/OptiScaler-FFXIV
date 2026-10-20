#pragma once
// [Menu] Style config key (docs/FEATURES.md, "Menu style"; docs/SEAMS.md section 3.4). Use: ext::Cfg().menuStyle.GetStyle().
//
// Header-only on purpose: several test recipes link ExtConfig.cpp without the feature sources. Bind() is defined in
// ExtConfig.cpp next to the other small config structs.

#include <Config.h>

#include "ext/menu/MenuStyle.h"

#include <string>

namespace ext::config
{
class Binder;
}

namespace ext
{
struct MenuStyleConfig
{
    // classic | ember. A string key: anything else reads as classic.
    CustomOptional<std::string> look { "classic" }; // Style

    menustyle::Style GetStyle() const { return menustyle::Parse(look.value_or_default()); }

    void Bind(config::Binder& b);
};
} // namespace ext
