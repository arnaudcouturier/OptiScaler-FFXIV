#pragma once
// Automatic ReShade scene depth: what runs at every NGX evaluate (the capture glue). The facade (ReShadeDepth.cpp) builds one
// CaptureEngine over the real process, tests/ext/reshade/reshade_depth_eval.cpp builds one over a fake host, a fake NVSDK_NGX_Parameter
// and a WARP device. Only this engine ever touches the copy pass (DepthCopy has no lock; it runs on the thread that owns the immediate
// context, the NGX evaluate thread): whoever else wants something done to it (the option was switched off, NGX ends) leaves a request
// in the EvalGate word, which the next OnEvaluated carries out.
//
// The order of the work, cheapest first:
//   1. one relaxed load of the gate word: nothing to do when the option is off and nothing is pending;
//   2. requests (withdraw, release) from other threads;
//   3. the option, shutdown, registration (a ReShade that loaded late is looked for, at most once a second), the subscription (done at load; only a late ReShade's is made here);
//   4. a consumer: some effect runtime asked for depth lately (else a published capture is withdrawn);
//   5. the sizes of this evaluate: render = per-axis minimum of the subrect and Width/Height, output = OutWidth/OutHeight, else the
//      Output texture, else the display size of the feature; a capture is made only when the output is the size of the consumer's back
//      buffer (that is the character-preview filter, and the size of the copy);
//   6. the game's Depth resource, its device's immediate context (the NATIVE one: NGX's own may be ReShade's proxy, whose state swaps
//      would go through ReShade), the copy.

#include "ext/reshade/ReShadeDepth.h"
#include "ext/reshade/ReShadeDepthCopy.h"
#include "ext/reshade/ReShadeDepthGlue.h"
#include "ext/reshade/ReShadeSeams.h" // ID3D11DeviceContext, NVSDK_NGX_Parameter

#include <atomic>
#include <cstdint>

namespace ext::reshade::depth
{
// What the engine asks of the process.
struct IEvalHost
{
    virtual ~IEvalHost() = default;
    virtual bool ShuttingDown() = 0;
    virtual int64_t NowMs() = 0;
    virtual uint32_t ThreadId() = 0;
    virtual bool Registered() = 0;
    // One late attempt (the planner decides if it is time); true when this call registered ReShade.
    virtual bool TryLateRegister(bool enabled, int64_t nowMs) = 0;
    // Subscribes the depth events if that is not done (cheap when it is, or when it failed for good).
    virtual void EnsureSubscribed() = 0;
    virtual ConsumerInfo GetConsumer(int64_t nowMs) = 0;
    // The immediate context the lighting hooks sit on, null when they are not installed; and the raw (unhooked) Draw that goes with it.
    virtual ID3D11DeviceContext* HookedContext() = 0;
    virtual DrawTriangleFn RawDraw() = 0;
};

// The display size the feature was created with, for the Output size of last resort. Null `get` = none.
struct DisplaySource
{
    glue::Extent (*get)(void* feature) = nullptr;
    void* feature = nullptr;
};

class CaptureEngine
{
  public:
    // `downstream` receives every capture the pass publishes (valid = false withdraws); it must outlive the engine.
    CaptureEngine(glue::EvalGate& gate, std::atomic<uint64_t>& frame, ICaptureSink& downstream, IEvalHost& host);
    // Test only: the process never destroys the engine. Releases the pass (publishing valid = false first).
    ~CaptureEngine();
    CaptureEngine(const CaptureEngine&) = delete;
    CaptureEngine& operator=(const CaptureEngine&) = delete;

    // The seam, on the NGX evaluate thread. `context` is the context NGX was given (possibly ReShade's proxy): it is only logged.
    void OnEvaluated(ID3D11DeviceContext* context, NVSDK_NGX_Parameter* parameters, const DisplaySource& display) noexcept;
    // NGX is going away. On the evaluate thread the pass is released at once, from any other thread it is left to the next OnEvaluated.
    void OnNgxShutdown() noexcept;

    // Read by the status line and the tests, any thread.
    bool NativeContextMissing() const { return _nativeMissing.load(std::memory_order_relaxed); }
    uint32_t EvalThread() const { return _evalThread.load(std::memory_order_relaxed); } // 0 until the first evaluate
    CaptureStats Stats() const;
    bool PassReady() const { return _copy != nullptr && _copy->Ready(); } // the evaluate thread's, or when it is not running

  private:
    void Run(ID3D11DeviceContext* context, NVSDK_NGX_Parameter* parameters, const DisplaySource& display);
    DepthCopy& PassFor(bool rawDraw);
    bool Guarded(void (*thunk)(void*), void* arg, const char* what);

    glue::EvalGate& _gate;
    std::atomic<uint64_t>& _frame;
    ICaptureSink& _downstream;
    IEvalHost& _host;

    std::atomic<uint32_t> _evalThread { 0 };
    std::atomic<uint64_t> _captured { 0 }, _refused { 0 };
    std::atomic<uint32_t> _lastResult { 0 };
    std::atomic<bool> _nativeMissing { false };

    // the evaluate thread's
    DepthCopy* _copy = nullptr; // created at the first capture, once the draw it wants to pass is known
    bool _copyUsesRawDraw = false;
    glue::ChangeTracker _captureLine;
    glue::ResultLog _resultLog;
    bool _identityLogged = false;
    bool _faultLogged = false;
};
} // namespace ext::reshade::depth
