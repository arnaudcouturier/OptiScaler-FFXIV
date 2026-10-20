#include "pch.h"

#include "PresetLogRelay.h"

#include <spdlog/sinks/base_sink.h>

#include <atomic>
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
std::atomic<const void*> g_attachedTo { nullptr };

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
        auto logger = spdlog::default_logger();
        if (!logger || g_attachedTo.load() != nullptr)
            return;
        logger->sinks().push_back(std::make_shared<RelaySink>());
        g_attachedTo.store(logger.get());
    }
    catch (...)
    {
    }
}

RelayState LogRelayState()
{
    const void* attached = g_attachedTo.load();
    if (attached == nullptr)
        return RelayState::NotAttached;
    const auto current = spdlog::default_logger_raw();
    return current == attached ? RelayState::Attached : RelayState::Lost;
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
