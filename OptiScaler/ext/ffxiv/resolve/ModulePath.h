#pragma once
// The path of the running executable, read without the MAX_PATH cut. Pure: the Win32 call is a parameter, no <windows.h>.
//
// GetModuleFileNameW into a 260 wide-character buffer truncates a long install path and returns the buffer size. The pre-fix startup read that as
// "this is not ffxiv_dx11.exe" and switched the whole game module off for a player whose install path has 260 characters or more (the legacy selector
// matched the PE stamp and size and never read the path). ReadModulePath grows the buffer until the path fits, up to the 32768 characters a
// Windows path can have.

#include <cstdint>
#include <string>
#include <string_view>

namespace ext::ffxiv::resolve
{
inline constexpr std::uint32_t kInitialModulePathChars = 260;
inline constexpr std::uint32_t kMaxModulePathChars = 32768;

// `get(buffer, size)` has the semantics of GetModuleFileNameW(nullptr, buffer, size): the number of characters copied (without the terminator),
// 0 on failure, and exactly `size` when the buffer was too small (the string is then truncated). Returns the path, or an empty string when it
// cannot be read (a failure, or a path longer than kMaxModulePathChars).
template <class GetFn> std::wstring ReadModulePath(GetFn&& get)
{
    std::uint32_t size = kInitialModulePathChars;
    for (;;)
    {
        std::wstring path(size, L'\0');
        const std::uint32_t n = static_cast<std::uint32_t>(get(path.data(), size));
        if (n == 0)
            return std::wstring();
        if (n < size)
        {
            path.resize(n);
            return path;
        }
        if (size >= kMaxModulePathChars)
            return std::wstring();
        size = size > kMaxModulePathChars / 2 ? kMaxModulePathChars : size * 2;
    }
}

// An exe path is ASCII for every game this fork matches; anything else never equals ffxiv_dx11.exe and shows as '?'. For the log only.
inline std::string NarrowAscii(std::wstring_view w)
{
    std::string s;
    s.reserve(w.size());
    for (const wchar_t c : w)
        s.push_back(static_cast<unsigned>(c) < 0x80u ? static_cast<char>(c) : '?');
    return s;
}

// The last path component (the log shows the file name, not the install path).
inline std::string FileNameOf(const std::string& path)
{
    const std::size_t at = path.find_last_of("\\/");
    return at == std::string::npos ? path : path.substr(at + 1);
}
} // namespace ext::ffxiv::resolve
