#pragma once
// The bodies of the two native detours of the source-level jitter fix ([FFXIV] JitterSource), as templates over the game's functions and the
// game-memory reads. FfxivJitterSourceHooks.cpp keeps only the Windows glue (the return address, the Detours trampolines, ReadProcessMemory, the
// clock); tests/ext/ffxiv/jitter/jitter_source_detours.cpp drives these bodies with a fake generator, a fake camera getter and fake reads, so the
// code that WRITES the game's memory is covered: the argument order, the record before the division, the caller and switch tests, which object is
// touched, the plan gates.
//
// The return address is read by the real detour (a template that called _ReturnAddress() itself would see its own caller) and passed in.

#include "ext/ffxiv/jitter/FfxivJitterSourceCore.h"

#include <cstring>

namespace ext::ffxiv::jitter::source
{
struct GenerateArgs
{
    std::uintptr_t caller = 0;          // the detour's return address
    std::uintptr_t expectedCaller = 0;  // exe + generatorCallReturnRva; 0 = no build, nothing to compare with
    std::uintptr_t opaque = 0;
    float* x = nullptr;                 // P + 0: the jitter object's first float
    float* y = nullptr;                 // P + 4
    unsigned frame = 0;
    unsigned phases = 0;
};

// The detour of the jitter generator. Only the camera update's call (the caller test) for the WORLD camera's jitter object (the pointer the
// camera getter returned to the NGX packet producer) is touched. Every other caller, every other object, every call while the world object is not
// yet known and every call while the fix is off goes straight to the game's function with the game's own arguments.
//   generator(opaque, x, y, frame, phases) -> result   the game's function (the trampoline)
//   readInputs(phases, GeneratorInputs&) -> bool        the game's settings, extents and sizes; false = a read failed (the call is left alone)
//   nowMs() -> std::uint64_t
template <class Generator, class ReadInputs, class Clock>
std::uintptr_t RunGenerate(SharedState& state, const GenerateArgs& a, Generator&& generator, ReadInputs&& readInputs, Clock&& nowMs)
{
    if (!state.Enabled() || a.x == nullptr || a.y == nullptr || a.expectedCaller == 0 || a.caller != a.expectedCaller)
        return generator(a.opaque, a.x, a.y, a.frame, a.phases);

    // The set of objects this call site is reached with (for the one diagnostic line), then the world-camera tie: another jittered object would be
    // divided by the world camera's strength and drawn on the +-0.83 px lattice without anybody telling the upscaler.
    try
    {
        state.NoteCallSiteObject(reinterpret_cast<std::uintptr_t>(a.x));
    }
    catch (...)
    {
    }
    const std::uintptr_t world = state.WorldObject();
    if (world == 0 || reinterpret_cast<std::uintptr_t>(a.x) != world)
        return generator(a.opaque, a.x, a.y, a.frame, a.phases);

    GeneratorInputs in;
    GeneratorPlan plan;
    bool planned = false;
    try
    {
        if (readInputs(a.phases, in))
        {
            plan = PlanGenerate(in);
            planned = true;
        }
    }
    catch (...)
    {
    }
    if (!planned || plan.why != PlanWhy::Normalize)
    {
        state.NoteLeftAlone(planned ? plan.why : PlanWhy::StrengthInvalid);
        return generator(a.opaque, a.x, a.y, a.frame, a.phases);
    }

    const std::uintptr_t result = generator(a.opaque, a.x, a.y, a.frame, plan.phases);
    try
    {
        Generated g;
        g.expectedX = *a.x; // the generator's own Halton value, recorded BEFORE the division
        g.expectedY = *a.y;
        g.normalized = NormalizeHalton(*a.x, *a.y, plan.strength);
        g.outX = *a.x;
        g.outY = *a.y;
        g.strength = plan.strength;
        g.phases = plan.phases;
        g.renderW = in.renderW;
        g.renderH = in.renderH;
        g.outW = in.outW;
        g.outH = in.outH;
        g.timeMs = nowMs();
        state.RecordGenerated(g, PlanWhy::Normalize);
    }
    catch (...)
    {
    }
    return result;
}

struct ObserveArgs
{
    std::uintptr_t caller = 0;          // the detour's return address
    std::uintptr_t expectedCaller = 0;  // exe + producerReturnRva; 0 = no build
    std::uintptr_t camera = 0;          // the getter's argument
};

// The detour of the camera getter: observe only. The world-scene NGX packet producer is the one caller sampled; the pointer it gets is the game's
// own, unchanged. That pointer is the world camera's jitter object, which the generator hook is tied to.
//   getter(camera) -> void*                      the game's function (the trampoline)
//   readObject(const void*, float (&)[4]) -> bool   the jitter object { jx, jy, ndcX, ndcY }
//   readRenderSize(std::uint32_t (&)[2]) -> bool    the renderer's scene size (width, height)
template <class Getter, class ReadObject, class ReadRenderSize, class Clock>
void* RunObserve(SharedState& state, const ObserveArgs& a, Getter&& getter, ReadObject&& readObject, ReadRenderSize&& readRenderSize, Clock&& nowMs)
{
    void* result = getter(a.camera);
    if (result == nullptr || !state.Enabled() || a.expectedCaller == 0 || a.caller != a.expectedCaller)
        return result;
    try
    {
        state.LearnWorldObject(reinterpret_cast<std::uintptr_t>(result));
        // read twice: a torn read (the game writing it) is not recorded
        float first[4] {}, second[4] {};
        std::uint32_t size[2] {};
        if (readObject(static_cast<const void*>(result), first) && readObject(static_cast<const void*>(result), second) &&
            std::memcmp(first, second, sizeof(first)) == 0 && readRenderSize(size))
        {
            Observed o;
            o.rawX = first[0];
            o.rawY = first[1];
            o.ndcX = first[2];
            o.ndcY = first[3];
            o.width = size[0];
            o.height = size[1];
            o.timeMs = nowMs();
            if (ProjectOffset(o))
                state.RecordObserved(o);
        }
    }
    catch (...)
    {
    }
    return result;
}
} // namespace ext::ffxiv::jitter::source
