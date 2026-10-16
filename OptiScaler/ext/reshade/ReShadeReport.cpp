#include "pch.h"

#include "ext/reshade/ReShadeProbeCore.h"
#include "ext/reshade/ReShadeReport.h"

#include <ctime>
#include <format>
#include <system_error>

namespace ext::reshade::probe
{
FileReport::FileReport(std::filesystem::path baseFolder) : _base(std::move(baseFolder)) {}

std::filesystem::path FileReport::Folder()
{
    std::lock_guard lock(_mutex);
    return _folder;
}

bool FileReport::Failed()
{
    std::lock_guard lock(_mutex);
    return _failed;
}

bool FileReport::OpenLocked()
{
    _opened = true;
    try
    {
        const std::time_t now = std::time(nullptr);
        std::tm tm {};
        localtime_s(&tm, &now);
        const std::string name = RunFolderName(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);

        std::error_code ec;
        std::filesystem::create_directories(_base, ec);
        if (ec)
            return false;

        // Prune older run folders of THIS module only (valid reshade-YYYYMMDD-HHMMSS names, directories).
        std::vector<std::string> existing;
        for (const auto& entry : std::filesystem::directory_iterator(_base, ec))
        {
            std::error_code ec2;
            if (entry.is_directory(ec2))
                existing.push_back(entry.path().filename().string());
        }
        for (const std::string& old : RunsToPrune(std::move(existing), kKeepRuns - 1))
            std::filesystem::remove_all(_base / old, ec);

        _folder = _base / name;
        std::filesystem::create_directories(_folder, ec);
        if (ec)
            return false;

        _file.open(_folder / "reshade-probe.txt", std::ios::out | std::ios::trunc);
        if (!_file.is_open())
            return false;

        _t0 = std::chrono::steady_clock::now();
        _file << std::format("# OptiScaler ReShade built-in add-on probe (S10 D0), run {}\n", name);
        _file << "# Every line is prefixed with the seconds since this file was opened. Lines starting with [F1]..[F5] "
                 "answer the feasibility questions; grep VERDICT for the one-line answers.\n";
        _file.flush();
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void FileReport::Line(const std::string& line)
{
    std::lock_guard lock(_mutex);
    if (_failed)
        return;

    if (!_opened)
    {
        if (!OpenLocked())
        {
            _failed = true;
            return;
        }
    }

    try
    {
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - _t0).count();
        _file << std::format("[+{:9.3f}s] ", seconds) << line << '\n';
        _file.flush();
    }
    catch (...)
    {
        _failed = true;
    }
}
} // namespace ext::reshade::probe
