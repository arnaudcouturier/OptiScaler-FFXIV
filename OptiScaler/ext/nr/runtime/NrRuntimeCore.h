#pragma once
// The NR runtime picker's glue, written against a small environment (paths, the config value, the clock, the log) so
// tests/ext/nr/nr_runtime_core.cpp runs the REAL code on temporary folders: the catalog build, the selection polled every frame, the
// background hash, the session lock. NrRuntime.cpp binds it to the product (Config, State, Util, the logger).
//
// Rules this code keeps (fix round 1 of the review):
//   * The render thread never waits for a disk search: the catalog is scanned OUTSIDE the state mutex and published as one
//     shared_ptr; Current() only reads the published catalog and polls the selector for a few microseconds under the mutex.
//   * The SHA-256 worker never logs (it can outlive the logger at process exit); results are logged by the next Catalog() call.
//     A file that cannot be hashed is remembered and not read again until the next rescan. The cache is keyed by path, size and
//     last write time, so a file replaced in place is hashed again. Shutdown() stops and joins the worker.
//   * Any exception while scanning leaves "no model found" (fail open), never an unwinding render thread.

#include "ext/nr/runtime/NrRuntimeScan.h"
#include "ext/nr/runtime/NrRuntimeSelection.h"
#include "ext/nr/runtime/NrRuntimeSwitch.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace ext::nr::runtime
{
struct Env
{
    std::function<fs::path()> dllDir;                  // the folder of the fork's DLL (with the old search start)
    std::function<fs::path()> exeDir;                  // the folder of the game executable
    std::function<std::optional<fs::path>()> mainDllPath; // [MainDllPath] when known (also a root of NR.<version> folders)
    std::function<bool()> allowStreamlineFolder;       // DLSS-G active: the streamline folder is not excluded from the legacy search
    std::function<std::string()> requested;            // [DlssNr] Runtime, UTF-8
    std::function<void(const std::string&)> setRequested;
    std::function<bool()> hybridPrecisionNow;
    std::function<bool()> amdBridgeActive;
    std::function<uint64_t()> nowMs;
    std::function<void(bool warning, const std::string&)> log;
    std::function<void()> onScan; // test hook, called inside every catalog scan (may block or throw); empty in production
};

class Runtime
{
  public:
    explicit Runtime(Env env) : _env(std::move(env)) {}

    // The selection as it stands now (see NrRuntime.h).
    std::shared_ptr<const Snapshot> Current()
    {
        const bool locked = _lock.Reason(_env.hybridPrecisionNow(), _env.amdBridgeActive()) != nullptr;
        EnsureCatalog();
        std::lock_guard<std::mutex> lock(_mutex);
        EnsureSelectorLocked();
        if (_selector->Current() && locked)
            return _selector->Current();
        const auto snapshot = _selector->Poll(Wide(_env.requested()), _env.nowMs());
        if (!_startup)
            _startup = snapshot;
        return snapshot;
    }

    std::shared_ptr<const Snapshot> Startup()
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_startup)
                return _startup;
        }
        Current();
        std::lock_guard<std::mutex> lock(_mutex);
        return _startup;
    }

    void RequestRuntime(const std::string& text)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _env.setRequested(text);
    }

    // The config value, read under the same mutex the writer takes.
    std::string Requested()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _env.requested();
    }

    // Looks at the disk again. The scan runs on the calling thread WITHOUT the state mutex; the result is published in one step.
    void Rescan() { ScanAndPublish(true); }

    bool Pending()
    {
        const bool locked = _lock.Reason(_env.hybridPrecisionNow(), _env.amdBridgeActive()) != nullptr;
        std::lock_guard<std::mutex> lock(_mutex);
        // While switching is locked nobody polls, so a request would wait forever: do not show it as waiting.
        return _selector && _selector->Pending() && !locked;
    }

    std::vector<Entry> Catalog()
    {
        EnsureCatalog();
        std::vector<std::string> toLog;
        std::vector<Entry> copy;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            StartHashingLocked();
            copy = *_catalog;
            for (auto& e : copy)
            {
                const auto it = _hashes.find(HashKey(e));
                if (it != _hashes.end())
                    e.sha256 = it->second;
            }
            toLog.swap(_hashLog);
        }
        for (const auto& line : toLog)
            _env.log(false, line);
        return copy;
    }

    const char* SwitchLockReason() { return _lock.Reason(_env.hybridPrecisionNow(), _env.amdBridgeActive()); }

    void NoteNgxSearchFolder(const fs::path& folder)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_ngxFolder.empty())
            _ngxFolder = folder;
    }

    fs::path NgxSearchFolder()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _ngxFolder;
    }

    // With no folder recorded (the NGX core was not given one) there is nothing the selected model could disagree with.
    bool FollowsNgxSearchPath(const Snapshot& s)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return !s.Available() || _ngxFolder.empty() || Key(s.path.parent_path()) == Key(_ngxFolder);
    }

    // Stops the hash worker and waits for it (a 1 MiB read at most). Safe to call twice.
    void Shutdown()
    {
        _stop = true;
        std::thread worker;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            worker = std::move(_worker);
        }
        if (worker.joinable())
            worker.join();
    }

    // For tests: how many files the worker has started to read.
    int HashAttempts() const { return _hashAttempts; }

    // For tests: waits until the hash worker has nothing left to do.
    void WaitForHashing()
    {
        for (;;)
        {
            {
                std::lock_guard<std::mutex> lock(_mutex);
                if (!_hashRunning)
                    return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    static std::wstring Key(const fs::path& p)
    {
        std::wstring k = p.native();
        for (auto& c : k)
            c = detail::Lower(c);
        return k;
    }

    static std::wstring Wide(const std::string& s)
    {
        if (s.empty())
            return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        if (n <= 0)
            return {};
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }

    static std::string Utf8(const std::wstring& w)
    {
        if (w.empty())
            return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        if (n <= 0)
            return {};
        std::string s(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }

  private:
    static std::wstring HashKey(const Entry& e)
    {
        return Key(e.path) + L"|" + std::to_wstring(e.sizeBytes) + L"|" + std::to_wstring(e.stamp);
    }

    // The candidates on disk now. No state of this object is touched: it runs without the mutex.
    std::vector<Entry> ScanCatalog()
    {
        std::vector<Entry> entries;
        try
        {
            if (_env.onScan)
                _env.onScan();
            const fs::path dllDir = _env.dllDir();
            const fs::path exeDir = _env.exeDir();
            const auto mainDll = _env.mainDllPath();
            fs::path streamline;
            if (mainDll)
                streamline = *mainDll / L"streamline";
            const bool allowAll = _env.allowStreamlineFolder();
            const ExcludeFn excluded = [streamline, allowAll](const fs::path& p)
            { return !allowAll && !streamline.empty() && IsUnder(p, streamline); };

            // Legacy file: beside OptiScaler first, then beside the executable, as the model search always did.
            auto legacy = FindLegacy(dllDir, L"nvngx_dlssnr.dll", excluded);
            if (!legacy)
                legacy = FindLegacy(exeDir, L"nvngx_dlssnr.dll", excluded);
            if (legacy)
            {
                Entry e;
                e.id = std::wstring(kLegacyId);
                e.legacy = true;
                std::error_code ec;
                const fs::path canon = fs::canonical(*legacy, ec);
                e.path = ec ? legacy->lexically_normal() : canon;
                entries.push_back(std::move(e));
            }

            std::vector<fs::path> roots;
            if (mainDll)
                roots.push_back(*mainDll);
            roots.push_back(dllDir / L"OptiScaler");
            roots.push_back(exeDir / L"OptiScaler");
            for (auto& e : ScanFolders(roots, L"nvngx_dlssnr.dll"))
            {
                // A folder entry that is the very file the legacy search found is the legacy file, not a second candidate.
                bool same = false;
                for (const auto& have : entries)
                    same = same || Key(have.path) == Key(e.path);
                if (!same)
                    entries.push_back(std::move(e));
            }
            for (auto& e : entries)
            {
                e.fileVersion = ReadFileVersion(e.path);
                e.sizeBytes = FileSize(e.path);
                e.stamp = FileStamp(e.path);
            }
        }
        catch (const std::exception& ex)
        {
            _env.log(true, std::string("DLSS-NR runtime: the catalog scan failed (") + ex.what() + "); no model is offered");
            entries.clear();
        }
        catch (...)
        {
            _env.log(true, "DLSS-NR runtime: the catalog scan failed; no model is offered");
            entries.clear();
        }
        return entries;
    }

    // Scans (off the state mutex, one scan at a time) and publishes. `again` = a rescan: forget hash failures, re-resolve the request.
    void ScanAndPublish(bool again)
    {
        std::lock_guard<std::mutex> scanLock(_scanMutex);
        if (!again && _catalogBuilt)
            return; // another thread did the first scan while this one waited
        std::vector<Entry> entries = ScanCatalog();
        const size_t count = entries.size();
        std::string listing;
        for (const auto& e : entries)
        {
            listing += "\n  ";
            listing += Utf8(e.id) + " " + Utf8(VersionText(OrderVersion(e))) + ", " + SizeText(e.sizeBytes) + ", " + Utf8(e.path.wstring());
        }
        {
            std::lock_guard<std::mutex> lock(_mutex);
            for (auto& e : entries)
            {
                const auto it = _hashes.find(HashKey(e));
                if (it != _hashes.end())
                    e.sha256 = it->second;
            }
            _catalog = std::make_shared<const std::vector<Entry>>(std::move(entries));
            if (again)
                _failedHashes.clear();
            if (_selector)
                _selector->Rescan();
            _catalogBuilt = true;
        }
        _env.log(false, "DLSS-NR runtime catalog: " + std::to_string(count) + " candidate(s)" + listing);
    }

    void EnsureCatalog()
    {
        if (!_catalogBuilt)
            ScanAndPublish(false);
    }

    // Caller holds the mutex and the catalog exists.
    void EnsureSelectorLocked()
    {
        if (_selector)
            return;
        _selector = std::make_unique<Selector>(
            [this](std::wstring_view requested)
            {
                return MakeSnapshot(*_catalog, requested); // called from Poll, under the mutex
            });
    }

    // The model is hundreds of MB: nothing reads it for a hash until the card asks for the catalog (a session that never opens the card
    // does no extra file reads). Caller holds the mutex.
    void StartHashingLocked()
    {
        if (_hashRunning || _stop)
            return;
        std::vector<std::pair<fs::path, std::wstring>> todo;
        for (const auto& e : *_catalog)
        {
            const std::wstring key = HashKey(e);
            if (_hashes.find(key) == _hashes.end() && _failedHashes.find(key) == _failedHashes.end())
                todo.emplace_back(e.path, key);
        }
        if (todo.empty())
            return;
        if (_worker.joinable())
            _worker.join(); // the previous worker has finished (_hashRunning is false)
        _hashRunning = true;
        try
        {
            _worker = std::thread([this, todo = std::move(todo)]() { HashWorker(todo); });
        }
        catch (...)
        {
            _hashRunning = false;
        }
    }

    void HashWorker(const std::vector<std::pair<fs::path, std::wstring>>& todo)
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (const auto& [path, key] : todo)
        {
            if (_stop)
                break;
            ++_hashAttempts;
            const std::string sha = Sha256File(path, &_stop);
            if (_stop)
                break;
            std::lock_guard<std::mutex> lock(_mutex);
            if (sha.empty())
                _failedHashes.insert(key); // not read again until the next rescan
            else
            {
                _hashes[key] = sha;
                _hashLog.push_back("DLSS-NR runtime " + Utf8(path.wstring()) + ": SHA-256 " + sha); // logged by the next Catalog() call
            }
        }
        std::lock_guard<std::mutex> lock(_mutex);
        _hashRunning = false; // the worker touches no logger: it can outlive it at process exit
    }

    Env _env;
    SessionLock _lock;
    std::mutex _scanMutex; // serialises scans; never held by the render thread after the first one
    std::mutex _mutex;     // short sections only: no disk access, no logging
    std::atomic<bool> _catalogBuilt { false };
    std::shared_ptr<const std::vector<Entry>> _catalog = std::make_shared<const std::vector<Entry>>();
    std::map<std::wstring, std::string> _hashes;
    std::set<std::wstring> _failedHashes;
    std::vector<std::string> _hashLog;
    std::unique_ptr<Selector> _selector;
    std::shared_ptr<const Snapshot> _startup;
    fs::path _ngxFolder;
    std::thread _worker;
    bool _hashRunning = false;
    std::atomic<bool> _stop { false };
    std::atomic<int> _hashAttempts { 0 };
};
} // namespace ext::nr::runtime
