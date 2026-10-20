#include "pch.h"

#include "ext/upscaler/NgxThreadTrace.h"
#include "ext/upscaler/NgxThreadTraceCore.h"

#include <mutex>

namespace ext::upscaler
{
namespace
{
constexpr uint64_t kEvaluateIntervalMs = 10000;

std::mutex& TrackerMutex()
{
    static std::mutex m;
    return m;
}

threadtrace::EvaluateTracker& Tracker()
{
    static threadtrace::EvaluateTracker t(kEvaluateIntervalMs);
    return t;
}
} // namespace

void LogCreateThread(unsigned renderWidth, unsigned renderHeight, unsigned displayWidth, unsigned displayHeight)
{
    LOG_INFO("{}", threadtrace::FormatCreate(GetCurrentThreadId(), renderWidth, renderHeight, displayWidth, displayHeight));
}

void LogEvaluateThread(const void* key)
{
    const auto tid = static_cast<uint32_t>(GetCurrentThreadId());
    threadtrace::Decision decision;
    {
        std::lock_guard<std::mutex> lock(TrackerMutex());
        decision = Tracker().OnEvaluate(reinterpret_cast<uintptr_t>(key), tid, GetTickCount64());
    }
    if (decision.log)
        LOG_INFO("{}", threadtrace::FormatEvaluate(reinterpret_cast<uintptr_t>(key), tid, decision));
}

void LogReleaseThread(unsigned handleId)
{
    LOG_INFO("{}", threadtrace::FormatRelease(handleId, GetCurrentThreadId()));
}
} // namespace ext::upscaler
