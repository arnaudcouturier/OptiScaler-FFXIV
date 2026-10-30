#pragma once
// Test stand-in for the hudless mode and engine state: the mode flag and the
// displayed phase plus its menu line, all driven by the test.

#include <cstdint>
#include <string>

namespace ext::hudless
{
enum class Mode : uint8_t
{
    Off,
    Recomposition,
    UiFree,
};

enum class Phase : uint8_t
{
    Off,
    Searching,
    Active,
    Fallback,
};

struct Status
{
    Mode mode = Mode::Off;
    Phase phase = Phase::Off;
};

inline Mode g_mode = Mode::Off;
inline Status g_status;
inline std::string g_statusLine = "Active";

inline Mode CurrentMode() { return g_mode; }
inline Status GetStatus() { return g_status; }
inline std::string StatusLine(const Status&) { return g_statusLine; }
} // namespace ext::hudless
