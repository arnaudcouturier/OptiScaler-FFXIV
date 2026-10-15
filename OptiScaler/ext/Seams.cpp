#include "pch.h"

#include "ext/Seams.h"

#include <resource_tracking/ResTrack_dx12.h>

#include <detours/detours.h>

// ext::Access definitions: the only fork code that names private upstream members (docs/SEAMS.md section 3.3).
// When upstream renames a member, this file is where the build breaks. Nothing is accessed yet; accessors are added
// together with the series that needs them.
namespace ext
{
bool Access::ResetHookAttached() { return ResTrack_Dx12::o_Reset != nullptr; }

bool Access::AttachResetHook(void* target)
{
    if (ResTrack_Dx12::o_Reset != nullptr)
        return true;
    if (target == nullptr)
        return false;

    ResTrack_Dx12::o_Reset = reinterpret_cast<decltype(ResTrack_Dx12::o_Reset)>(target);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&) ResTrack_Dx12::o_Reset, ResTrack_Dx12::hkReset);
    auto detourResult = DetourTransactionCommit();

    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to hook CommandList Reset: {:X}", detourResult);
        ResTrack_Dx12::o_Reset = nullptr;
        return false;
    }
    return true;
}
} // namespace ext
