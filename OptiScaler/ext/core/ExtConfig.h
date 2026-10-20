#pragma once
// Fork config keys (docs/SEAMS.md section 3.4).
//
// Each feature owns a plain struct of upstream's CustomOptional<T, Default> fields in its own directory and a
// `Bind(ext::config::Binder&)` member that lists (section, key, field). Ini section/key names are identical to the
// reference build, including fork keys inside upstream sections ([XeFG], [DLSSG], [HDR], ...).
//
//   // ext/nr/NrConfig.h
//   struct NrConfig
//   {
//       CustomOptional<bool> enabled { false };              // default false
//       CustomOptional<int32_t, NoDefault> passes;            // no default, "auto" until set
//       CustomOptional<float> scale { 1.0f };
//       void Bind(ext::config::Binder& b)
//       {
//           b.Add("DlssNr", "Enabled", enabled);
//           b.Add("DlssNr", "Passes", passes);
//           b.Add("DlssNr", "Scale", scale);
//           b.AddEnum<Mode>("DlssNr", "Mode", mode, Mode::Count);  // unsigned enums with a Count entry
//       }
//   };
//   // ExtConfig.h: add `NrConfig nr;` to ExtConfig, ExtConfig.cpp: call `nr.Bind(b)` in ExtConfig::Bind.
//   // Use: ext::Cfg().nr.enabled.value_or_default()
//
// Load/Save use the CSimpleIniA instance of Config.cpp, with the same conventions as upstream: the value "auto" (any
// case) means unset, bools are true/false, ints accept 0x hex, a CustomOptional with a default is saved only when it
// differs from the default.

#include <Config.h>

#include "ext/dx11bridge/NgxReleaseConfig.h"
#include "ext/fg/FgConfig.h"
#include "ext/ffxiv/FfxivConfig.h"
#include "ext/ffxiv/ui/UiConfig.h"
#include "ext/hdr/HdrConfig.h"
#include "ext/hudless/HudlessConfig.h"
#include "ext/menu/MenuStyleConfig.h"
#include "ext/nr/NrConfig.h"
#include "ext/reshade/ReShadeConfig.h"
#include "ext/upscaler/UpscalerConfig.h"
#include "ext/window/WindowConfig.h"

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace ext::config
{
// Operations on one bound field; built by Binder::Add, never by hand.
struct Binding
{
    const char* section;
    const char* key;
    void* field;
    void (*load)(const Binding&, CSimpleIniA&);
    void (*save)(const Binding&, CSimpleIniA&);
    uint32_t enumCount; // exclusive upper bound for enum fields, 0 = no check
    bool hex;           // saved as 0x... (ints and enums)
};

namespace detail
{
std::optional<std::string> ReadRaw(CSimpleIniA& ini, const char* section, const char* key, bool lowercase);
std::optional<bool> ParseBool(const std::optional<std::string>& raw);
std::optional<int64_t> ParseInt(const std::optional<std::string>& raw);
std::optional<float> ParseFloat(const std::optional<std::string>& raw);
std::string FormatBool(std::optional<bool> v);
std::string FormatInt(std::optional<int64_t> v, bool hex);
std::string FormatFloat(std::optional<float> v);
std::string WideToUtf8(const std::wstring& w);
std::wstring Utf8ToWide(const std::string& s);

template <typename T, HasDefaultValue B> void Load(const Binding& b, CSimpleIniA& ini)
{
    auto& f = *static_cast<CustomOptional<T, B>*>(b.field);

    if constexpr (std::is_same_v<T, bool>)
        f.set_from_config(ParseBool(ReadRaw(ini, b.section, b.key, true)));
    else if constexpr (std::is_same_v<T, float>)
        f.set_from_config(ParseFloat(ReadRaw(ini, b.section, b.key, false)));
    else if constexpr (std::is_same_v<T, std::string>)
        f.set_from_config(ReadRaw(ini, b.section, b.key, false));
    else if constexpr (std::is_same_v<T, std::wstring>)
    {
        auto s = ReadRaw(ini, b.section, b.key, false);
        f.set_from_config(s ? std::optional<std::wstring>(Utf8ToWide(*s)) : std::nullopt);
    }
    else if constexpr (std::is_enum_v<T>)
    {
        auto v = ParseInt(ReadRaw(ini, b.section, b.key, false));
        if (v && *v >= 0 && (b.enumCount == 0 || static_cast<uint64_t>(*v) < b.enumCount))
            f.set_from_config(static_cast<T>(*v));
        else
            f.set_from_config(std::nullopt);
    }
    else
    {
        static_assert(std::is_integral_v<T>, "unsupported config field type");
        auto v = ParseInt(ReadRaw(ini, b.section, b.key, false));
        f.set_from_config(v ? std::optional<T>(static_cast<T>(*v)) : std::nullopt);
    }
}

template <typename T, HasDefaultValue B> void Save(const Binding& b, CSimpleIniA& ini)
{
    auto& f = *static_cast<CustomOptional<T, B>*>(b.field);
    std::string out;

    if constexpr (std::is_same_v<T, bool>)
        out = FormatBool(f.value_for_config());
    else if constexpr (std::is_same_v<T, float>)
        out = FormatFloat(f.value_for_config());
    else if constexpr (std::is_same_v<T, std::string>)
        out = f.value_for_config_or("auto");
    else if constexpr (std::is_same_v<T, std::wstring>)
        out = WideToUtf8(f.value_for_config_or(L"auto"));
    else if constexpr (std::is_enum_v<T>)
    {
        auto v = f.value_for_config();
        out = FormatInt(v ? std::optional<int64_t>(static_cast<int64_t>(*v)) : std::nullopt, b.hex);
    }
    else
    {
        auto v = f.value_for_config();
        out = FormatInt(v ? std::optional<int64_t>(static_cast<int64_t>(*v)) : std::nullopt, b.hex);
    }

    ini.SetValue(b.section, b.key, out.c_str());
}
} // namespace detail

// Collects the (section, key, field) table once; ExtConfig::Bind fills it.
class Binder
{
  public:
    // bool, int types, float, std::string, std::wstring
    template <typename T, HasDefaultValue B> void Add(const char* section, const char* key, CustomOptional<T, B>& field, bool hex = false)
    {
        static_assert(!std::is_enum_v<T>, "use AddEnum for enums");
        _table.push_back({ section, key, &field, &detail::Load<T, B>, &detail::Save<T, B>, 0, hex });
    }

    // Enum stored as its integer value; values >= count are ignored on load (like upstream readEnum)
    template <typename E, HasDefaultValue B>
    void AddEnum(const char* section, const char* key, CustomOptional<E, B>& field, E count, bool hex = false)
    {
        static_assert(std::is_enum_v<E>, "enum required");
        _table.push_back({ section, key, &field, &detail::Load<E, B>, &detail::Save<E, B>,
                           static_cast<uint32_t>(count), hex });
    }

    const std::vector<Binding>& Table() const { return _table; }

  private:
    std::vector<Binding> _table;
};

// Called by the seams in Config.cpp (declared in ext/Seams.h)
void Load(CSimpleIniA& ini);
void Save(CSimpleIniA& ini);
void SaveXeFG(CSimpleIniA& ini); // only the bindings whose section is "XeFG"
} // namespace ext::config

namespace ext
{
// Aggregates the per-feature config structs. Ports add `<Feature>Config <area>;` members here.
struct ExtConfig
{
    FgConfig fg; // ext/fg
    FfxivConfig ffxiv; // ext/ffxiv
    ext::ffxiv::ui::UiConfig ui; // ext/ffxiv/ui (S10 A): [HudLess] Diagnostics
    NrConfig nr; // ext/nr
    HdrConfig hdr; // ext/hdr
    HudlessConfig hudless; // ext/hudless
    UpscalerConfig upscaler; // ext/upscaler
    ReShadeConfig reshade; // ext/reshade
    WindowConfig window; // ext/window
    MenuStyleConfig menuStyle; // ext/menu: [Menu] Style
    NgxReleaseConfig ngxRelease; // ext/dx11bridge: [NgxRelease] SkipReleaseSleep

    void Bind(config::Binder& b);
    void PostLoad(); // validation that ran right after reading the keys (end of config::Load)
};

ExtConfig& Cfg();
} // namespace ext
