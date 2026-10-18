#pragma once
// The live command channel of the character preview probe: <game folder>\OptiScaler\chara-probe.cmd in (one command per line, parsed
// by CharaProbeRules.h), chara-probe.out out (timestamped result lines). No D3D, no COM; nothing here throws.

#include "ext/ffxiv/chara/CharaProbeFiles.h"

namespace ext::ffxiv::chara::probe
{
// ---------------------------------------------------------------------------------------------------------------------
// The live command channel: <game folder>\OptiScaler\chara-probe.cmd in, chara-probe.out out
// ---------------------------------------------------------------------------------------------------------------------

inline constexpr uint64_t kCommandPollMs = 500;
inline constexpr uint64_t kCommandMinAge = 1'000'000;      // 100 ms in FILETIME units (100 ns): a file written more recently may be half written
inline constexpr uint64_t kFutureTolerance = 20'000'000;   // 2 s: a last-write time further ahead than this cannot be a write in progress
inline constexpr uint64_t kOutputCap = 256ull * 1024;      // bytes of chara-probe.out
inline constexpr uint64_t kOutputReserve = 192;            // room kept for the "limit reached" line

enum class Disposition : uint8_t
{
    Removed, // the command file was deleted
    Renamed, // it could not be deleted, it was renamed to chara-probe.cmd.done
    Kept,    // neither worked: it is remembered and not run again until it changes
};

// Deletes the file, else renames it to <name>.done, else gives up. `remove(path)` and `rename(from, to)` return true on success.
template <class RemoveFn, class RenameFn>
Disposition Dispose(const fs::path& path, RemoveFn&& remove, RenameFn&& rename)
{
    if (remove(path))
        return Disposition::Removed;
    fs::path done = path;
    done += ".done";
    if (rename(path, done))
        return Disposition::Renamed;
    return Disposition::Kept;
}

class CommandChannel
{
  public:
    explicit CommandChannel(fs::path folder) : _folder(std::move(folder)) {}

    fs::path CommandPath() const { return _folder / "chara-probe.cmd"; }
    fs::path OutputPath() const { return _folder / "chara-probe.out"; }

    struct Polled
    {
        enum class State : uint8_t
        {
            None,       // no command file (or one that already ran and could not be removed)
            NotReady,   // a file that was written less than 100 ms ago: looked at again next time
            Unreadable, // the file could not be opened (an editor holds it): looked at again next time
            Commands,   // the file was read; `parsed` holds its commands (possibly none) and it has been disposed of
        };
        State state = State::None;
        ParsedFile parsed;
        uint64_t fileBytes = 0;
        Disposition disposition = Disposition::Removed;
    };

    // `nowFileTime` is the current time as a FILETIME value (100 ns units). Reads at most kMaxCommandBytes + 1 bytes. The file is
    // deleted (else renamed to .done, else remembered by last-write time and size) right after it is read, before the commands run.
    Polled Poll(uint64_t nowFileTime)
    {
        Polled polled;
        try
        {
            const fs::path path = CommandPath();
            WIN32_FILE_ATTRIBUTE_DATA data {};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                _remembered = false;
                return polled;
            }
            const uint64_t written = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
            const uint64_t size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            if (_remembered && _writtenSeen == written && _sizeSeen == size)
                return polled; // already ran, could not be removed
            _remembered = false;

            const bool old = written > nowFileTime ? written - nowFileTime > kFutureTolerance : nowFileTime - written >= kCommandMinAge;
            if (!old)
            {
                polled.state = Polled::State::NotReady;
                return polled;
            }

            std::string text;
            {
                std::ifstream file(path, std::ios::binary);
                if (!file)
                {
                    polled.state = Polled::State::Unreadable;
                    return polled;
                }
                text.resize(static_cast<size_t>(std::min<uint64_t>(size, kMaxCommandBytes + 1)));
                file.read(text.data(), static_cast<std::streamsize>(text.size()));
                text.resize(static_cast<size_t>(std::max<std::streamsize>(file.gcount(), 0)));
            }

            polled.disposition = Dispose(
                path,
                [](const fs::path& p)
                {
                    std::error_code ec;
                    fs::remove(p, ec);
                    return !ec && !fs::exists(p, ec);
                },
                [](const fs::path& from, const fs::path& to)
                {
                    std::error_code ec;
                    fs::rename(from, to, ec);
                    return !ec;
                });
            if (polled.disposition == Disposition::Kept)
            {
                _remembered = true;
                _writtenSeen = written;
                _sizeSeen = size;
            }
            polled.fileBytes = size;
            polled.parsed = ParseFile(text);
            if (size > kMaxCommandBytes)
                polled.parsed.truncatedBytes = true;
            polled.state = Polled::State::Commands;
        }
        catch (...)
        {
            polled = Polled {};
        }
        return polled;
    }

    // Appends lines (each ends with a newline) to chara-probe.out under a mutex. The file is capped at 256 KiB: a file that is already
    // near the cap when the first line of this channel arrives starts over, and when the cap is reached a last line says so and later
    // lines are dropped. Returns false when nothing was written.
    bool AppendOutput(const std::vector<std::string>& lines)
    {
        std::lock_guard lock(_outputMutex);
        try
        {
            if (_limitReached)
                return false;
            std::error_code ec;
            fs::create_directories(_folder, ec);
            const fs::path path = OutputPath();
            uint64_t existing = 0;
            if (fs::exists(path, ec))
            {
                const auto size = fs::file_size(path, ec);
                existing = ec ? 0 : static_cast<uint64_t>(size);
            }
            std::string block;
            bool restart = false;
            if (!_started)
            {
                _started = true;
                if (existing > kOutputCap - kOutputReserve)
                {
                    restart = true;
                    existing = 0;
                    block = "(the earlier contents of this file went over the 256 KiB limit and were discarded)\n";
                }
            }
            for (const std::string& line : lines)
            {
                for (char c : line)
                    block.push_back((static_cast<unsigned char>(c) < 0x20 || c == 0x7F) ? ' ' : c);
                block.push_back('\n');
            }
            if (existing + block.size() > kOutputCap - kOutputReserve)
            {
                _limitReached = true;
                block = "(output limit of 256 KiB reached; further lines are dropped until the game restarts)\n";
            }
            std::ofstream file(path, std::ios::binary | (restart ? std::ios::trunc : std::ios::app));
            if (!file)
                return false;
            file.write(block.data(), static_cast<std::streamsize>(block.size()));
            file.flush();
            return file.good() && !_limitReached;
        }
        catch (...)
        {
            return false;
        }
    }

  private:
    fs::path _folder;
    bool _remembered = false;
    uint64_t _writtenSeen = 0;
    uint64_t _sizeSeen = 0;
    std::mutex _outputMutex;
    bool _started = false;
    bool _limitReached = false;
};
} // namespace ext::ffxiv::chara::probe
