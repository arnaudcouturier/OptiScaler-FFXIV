#pragma once
// Stand-in for the product config when a test builds the draw diagnostic's runtime alone: the [FFXIV] developer keys at the shipped
// defaults, and the hook pointer the diagnostic's config load registers itself with.
#include <cstdint>
#include <string>

namespace ext
{
template <class T> struct OptionalValue
{
    T value {};
    T value_or_default() const { return value; }
};

struct FfxivConfig
{
    OptionalValue<std::string> drawDiagnostics { "off" };
    OptionalValue<int32_t> drawDiagnosticsFrames { 3 };
    OptionalValue<int32_t> drawDiagnosticsDelaySeconds { 0 };
    OptionalValue<std::string> drawDiagnosticsKey { "off" };
    OptionalValue<std::string> drawDiagnosticsDump { "off" };
    OptionalValue<int32_t> drawDiagnosticsDumpBudgetMiB { 256 };
    static inline void (*drawDiagnosticsOnLoaded)(bool) = nullptr; // end of every config load (the diagnostic's OnConfigLoaded)
};

struct ExtConfig
{
    FfxivConfig ffxiv;
};

inline ExtConfig& Cfg()
{
    static ExtConfig config;
    return config;
}
} // namespace ext
