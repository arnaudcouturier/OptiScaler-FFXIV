#pragma once
// reshade-probe.txt writer (S10 package D0): <base>\reshade-<yyyymmdd-hhmmss>\reshade-probe.txt, created on the first
// line (never from DllMain), flushed after every line so a crash leaves the report up to that moment, older
// reshade-* run folders beyond 5 pruned. No product dependencies: the caller passes the base folder
// (<game>\OptiScaler\hudless-diag), so the code can only ever write below it.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace ext::reshade::probe
{
struct IReport
{
    virtual ~IReport() = default;
    virtual void Line(const std::string& line) = 0; // thread-safe
};

class FileReport final : public IReport
{
  public:
    static constexpr size_t kKeepRuns = 5;

    explicit FileReport(std::filesystem::path baseFolder);

    void Line(const std::string& line) override;

    std::filesystem::path Folder();   // run folder (empty until the first line, or when creating it failed)
    bool Failed();                    // true when the folder or file could not be created

  private:
    bool OpenLocked();

    std::mutex _mutex;
    std::filesystem::path _base;
    std::filesystem::path _folder;
    std::ofstream _file;
    bool _opened = false;
    bool _failed = false;
    std::chrono::steady_clock::time_point _t0;
};
} // namespace ext::reshade::probe
