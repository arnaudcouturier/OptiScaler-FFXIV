#include "pch.h"

#include "ext/Seams.h"
#include "ext/core/ExtConfig.h"

#include <algorithm>
#include <format>
#include <sstream>

namespace ext::config
{
namespace detail
{
std::optional<std::string> ReadRaw(CSimpleIniA& ini, const char* section, const char* key, bool lowercase)
{
    std::string value = ini.GetValue(section, key, "auto");

    std::string lower = value;
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (lower == "auto")
        return std::nullopt;

    return lowercase ? lower : value;
}

std::optional<bool> ParseBool(const std::optional<std::string>& raw)
{
    if (raw == "true")
        return true;
    if (raw == "false")
        return false;

    return std::nullopt;
}

std::optional<int64_t> ParseInt(const std::optional<std::string>& raw)
{
    if (!raw)
        return std::nullopt;

    const std::string& s = *raw;

    try
    {
        size_t idx = 0;
        int64_t result;

        // same hex prefix detection as upstream readInt
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            result = std::stoll(s, &idx, 16);
        else
            result = std::stoll(s, &idx, 10);

        // whole string has to be consumed
        return idx == s.size() ? std::optional<int64_t>(result) : std::nullopt;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

std::optional<float> ParseFloat(const std::optional<std::string>& raw)
{
    if (!raw)
        return std::nullopt;

    std::istringstream iss(*raw);
    float result;

    if ((iss >> result) && iss.eof())
        return result;

    return std::nullopt;
}

std::string FormatBool(std::optional<bool> v)
{
    if (!v)
        return "auto";

    return *v ? "true" : "false";
}

std::string FormatInt(std::optional<int64_t> v, bool hex)
{
    if (!v)
        return "auto";

    // hex only for positive values: key codes are saved as 0x.., "unbound" (-1) stays decimal
    if (hex && *v > 0)
        return std::format("{:#x}", *v);

    return std::to_string(*v);
}

std::string FormatFloat(std::optional<float> v)
{
    if (!v)
        return "auto";

    return std::to_string(*v);
}

std::string WideToUtf8(const std::wstring& w) { return wstring_to_string(w); }

std::wstring Utf8ToWide(const std::string& s) { return string_to_wstring(s); }
} // namespace detail

namespace
{
const std::vector<Binding>& Table()
{
    static const std::vector<Binding> table = []
    {
        Binder b;
        Cfg().Bind(b);
        return b.Table();
    }();

    return table;
}
} // namespace

void Load(CSimpleIniA& ini)
{
    for (const auto& b : Table())
        b.load(b, ini);

    Cfg().PostLoad();
}

void Save(CSimpleIniA& ini)
{
    for (const auto& b : Table())
        b.save(b, ini);

    Cfg().hdr.Retire(ini);
}

void SaveXeFG(CSimpleIniA& ini)
{
    for (const auto& b : Table())
    {
        if (strcmp(b.section, "XeFG") == 0)
            b.save(b, ini);
    }
}
} // namespace ext::config

namespace ext
{
void FfxivConfig::Bind(config::Binder& b)
{
    b.Add("Companion", "HudReplacement", companionHudReplacement);
    b.Add("Companion", "HudInterpolation", companionHudInterpolation);
}

void UpscalerConfig::Bind(config::Binder& b) { b.Add("QualityOverrides", "ForcePerfQuality", forcePerfQuality); }

void ExtConfig::Bind(config::Binder& b)
{
    fg.Bind(b);
    ffxiv.Bind(b);
    nr.Bind(b);
    hdr.Bind(b);
    upscaler.Bind(b);
}

void ExtConfig::PostLoad()
{
    fg.PostLoad();
    nr.PostLoad();
    hdr.PostLoad();
}

ExtConfig& Cfg()
{
    static ExtConfig cfg;
    return cfg;
}
} // namespace ext
