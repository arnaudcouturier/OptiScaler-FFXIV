#pragma once
// Test stand-in for the unlock status: only the version readout the collector
// gates through the version check is modelled. While discovery is provisional
// it reports a sentence, never a version.

#include <string>

namespace MfgUnlock
{
struct Status
{
    std::string SnippetVersion;
};

inline Status g_status;

inline Status LastStatus() { return g_status; }
} // namespace MfgUnlock
