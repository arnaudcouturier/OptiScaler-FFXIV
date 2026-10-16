#include "pch.h"

#include "ext/ffxiv/GameBuild.h"
#include "../core/ProcessMemory.h"

namespace ext::ffxiv
{
const GameBuild* MatchBuild(std::uint32_t timeDateStamp, std::uint32_t sizeOfImage)
{
    if (timeDateStamp == kCurrentBuild.timeDateStamp && sizeOfImage == kCurrentBuild.sizeOfImage)
        return &kCurrentBuild;
    return nullptr;
}

const GameBuild* MatchCurrentProcess()
{
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (base == 0)
        return nullptr;

    IMAGE_DOS_HEADER dos {};
    if (!ext::core::ReadProcessValue(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
        dos.e_lfanew > 0x100000)
        return nullptr;

    IMAGE_NT_HEADERS64 nt {};
    if (!ext::core::ReadProcessValue(base + static_cast<uintptr_t>(dos.e_lfanew), nt) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return nullptr;

    return MatchBuild(nt.FileHeader.TimeDateStamp, nt.OptionalHeader.SizeOfImage);
}
} // namespace ext::ffxiv
