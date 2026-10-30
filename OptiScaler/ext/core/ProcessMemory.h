#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>

// Shared "safe ReadProcessMemory on our own process" wrapper. Two sites independently
// hand-wrote this pair: the removed nameplate packet reader and
// ext::ffxiv::quality::FfxivNativeQualityHooks::ReadMemory. Only the quality hooks forward to this pair now;
// this is the only place left with the actual ReadProcessMemory call.
//
// Lives in ext/core so every ext/ffxiv::* submodule stays independently usable without a cross-feature
// include. Dependency-free besides
// <windows.h> so it can be pulled into a header a tests/ext recipe builds standalone.
namespace ext::core
{
template <class T> inline bool ReadProcessValue(uintptr_t address, T& value)
{
    SIZE_T got = 0;
    return address != 0 &&
           ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), &value, sizeof(value),
                              &got) &&
           got == sizeof(value);
}

inline bool ReadProcessBytes(uintptr_t address, void* out, size_t size)
{
    SIZE_T got = 0;
    return address != 0 &&
           ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), out, size, &got) &&
           got == size;
}
} // namespace ext::core
