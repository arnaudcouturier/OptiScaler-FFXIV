#pragma once
// NR runtime catalog, file system side: the legacy search (the old recursive search WITHOUT the NR.* folders), the scan of the
// NR.<version> folders, the file version resource and the SHA-256 of a model file. Header-only so the tests build it as it is;
// tests/ext/nr/nr_runtime_scan.cpp covers it with temporary folders.

#include "ext/nr/runtime/NrRuntimeCatalog.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <winver.h>

#include <atomic>
#include <functional>
#include <queue>
#include <utility>
#include <set>
#include <system_error>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "version.lib")

namespace ext::nr::runtime
{
namespace fs = std::filesystem;

// Files the legacy search must not return (for the production search: anything under <OptiScaler folder>\streamline, as in
// Util::FindFilePath).
using ExcludeFn = std::function<bool(const fs::path&)>;

namespace detail
{
// A folder that simply is not there (or is not a folder) is an ordinary answer; any other error means the listing may be incomplete.
inline bool BenignDirectoryError(const std::error_code& ec)
{
    return ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory;
}

// An NR.* folder is a candidate of the picker, never "the" legacy file.
inline bool SkipDirectory(const fs::path& dir) { return HasRuntimePrefix(dir.filename().wstring()); }

// How many folder levels below the start folder the legacy search enters. The search runs on the render thread (the first caller of
// Current()), and a folder tree can be unbounded or cyclic; a real install keeps the file within a few levels of the OptiScaler folder.
inline constexpr unsigned kLegacySearchMaxDepth = 8;

// A symbolic link or a junction (mount point). The search does not follow those: a link back to a parent folder would never end.
// Other reparse points (cloud placeholders and the like) are ordinary folders for this purpose. A folder whose attributes cannot be
// read is treated as a link (not entered), which is the safe answer.
inline bool IsLinkFolder(const fs::path& dir)
{
    WIN32_FIND_DATAW data {};
    HANDLE find = FindFirstFileW(dir.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
        return true;
    FindClose(find);
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
        return false;
    return data.dwReserved0 == IO_REPARSE_TAG_SYMLINK || data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT;
}
} // namespace detail

// Same order as Util::FindFilePath (a direct hit in startDir, then a breadth-first search under it, then the same search under the
// game root found through the Unreal Engine folder layout), except that no NR.* folder is entered, no folder is entered more than
// kLegacySearchMaxDepth (8) levels below the start folder, and no symbolic link or junction is entered. The answer is therefore the
// one FindFilePath gives only for a tree without NR.* folders, without links and no deeper than 8 levels. A shared runtime folder
// that is a junction INSIDE the game folder is no longer found (FindFilePath follows it); the start folder itself may be a junction.
// `incomplete` (when given) is set when a folder could not be listed or a listing broke off half way: "not found" is then only "not found
// in what could be read", and the caller must not treat it as a fact.
inline std::optional<fs::path> FindLegacy(const fs::path& startDir, const fs::path& fileName, const ExcludeFn& excluded = {},
                                          bool* incomplete = nullptr)
{
    std::error_code ec;
    const fs::path direct = startDir / fileName;
    if (fs::exists(direct, ec) && fs::is_regular_file(direct, ec))
        return direct;

    const auto bfs = [&](const fs::path& root) -> std::optional<fs::path>
    {
        std::queue<std::pair<fs::path, unsigned>> pending; // folder, levels below the start folder
        pending.push({ root, 0 });
        while (!pending.empty())
        {
            const fs::path dir = pending.front().first;
            const unsigned depth = pending.front().second;
            pending.pop();
            std::error_code iterEc;
            fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, iterEc);
            if (iterEc)
            {
                if (incomplete != nullptr && !detail::BenignDirectoryError(iterEc))
                    *incomplete = true;
                continue;
            }
            // increment(ec) rather than a range-for: operator++ throws on an error in the middle of a folder.
            for (; !iterEc && it != fs::directory_iterator(); it.increment(iterEc))
            {
                const auto& entry = *it;
                std::error_code typeEc;
                if (entry.is_directory(typeEc))
                {
                    if (depth < detail::kLegacySearchMaxDepth && !detail::SkipDirectory(entry.path()) && !detail::IsLinkFolder(entry.path()))
                        pending.push({ entry.path(), depth + 1 });
                }
                else if (entry.path().filename() == fileName)
                {
                    if (!excluded || !excluded(entry.path()))
                        return entry.path();
                }
            }
            if (iterEc && incomplete != nullptr)
                *incomplete = true; // the listing broke off half way
        }
        return std::nullopt;
    };

    if (auto found = bfs(startDir))
        return found;

    // Unreal Engine / WinGDK layout: the files may sit next to the Win64 folder, further up (as in FindFilePath).
    const fs::path parent = startDir.parent_path().parent_path();
    unsigned count = 0;
    for (const wchar_t* folder : { L"Win64", L"WinGDK", L"Win64MasterMasterSteamPGO" })
    {
        if (fs::exists(parent / folder, ec) && fs::is_directory(parent / folder, ec))
        {
            fs::path gameRoot;
            if (count < 2)
            {
                gameRoot = parent.parent_path();
                if (fs::exists(gameRoot / L"Binaries", ec) && fs::is_directory(gameRoot / L"Binaries", ec))
                    gameRoot = gameRoot.parent_path();
                else
                {
                    gameRoot = gameRoot.parent_path();
                    if (fs::exists(gameRoot / L"Binaries", ec) && fs::is_directory(gameRoot / L"Binaries", ec))
                        gameRoot = gameRoot.parent_path();
                }
            }
            else
                gameRoot = parent.parent_path();

            if (auto found = bfs(gameRoot))
                return found;
            break;
        }
        ++count;
    }
    return std::nullopt;
}

// True when `path` is `root` or lies below it (both made lexically normal first).
inline bool IsUnder(const fs::path& path, const fs::path& root)
{
    const auto p = path.lexically_normal();
    const auto r = root.lexically_normal();
    auto pi = p.begin();
    for (auto ri = r.begin(); ri != r.end(); ++ri, ++pi)
    {
        if (ri->empty())
            break; // a trailing separator
        if (pi == p.end() || !detail::EqualsNoCase(pi->wstring(), ri->wstring()))
            return false;
    }
    return true;
}

// Every NR.<version> folder directly inside one of the roots that holds the model file, one entry per folder name (the first root
// wins). Paths are made canonical so one file can never be two entries of the forwarder's module cache.
inline std::vector<Entry> ScanFolders(const std::vector<fs::path>& roots, const fs::path& fileName, bool* incomplete = nullptr)
{
    std::vector<Entry> found;
    for (const auto& root : roots)
    {
        std::error_code ec;
        fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
        if (ec)
        {
            if (incomplete != nullptr && !detail::BenignDirectoryError(ec))
                *incomplete = true;
            continue;
        }
        for (; !ec && it != fs::directory_iterator(); it.increment(ec))
        {
            const auto& dirEntry = *it;
            std::error_code typeEc;
            const std::wstring name = dirEntry.path().filename().wstring();
            const auto version = ParseFolderVersion(name);
            if (!version || !dirEntry.is_directory(typeEc))
                continue;
            const fs::path file = dirEntry.path() / fileName;
            if (!fs::is_regular_file(file, typeEc))
                continue;
            bool duplicate = false;
            for (const auto& have : found)
                duplicate = duplicate || detail::EqualsNoCase(have.id, name);
            if (duplicate)
                continue;
            Entry e;
            e.id = name;
            e.folderVersion = *version;
            std::error_code canonEc;
            const fs::path canon = fs::canonical(file, canonEc);
            e.path = canonEc ? file.lexically_normal() : canon;
            found.push_back(std::move(e));
        }
        if (ec && incomplete != nullptr)
            *incomplete = true; // the listing broke off half way
    }
    std::sort(found.begin(), found.end(), [](const Entry& a, const Entry& b) { return detail::LessNoCase(a.id, b.id); });
    return found;
}

// The four-part file version from the version resource; empty when the file has none.
inline VersionParts ReadFileVersion(const fs::path& file)
{
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &handle);
    if (size == 0)
        return {};
    std::vector<char> data(size);
    if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data()))
        return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &length) || info == nullptr ||
        length < sizeof(VS_FIXEDFILEINFO))
        return {};
    return { HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS),
             LOWORD(info->dwFileVersionLS) };
}

inline uint64_t FileSize(const fs::path& file)
{
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    return ec ? 0 : static_cast<uint64_t>(size);
}

// Last write time as a number (0 when it cannot be read).
inline uint64_t FileStamp(const fs::path& file)
{
    std::error_code ec;
    const auto time = fs::last_write_time(file, ec);
    return ec ? 0 : static_cast<uint64_t>(time.time_since_epoch().count());
}

namespace detail
{
inline std::string Hex(const unsigned char* bytes, size_t count)
{
    static const char digits[] = "0123456789abcdef";
    std::string text;
    for (size_t i = 0; i < count; ++i)
    {
        text += digits[bytes[i] >> 4];
        text += digits[bytes[i] & 15];
    }
    return text;
}

// Streaming SHA-256 over a callback that delivers chunks (returns the number of bytes, 0 at the end, -1 on error).
class Sha256
{
  public:
    Sha256()
    {
        if (BCryptOpenAlgorithmProvider(&_alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
            _alg = nullptr;
        else if (BCryptCreateHash(_alg, &_hash, nullptr, 0, nullptr, 0, 0) != 0)
            _hash = nullptr;
    }
    ~Sha256()
    {
        if (_hash)
            BCryptDestroyHash(_hash);
        if (_alg)
            BCryptCloseAlgorithmProvider(_alg, 0);
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    bool Update(const void* data, size_t size)
    {
        return _hash && BCryptHashData(_hash, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(size), 0) == 0;
    }
    std::string Finish()
    {
        unsigned char digest[32] = {};
        if (!_hash || BCryptFinishHash(_hash, digest, sizeof digest, 0) != 0)
            return {};
        return Hex(digest, sizeof digest);
    }

  private:
    BCRYPT_ALG_HANDLE _alg = nullptr;
    BCRYPT_HASH_HANDLE _hash = nullptr;
};
} // namespace detail

inline std::string Sha256Hex(const void* data, size_t size)
{
    detail::Sha256 h;
    return h.Update(data, size) ? h.Finish() : std::string();
}

// SHA-256 of a file, read in 1 MiB pieces; empty on any read error. Meant for a worker thread: the model is hundreds of MB.
inline std::string Sha256File(const fs::path& file, const std::function<bool()>& cancel = {})
{
    HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return {};
    detail::Sha256 h;
    std::vector<unsigned char> buffer(1u << 20);
    std::string result;
    bool ok = true;
    for (;;)
    {
        if (cancel && cancel())
        {
            ok = false; // asked to stop (the process is shutting down)
            break;
        }
        DWORD got = 0;
        if (!ReadFile(f, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr))
        {
            ok = false;
            break;
        }
        if (got == 0)
            break;
        if (!h.Update(buffer.data(), got))
        {
            ok = false;
            break;
        }
    }
    CloseHandle(f);
    return ok ? h.Finish() : std::string();
}
} // namespace ext::nr::runtime
