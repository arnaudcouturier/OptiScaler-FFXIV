#pragma once
// NR runtime catalog: which NR model files exist next to the game, and which one a request picks. Pure logic (no file access,
// no logging), tested on the host by tests/ext/nr/nr_runtime_catalog.cpp. The file system side is NrRuntimeScan.h.
//
// A candidate is the legacy-location file (found by the old recursive search) or a file inside a folder named
// "NR.<digits>[.<digits>...]" (for example OptiScaler\NR.310.9\nvngx_dlssnr.dll). The folder naming and the idea of one
// immutable selection with a revision follow the community fork by Nickle (GPL-3.0); the code here is this fork's own.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ext::nr::runtime
{
using VersionParts = std::vector<uint32_t>;

inline constexpr std::wstring_view kLegacyId = L"legacy";

namespace detail
{
inline wchar_t Lower(wchar_t c) { return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c; }
inline bool EqualsNoCase(std::wstring_view a, std::wstring_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (Lower(a[i]) != Lower(b[i]))
            return false;
    return true;
}
inline bool LessNoCase(std::wstring_view a, std::wstring_view b)
{
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
        if (Lower(a[i]) != Lower(b[i]))
            return Lower(a[i]) < Lower(b[i]);
    return a.size() < b.size();
}
inline std::wstring_view Trim(std::wstring_view s)
{
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t'))
        s.remove_suffix(1);
    return s;
}
} // namespace detail

// True for any name that starts with "NR." (any case). The legacy search skips these folders whole.
inline bool HasRuntimePrefix(std::wstring_view name)
{
    return name.size() >= 3 && detail::EqualsNoCase(name.substr(0, 3), L"NR.");
}

// Digits separated by single dots, nothing else; each number at most 9 digits so it cannot overflow.
inline std::optional<VersionParts> ParseDottedNumbers(std::wstring_view text)
{
    if (text.empty())
        return std::nullopt;
    VersionParts parts;
    uint64_t value = 0;
    size_t digits = 0;
    for (const wchar_t c : text)
    {
        if (c >= L'0' && c <= L'9')
        {
            if (++digits > 9)
                return std::nullopt;
            value = value * 10 + static_cast<uint64_t>(c - L'0');
        }
        else if (c == L'.')
        {
            if (digits == 0)
                return std::nullopt;
            parts.push_back(static_cast<uint32_t>(value));
            value = 0;
            digits = 0;
        }
        else
            return std::nullopt;
    }
    if (digits == 0)
        return std::nullopt;
    parts.push_back(static_cast<uint32_t>(value));
    return parts;
}

// The version a folder name carries, or nothing when the name is not a valid runtime folder name.
inline std::optional<VersionParts> ParseFolderVersion(std::wstring_view name)
{
    if (!HasRuntimePrefix(name))
        return std::nullopt;
    return ParseDottedNumbers(name.substr(3));
}

inline bool IsRuntimeFolderName(std::wstring_view name) { return ParseFolderVersion(name).has_value(); }

// Component by component, a missing component counts as zero: 310.9 equals 310.9.0.
inline int CompareVersions(const VersionParts& a, const VersionParts& b)
{
    const size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
    {
        const uint32_t x = i < a.size() ? a[i] : 0;
        const uint32_t y = i < b.size() ? b[i] : 0;
        if (x != y)
            return x < y ? -1 : 1;
    }
    return 0;
}

inline std::wstring VersionText(const VersionParts& v)
{
    std::wstring text;
    for (size_t i = 0; i < v.size(); ++i)
    {
        if (i != 0)
            text += L'.';
        text += std::to_wstring(v[i]);
    }
    return text;
}

struct Entry
{
    std::wstring id;            // "legacy" or the folder name as it is on disk
    std::filesystem::path path; // the model file
    bool legacy = false;
    VersionParts fileVersion;   // from the file's version resource; empty when it has none
    VersionParts folderVersion; // from the folder name; empty for the legacy file
    uint64_t sizeBytes = 0;
    uint64_t stamp = 0;         // last write time of the file; with the size it tells a replaced file from the one that was hashed
    std::string sha256;         // 64 lowercase hex characters, empty until computed
};

// The version used to order candidates: what the file says about itself, else what its folder says.
inline const VersionParts& OrderVersion(const Entry& e)
{
    return e.fileVersion.empty() ? e.folderVersion : e.fileVersion;
}

// True when `a` is the better Automatic choice than `b`: the newer version; on a tie the legacy file; on a further tie the
// folder name that sorts first (so NR.310.9 and NR.310.9.0 always give the same answer).
inline bool Preferred(const Entry& a, const Entry& b)
{
    const int c = CompareVersions(OrderVersion(a), OrderVersion(b));
    if (c != 0)
        return c > 0;
    if (a.legacy != b.legacy)
        return a.legacy;
    return detail::LessNoCase(a.id, b.id);
}

inline std::optional<size_t> ChooseAutomatic(const std::vector<Entry>& entries)
{
    std::optional<size_t> best;
    for (size_t i = 0; i < entries.size(); ++i)
        if (!best || Preferred(entries[i], entries[*best]))
            best = i;
    return best;
}

enum class RequestKind
{
    Automatic, // empty, or "auto"
    Legacy,    // "legacy"
    Folder,    // a valid NR.<version> folder name
    Invalid,   // anything else (never turned into a path)
};

struct Request
{
    RequestKind kind = RequestKind::Automatic;
    std::wstring folder; // Folder only
};

inline Request ParseRequest(std::wstring_view text)
{
    text = detail::Trim(text);
    Request r;
    if (text.empty() || detail::EqualsNoCase(text, L"auto"))
        return r;
    if (detail::EqualsNoCase(text, kLegacyId))
    {
        r.kind = RequestKind::Legacy;
        return r;
    }
    if (IsRuntimeFolderName(text))
    {
        r.kind = RequestKind::Folder;
        r.folder = std::wstring(text);
        return r;
    }
    r.kind = RequestKind::Invalid;
    return r;
}

enum class Status
{
    Ok,
    NothingInstalled, // Automatic, and no candidate exists
    ExplicitMissing,  // the requested folder (or the legacy file) is not installed; nothing else is loaded in its place
    InvalidName,      // the request is not a valid folder name
};

struct Resolution
{
    Status status = Status::NothingInstalled;
    std::optional<size_t> index; // into the entries, when status is Ok
    Request request;
};

inline Resolution Resolve(const std::vector<Entry>& entries, std::wstring_view requested)
{
    Resolution r;
    r.request = ParseRequest(requested);
    switch (r.request.kind)
    {
    case RequestKind::Automatic:
        r.index = ChooseAutomatic(entries);
        r.status = r.index ? Status::Ok : Status::NothingInstalled;
        return r;
    case RequestKind::Legacy:
        for (size_t i = 0; i < entries.size(); ++i)
            if (entries[i].legacy)
                r.index = i;
        break;
    case RequestKind::Folder:
        for (size_t i = 0; i < entries.size(); ++i)
            if (!entries[i].legacy && detail::EqualsNoCase(entries[i].id, r.request.folder))
                r.index = i;
        break;
    case RequestKind::Invalid:
        r.status = Status::InvalidName;
        return r;
    }
    // An explicit choice that is not installed never quietly runs a different model.
    r.status = r.index ? Status::Ok : Status::ExplicitMissing;
    return r;
}

// "263.4 MB" (1024 based, one decimal), "812 KB", "37 B".
inline std::string SizeText(uint64_t bytes)
{
    char buf[48];
    if (bytes >= (1ull << 20))
        std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else if (bytes >= (1ull << 10))
        std::snprintf(buf, sizeof buf, "%.0f KB", static_cast<double>(bytes) / 1024.0);
    else
        std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(bytes));
    return buf;
}

// The first 12 hex characters of the SHA-256, or a note while the hash is still being computed.
inline std::string ShortSha(const std::string& hex)
{
    if (hex.size() < 12)
        return "hashing...";
    return hex.substr(0, 12);
}
} // namespace ext::nr::runtime
