// S10 D0: the vendored ReShade v6.8.0 add-on API headers (ext/reshade/third_party/reshade) build as the add-on
// translation units include them (C++20, /W4, conformant), expose the API version this fork registers with, and keep the
// event signatures the probe callbacks are written against. A header update must update this test and kApiVersion.
#include "ext/reshade/ReShadeRegister.h"
#include "ext/reshade/third_party/reshade/reshade.hpp"

#include <cstdio>
#include <type_traits>

namespace
{
using reshade::addon_event;
using reshade::addon_event_traits;
using namespace reshade::api;

static_assert(RESHADE_API_VERSION == 20, "the fork registers with add-on API 20 (ReShade 6.8.0)");
static_assert(ext::reshade::kApiVersion == RESHADE_API_VERSION);

// Handles are 64-bit opaque values (D3D11: the native COM pointer).
static_assert(sizeof(resource) == 8 && sizeof(resource_view) == 8);
static_assert(sizeof(effect_technique) == 8 && sizeof(effect_texture_variable) == 8);
static_assert(std::is_abstract_v<effect_runtime> && std::is_abstract_v<command_list> && std::is_abstract_v<device>);

// Events the probe subscribes to, with the signatures its callbacks use.
static_assert(std::is_same_v<addon_event_traits<addon_event::init_effect_runtime>::decl, void (*)(effect_runtime*)>);
static_assert(std::is_same_v<addon_event_traits<addon_event::destroy_effect_runtime>::decl, void (*)(effect_runtime*)>);
static_assert(std::is_same_v<addon_event_traits<addon_event::reshade_present>::decl, void (*)(effect_runtime*)>);
static_assert(std::is_same_v<addon_event_traits<addon_event::reshade_reloaded_effects>::decl, void (*)(effect_runtime*)>);
static_assert(std::is_same_v<addon_event_traits<addon_event::reshade_begin_effects>::decl,
                             void (*)(effect_runtime*, command_list*, resource_view, resource_view)>);
static_assert(std::is_same_v<addon_event_traits<addon_event::reshade_finish_effects>::decl,
                             void (*)(effect_runtime*, command_list*, resource_view, resource_view)>);
static_assert(std::is_same_v<addon_event_traits<addon_event::reshade_set_current_preset_path>::decl,
                             void (*)(effect_runtime*, const char*)>);
static_assert(std::is_same_v<addon_event_traits<addon_event::present>::decl,
                             void (*)(command_queue*, swapchain*, const rect*, const rect*, uint32_t, const rect*)>);
// No event can veto a render pass: begin/finish return void, the state events can only veto a state change.
static_assert(std::is_void_v<addon_event_traits<addon_event::reshade_begin_effects>::type>);
static_assert(std::is_void_v<addon_event_traits<addon_event::reshade_finish_effects>::type>);
static_assert(std::is_same_v<addon_event_traits<addon_event::reshade_set_effects_state>::type, bool>);

// Numeric ids that ReShade documents explicitly.
static_assert(static_cast<int>(addon_event::reshade_present) == 75);
static_assert(static_cast<int>(addon_event::reshade_set_current_preset_path) == 84);
static_assert(static_cast<int>(addon_event::reshade_set_effects_state) == 94);
static_assert(static_cast<int>(addon_event::finish_present) == 100);

// The three header-only helpers that call a cached function pointer WITHOUT a null check must never be used.
// (Documented here so a reviewer greps for them: log::message, get_config_value, get_reshade_base_path.)
} // namespace

int main()
{
    std::puts("PASS: ReShade v6.8.0 add-on API headers compile in the add-on configuration; API 20; event signatures as expected");
    return 0;
}
