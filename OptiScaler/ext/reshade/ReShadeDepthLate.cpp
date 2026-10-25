#include "pch.h"

#include "ext/reshade/ReShadeAddon.h"
#include "ext/reshade/ReShadeD3D11.h"
#include "ext/reshade/ReShadeDepthCore.h"
#include "ext/reshade/ReShadeDepthEvents.h"
#include "ext/reshade/ReShadeDepthLate.h"
#include "ext/reshade/ReShadeRegister.h"

#include <atomic>

namespace ext::reshade::depth
{
namespace
{
std::atomic<int64_t> g_lastAttemptMs { 0 };
std::atomic<uint32_t> g_attempts { 0 };

// The module scan inside RegisterAddon walks every module of the process and asks each for an export: a module that is unloaded under
// it can fault. A POD thunk under SEH, like every other call into the add-on API from outside a callback.
struct RegisterJob
{
    HMODULE self;
    RegisterResult result;
};
void RegisterThunk(void* p)
{
    auto& j = *static_cast<RegisterJob*>(p);
    j.result = RegisterAddon(j.self, nullptr); // null: search the process
}

// ReShade may FreeLibrary a module it registered when it shuts down (unverified); the callbacks must stay mapped for the whole process
// lifetime. The proxy DLL lives that long anyway. The same pin OnReshadeLoaded makes.
void PinSelf()
{
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&TryLateRegister),
                       &pinned);
}
} // namespace

LateStatus GetLateStatus() noexcept
{
    LateStatus s;
    s.attempts = g_attempts.load(std::memory_order_acquire);
    s.gaveUp = s.attempts >= LateRegistrationPlanner::kMaxAttempts;
    s.lastAttemptMs = g_lastAttemptMs.load(std::memory_order_acquire);
    return s;
}

bool TryLateRegister(bool enabled, int64_t nowMs)
{
    try
    {
        // This runs at every evaluate while ReShade is absent: the cheap exits (the attempts are used up, the last one is young) come
        // before anything is read from the config. The planner below has the last word.
        if (g_attempts.load(std::memory_order_acquire) >= LateRegistrationPlanner::kMaxAttempts ||
            nowMs - g_lastAttemptMs.load(std::memory_order_acquire) < LateRegistrationPlanner::kRetryIntervalMs)
            return false;
        const AddonStatus status = Status();
        if (status.registered)
            return false;
        int64_t last = g_lastAttemptMs.load(std::memory_order_acquire);
        if (!LateRegistrationPlanner::ShouldRetry(enabled, status.attempted, status.reason, g_attempts.load(std::memory_order_acquire),
                                                  nowMs, last))
            return false;
        // Single-flight: whoever moves the last attempt time makes this attempt; the others see a young attempt and stay out.
        if (!g_lastAttemptMs.compare_exchange_strong(last, nowMs, std::memory_order_acq_rel))
            return false;
        const uint32_t attempt = g_attempts.fetch_add(1, std::memory_order_acq_rel) + 1;

        RegisterJob job {};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&TryLateRegister), &job.self))
            job.result.status = RegisterStatus::NoSelf;
        else if (d3d11::GuardedCall(&RegisterThunk, &job) != 0)
            job.result = RegisterResult { RegisterStatus::Exception, nullptr, nullptr };

        NoteRegistration(job.result.status);
        if (job.result.status == RegisterStatus::Ok)
        {
            PinSelf();
            LOG_INFO("Automatic ReShade depth: ReShade registered late (API {}, attempt {})", kApiVersion, attempt);
            EnsureSubscribed(); // the depth events only; the boundary tracking, the probe and the window event stay where OnReshadeLoaded left them
            return true;
        }
        if (job.result.status != RegisterStatus::NoModule)
            LOG_INFO("Automatic ReShade depth: ReShade found but not usable ({}); not trying again", Describe(job.result.status));
        else if (attempt == 1)
            LOG_INFO("Automatic ReShade depth: ReShade is not loaded yet, looking again every second (up to {} times)",
                     LateRegistrationPlanner::kMaxAttempts);
        else if (attempt >= LateRegistrationPlanner::kMaxAttempts)
            LOG_INFO("Automatic ReShade depth: no ReShade found after {} attempts; not looking any more", attempt);
    }
    catch (...)
    {
    }
    return false;
}
} // namespace ext::reshade::depth
