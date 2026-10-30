#include "pch.h"

#include "ext/fgrec/FgRecSeams.h"

#include "ext/core/Banner.h"
#include "ext/core/ExtConfig.h"
#include "ext/fgrec/FgRecorder.h"
#include "ext/game/Game.h"

#include <State.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

// The process reads of the FG event recorder: the key, the provider and frame ids and the session info.
// The core (FgRecorder.cpp) stays free of process types so its cost can be measured alone; everything that
// names State, Config or the game table lives here. Nothing here throws into the caller.
//
// Cost: the key is cached in g_armed (one relaxed load and one counter increment per tap) and refreshed
// from the config every 16th tap per thread; the session info is built only when a start is actually needed
// (the worker is down), never on a running or off tap.
namespace ext::fgrec
{
namespace
{
// The folder of this module (the DLL beside which fg-recordings\ is written). Found once, from the module
// handle of a function in this file: no caller passes a path in, and the directory itself is never emitted.
std::wstring ThisModuleDir()
{
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ThisModuleDir), &module) == 0 ||
        module == nullptr)
        return {};
    wchar_t path[32768] = {};
    if (GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path))) == 0)
        return {};
    std::wstring dir = path;
    if (const size_t slash = dir.find_last_of(L"\\/"); slash != std::wstring::npos)
        dir.resize(slash);
    return dir;
}

bool KeyOn() noexcept
{
    try
    {
        return ext::Cfg().framePacing.fgRecorder.value_or_default();
    }
    catch (...)
    {
        return false;
    }
}

// The cached key: the only thing fast taps read (one relaxed load, plus one
// thread-local tick). Refreshed from the config by the poll below, never on the
// hot path itself.
std::atomic<bool> g_armed { false };
thread_local uint64_t t_poll = 0;
constexpr uint64_t kPollMask = 15; // re-read the key every 16 taps per thread

void PollOff()
{
    if ((++t_poll & kPollMask) != 0)
        return;
    if (!KeyOn())
        return;
    // Off -> on: arm, and clear a failure that latched while the key was off. The key-off clear above can run
    // before the worker's last write fails, so without the rearm the first tap of the new session would return
    // on the latch and only a second key toggle would record. It is the arm edge only, once per key-on.
    if (!g_armed.exchange(true, std::memory_order_relaxed))
        RearmKey();
}

void PollOn()
{
    if ((++t_poll & kPollMask) != 0)
        return;
    if (KeyOn())
        return;
    g_armed.store(false, std::memory_order_relaxed); // on -> off: disarm and stop
    EnsureStarted(false, SessionInfo {});
}

// The session info is gathered once, when a start is actually needed. The
// module dir is cached in a function-local static (no module calls per tap).
void BuildSessionInfo(SessionInfo& info, std::string& summaryStore)
{
    info.version = ext::core::ForkVersion();
    const auto& state = State::Instance();
    info.exe = state.gameExe.c_str();
    info.build = ext::game::Current().lighting != nullptr ? "matched" : "unmatched";
    char text[128] = {};
    std::snprintf(text, sizeof text, "fg=%s automfg=%s", ProviderLabel(static_cast<uint32_t>(state.activeFgOutput)),
                  ext::Cfg().fg.autoMfg.value_or_default() ? "on" : "off");
    summaryStore = text;
    info.summary = summaryStore.c_str();
    static const std::wstring dir = ThisModuleDir();
    info.moduleDir = dir.c_str();
}

bool SlowStart()
{
    if (ext::fgrec::Failed())
        return false; // latched: a failed start never retries from this thread
    std::string summary;
    SessionInfo info;
    BuildSessionInfo(info, summary);
    EnsureStarted(true, info);
    return ext::fgrec::Alive();
}

bool StartIfNeeded()
{
    if (ext::fgrec::Alive())
        return true; // one relaxed load: the worker is up, enqueue
    return SlowStart();
}

// Every tap enters here first: cached key, then liveness. True means armed and
// a start was requested (the worker drains from here); false means off, or a
// latched failure, and the tap emits nothing.
bool TapEnter()
{
    if (!g_armed.load(std::memory_order_relaxed))
    {
        PollOff();
        if (!g_armed.load(std::memory_order_relaxed))
            return false;
    }
    else
    {
        PollOn();
        if (!g_armed.load(std::memory_order_relaxed))
            return false; // turned off just now
    }
    return StartIfNeeded();
}

LARGE_INTEGER QpcNow()
{
    LARGE_INTEGER now {};
    QueryPerformanceCounter(&now);
    return now;
}
} // namespace

// One open present per thread and site (begin ... end in different statements): the end carries the
// duration in QPC ticks (the analyzer converts with the session's qpc_frequency). A missing begin
// (an end without one) reports a zero duration rather than a wrong one.
thread_local LARGE_INTEGER tFgBegin {};
thread_local LARGE_INTEGER tOutBegin {};

int64_t TicksSince(LARGE_INTEGER begin)
{
    if (begin.QuadPart == 0)
        return 0;
    const LARGE_INTEGER end = QpcNow();
    return end.QuadPart >= begin.QuadPart ? end.QuadPart - begin.QuadPart : 0;
}

void FgPresentBegin(bool willPresent, UINT sync, UINT flags) noexcept
{
    try
    {
        if (!TapEnter())
            return;
        if (!willPresent)
            return;
        tFgBegin = QpcNow();
        const auto& state = State::Instance();
        NoteProvider(MapProvider(static_cast<uint32_t>(state.activeFgOutput)));
        Emit(kFgPresentBegin, state.fgLastFrame, static_cast<int64_t>(sync), static_cast<int64_t>(flags), 0, nullptr);
    }
    catch (...)
    {
    }
}

void FgPresentEnd(bool willPresent, HRESULT hr) noexcept
{
    try
    {
        // Read and clear the begin slot before anything can bail out: a tap that
        // returns early must never leave a slot for a later session to pair with.
        const LARGE_INTEGER begin = tFgBegin;
        tFgBegin = LARGE_INTEGER {};
        if (!TapEnter())
            return;
        if (!willPresent)
            return;
        const auto& state = State::Instance();
        NoteProvider(MapProvider(static_cast<uint32_t>(state.activeFgOutput)));
        Emit(kFgPresentEnd, state.fgLastFrame, static_cast<int64_t>(hr), TicksSince(begin), 0, nullptr);
    }
    catch (...)
    {
    }
}

void OutPresentBegin(bool willPresent, UINT sync, UINT flags) noexcept
{
    try
    {
        if (!TapEnter())
            return;
        if (!willPresent)
            return;
        tOutBegin = QpcNow();
        const auto& state = State::Instance();
        NoteProvider(MapProvider(static_cast<uint32_t>(state.activeFgOutput)));
        Emit(kOutPresentBegin, state.frameCount, static_cast<int64_t>(sync), static_cast<int64_t>(flags), 0, nullptr);
    }
    catch (...)
    {
    }
}

void OutPresentEnd(bool willPresent, HRESULT hr) noexcept
{
    try
    {
        // See FgPresentEnd: the slot is taken and cleared before any early return.
        const LARGE_INTEGER begin = tOutBegin;
        tOutBegin = LARGE_INTEGER {};
        if (!TapEnter())
            return;
        if (!willPresent)
            return;
        const auto& state = State::Instance();
        NoteProvider(MapProvider(static_cast<uint32_t>(state.activeFgOutput)));
        Emit(kOutPresentEnd, state.frameCount, static_cast<int64_t>(hr), TicksSince(begin), 0, nullptr);
    }
    catch (...)
    {
    }
}

bool RecArmed() noexcept
{
    try
    {
        return g_armed.load(std::memory_order_relaxed); // the cached key: one relaxed load
    }
    catch (...)
    {
        return false;
    }
}

unsigned MapProvider(uint32_t out)
{
    // Values of the upstream FGOutput (State.h): NoFG 0, FSRFG 1, DLSSG 2, XeFG 3.
    switch (static_cast<FGOutput>(out))
    {
    case FGOutput::DLSSG:
        return kProvDlssG;
    case FGOutput::XeFG:
        return kProvXeFg;
    case FGOutput::FSRFG:
        return kProvFsr;
    default:
        return kProvNone;
    }
}

const char* ProviderLabel(uint32_t out)
{
    switch (static_cast<FGOutput>(out))
    {
    case FGOutput::DLSSG:
        return "dlssg";
    case FGOutput::XeFG:
        return "xefg";
    case FGOutput::FSRFG:
        return "fsr";
    default:
        return "none";
    }
}

void OnReflexSleep(int64_t ticks) noexcept
{
    try
    {
        if (!TapEnter())
            return;
        Emit(kSleep, State::Instance().fgLastFrame, ticks, 0, 0, "reflex");
    }
    catch (...)
    {
    }
}

void OnHiddenPresent(HRESULT hr, int64_t ticks) noexcept
{
    try
    {
        if (!TapEnter())
            return;
        Emit(kHiddenPresent, State::Instance().frameCount, static_cast<int64_t>(hr), ticks, 0, nullptr);
    }
    catch (...)
    {
    }
}

void OnAutoMfg(bool warn, const char* text) noexcept
{
    try
    {
        if (text == nullptr || text[0] == 0)
            return;
        if (!TapEnter())
            return;
        Emit(kAutoMfg, State::Instance().fgLastFrame, warn ? 1 : 0, 0, 0, text);
    }
    catch (...)
    {
    }
}

void OnNrRebuild() noexcept
{
    try
    {
        if (!TapEnter())
            return;
        const uint64_t frame = State::Instance().frameCount;
        static std::atomic<uint64_t> lastRebuildFrame { UINT64_MAX };
        if (lastRebuildFrame.exchange(frame, std::memory_order_relaxed) == frame)
            return; // park and build of one rebuild share the frame: one record
        Emit(kNrRebuild, State::Instance().fgLastFrame, 0, 0, 0, "model");
    }
    catch (...)
    {
    }
}

void OnProviderOn(const char* label) noexcept
{
    try
    {
        if (!TapEnter())
            return;
        NoteGeneration(); // a new generation starts on activation: later records carry it
        Emit(kProviderOn, State::Instance().frameCount, 0, 0, 0, label);
    }
    catch (...)
    {
    }
}

void OnProviderOff(const char* label) noexcept
{
    try
    {
        if (!TapEnter())
            return;
        Emit(kProviderOff, State::Instance().frameCount, 0, 0, 0, label);
    }
    catch (...)
    {
    }
}

void OnPresetSwitch(const char* from, const char* to) noexcept
{
    try
    {
        char text[64] = {};
        std::snprintf(text, sizeof text, "%s -> %s", from != nullptr ? from : "?", to != nullptr ? to : "?");
        if (!TapEnter())
            return;
        NoteGeneration();
        Emit(kPresetSwitch, State::Instance().fgLastFrame, 0, 0, 0, text);
    }
    catch (...)
    {
    }
}

void OnModule(const wchar_t* fileName) noexcept
{
    try
    {
        if (fileName == nullptr || fileName[0] == 0)
            return;
        // File name only, never a path: the label is what follows the last separator.
        const wchar_t* base = fileName;
        for (const wchar_t* p = fileName; *p != 0; ++p)
            if (*p == L'\\' || *p == L'/')
                base = p + 1;
        char narrow[192] = {};
        const int n =
            WideCharToMultiByte(CP_UTF8, 0, base, -1, narrow, static_cast<int>(sizeof narrow), nullptr, nullptr);
        if (n <= 1)
            return; // unconvertible: record nothing rather than a wrong label
        if (!TapEnter())
            return;
        Emit(kModule, State::Instance().frameCount, 0, 0, 0, narrow);
    }
    catch (...)
    {
    }
}

void OnXeFactor(int from, int to) noexcept
{
    try
    {
        if (from == to)
            return;
        if (!TapEnter())
            return;
        Emit(kXeFactor, State::Instance().fgLastFrame, from, to, 0, nullptr);
    }
    catch (...)
    {
    }
}

} // namespace ext::fgrec
