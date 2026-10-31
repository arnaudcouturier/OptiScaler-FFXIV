#pragma once
// The status hand-off from the present thread to the menu and to status.json.
//
// The present thread writes the status, the menu and the status-file threads read it. No lock is taken: every field is
// an independent relaxed atomic, and `state` is the last thing the writer stores (with release) and the first thing the
// reader loads (with acquire). A reader that sees a new state therefore sees the fields that go with it, and a torn mix
// of two status lines is impossible in the one place it would matter. A whole seqlock is not needed: if a reader is one
// present behind, the line it shows is still a real status of this session.
//
// The writer is the only one that stores; nothing here allocates, takes a lock or formats a string, so it is safe on the
// present thread.
//
// Pure: no Windows or upstream include (tests/ext/fg/fg_switch_runtime.cpp).

#include "ext/fg/outswitch/SwitchTypes.h"

#include <atomic>
#include <cstdint>

namespace ext::fg::outswitch
{
class StatusBox
{
  public:
    void Publish(const Status& status)
    {
        // Every field first, relaxed: they belong to the state that is written last.
        reason_.store(static_cast<uint8_t>(status.reason), std::memory_order_relaxed);
        target_.store(static_cast<uint8_t>(status.target), std::memory_order_relaxed);
        active_.store(static_cast<uint8_t>(status.active), std::memory_order_relaxed);
        generation_.store(status.generation, std::memory_order_relaxed);
        hitch_.store(status.lastHitchMs, std::memory_order_relaxed);
        left_.store(status.switchesLeft, std::memory_order_relaxed);
        probation_.store(status.probationPresents, std::memory_order_relaxed);
        state_.store(static_cast<uint8_t>(status.state), std::memory_order_release); // last
    }

    Status Read() const
    {
        Status status;
        status.state = static_cast<State>(state_.load(std::memory_order_acquire)); // first
        status.reason = static_cast<Fail>(reason_.load(std::memory_order_relaxed));
        status.target = static_cast<Target>(target_.load(std::memory_order_relaxed));
        status.active = static_cast<Target>(active_.load(std::memory_order_relaxed));
        status.generation = generation_.load(std::memory_order_relaxed);
        status.lastHitchMs = hitch_.load(std::memory_order_relaxed);
        status.switchesLeft = left_.load(std::memory_order_relaxed);
        status.probationPresents = probation_.load(std::memory_order_relaxed);
        return status;
    }

    // The cheap read a menu frame does when it only needs the state word.
    State StateNow() const { return static_cast<State>(state_.load(std::memory_order_acquire)); }

  private:
    std::atomic<uint8_t> reason_ { static_cast<uint8_t>(Fail::None) };
    std::atomic<uint8_t> target_ { static_cast<uint8_t>(Target::Plain) };
    std::atomic<uint8_t> active_ { static_cast<uint8_t>(Target::Plain) };
    std::atomic<uint32_t> generation_ { 0 };
    std::atomic<uint32_t> hitch_ { 0 };
    std::atomic<uint32_t> left_ { 0 };
    std::atomic<uint32_t> probation_ { 0 };
    std::atomic<uint8_t> state_ { static_cast<uint8_t>(State::Idle) };
};
} // namespace ext::fg::outswitch
