#pragma once
// A path as UTF-8 text for log lines.
//
// std::filesystem::path::string() converts through the ANSI code page and throws std::filesystem::filesystem_error for a name the
// code page cannot represent (for example a game folder with Cyrillic or Japanese characters on a Windows with another locale). A log
// call evaluates its arguments in the caller, so such a throw leaves whatever function wrote the log line. This helper never throws:
// the conversion is UTF-8 and a failure gives a short placeholder.

#include <windows.h>

#include <filesystem>
#include <string>

namespace ext
{
inline std::string PathText(const std::filesystem::path& path) noexcept
{
    try
    {
        const std::wstring& wide = path.native();
        if (wide.empty())
            return {};
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (bytes <= 0)
            return "?";
        std::string text(static_cast<size_t>(bytes), '\0');
        if (WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), text.data(), bytes, nullptr, nullptr) <= 0)
            return "?";
        return text;
    }
    catch (...)
    {
        return "?"; // out of memory
    }
}
} // namespace ext
