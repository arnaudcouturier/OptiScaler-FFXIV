#include "pch.h"

#include "ext/Seams.h"
#include "ext/core/ExtConfig.h"
#include "ext/dx11bridge/BridgeAccess.h"
#include "ext/fg/outswitch/BridgeRegistry.h"
#include "ext/fg/outswitch/LibraryModel.h"
#include "ext/fg/outswitch/Probe.h"

#include <State.h>

#include <dxgi1_6.h>

#include <atomic>
#include <cwchar>

namespace
{
using ext::fg::outswitch::Class;
using ext::fg::outswitch::ClassText;
using ext::fg::outswitch::LibraryIdentity;

// 0 = not tried, 1 = running, 2 = done. Latched: the key is read at most once per process, so a present never pays for it.
std::atomic<int> g_state { 0 };
// P1's answer, for the preflight's reference-count comparison (StandstillRefs()).
std::atomic<uint32_t> g_standstillRefs { 0 };

// Which class the live chain belongs to, from the configured output.
Class LiveClass()
{
    switch (State::Instance().activeFgOutput)
    {
    case FGOutput::FSRFG:
        return Class::FsrFg;
    case FGOutput::XeFG:
        return Class::XeFg;
    default:
        return Class::Plain;
    }
}

// A library build: the file name, its size and the PE header's TimeDateStamp of the module a vtable slot lives in. Read
// from the mapped image, so this needs no version.dll import (the product ships as a proxy and must not import a system
// DLL it does not use).
bool ModuleIdentity(void* address, LibraryIdentity& identity)
{
    MEMORY_BASIC_INFORMATION info {};
    if (address == nullptr || VirtualQuery(address, &info, sizeof(info)) == 0 || info.AllocationBase == nullptr)
        return false;

    wchar_t path[MAX_PATH] {};
    if (GetModuleFileNameW(static_cast<HMODULE>(info.AllocationBase), path, MAX_PATH) == 0)
        return false;

    const wchar_t* name = std::wcsrchr(path, L'\\');
    name = name != nullptr ? name + 1 : path;
    if (WideCharToMultiByte(CP_UTF8, 0, name, -1, identity.file, static_cast<int>(sizeof(identity.file)), nullptr,
                            nullptr) <= 0)
        return false;

    WIN32_FILE_ATTRIBUTE_DATA attributes {};
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &attributes))
        identity.size = (static_cast<uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;

    const auto* base = static_cast<const uint8_t*>(info.AllocationBase);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic == IMAGE_DOS_SIGNATURE)
    {
        const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (pe->Signature == IMAGE_NT_SIGNATURE)
            identity.stamp = pe->FileHeader.TimeDateStamp;
    }

    return !identity.Empty();
}

// The chain's present slot and the module it lives in.
void ChainPresentSlot(IDXGISwapChain* chain, void*& slot, LibraryIdentity& identity)
{
    slot = nullptr;
    if (chain == nullptr)
        return;
    void** vtable = *reinterpret_cast<void***>(chain);
    if (vtable == nullptr)
        return;
    slot = vtable[8];
    ModuleIdentity(slot, identity);
}

void Run(Dx11wDx12SC& sc, bool testPresent)
{
    IDXGISwapChain4* live = ext::Access::Bridge::FgSwapChain(sc);
    const HWND window = ext::Access::Bridge::Window(sc);
    const Class liveClass = LiveClass();
    LOG_INFO("[fg-probe] begin thread={} window={} liveClass={}", GetCurrentThreadId(), static_cast<void*>(window),
             ClassText(liveClass));

    if (live == nullptr)
    {
        LOG_INFO("[fg-probe] not measured: no frame-generation chain on the bridge");
        return;
    }

    // P1 + P4: the standstill count and the present slot. At the first present the boot code has already dropped its own
    // references, so the number is the steady state and not two references high, which is what measuring in the bridge
    // constructor reported.
    LibraryIdentity identity;
    void* slot = nullptr;
    ChainPresentSlot(live, slot, identity);
    const ULONG before = live->AddRef();
    const ULONG after = live->Release();
    g_standstillRefs.store(after, std::memory_order_relaxed);
    // The configuration token is the one a model row has to carry for this class (LibraryModel.h, ConfigUsable): the
    // product always attaches a XeLL context to its XeFG context, and calls the FSR FG library directly.
    const char* config = liveClass == Class::XeFg ? "xell" : "direct";

    // The companion DLL the measured configuration needs: an XeFG row is keyed on the pair (libxess_fg.dll and
    // libxell.dll), so the in-game line names both builds and a row can be checked against the DLLs a player runs.
    LibraryIdentity companion;
    if (liveClass == Class::XeFg)
    {
        const HMODULE xell = GetModuleHandleW(L"libxell.dll");
        if (xell != nullptr)
            ModuleIdentity(reinterpret_cast<void*>(xell), companion);
    }

    LOG_INFO("[fg-probe] live class={} config={} refsBefore={} refsAfter={} vtable8={} file={} size={} stamp={:08X}",
             ClassText(liveClass), config, before, after, slot, identity.file,
             static_cast<unsigned long long>(identity.size), identity.stamp);
    if (!companion.Empty())
        LOG_INFO("[fg-probe] companion class={} file={} size={} stamp={:08X}", ClassText(liveClass), companion.file,
                 static_cast<unsigned long long>(companion.size), companion.stamp);

    // P9: one test present, and only with the probe key on. Read-only for the frame-generation work (a test present
    // does not present a frame).
    if (testPresent)
    {
        const HRESULT presentResult = live->Present(0, DXGI_PRESENT_TEST);
        LOG_INFO("[fg-probe] test-present class={} hr=0x{:X}", ClassText(liveClass),
                 static_cast<uint32_t>(presentResult));
    }

    // The destructive half is a separate process: tools/fg-probe. It is never run from here.
    LOG_INFO("[fg-probe] done: the release model is measured by tools/fg-probe, never in the game's process");
}
} // namespace

namespace ext::fg::outswitch::probe
{
void OnFirstPresent()
{
    if (g_state.load(std::memory_order_relaxed) == 2)
        return; // one relaxed load per present, nothing else once the answer is latched

    // The standstill count is tied to the hot swap key, not only to the probe key: a switch needs it to compare the live
    // chain against the row, and the read is a single AddRef/Release on the live chain. The test present stays with the
    // probe, which is the developer measurement.
    const bool probeKey = ext::Cfg().fg.outputSwitchProbe.value_or_default();
    const bool hotSwapKey = ext::Cfg().fg.outputHotSwap.value_or_default();
    if (!probeKey && !hotSwapKey)
    {
        g_state.store(2, std::memory_order_relaxed);
        return;
    }

    int expected = 0;
    if (!g_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
        return;

    try
    {
        auto* sc = static_cast<Dx11wDx12SC*>(Bridges().Bridge());
        if (sc != nullptr)
            Run(*sc, probeKey);
        else
            LOG_INFO("[fg-probe] not measured: the bridge was not recorded");
    }
    catch (...)
    {
        LOG_INFO("[fg-probe] stopped by an exception");
    }
    g_state.store(2, std::memory_order_release);
}

uint32_t StandstillRefs() { return g_standstillRefs.load(std::memory_order_relaxed); }
} // namespace ext::fg::outswitch::probe
