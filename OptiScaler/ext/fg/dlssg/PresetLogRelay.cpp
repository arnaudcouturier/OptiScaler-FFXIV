#include "pch.h"

#include "PresetLogRelay.h"

#include <Config.h>

#include <spdlog/sinks/base_sink.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ext::fg::preset
{
namespace
{
// The queue and the readout are never destroyed: the logger may still call the sink while the process shuts down.
struct QueueState
{
    std::mutex mutex;
    std::vector<std::string> lines; // unrecognised preset lines waiting to be logged
};
QueueState& Queue_()
{
    static QueueState* state = new QueueState;
    return *state;
}
std::atomic<bool> g_queued { false };
// The logger the sink was added to, by a weak reference (never destroyed, see Queue_).
struct AttachedState
{
    std::mutex mutex;
    std::weak_ptr<spdlog::logger> logger;
    bool attempted = false;
    bool asyncSkipped = false;
    bool attached = false;
};
AttachedState& Attached_()
{
    static AttachedState* state = new AttachedState;
    return *state;
}

void Queue(std::string_view line)
{
    auto& queue = Queue_();
    std::lock_guard lock(queue.mutex);
    if (queue.lines.size() < 8)
        queue.lines.emplace_back(line.substr(0, kMaxLineChars));
    g_queued.store(true, std::memory_order_release);
}

// Reads every message that reaches the logger. It must never log, throw or block: it runs inside the logger.
class RelaySink final : public spdlog::sinks::base_sink<std::mutex>
{
  protected:
    void sink_it_(const spdlog::details::log_msg& msg) override
    {
        try
        {
            const std::string_view text(msg.payload.data(), msg.payload.size());
            if (ReadoutState().Feed(text, GetTickCount64()))
                Queue(text);
        }
        catch (...)
        {
        }
    }

    void flush_() override {}
};
} // namespace

Readout& ReadoutState()
{
    static Readout* readout = new Readout; // never destroyed, see Queue_
    return *readout;
}

void AttachLogRelay()
{
    try
    {
        auto& holder = Attached_();
        std::lock_guard lock(holder.mutex);
        if (holder.attempted)
            return;
        holder.attempted = true;
        // With asynchronous logging a worker thread may be iterating the sinks while we add one: do not touch the logger then.
        if (Config::Instance()->LogAsync.value_or_default())
        {
            holder.asyncSkipped = true;
            return;
        }
        auto logger = spdlog::default_logger();
        if (!logger)
            return;
        logger->sinks().push_back(std::make_shared<RelaySink>());
        holder.attached = true;
        holder.logger = logger; // a weak reference: the control block keeps the address from being reused while we look at it
    }
    catch (...)
    {
    }
}

RelayState LogRelayState()
{
    auto& holder = Attached_();
    std::lock_guard lock(holder.mutex);
    if (holder.asyncSkipped)
        return RelayState::AsyncLogging;
    if (!holder.attached)
        return RelayState::NotAttached;
    const auto attached = holder.logger.lock();
    return attached && attached == spdlog::default_logger() ? RelayState::Attached : RelayState::Lost;
}

bool UiRecompositionPaused()
{
    const int request = Requests().Active();
    if (request != kPresetA && request != kPresetC)
        return false; // the common case costs nothing
    const auto confirmed = ReadoutState().Confirm(request);
    return PausesUiRecomposition(request, confirmed.effect, confirmed.uir);
}

void FlushCandidateLines()
{
    if (!g_queued.load(std::memory_order_acquire))
        return;
    std::vector<std::string> lines;
    {
        auto& queue = Queue_();
        std::lock_guard lock(queue.mutex);
        lines.swap(queue.lines);
        g_queued.store(false, std::memory_order_release);
    }
    for (const auto& line : lines)
        LOG_INFO("DLSS-G preset line, wording not recognised yet: {}", line);
}
} // namespace ext::fg::preset
