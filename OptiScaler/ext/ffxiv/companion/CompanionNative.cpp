#include "pch.h"
#include "CompanionNative.h"
#include "CompanionPackets.h"
#include "CompanionRuntime.h"
#include "CompanionGpu.h"
#include "CompanionLayer.h"
#include "ext/core/DetourThreads.h"
#include "ext/ffxiv/resolve/Gate.h"
#include "ext/ffxiv/resolve/ModulePath.h"
#include "ext/ffxiv/ui/PlatePackets.h" // ext: developer probe of the UI draws (log only)
#include "replay/FfxivNameplateLive.h"
#include <detours/detours.h>
#include <filesystem>

namespace ext::ffxiv::companion::Native
{
using Batch = void(__fastcall*)(uintptr_t, char);
static Batch batch16 = nullptr, batch32 = nullptr;
using Enqueue=int(__fastcall*)(uintptr_t,uintptr_t);
using BindState=void(__fastcall*)(uintptr_t,uintptr_t,UINT);
static Enqueue enqueue=nullptr;
static BindState bindState=nullptr;
static int __fastcall OnEnqueue(uintptr_t context,uintptr_t command)
{Layer::EmitVisibilityCommand(command);ext::ffxiv::ui::plate::OnEnqueue(command);return enqueue(context,command);}
static void __fastcall OnBindState(uintptr_t context,uintptr_t state,UINT instances)
{bindState(context,state,instances);Layer::BindVisibilityCommand(context,state);ext::ffxiv::ui::plate::OnBindState(context,state);}
static uintptr_t base = 0;
static std::atomic<bool> installed {false}, requested {false};
static std::atomic<uint64_t> captures {0}, replacements {0}, packetCount {0}, fallbacks {0};
struct Pending
{
    Packets::Queue before;
    std::vector<Packets::Packet> packets;
    std::vector<Packets::Entry> entries;
    ULONGLONG capturedAt = 0;
    bool begun = false, inside = false;
};
static thread_local Pending pending;

static Packets::Queue Queue()
{
    uintptr_t module = 0, renderer = 0, queue = 0;
    const auto& build = ext::ffxiv::ActiveBuild().companion;
    if (!Packets::Read(base + build.nameplateModulePointerRva, module) || !Packets::Read(module + build.moduleStatePointerOffset, renderer) ||
        !Packets::Read(renderer + build.stateQueuePointerOffset, queue)) return {};
    const auto snapshot = Packets::Snapshot(queue);
    // Confirm the address in the first in-game log: once for the first valid queue, once for the first populated one.
    static std::atomic<bool> validLogged {false}, populatedLogged {false};
    if (snapshot.address && !validLogged.exchange(true))
        LOG_INFO("Companion native submissions: first queue snapshot address={:#x} head={:#x} count={}", snapshot.address, snapshot.head, snapshot.count);
    if (snapshot.count && !populatedLogged.exchange(true))
        LOG_INFO("Companion native submissions: first non-empty queue snapshot address={:#x} count={}", snapshot.address, snapshot.count);
    return snapshot;
}
static void Begin() noexcept
{
    if (pending.inside) return;
    pending.begun = false;
    if (!pending.packets.empty()) { ++fallbacks; pending.packets.clear(); }
    if (!requested.load() || !installed.load() || pending.inside) return;
    try
    {
        Layer::BeginSnapshot();
        pending.before = Queue();
        pending.begun = pending.before.address != 0;
    }
    // Begin() is noexcept but reaches Layer::BeginSnapshot()/Queue() (Packets::Snapshot), which
    // is not guaranteed not to throw (e.g. std::bad_alloc), and is invoked across the DLL export
    // boundary (OptiScalerCompanion_BeginNamePlateV1). An escaping exception here is
    // std::terminate() territory on top of being UB at the boundary. pending.begun was already
    // set false above, before the risky calls, so a throw just means "begin" never completed --
    // mirror End()'s own catch (fallbacks counter, drop any stale frame state).
    catch (...) { ++fallbacks; Layer::InvalidateFrame(); }
}
static void End() noexcept
{
    if (pending.inside) return;
    const bool begun = pending.begun;
    pending.begun = false;
    if (!begun || !requested.load() || !installed.load()) return;
    try
    {
        if (!Packets::Capture(pending.before, Queue(), pending.packets)) { ++fallbacks; Gpu::Clear(); Layer::InvalidateFrame(); return; }
        Gpu::Publish(pending.packets);
        Layer::Capture(pending.before);
        if (pending.packets.empty()) return;
        pending.capturedAt = GetTickCount64();
        ++captures;
    }
    catch (...) { pending.packets.clear(); ++fallbacks; Gpu::Clear(); Layer::InvalidateFrame(); }
}
static void Invoke(Batch original, uintptr_t renderer, char mode)
{
    if (requested.load() && !pending.inside && Layer::ReplaceBatch(original,renderer,mode))
    { pending.packets.clear(); return; }
    if (!requested.load() || pending.inside || pending.packets.empty())
    { original(renderer, mode); return; }
    if (GetTickCount64() - pending.capturedAt > 250)
    { pending.packets.clear(); ++fallbacks; original(renderer, mode); return; }
    uintptr_t list = 0;
    uint32_t count = 0;
    // Retain bounded capacity between frames instead of allocating a command list
    // on every native UI submission. No additional queue or frame delay is introduced.
    auto& entries = pending.entries;
    auto match = Packets::Match::Invalid;
    try
    {
        if (Packets::Read(renderer + ext::ffxiv::ActiveBuild().companion.rendererBatchListOffset, list) && Packets::Read(renderer + ext::ffxiv::ActiveBuild().companion.rendererBatchCountOffset, count) &&
            count && count <= Packets::MaxEntries)
        {
            entries.resize(count);
            if (Packets::ReadBytes(list, entries.data(), entries.size() * sizeof(entries[0])))
                match = Packets::Substitute(entries, pending.packets);
        }
    }
    catch (...) { match = Packets::Match::Invalid; }
    // A different batch can be consumed first. Leave it intact and await the owning batch.
    if (match == Packets::Match::None) { original(renderer, mode); return; }
    if (match != Packets::Match::Complete)
    { pending.packets.clear(); ++fallbacks; original(renderer, mode); return; }
    {
        Packets::PointerScope scope;
        if (!scope.Enter(renderer + ext::ffxiv::ActiveBuild().companion.rendererBatchListOffset, list, reinterpret_cast<uintptr_t>(entries.data())))
        { pending.packets.clear(); ++fallbacks; original(renderer, mode); return; }
        struct Guard { Guard() { pending.inside = true; } ~Guard() { pending.inside = false; } } guard;
        // The verified batch reads headers synchronously and derives indices into the
        // game's existing vertex allocation. Only our header/list addresses differ.
        // Original is called exactly once, at its native position among other UI draws.
        original(renderer, mode);
        packetCount += pending.packets.size();
        const auto completed = ++replacements;
        if (completed == 1 || completed % 600 == 0)
            LOG_INFO("Companion native submissions: batches {}, packets {}, fallback {}, age {} ms",
                completed, packetCount.load(), fallbacks.load(), GetTickCount64() - pending.capturedAt);
    } // Restore the engine list before releasing any of our command headers.
    pending.packets.clear();
}
struct VisibilityScope
{
    bool active=false;
    VisibilityScope(uintptr_t renderer,UINT bytes)
    {if(requested.load() && !pending.inside)active=Layer::BeginVisibilityNative(base,renderer,bytes,pending.packets,pending.capturedAt);}
    ~VisibilityScope(){if(active)Layer::EndVisibilityNative();}
};
static void __fastcall OnBatch16(uintptr_t renderer, char mode) { VisibilityScope scope(renderer,2);ext::ffxiv::ui::plate::BatchScope probe(base,renderer,2);Invoke(batch16, renderer, mode); }
static void __fastcall OnBatch32(uintptr_t renderer, char mode) { VisibilityScope scope(renderer,4);ext::ffxiv::ui::plate::BatchScope probe(base,renderer,4);Invoke(batch32, renderer, mode); }

void Install()
{
    if (installed.load() || batch16) return;
    // the exe name, read without the MAX_PATH cut (the gate and the game selector read it the same way)
    const std::wstring executable = ext::ffxiv::resolve::ReadModulePath([](wchar_t* buffer, std::uint32_t size) { return GetModuleFileNameW(nullptr, buffer, size); });
    if (!ext::ffxiv::resolve::IsFfxivExeName(std::wstring_view(executable))) return;
    base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    // The build gate (ext/ffxiv/resolve/Gate.h) replaces the old exact-build match: every companion seam usable (location AND shape of the four
    // Detour sites), on an unknown build the key on, and under [FFXIV] FfxivLegacyGameBuild exactly the old "this is the known build". Seams.cpp only
    // calls Install() when the gate said yes; this repeats the test so a direct caller cannot bypass it. The gate's own [ffxiv-gate] line says why.
    if (!ext::ffxiv::FeatureEnabled(ext::ffxiv::resolve::Feature::CompanionNative))
    { LOG_INFO("Companion native submissions: the build gate does not allow the hooks; no batch hooks installed"); return; }
    const auto& build = ext::ffxiv::ActiveBuild().companion;
    // the header and queue-entry offsets index fixed buffers: refuse an offset outside them (the gate checks the same bounds)
    if (!ext::ffxiv::CompanionOffsetsInBounds(build))
    { LOG_WARN("Companion native submissions: a batch header offset is outside the header; no batch hooks installed"); return; }
    // Current disk SHA256 was verified as B38840761E2677128B6BEC1CEAEE3F2870CD84E1D38AB7958216373058383780. In-process guards also reject
    // patches/hook detours at either entry point instead of assuming disk identity suffices.
    std::array<unsigned char, 16> a, b;
    if (!Packets::Read(base + build.batch16Rva, a) || !Packets::Read(base + build.batch32Rva, b) ||
        a != build.batchInstructions || b != build.batchInstructions)
    { LOG_WARN("Companion native submissions: batch byte guards rejected; native drawing unchanged"); return; }
    if (!Packets::Read(base + build.enqueueRva, a) || !Packets::Read(base + build.bindStateRva, b) ||
        a != build.enqueueInstructions || b != build.bindStateInstructions)
    {LOG_WARN("Companion native submissions: queue hook byte guards rejected");return;}
    // The UI module global is not byte-guarded (medium confidence). Read-only probe of the chain: it may be null
    // this early, so never refuse on it; the first in-game log confirms or refutes the address.
    {
        static bool chainLogged = false;
        if (!chainLogged)
        {
            chainLogged = true;
            constexpr auto plausible = [](uintptr_t p) { return p >= 0x10000 && p < 0x00007ffffffe0000ull; };
            uintptr_t module = 0, renderer = 0, queue = 0;
            if (Packets::Read(base + build.nameplateModulePointerRva, module) && plausible(module) &&
                Packets::Read(module + ext::ffxiv::ActiveBuild().companion.moduleStatePointerOffset, renderer) && plausible(renderer))
                Packets::Read(renderer + ext::ffxiv::ActiveBuild().companion.stateQueuePointerOffset, queue);
            LOG_INFO("Companion native submissions: queue chain module={:#x} renderer={:#x} queue={:#x}", module, renderer, queue);
        }
    }
    enqueue=reinterpret_cast<Enqueue>(base+build.enqueueRva);bindState=reinterpret_cast<BindState>(base+build.bindStateRva);
    batch16 = reinterpret_cast<Batch>(base + build.batch16Rva);
    batch32 = reinterpret_cast<Batch>(base + build.batch32Rva);
    // Single-thread enlistment only, deliberately, even though Detach() below enlists every
    // process thread. Install happens once per launch, early; Detours itself allocates (per
    // ext::core::DetourThreadScope's own comment, now verified against the real detours.cpp
    // source) while every enlisted thread sits suspended for the whole transaction, so enlisting
    // more threads here would spread a confirmed deadlock hazard to a path that fires on every
    // single session, not just the rare detach path. The crash risk multi-thread enlistment
    // exists to prevent (resuming a thread with its IP inside a patched prologue) is accepted
    // here, matching this codebase's existing convention at 120+ other single-thread Detours call
    // sites -- see the code-review ledger entry for the full trade-off.
    if (DetourTransactionBegin() != NO_ERROR) { batch16 = batch32 = nullptr; return; }
    bool ok = DetourUpdateThread(GetCurrentThread()) == NO_ERROR;
    ok &= DetourAttach(reinterpret_cast<PVOID*>(&batch16), OnBatch16) == NO_ERROR;
    ok &= DetourAttach(reinterpret_cast<PVOID*>(&batch32), OnBatch32) == NO_ERROR;
    ok &= DetourAttach(reinterpret_cast<PVOID*>(&enqueue), OnEnqueue) == NO_ERROR;
    ok &= DetourAttach(reinterpret_cast<PVOID*>(&bindState), OnBindState) == NO_ERROR;
    LONG result = ERROR_INVALID_FUNCTION;
    if (ok) result = DetourTransactionCommit(); else DetourTransactionAbort();
    if (result != NO_ERROR) { batch16 = batch32 = nullptr; return; }
    installed.store(true);
    beginNativeDraw.store(Begin);
    endNativeDraw.store(End);
    LOG_INFO("Companion native submissions: guarded batch hooks ready; substitution off until requested");
}
void Detach()
{
    // Companion::Unhook() wraps the whole Native::Detach() call in a catch-all that only
    // logs and otherwise papers over a partial failure; that must not be the only thing
    // standing between a thrown exception and leaving `installed`/hook state stale. Contain
    // Layer::Stop() specifically (it touches std::lock_guard/std::filesystem via the replay
    // capture path, both of which can throw under resource exhaustion) so our own cleanup
    // below -- and the already-late installed.store(false) further down, inside the
    // DetourTransactionCommit success branch -- still runs and stays truthful.
    //
    // Signal the nameplate-live worker directly first: it is only atomic stores, so it cannot
    // throw, and it must run even if Layer::Stop() throws before reaching its own internal call
    // to the same function below (otherwise JoinWorker() further down would wait out its full
    // timeout for a worker that was never actually told to stop).
    ext::ffxiv::companion::replay::FfxivNameplateLive::Stop();
    try { Layer::Stop(); }
    catch (...) { LOG_WARN("Companion native submissions: Layer::Stop() failed during teardown; continuing hook detach"); }
    Gpu::requested.store(false); Gpu::Clear();
    requested.store(false);
    beginNativeDraw.store(nullptr); endNativeDraw.store(nullptr);
    // Best-effort bounded wait for the nameplate-live worker thread to actually exit. Nothing
    // else in this teardown path waits for it, and an independent D3D11/DXGI/DirectComposition
    // present loop left running past Detach() is undesirable on its own regardless of this
    // function's own hook state; placed after the resets above so substitution is already off
    // for the duration of the wait.
    ext::ffxiv::companion::replay::FfxivNameplateLive::JoinWorker();
    if (!installed.load()) return;
    // Same enlistment as Install(): on detach specifically, if another thread's saved
    // instruction pointer sits inside a patched prologue when the trampoline is reverted,
    // resuming that thread unrelocated can crash the game.
    ext::core::DetourThreadScope threads;
    if (!threads.Ok())
    {
        LOG_WARN("Companion native submissions: unable to enlist game threads; hooks left attached");
        return;
    }
    if (DetourTransactionBegin() != NO_ERROR) return;
    bool ok = threads.Update() == NO_ERROR;
    ok &= DetourDetach(reinterpret_cast<PVOID*>(&batch16), OnBatch16) == NO_ERROR;
    ok &= DetourDetach(reinterpret_cast<PVOID*>(&batch32), OnBatch32) == NO_ERROR;
    ok &= DetourDetach(reinterpret_cast<PVOID*>(&enqueue), OnEnqueue) == NO_ERROR;
    ok &= DetourDetach(reinterpret_cast<PVOID*>(&bindState), OnBindState) == NO_ERROR;
    if (ok)
    {
        if (DetourTransactionCommit() == NO_ERROR) { installed.store(false); batch16 = batch32 = nullptr; }
    }
    else DetourTransactionAbort();
}
void SetRequested(bool value)
{
    requested.store(value && installed.load());
    if (!requested.load()) { Gpu::requested.store(false); Gpu::Clear(); Layer::Stop(); }
}
Statistics GetStatistics()
{
    return {installed.load(), requested.load(), captures.load(), replacements.load(), packetCount.load(), fallbacks.load()};
}
}
