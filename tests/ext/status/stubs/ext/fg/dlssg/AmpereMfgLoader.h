#pragma once
// Test stand-in for the Ampere loader status: only the folder probe the
// collector reads is modelled.

namespace AmpereMfgLoader
{
struct Status
{
    bool Enabled = false;
    bool DllFound = false;
};

inline Status g_status;

inline Status LastStatus() { return g_status; }
} // namespace AmpereMfgLoader
