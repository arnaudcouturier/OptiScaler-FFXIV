#pragma once
// Registration of OptiScaler's own module as a ReShade add-on (docs/HUDLESS.md section 16).
//
// Free of ReShade headers so tests and the menu can use it; the implementation (ReShadeRegister.cpp) is the only place
// that calls the header-only registration of ReShade's public API. Everything is checked BEFORE the first ReShade API
// call: the header-only helpers cache the module they resolved in function-local statics and call the cached function
// pointers without a null check (log::message, get_config_value, get_reshade_base_path), so a module that lacks the
// exports must never reach them.

#include <Windows.h>

#include <cstdint>

namespace ext::reshade
{
// ReShade add-on API version this build registers with (static_assert'd against RESHADE_API_VERSION in the .cpp).
inline constexpr uint32_t kApiVersion = 20;

enum class RegisterStatus : uint8_t
{
    Ok,
    NoSelf,         // our own module handle is unknown
    NoModule,       // no ReShade module given and none found in the process
    ExportsMissing, // the module lacks one of the add-on exports (ReShade build without add-on support, or not ReShade)
    Refused,        // ReShadeRegisterAddon returned false (API newer than this ReShade, add-ons disabled, ...)
    Exception,
};

// Short reason text for the single log line and the report ("registered" is not a reason, see Ok).
const char* Describe(RegisterStatus status);

struct RegisterResult
{
    RegisterStatus status = RegisterStatus::NoModule;
    HMODULE reshade = nullptr;           // the ReShade module that was inspected / registered with
    const char* missingExport = nullptr; // first missing export when status == ExportsMissing
};

// `self` = OptiScaler's own module (the proxy DLL). `reshadeModule` may be null: the process is then searched for a
// module that exports ReShadeRegisterAddon + ReShadeUnregisterAddon. Never throws.
RegisterResult RegisterAddon(HMODULE self, HMODULE reshadeModule);
} // namespace ext::reshade
