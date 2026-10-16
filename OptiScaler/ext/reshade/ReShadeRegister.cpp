#include "pch.h"

#include "ext/reshade/ReShadeRegister.h"

// reshade_overlay.hpp redirects every ImGui:: call to ReShade's function table and demands exactly the ImGui version it
// was written against; OptiScaler's own ImGui must never be visible to a translation unit that includes ReShade's API.
#ifdef IMGUI_VERSION_NUM
#error "ReShadeRegister.cpp must not see imgui.h: keep ReShade's headers and ImGui in separate translation units"
#endif

#include "ext/reshade/third_party/reshade/reshade.hpp"

static_assert(ext::reshade::kApiVersion == RESHADE_API_VERSION,
              "ReShade headers were updated: re-check the registration code and kApiVersion");

namespace ext::reshade
{
const char* Describe(RegisterStatus status)
{
    switch (status)
    {
    case RegisterStatus::Ok:
        return "registered";
    case RegisterStatus::NoSelf:
        return "own module handle unknown";
    case RegisterStatus::NoModule:
        return "ReShade is not loaded";
    case RegisterStatus::ExportsMissing:
        return "this ReShade does not export the add-on API (build without add-on support?)";
    case RegisterStatus::Refused:
        return "ReShade refused the registration (add-on API 20 unsupported, or add-ons disabled)";
    case RegisterStatus::Exception:
        return "exception during registration";
    }
    return "unknown";
}

RegisterResult RegisterAddon(HMODULE self, HMODULE reshadeModule)
{
    RegisterResult result;

    try
    {
        if (self == nullptr)
        {
            result.status = RegisterStatus::NoSelf;
            return result;
        }

        HMODULE module = reshadeModule;
        if (module == nullptr)
            module = ::reshade::internal::get_reshade_module_handle(); // searches the process, caches only a hit

        result.reshade = module;
        if (module == nullptr)
        {
            result.status = RegisterStatus::NoModule;
            return result;
        }

        // Every export the header-only helpers resolve later; checked here first (see the header comment).
        static constexpr const char* kRequired[] = { "ReShadeRegisterAddon", "ReShadeUnregisterAddon",
                                                     "ReShadeRegisterEvent", "ReShadeUnregisterEvent" };
        for (const char* name : kRequired)
        {
            if (GetProcAddress(module, name) == nullptr)
            {
                result.status = RegisterStatus::ExportsMissing;
                result.missingExport = name;
                return result;
            }
        }

        // First ReShade API call of the process, with the explicit module.
        result.status = ::reshade::register_addon(self, module) ? RegisterStatus::Ok : RegisterStatus::Refused;
    }
    catch (...)
    {
        result.status = RegisterStatus::Exception;
    }

    return result;
}
} // namespace ext::reshade
