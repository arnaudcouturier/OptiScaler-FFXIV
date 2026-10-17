#include "pch.h"

#include "ext/upscaler/DlssDlls.h"

#include <Config.h>
#include <State.h>
#include <Util.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <mutex>

namespace ext::upscaler
{
namespace
{
using PFN_GetSnippetVersion = uint32_t (*)(void);

std::wstring Lower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return s;
}

std::wstring Canon(const std::filesystem::path& p)
{
    std::error_code ec;
    std::filesystem::path out = std::filesystem::weakly_canonical(p, ec);
    if (ec)
        out = p;
    out.make_preferred();
    return Lower(out.wstring());
}

// The version of ONE specific copy, only if that exact file is already loaded in the process (no LoadLibrary, nothing
// new is mapped): GetModuleHandleW with the full path distinguishes two modules with the same base name.
DlssDllEntry Probe(const std::filesystem::path& file, const std::string& where)
{
    DlssDllEntry e;
    e.where = where;
    HMODULE module = ::GetModuleHandleW(file.c_str());
    if (module == nullptr)
        return e;

    e.loaded = true;
    if (auto fn = reinterpret_cast<PFN_GetSnippetVersion>(::GetProcAddress(module, "NVSDK_NGX_GetSnippetVersion")))
        e.snippet = fn();
    return e;
}

std::vector<DlssDllRow> Compute()
{
    std::vector<std::filesystem::path> folders;
    for (const auto& p : State::Instance().NVNGX_FeatureInfo_Paths) // NGX's search order, filled when NGX is initialised
        if (!p.empty())
            folders.emplace_back(p);
    if (Config::Instance()->MainDllPath.has_value())
        folders.emplace_back(Config::Instance()->MainDllPath.value());
    folders.emplace_back(Util::ExePath().parent_path());

    const std::wstring optiDir = Config::Instance()->MainDllPath.has_value()
                                     ? Canon(Config::Instance()->MainDllPath.value())
                                     : std::wstring();
    const std::wstring gameDir = Canon(Util::ExePath().parent_path());

    std::vector<DlssDllEntry> entries;
    std::vector<std::wstring> seen;
    for (const auto& folder : folders)
    {
        const std::wstring key = Canon(folder);
        if (std::find(seen.begin(), seen.end(), key) != seen.end())
            continue;
        seen.push_back(key);

        const auto file = folder / L"nvngx_dlss.dll";
        std::error_code ec;
        if (!std::filesystem::exists(file, ec))
            continue;

        const std::string where = key == optiDir   ? "OptiScaler folder"
                                  : key == gameDir ? "game folder"
                                                   : wstring_to_string(folder.filename().wstring()) + " folder";
        entries.push_back(Probe(file, where));
    }

    return FormatDlssDllRows(entries);
}
} // namespace

std::vector<DlssDllRow> DlssDllRows(const std::string& upscalerShortName)
{
    if (upscalerShortName.find("DLSS") == std::string::npos)
        return {};

    try
    {
        static std::mutex mutex;
        static std::vector<DlssDllRow> cache;
        static std::chrono::steady_clock::time_point stamp {};
        static bool logged = false;

        std::lock_guard lock(mutex);
        const auto now = std::chrono::steady_clock::now();
        if (stamp == std::chrono::steady_clock::time_point {} || now - stamp > std::chrono::seconds(2))
        {
            cache = Compute();
            stamp = now;
            if (!logged && !cache.empty())
            {
                logged = true;
                for (const auto& r : cache)
                    LOG_INFO("DLSS DLLs: {}: {}", r.label, r.value);
            }
        }
        return cache;
    }
    catch (...)
    {
        return {};
    }
}
} // namespace ext::upscaler
