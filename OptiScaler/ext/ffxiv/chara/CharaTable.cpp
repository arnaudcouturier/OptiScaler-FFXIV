#include "pch.h"

#include "ext/core/ProcessMemory.h"
#include "ext/ffxiv/GameBuild.h"
#include "ext/ffxiv/chara/CharaTable.h"

#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <vector>

namespace ext::ffxiv::chara::table
{
namespace
{
constexpr size_t kHeaderBytes = 0x1000;
constexpr size_t kScanChunk = 1u << 20;

// The feature is off for this session; this writes the one log line about it.
Resolution Inert(std::string reason)
{
    Resolution r;
    r.state = AccessorState::Inert;
    LOG_WARN("FFXIV character preview: switched off for this session ({})", reason);
    r.reason = std::move(reason);
    return r;
}

Resolution DoResolve()
{
    const GameBuild* build = MatchCurrentProcess();
    if (build == nullptr)
        return Inert("this game build is not the one the preview support was checked against");
    const CharaViewBuild& cv = build->charaView;

    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::vector<uint8_t> headers(kHeaderBytes);
    if (base == 0 || !ext::core::ReadProcessBytes(base, headers.data(), headers.size()))
        return Inert("the executable headers could not be read");

    Section text;
    if (!FindSection(headers.data(), headers.size(), ".text", text))
        return Inert("the executable has no readable .text section");
    if (text.size > cv.maxTextBytes)
        return Inert("the .text section is larger than the scan limit");

    const auto started = std::chrono::steady_clock::now();
    std::vector<uint8_t> scratch(kScanChunk + cv.pattern.size());
    bool unreadable = false;
    const ScanResult scan = ScanRegion(
        [&](size_t offset, void* out, size_t bytes) { return ext::core::ReadProcessBytes(base + text.rva + offset, out, bytes); },
        text.size, cv.pattern.data(), cv.patternMask.data(), cv.pattern.size(), kScanChunk, scratch.data(), unreadable);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();

    if (unreadable)
        return Inert("the executable code could not be read for the accessor scan");
    if (scan.hits != 1)
        return Inert(std::format("the preview accessor pattern was found {} times in the executable (exactly one is needed)",
                                 scan.hits));

    const uint32_t matchRva = text.rva + static_cast<uint32_t>(scan.firstOffset);
    int32_t disp = 0;
    if (!ext::core::ReadProcessValue(base + matchRva + 3, disp))
        return Inert("the accessor displacement could not be read");

    const int64_t globalRva = RipTargetRva(matchRva, cv.ripInstructionLength, disp);
    if (globalRva <= 0 || globalRva >= static_cast<int64_t>(UINT32_MAX))
        return Inert("the accessor points outside the executable image");

    // Cross-check against the values documented for the matched build. A matched build with a different hit means the
    // documentation or the scan is wrong: stay off rather than guess.
    if (matchRva != cv.accessorRva || static_cast<uint32_t>(globalRva) != cv.managerPointerRva)
    {
        return Inert(std::format("the accessor is at RVA {:#x} and the manager pointer at RVA {:#x}, the matched build documents "
                                 "{:#x} and {:#x}",
                                 matchRva, static_cast<uint64_t>(globalRva), cv.accessorRva, cv.managerPointerRva));
    }

    Resolution r;
    r.state = AccessorState::Ready;
    r.accessorRva = matchRva;
    r.managerPointerRva = static_cast<uint32_t>(globalRva);
    r.managerPointerAddress = base + static_cast<uintptr_t>(globalRva);
    LOG_INFO("CharaView native lookup: verified accessor available true (accessor RVA {:#x}, manager pointer RVA {:#x}, "
             "scan {} ms over {} bytes)",
             r.accessorRva, r.managerPointerRva, ms, text.size);
    return r;
}

Snapshot LoadSnapshot(const Resolution& r)
{
    Snapshot s;
    s.ready = true; // the accessor is verified; an empty table only means the renderer does not exist yet

    uint64_t manager = 0;
    if (!ext::core::ReadProcessValue(r.managerPointerAddress, manager) || manager == 0)
        return s;

    const Layout layout {};
    std::array<uint8_t, 0x400> copy {};
    if (layout.managerBytes > copy.size() || !ext::core::ReadProcessBytes(static_cast<uintptr_t>(manager), copy.data(), layout.managerBytes))
        return s;

    return ParseManager(copy.data(), layout.managerBytes, layout,
                        [](uintptr_t address, void* out, size_t bytes) { return ext::core::ReadProcessBytes(address, out, bytes); });
}
} // namespace

namespace
{
std::once_flag g_once;
Resolution g_resolution;
std::atomic<bool> g_resolved { false };
} // namespace

const Resolution& Resolve()
{
    std::call_once(g_once,
                   []
                   {
                       g_resolution = DoResolve();
                       g_resolved.store(true, std::memory_order_release);
                   });
    return g_resolution;
}

bool Ready() { return Resolve().state == AccessorState::Ready; }

const char* InertReason()
{
    // Never starts the (one-time) executable scan: the menu asks this while the feature is off.
    if (!g_resolved.load(std::memory_order_acquire))
        return "";
    return g_resolution.state == AccessorState::Inert ? g_resolution.reason.c_str() : "";
}

const Snapshot& Current(uint64_t nowMs)
{
    static const Snapshot notReady {};
    const Resolution& r = Resolve();
    if (r.state != AccessorState::Ready)
        return notReady;

    thread_local Cache cache;
    return cache.Get(nowMs, [&] { return LoadSnapshot(r); });
}

void LogTable(const char* when, const Snapshot& s)
{
    LOG_INFO("CharaView table ({}): {} of {} entries valid", when, s.ValidCount(), kEntries);
    for (size_t i = 0; i < kEntries; ++i)
    {
        const Entry& e = s.entries[i];
        if (e.Valid())
            LOG_INFO("CharaView table ({}): entry {} ({}) texture {:#x} {}x{}", when, i, EntryRole(i), e.resource, e.width,
                     e.height);
    }
}
} // namespace ext::ffxiv::chara::table
