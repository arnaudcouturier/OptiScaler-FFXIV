#pragma once
// The stand-in world the two tick-lease product tests share: a page-aligned "game image" the test owns, its Framework instance and vtable, a tick
// trampoline, the VirtualQuery/VirtualProtect/pin/compare-exchange stand-ins, and the probe-side stubs TickLease.cpp calls. Each test is a separate
// binary that compiles ext/latency/TickLease.cpp against this header, so the production install order runs end to end without the game.
//
// Kept in a header so both binaries exercise exactly the same harness; nothing here is product code. The header owns the file-local helpers, so a test
// includes it once and adds its own anonymous namespace.
#include "pch.h"

#include "../ffxiv/chara/check.h"

#include "ext/ffxiv/ActiveBuild.h"
#include "ext/latency/TickLease.h"
#include "ext/latency/TickProbe.h"

#include <State.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr std::size_t kPageSize = 4096;
constexpr std::size_t kPages = 16;
// The fake image is a real allocation with execute permission (the tick trampoline is executed); its page protections are simulated in g_protect,
// because VirtualProtect is redirected to the harness.
std::uint8_t* g_image = nullptr;
std::uintptr_t g_base = 0;
constexpr std::size_t kImageSize = kPages * kPageSize;

constexpr std::uint32_t kTickRva = 0x40;          // the two-instruction trampoline to FakeTick
constexpr std::uint32_t kInstanceGlobalRva = 0x800; // the qword holding the Framework pointer
constexpr std::uint32_t kInstanceAt = 0x1000;     // the Framework object itself
constexpr std::uint32_t kVtableAt = 0x2000;       // its vtable (slot 4 is the tick)
constexpr std::uint32_t kVtableNearEnd = 0x2ff8;  // a vtable whose slot 4 crosses into the next page
constexpr std::uint32_t kFrameCounterOffset = 0x30;
constexpr std::uint32_t kPerfValueOffset = 0x10;
constexpr std::uint32_t kPerfFreqOffset = 0x08;
constexpr std::uint32_t kFrameCounterStart = 100;

DWORD g_protect[kPages] {};

int g_tickCalls = 0;
bool g_tickResult = true;
void* g_lastFramework = nullptr;
bool FakeTick(void* framework)
{
    ++g_tickCalls;
    g_lastFramework = framework;
    auto* counter = reinterpret_cast<std::uint32_t*>(g_image + kInstanceAt + kFrameCounterOffset);
    *counter = kFrameCounterStart + g_tickCalls; // the game's first instruction increments Framecounter
    return g_tickResult;
}

// ---- the harness state the product stubs and the Win32 stand-ins share ----
bool g_configKey = false; // what the ini would say
bool g_keyOn = false;     // the mirror ext::latency::KeyOn() reports
bool g_armed = false;     // what ext::latency::Armed() reports
bool g_failPin = false;
bool g_failProtect = false;
int g_pinCalls = 0;
int g_protectCalls = 0;
int g_exchangeCalls = 0;
int g_queryCalls = 0;
void (*g_onExchange)() = nullptr; // runs from inside the compare-exchange, where the game thread takes the slot
bool g_raceObserverResult = false;
std::vector<std::string> g_notes;

struct TickRecord
{
    std::uint32_t frame = 0;
    bool counter = false;
};
std::vector<TickRecord> g_tickBegins;
std::vector<std::uint32_t> g_tickEnds;

void SetPage(std::uint32_t at, DWORD protect) { g_protect[at / kPageSize] = protect; }

void ResetImage()
{
    if (g_image == nullptr)
    {
        g_image = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kImageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        g_base = reinterpret_cast<std::uintptr_t>(g_image);
    }
    if (g_image == nullptr)
    {
        std::printf("FAIL: cannot allocate the stand-in image\n");
        ++g_failures;
        return;
    }
    std::memset(g_image, 0, kImageSize);
    for (DWORD& protect : g_protect)
        protect = PAGE_READONLY;
    SetPage(0, PAGE_EXECUTE_READ); // the trampoline
    // 48 B8 <imm64> FF E0: mov rax, FakeTick ; jmp rax
    std::uint8_t* tick = g_image + kTickRva;
    const auto target = reinterpret_cast<std::uintptr_t>(&FakeTick);
    tick[0] = 0x48;
    tick[1] = 0xB8;
    std::memcpy(tick + 2, &target, sizeof(target));
    tick[10] = 0xFF;
    tick[11] = 0xE0;
    // the global, the instance and its vtable
    *reinterpret_cast<void**>(g_image + kInstanceGlobalRva) = g_image + kInstanceAt;
    *reinterpret_cast<void**>(g_image + kInstanceAt) = g_image + kVtableAt;
    *reinterpret_cast<std::uint32_t*>(g_image + kInstanceAt + kFrameCounterOffset) = kFrameCounterStart;
    auto** vtable = reinterpret_cast<void**>(g_image + kVtableAt);
    for (int i = 0; i < 8; ++i)
        vtable[i] = g_image + kTickRva;
    vtable[4] = g_image + kTickRva;
}

// A second vtable whose slot 4 (kVtableNearEnd + 32) starts in the next page, which the test makes inaccessible.
void MakeVtableAtEnd()
{
    auto** vtable = reinterpret_cast<void**>(g_image + kVtableNearEnd);
    for (int i = 0; i < 4; ++i)
        vtable[i] = g_image + kTickRva;
    // slot 4 starts at kVtableNearEnd + 32 = 0x3018, in the following page
    SetPage(kVtableNearEnd + 32, PAGE_NOACCESS);
    *reinterpret_cast<void**>(g_image + kInstanceAt) = g_image + kVtableNearEnd;
}

void SetInstanceGlobal(const void* value) { *reinterpret_cast<const void**>(g_image + kInstanceGlobalRva) = value; }
void SetInstance(const void* value) { *reinterpret_cast<const void**>(g_image + kInstanceAt) = value; }

ext::ffxiv::detail::ActiveState& Active()
{
    static ext::ffxiv::detail::ActiveState* state = []
    {
        auto* created = new ext::ffxiv::detail::ActiveState {};
        created->gate.started = true;
        ext::ffxiv::detail::g_active.store(created, std::memory_order_release);
        return created;
    }();
    return *state;
}

void SetBuild(std::uint8_t resolved, std::uint32_t frameCounter = kFrameCounterOffset, std::uint32_t perfValue = kPerfValueOffset,
              std::uint32_t perfFreq = kPerfFreqOffset)
{
    auto& state = Active();
    state.build = ext::ffxiv::kCurrentBuild;
    state.build.latency = ext::ffxiv::LatencyBuild { kTickRva, kInstanceGlobalRva, frameCounter, perfValue, perfFreq };
    state.gate.At(ext::ffxiv::resolve::Feature::LatencyTick).enabled = true;
    state.gate.At(ext::ffxiv::resolve::Feature::LatencyTick).reason = "the test published the feature";
    state.gate.latencyOffsetsResolved = resolved;
}

void ResetAttemptCounters()
{
    g_pinCalls = g_protectCalls = g_exchangeCalls = g_queryCalls = 0;
}

bool AnyNoteContains(const char* text)
{
    for (const std::string& note : g_notes)
    {
        if (note.find(text) != std::string::npos)
            return true;
    }
    return false;
}

// One refusal: OnDeviceHooked runs the production order, nothing is written, and the log names the reason. Returns the note count before the call.
void ExpectRefusal(const char* reason, void (*before)() = nullptr)
{
    if (before != nullptr)
        before();
    ResetAttemptCounters();
    const std::size_t notes = g_notes.size();
    ext::latency::OnDeviceHooked();
    CHECK(g_notes.size() > notes);
    if (!AnyNoteContains(reason))
        std::printf("FAIL: no note contains '%s' (the last note is '%s')\n", reason, g_notes.empty() ? "" : g_notes.back().c_str());
    CHECK(AnyNoteContains(reason));
    CHECK(g_exchangeCalls == 0); // nothing was written
}
} // namespace

// The stubs of the probe side that TickLease.cpp calls, and of the key mirror it reads.
namespace ext::latency
{
bool KeyOn() { return g_keyOn; }
bool Armed() { return g_armed; }
void RefreshKeys() { g_keyOn = g_configKey; }
void OnTickBegin(std::uint32_t frameBefore, bool frameCounter) { g_tickBegins.push_back({ frameBefore, frameCounter }); }
void OnTickEnd(std::uint32_t frameAfter) { g_tickEnds.push_back(frameAfter); }
void Note(const char* text)
{
    if (text != nullptr)
        g_notes.emplace_back(text);
}
} // namespace ext::latency

// ---- the four Win32 calls, redirected by the pch stand-in ----
BOOL TestVirtualQuery(const void* address, MEMORY_BASIC_INFORMATION* info, SIZE_T size)
{
    if (info == nullptr || size < sizeof(*info))
        return 0;
    ++g_queryCalls;
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    if (at < g_base || at >= g_base + kImageSize)
        return 0; // unmapped: the test never maps anything else
    const std::size_t page = (at - g_base) / kPageSize;
    info->BaseAddress = reinterpret_cast<void*>(g_base + page * kPageSize);
    info->AllocationBase = reinterpret_cast<void*>(g_base);
    info->AllocationProtect = PAGE_READONLY;
    info->RegionSize = kPageSize;
    info->State = MEM_COMMIT;
    info->Protect = g_protect[page];
    info->Type = MEM_IMAGE;
    return sizeof(*info);
}

BOOL TestVirtualProtect(void* address, SIZE_T, DWORD protect, PDWORD previous)
{
    ++g_protectCalls;
    if (g_failProtect)
        return FALSE;
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    if (at < g_base || at >= g_base + kImageSize)
        return FALSE;
    const std::size_t page = (at - g_base) / kPageSize;
    if (previous != nullptr)
        *previous = g_protect[page];
    g_protect[page] = protect;
    return TRUE;
}

HMODULE TestGetModuleHandleW(LPCWSTR name)
{
    if (name != nullptr)
        return nullptr;
    return reinterpret_cast<HMODULE>(g_base);
}

BOOL TestGetModuleHandleExW(DWORD, LPCWSTR, HMODULE* module)
{
    ++g_pinCalls;
    if (g_failPin)
        return FALSE;
    if (module != nullptr)
        *module = reinterpret_cast<HMODULE>(g_base);
    return TRUE;
}

void* TestInterlockedCompareExchangePointer(void* volatile* destination, void* exchange, void* comparand)
{
    ++g_exchangeCalls;
    void* old = *destination;
    if (old == comparand)
    {
        *destination = exchange;
        if (g_onExchange != nullptr)
            g_onExchange(); // the game thread taking the slot at the instant it changes
    }
    return old;
}

namespace
{
void CallObserverFromSlot()
{
    auto** vtable = reinterpret_cast<void**>(g_image + kVtableAt);
    const auto observer = reinterpret_cast<bool (*)(void*)>(vtable[4]);
    g_raceObserverResult = observer(reinterpret_cast<void*>(g_image + kInstanceAt));
}
} // namespace
