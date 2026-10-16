#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>

// Shared "safe ReadProcessMemory on our own process" wrapper. Three sites independently
// hand-wrote this pair: ext::ffxiv::companion::Packets::Read/ReadBytes (CompanionPackets.h),
// ext::ffxiv::companion::replay::FfxivNameplatePackets's own former copy, and
// ext::ffxiv::quality::FfxivNativeQualityHooks::ReadMemory. All three now forward to this pair;
// this is the only place left with the actual ReadProcessMemory call.
//
// Lives in ext/core rather than under ext/ffxiv/companion so the quality module does not have
// to include a companion header to get it -- ext::ffxiv::lighting::FfxivLightingCapture already
// avoids the same kind of cross-feature include on its own CompanionCallbacks (see the comment
// there), keeping every ext::ffxiv::* submodule independently usable. Dependency-free besides
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
