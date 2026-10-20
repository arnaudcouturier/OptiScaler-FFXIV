#pragma once
// When the character-preview NR module has to look at the NR runtime selection again (review N1). Pure: CharaNr.cpp hands in the facts it
// holds and does what the answer says; tests/ext/ffxiv/chara/chara_nr_runtime_gate.cpp drives it THROUGH the real availability and
// plumbing rules.
//
// The point: the module's draw entry points (RunCoreSteps) are reached only while preview NR is usable, and both a model failure latch
// and a "no model" probe CLOSE that gate. Whatever clears them must therefore run from a place the closed gate does not stop: the
// maintenance call. The same observation is also made at the top of RunCoreSteps so an open gate reacts within the same draw.

#include <cstdint>

namespace ext::nr::runtime
{
struct PreviewRuntimeView
{
    bool inited = false;           // the module's compute pass, fences and backend exist
    bool modelMissing = false;     // the model probe says no model file
    bool modelLatched = false;     // a failure the model file caused ended preview NR (until another runtime is selected)
    uint64_t backendRevision = 0;  // the runtime revision the backend was acquired for
    uint64_t probedRevision = 0;   // the runtime revision the model probe was made for
    uint64_t latchedRevision = 0;  // the runtime revision that was current when the model latch was set
};

struct PreviewRuntimeActions
{
    bool clearModelLatch = false; // another model gets its own chance
    bool resync = false;          // re-acquire the backend (model path and revision) and let the streams rebuild
    bool reprobe = false;         // started without a model (or selected one that is not installed): probe the files again
};

inline PreviewRuntimeActions ObservePreviewRuntime(const PreviewRuntimeView& v, uint64_t currentRevision)
{
    PreviewRuntimeActions a;
    if (v.modelLatched && currentRevision != v.latchedRevision)
        a.clearModelLatch = true;
    if (v.inited)
        a.resync = currentRevision != v.backendRevision;
    else if (v.modelMissing)
        a.reprobe = currentRevision != v.probedRevision;
    return a;
}
} // namespace ext::nr::runtime
