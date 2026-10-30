#pragma once
// Test stand-in for the build banner: fixed strings the tests assert through.

namespace ext::core
{
inline const char* ForkVersion() { return "test-fork"; }
inline const char* UpstreamBase() { return "test-upstream"; }
} // namespace ext::core
