#pragma once
// The benchmark's scripted switches and its fault injection: [FrameGen] FGOutputSwitchScript and FGOutputSwitchFault.
//
// The script is a list of "seconds:target" pairs, for example "45:xefg,90:none,135:fsrfg". Each time is counted from the
// first bridge present that saw the key on; when it arrives, the configured frame-generation output is set to that target,
// which is exactly what a player does in the menu and what the live switch then notices. A step with an unknown target, a
// missing time or a fractional one is dropped and the rest of the list still runs: a typo costs one step, never the run.
//
// The fault key names one step to fail once, so the rollback to plain presentation can be proven in a real process:
// drain, teardown, create, adopt, replay or probation. Anything else means no fault. Both keys are diagnostic, ini only,
// empty (nothing scripted, no fault) by default, and honoured only while the feature key is on.
//
// Pure: no Windows or upstream include (tests/ext/fg/fg_output_switch_script.cpp).

#include "ext/fg/outswitch/SwitchTypes.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ext::fg::outswitch
{
inline constexpr size_t kMaxScriptSteps = 16;

struct ScriptStep
{
    uint32_t atMs = 0;
    Target target = Target::Plain;
};

struct Script
{
    ScriptStep steps[kMaxScriptSteps] {};
    size_t count = 0;
    size_t next = 0; // the first step that has not fired yet
    bool started = false;
    uint64_t startMs = 0; // the first present that saw the key on

    bool Empty() const { return count == 0; }

    // Starts the clock on the first call after a load (the benchmark counts from the first present).
    void Start(uint64_t nowMs)
    {
        if (started)
            return;
        started = true;
        startMs = nowMs;
    }

    // The target the configured output must take now, if a step is due. Every due step is consumed, so a slow frame
    // catches up instead of firing the same step again; `target` is the last one due.
    bool Due(uint64_t nowMs, Target& target)
    {
        if (!started || next >= count || nowMs < startMs)
            return false;
        const uint64_t elapsed = nowMs - startMs;
        bool fired = false;
        while (next < count && elapsed >= steps[next].atMs)
        {
            target = steps[next].target;
            ++next;
            fired = true;
        }
        return fired;
    }
};

// "xefg" / "fsrfg" / "none": the names the script and the log use.
inline bool ParseTargetName(const char* text, size_t length, Target& out)
{
    static constexpr struct
    {
        const char* name;
        Target target;
    } kNames[] = { { "none", Target::Plain }, { "plain", Target::Plain }, { "fsrfg", Target::FsrFg },
                   { "xefg", Target::XeFg } };
    for (const auto& entry : kNames)
    {
        const size_t nameLength = std::strlen(entry.name);
        if (nameLength == length && std::strncmp(text, entry.name, length) == 0)
        {
            out = entry.target;
            return true;
        }
    }
    return false;
}

// "45:xefg,90:none": the whole list, or as many steps as fit, in the order they were written. The parser never reorders and
// never drops a later step because an earlier one was wrong.
inline void ParseScript(const char* text, Script& out)
{
    out = Script {};
    if (text == nullptr)
        return;

    size_t position = 0;
    while (text[position] != '\0' && out.count < kMaxScriptSteps)
    {
        // One entry: up to the next comma.
        size_t end = position;
        while (text[end] != '\0' && text[end] != ',')
            ++end;

        size_t cursor = position;
        while (cursor < end && (text[cursor] == ' ' || text[cursor] == '\t'))
            ++cursor;

        // The time: digits only, seconds.
        uint64_t seconds = 0;
        bool digits = false;
        bool overflow = false;
        while (cursor < end && text[cursor] >= '0' && text[cursor] <= '9')
        {
            digits = true;
            seconds = seconds * 10 + static_cast<uint64_t>(text[cursor] - '0');
            if (seconds > 0xFFFFFFFFull / 1000ull)
                overflow = true;
            ++cursor;
        }

        while (cursor < end && (text[cursor] == ' ' || text[cursor] == '\t'))
            ++cursor;

        if (digits && !overflow && cursor < end && text[cursor] == ':')
        {
            ++cursor;
            while (cursor < end && (text[cursor] == ' ' || text[cursor] == '\t'))
                ++cursor;
            size_t nameEnd = end;
            while (nameEnd > cursor && (text[nameEnd - 1] == ' ' || text[nameEnd - 1] == '\t'))
                --nameEnd;

            Target target = Target::Plain;
            if (ParseTargetName(text + cursor, nameEnd - cursor, target))
            {
                out.steps[out.count].atMs = static_cast<uint32_t>(seconds * 1000ull);
                out.steps[out.count].target = target;
                ++out.count;
            }
        }

        position = end + 1;
    }
}

// The step a fault key names, or Step::Idle for "no fault" and for anything the key does not name.
inline Step ParseFaultStep(const char* text)
{
    static constexpr struct
    {
        const char* name;
        Step step;
    } kNames[] = { { "drain", Step::Drain },         { "teardown", Step::Teardown }, { "create", Step::Create },
                   { "adopt", Step::Adopt },          { "replay", Step::Replay },     { "probation", Step::Probation } };
    if (text == nullptr)
        return Step::Idle;
    while (*text == ' ' || *text == '\t')
        ++text;
    const size_t length = std::strlen(text);
    for (const auto& entry : kNames)
        if (std::strlen(entry.name) == length && std::strncmp(text, entry.name, length) == 0)
            return entry.step;
    return Step::Idle;
}
} // namespace ext::fg::outswitch
