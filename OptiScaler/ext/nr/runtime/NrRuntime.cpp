#include "pch.h"

#include "ext/nr/runtime/NrRuntime.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/PathText.h"
#include "ext/nr/NrConfig.h"
#include "ext/nr/amd/AmdBridge.h"
#include "ext/nr/runtime/NrRuntimeCore.h"

#include <Config.h>
#include <State.h>
#include <Util.h>

// The product binding of the picker's glue (NrRuntimeCore.h): where the paths, the config value, the clock and the log come from.
// The logic itself is in the header and is run by tests/ext/nr/nr_runtime_core.cpp on temporary folders.

namespace ext::nr::runtime
{
std::wstring Wide(const std::string& s) { return Runtime::Wide(s); }
std::string Utf8(const std::wstring& w) { return Runtime::Utf8(w); }

namespace
{
Runtime& R()
{
    static Runtime& runtime = *new Runtime(Env {
        /* dllDir */ [] { return Util::DllPath().remove_filename(); },
        /* exeDir */ [] { return Util::ExePath().remove_filename(); },
        /* mainDllPath */
        []() -> std::optional<fs::path>
        {
            if (Config::Instance()->MainDllPath.has_value())
                return fs::path(Config::Instance()->MainDllPath.value());
            return std::nullopt;
        },
        /* allowStreamlineFolder */ [] { return State::Instance().activeFgOutput == FGOutput::DLSSG; },
        /* requested */ [] { return ext::Cfg().nr.runtime.value_or_default(); },
        /* setRequested */ [](const std::string& text) { ext::Cfg().nr.runtime = text; },
        /* hybridPrecisionNow */ [] { return ext::Cfg().nr.precision.value_or_default() == 4; },
        /* amdBridgeActive */ [] { return !DlssNr::AmdBridge::Status().empty(); },
        /* nowMs */ [] { return static_cast<uint64_t>(GetTickCount64()); },
        /* log */
        [](bool warning, const std::string& text)
        {
            if (warning)
                LOG_WARN("{}", text);
            else
                LOG_INFO("{}", text);
        },
        /* onScan */ nullptr,
        /* injectIncomplete */ nullptr,
        /* shuttingDown */ [] { return State::Instance().isShuttingDown; } }); // never destroyed: the hash worker is stopped by Shutdown(), but the object must outlive static teardown
    return runtime;
}
} // namespace

std::shared_ptr<const Snapshot> Current() { return R().Current(); }
std::shared_ptr<const Snapshot> Startup() { return R().Startup(); }
void RequestRuntime(const std::string& text) { R().RequestRuntime(text); }
std::string Requested() { return R().Requested(); }
std::string ScanNote() { return R().ScanNote(); }
void Rescan() { R().Rescan(); }
bool Pending() { return R().Pending(); }
std::vector<Entry> Catalog() { return R().Catalog(); }
const char* SwitchLockReason() { return R().SwitchLockReason(); }
void NoteNgxSearchFolder(const fs::path& folder) { R().NoteNgxSearchFolder(folder); }
fs::path NgxSearchFolder() { return R().NgxSearchFolder(); }
bool FollowsNgxSearchPath(const Snapshot& s) { return R().FollowsNgxSearchPath(s); }
void Shutdown() { R().Shutdown(); }
void Resume() { R().Resume(); }
} // namespace ext::nr::runtime
