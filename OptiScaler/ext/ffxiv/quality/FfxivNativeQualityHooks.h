#pragma once
// Native FFXIV scene-resolution hooks behind INativeQuality (C1, C5; docs/SEAMS-catalog.md "upscaler",
// docs/SEAMS.md 3.8). Detours two native functions so a forced preset reaches the game's own per-quality render
// table, and reads the native settings struct for the native-DLAA gate. Ported from the oracle's
// FfxivNativeQuality (misc/FfxivNativeQuality.h, folded together with its install logic into
// upscalers/IFeature.cpp there; split back out into its own game-module directory here, see SEAMS.md 3.8). Every
// RVA/offset/byte guard comes from ext::ffxiv::GameBuild.h; a game patch means editing that file, not this one.

#include <detours/detours.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "ext/core/ExtConfig.h"
#include "ext/core/ProcessMemory.h"
#include "ext/ffxiv/GameBuild.h"
#include "ext/upscaler/UpscalerSeams.h"

namespace ext::ffxiv::quality::FfxivNativeQualityHooks
{
// Canonical implementation lives in ext::core::ReadProcessBytes (ext/core/ProcessMemory.h, also
// used by ext::ffxiv::companion::Packets); kept as a named forwarder since this module calls
// ReadMemory(const void*, ...) throughout and deliberately does not include a companion header
// to get the shared helper (see ProcessMemory.h's own comment on why it lives in ext/core).
inline bool ReadMemory(const void* address, void* output, size_t bytes)
{
    return ext::core::ReadProcessBytes(reinterpret_cast<uintptr_t>(address), output, bytes);
}

inline uintptr_t Base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

// Matches the running process against GameBuild.h the same way Game.cpp's SelectGame does (duplicated rather than
// shared, same as ext::ffxiv::companion::Install: this module has to work even if Game::Current() has not reached
// here yet, and it is cheap -- a handful of in-process pointer reads, no syscalls). Computed once: the match
// cannot change during the process lifetime.
inline const NativeQualityBuild* MatchedBuildOnce()
{
    const auto base = Base();
    if (base == 0)
        return nullptr;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000)
        return nullptr;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return nullptr;

    const auto* build = ext::ffxiv::MatchBuild(nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
    return build != nullptr ? &build->nativeQuality : nullptr;
}

inline const NativeQualityBuild* Build()
{
    static const NativeQualityBuild* build = MatchedBuildOnce();
    return build;
}

// ---- Native DLAA gate (FfxivNativeQuality::NativeDlaaRequired in the oracle) --------------------------------------

// Covers every settings offset read below; derived from GameBuild.h rather than the oracle's hardcoded 0x56; ties
// the snapshot to kCurrentBuild specifically (today's only build), revisit if a future build's offsets grow past
// this one's.
inline constexpr size_t kSettingsSnapshotSize =
    std::max({ kCurrentBuild.nativeQuality.settingsQualityOffset, kCurrentBuild.nativeQuality.settingsDlaaOffset,
               kCurrentBuild.nativeQuality.settingsRendererOffset, kCurrentBuild.nativeQuality.settingsAdditionalOffset,
               kCurrentBuild.nativeQuality.settingsModeOffset, kCurrentBuild.nativeQuality.settingsSelectorOffset }) +
    1;

// Byte-verifies the settings read path once (fail closed, log once) and caches the result: both DoInstall and
// DlaaRequiredNow gate on this, matching the oracle's EnsureFfxivNativeQuality probe.
inline bool SettingsGuardOk()
{
    static const bool ok = []
    {
        const auto* build = Build();
        if (build == nullptr)
            return false;

        auto actual = build->settingsInstructions;
        const bool match =
            ReadMemory(reinterpret_cast<const void*>(Base() + build->settingsInstructionsRva), actual.data(),
                       actual.size()) &&
            actual == build->settingsInstructions;

        if (!match)
            LOG_WARN("FFXIV probe: unsupported executable instructions; settings read disabled");

        return match;
    }();
    return ok;
}

inline bool DlaaRequiredNow()
{
    if (!SettingsGuardOk())
        return false;

    const auto* build = Build();
    const auto base = Base();

    uintptr_t settings = 0;
    std::array<unsigned char, kSettingsSnapshotSize> snapshot {};
    const bool valid =
        ReadMemory(reinterpret_cast<const void*>(base + build->settingsPointerRva), &settings, sizeof(settings)) &&
        settings != 0 && ReadMemory(reinterpret_cast<const void*>(settings), snapshot.data(), snapshot.size());
    const bool required = valid && snapshot[build->settingsModeOffset] == 2 && snapshot[build->settingsDlaaOffset] == 0;

    // Log only when the gate flips (first call included); the exchange keeps the per-frame path cheap.
    static std::atomic<int> lastRequired { -1 };
    if (lastRequired.exchange(required ? 1 : 0) != (required ? 1 : 0))
        LOG_INFO("FFXIV native DLAA gate: settings={:#x} read={} +0x44={:#04x} +0x45={:#04x} +0x54={:#04x} "
                 "+0x55={:#04x} required={}",
                 settings, valid, snapshot[build->settingsQualityOffset], snapshot[build->settingsDlaaOffset],
                 snapshot[build->settingsModeOffset], snapshot[build->settingsSelectorOffset], required);
    return required;
}

inline std::atomic<uint64_t> dlaaCacheTick { 0 };
inline std::atomic<int> dlaaCacheValue { 0 };
inline std::atomic<bool> dlaaCacheValid { false };

// DlaaRequired, cached per frame (ext::game::INativeQuality): re-reads the settings struct at most once per
// distinct GetTickCount64() value instead of on every call site (EffectivePerfQuality, ApplyForcedQuality and the
// renderer-update detour below all ask, at least once a frame each). No mutex, no string compare: three relaxed
// atomics on the cache-hit path.
inline bool DlaaRequiredCached()
{
    const auto tick = GetTickCount64();
    if (dlaaCacheValid.load(std::memory_order_relaxed) && dlaaCacheTick.load(std::memory_order_relaxed) == tick)
        return dlaaCacheValue.load(std::memory_order_relaxed) != 0;

    const bool required = DlaaRequiredNow();
    dlaaCacheValue.store(required ? 1 : 0, std::memory_order_relaxed);
    dlaaCacheTick.store(tick, std::memory_order_relaxed);
    dlaaCacheValid.store(true, std::memory_order_relaxed);
    return required;
}

// ---- Install / detours (InstallFfxivNativeQuality, ObserveFfxivQuery, ObserveFfxivRendererUpdate in the oracle) --

inline std::atomic<bool> available { false };
inline std::atomic<int> requestedQuality { -1 };
inline std::atomic<unsigned int> requestSequence { 0 };

using RendererUpdate = void (*)(uintptr_t, float);
using ResizeCallbacks = unsigned char (*)(uintptr_t);
using QuerySize = uintptr_t (*)(const unsigned int*, unsigned int*, unsigned int*, unsigned int*);

inline RendererUpdate originalRendererUpdate = nullptr;
inline ResizeCallbacks dispatchResizeCallbacks = nullptr;
inline QuerySize originalQuerySize = nullptr;

inline void Request(int quality)
{
    requestedQuality.store(quality >= 0 && quality <= 5 ? quality : -1);
    requestSequence.fetch_add(1);
}

// Renderer-update detour body: a pending Request() reaches the native per-quality table by invalidating its
// display-size cache and calling dispatchResizeCallbacks directly (not detoured), which makes the native renderer
// re-query NGX. Runs once per sequence number, on the game's own update thread.
inline void ProcessPendingRequest(uintptr_t renderer)
{
    if (DlaaRequiredCached())
        return; // native DLAA already owns the scene size; do not fight it with a preset request

    static unsigned int handled = 0;
    const auto sequence = requestSequence.load();
    if (sequence == handled)
        return;

    const auto* build = Build();
    if (build == nullptr)
        return;
    const auto base = Base();

    uintptr_t settings = 0, manager = 0, context = 0, device = 0, callbacks = 0;
    unsigned char type = 0, flags = 0;
    if (!ReadMemory(reinterpret_cast<void*>(base + build->settingsPointerRva), &settings, sizeof(settings)) ||
        settings == 0 ||
        !ReadMemory(reinterpret_cast<void*>(settings + build->settingsModeOffset), &type, sizeof(type)) ||
        type != 2 ||
        !ReadMemory(reinterpret_cast<void*>(base + build->managerPointerRva), &manager, sizeof(manager)) ||
        manager == 0 ||
        !ReadMemory(reinterpret_cast<void*>(manager + build->managerContextOffset), &context, sizeof(context)) ||
        context == 0 ||
        !ReadMemory(reinterpret_cast<void*>(context + build->contextFlagsOffset), &flags, sizeof(flags)) ||
        (flags & 1) == 0 || (flags & 0x30) != 0 ||
        !ReadMemory(reinterpret_cast<void*>(base + build->devicePointerRva), &device, sizeof(device)) ||
        device == 0 ||
        !ReadMemory(reinterpret_cast<void*>(device + build->deviceCallbacksOffset), &callbacks, sizeof(callbacks)) ||
        callbacks == 0)
        return;

    // Invalidate only the quality table's display-size cache. The native callback then queries NGX again and
    // propagates the result to rendering textures on the game's update thread.
    const unsigned long long invalidDisplay = 0;
    SIZE_T written = 0;
    if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(context + build->contextCacheOffset),
                             &invalidDisplay, sizeof(invalidDisplay), &written) ||
        written != sizeof(invalidDisplay))
        return;

    const auto result = dispatchResizeCallbacks(callbacks);
    handled = sequence;
    unsigned int size[2] {};
    ReadMemory(reinterpret_cast<void*>(renderer + build->rendererSceneSizeOffset), size, sizeof(size));
    LOG_INFO("FFXIV native quality: request={} preset={} callbacks={} scene={}x{} thread={}", sequence,
             requestedQuality.load(), result, size[0], size[1], GetCurrentThreadId());
}

inline void ObserveRendererUpdate(uintptr_t renderer, float elapsed)
{
    ProcessPendingRequest(renderer);
    originalRendererUpdate(renderer, elapsed);
}

// Query-size detour body: once a pending request's resize round-trip lands back here with a matching answer, also
// constrain the native scene selector (not just NGX feature-creation metadata). Exact equality on render dims --
// a different rule from ApplyForcedQuality's 32px slack (ext/upscaler/ForcedQuality.h): this compares the native
// query's own target against the fresh LastQualityAnswer it was itself responsible for producing, not a possibly
// stale earlier create.
inline uintptr_t ObserveQuery(const unsigned int* display, unsigned int* optimal, unsigned int* minimum,
                               unsigned int* maximum)
{
    const auto result = originalQuerySize(display, optimal, minimum, maximum);
    const int requested = requestedQuality.load();
    if ((result & 0xff) != 0 && requested >= 0 && requested <= 5)
    {
        const auto answer = ext::upscaler::LastQualityAnswer();
        unsigned int target[2] {}, output[2] {};
        if (ReadMemory(optimal, target, sizeof(target)) && ReadMemory(display, output, sizeof(output)) &&
            answer.quality == requested && answer.renderWidth == target[0] && answer.renderHeight == target[1] &&
            target[0] > 0 && target[1] > 0 && target[0] <= output[0] && target[1] <= output[1])
        {
            memcpy(minimum, target, sizeof(target));
            memcpy(maximum, target, sizeof(target));
        }
    }
    return result;
}

// Known, accepted risk (reviewed, not an oversight): THREAD_SUSPEND_RESUME (requested on
// OpenThread, below) is only an access right -- it does not suspend anything by itself. The
// actual suspension happens inside the DetourUpdateThread calls further down, each of which may
// allocate its own per-thread bookkeeping while earlier threads in the same call are already
// suspended; DetourAttach and DetourTransactionCommit can allocate too, all before the
// transaction resumes anyone. Whether that bookkeeping comes from the CRT heap (hazardous:
// another suspended thread could hold its lock) or elsewhere is not verified: this repo vendors
// only detours.h and a prebuilt detours.lib, no source, so Detours' internal allocator cannot be
// inspected here. Trampolines themselves are conventionally VirtualAlloc'd, but that is a
// different allocation path and is not a mitigation for this one. Deliberately not restructured:
// changing the suspend/attach ordering to address an unverified hazard risks introducing a real
// bug in exchange for a theoretical one. The same note applies to ext::core::DetourThreadScope
// (ext/core/DetourThreads.h), which shares this enlistment pattern with the two other call
// sites that patch D3D11 vtable slots (ext::ffxiv::companion::Native and
// ext::ffxiv::lighting::FfxivLightingCapture).
inline bool DoInstall()
{
    if (!SettingsGuardOk())
        return false; // same probe NativeDlaaRequired fails closed on; EnsureFfxivNativeQuality refused to install here too

    const auto* build = Build();
    if (build == nullptr)
        return false;
    const auto base = Base();

    auto queryActual = build->querySizeInstructions;
    if (!ReadMemory(reinterpret_cast<void*>(base + build->querySizeRva), queryActual.data(), queryActual.size()) ||
        queryActual != build->querySizeInstructions)
    {
        LOG_WARN("FFXIV native quality: instruction mismatch; hooks disabled");
        return false;
    }

    auto updateActual = build->rendererUpdateInstructions;
    auto dispatchActual = build->resizeCallbacksInstructions;
    if (!ReadMemory(reinterpret_cast<void*>(base + build->rendererUpdateRva), updateActual.data(),
                     updateActual.size()) ||
        !ReadMemory(reinterpret_cast<void*>(base + build->resizeCallbacksRva), dispatchActual.data(),
                     dispatchActual.size()) ||
        updateActual != build->rendererUpdateInstructions || dispatchActual != build->resizeCallbacksInstructions)
    {
        LOG_WARN("FFXIV native quality: update instruction mismatch; hooks disabled");
        return false;
    }

    originalRendererUpdate = reinterpret_cast<RendererUpdate>(base + build->rendererUpdateRva);
    dispatchResizeCallbacks = reinterpret_cast<ResizeCallbacks>(base + build->resizeCallbacksRva);
    originalQuerySize = reinterpret_cast<QuerySize>(base + build->querySizeRva);

    // Enlist existing game threads so Detours can relocate any instruction pointer in a patched prologue.
    std::vector<HANDLE> threads;
    HANDLE snapshotHandle = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshotHandle == INVALID_HANDLE_VALUE)
        return false; // matches the oracle: no log here, only on the enlist-failure path below

    THREADENTRY32 entry {};
    entry.dwSize = sizeof(entry);
    bool readable = Thread32First(snapshotHandle, &entry) != FALSE;
    bool threadsReady = readable;
    while (readable)
    {
        if (entry.th32OwnerProcessID == GetCurrentProcessId() && entry.th32ThreadID != GetCurrentThreadId())
        {
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE,
                                        entry.th32ThreadID);
            if (thread != nullptr)
                threads.push_back(thread);
            else if (GetLastError() != ERROR_INVALID_PARAMETER)
                threadsReady = false;
        }
        readable = Thread32Next(snapshotHandle, &entry) != FALSE;
    }
    CloseHandle(snapshotHandle);
    if (!threadsReady)
    {
        for (auto thread : threads)
            CloseHandle(thread);
        LOG_WARN("FFXIV native quality: unable to enlist game threads; hooks disabled");
        return false;
    }

    LONG result = DetourTransactionBegin();
    if (result == NO_ERROR)
    {
        result = DetourUpdateThread(GetCurrentThread());
        for (auto thread : threads)
        {
            if (result != NO_ERROR)
                break;
            result = DetourUpdateThread(thread);
        }
        if (result == NO_ERROR)
            result = DetourAttach(&(PVOID&) originalQuerySize, ObserveQuery);
        if (result == NO_ERROR)
            result = DetourAttach(&(PVOID&) originalRendererUpdate, ObserveRendererUpdate);
        if (result == NO_ERROR)
            result = DetourTransactionCommit();
        else
            DetourTransactionAbort();
    }
    for (auto thread : threads)
        CloseHandle(thread);

    if (result == NO_ERROR)
    {
        available.store(true);
        const int forced = ext::Cfg().upscaler.forcePerfQuality.value_or_default();
        if (forced >= 0 && forced <= 5)
            Request(forced);
    }
    LOG_INFO("FFXIV native quality v2: hook transaction result={} (0=installed); native scene updates enabled",
             result);
    return result == NO_ERROR;
}

inline std::atomic<int> installState { 0 }; // 0 = not resolved yet, 1 = installed, -1 = refused

// EnsureHooks (ext::game::INativeQuality): lazy, call_once, cheap on every call after the first. The fast path is
// one relaxed atomic load -- the brief's "pointer/bool test, no mutex, no string compare" -- with std::call_once
// doing the one-time work exactly once even if several threads race in before it resolves.
inline bool Install()
{
    const int state = installState.load(std::memory_order_relaxed);
    if (state != 0)
        return state > 0;

    static std::once_flag once;
    std::call_once(once,
                    [] { installState.store(DoInstall() ? 1 : -1, std::memory_order_relaxed); });
    return installState.load(std::memory_order_relaxed) > 0;
}
} // namespace ext::ffxiv::quality::FfxivNativeQualityHooks
