#pragma once
// Screen-space reflections: the frame-exact A/B on the capture frame (the rules are in SsrAbRules.h).
//
// The one-frame dump (`DrawDiagnosticsDump = frame`) asks, at a water draw the reflections served in the capture frame, for the picture of what the
// reflections do to that draw: the same draw is issued ONCE MORE with the engine's own reflection map bound, into a private copy of the draw's render
// target that was made immediately before (so blending sees the same destination), and then the real draw runs with this module's map as it always
// does. The dump keeps the target before the extra draw (`ssr-ab-before`: the coverage), the private copy after it (`ssr-ab-engine`) and the game's
// own target after the real draw (`ssr-ab-ours`). The two pictures differ by the effect of the reflections and by nothing else.
//
// Which draw: the reader draws of the capture frame are offered in order (SsrAbRules.h: at most kMaxTries) and the first whose extra draw wrote at least
// the coverage floor is kept; an occlusion query around the extra draw counts its pixels (and one around the real draw of the kept draw counts its own:
// the two must agree). Every try is one line in facts.txt.
//
// What the extra draw may not change: the depth-stencil buffer (it is D24S8: neither depth nor stencil), any other target, any unordered access
// view, and any piece of pipeline state it touches (the explicit-state guard restores the output merger and the depth-stencil state; the reflection
// slot is put back to this module's view). A draw that writes more than RT0, or binds an output-merger UAV, is declined with its reason and gets no
// extra draw.
//
// Cost when no capture is open: one relaxed load of the open mask per served reader draw, nothing else. No key: the one-frame dump is the switch.

#include "ext/ffxiv/diag/StageDump.h"
#include "ext/ffxiv/engine/SsrAbRules.h"
#include "ext/ffxiv/engine/SsrApply.h"

#include <d3d11_1.h>
#include <wrl/client.h>

#include <cstdint>

namespace ext::ffxiv::engine::ssr::ab
{
using DrawFn = void (*)(void* user);

// One served reader draw's A/B. It lives on the draw hook's stack between Begin and End.
struct Call
{
    ID3D11DeviceContext* context = nullptr;
    const BoundReader* reader = nullptr; // the draw's swap guard: its slot and the engine's view are what the extra draw binds
    DrawFn draw = nullptr;               // the draw the real call runs (what the draw path would have run without the A/B)
    void* user = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> target; // the game's RT0 texture, for the picture after the real draw
    ext::ffxiv::diag::StageWindow window {};        // the viewport the draw worked in
    uint32_t psCrc = 0;                             // the draw's pixel shader, by the CRC the capture names it with
    uint32_t framePosition = 0;                     // the draw's position in the capture frame
    uint32_t tryIndex = 0;                          // the draw's number among the capture frame's offers (from 1)
    bool active = false;                            // the thunk was installed: End takes the picture after the real draw
    bool shadowRan = false;                         // the extra draw was issued
    bool accepted = false;                          // the draw was kept: its pictures are queued, End takes the third
    // What the kept draw's facts line says, and the query around its real draw.
    Microsoft::WRL::ComPtr<ID3D11Query> realQuery;
    bool extraKnown = false;
    uint64_t extraSamples = 0;
    uint32_t slot = 0, targetWidth = 0, targetHeight = 0, targetFormat = 0, stencilWriteMask = 0;
    bool depthBound = false, hadDepthState = false, depthWrites = false;
};

// Called right after the swap guard served the draw. While no capture is open it returns false at one relaxed load. When this draw is the capture
// frame's A/B draw and the A/B can run, `draw` and `user` are replaced by a thunk that issues the extra draw and then the real one (exactly once),
// and the function returns true. A draw that is declined is left untouched, and the decline is written once.
bool Begin(Call& call, ID3D11DeviceContext* context, const BoundReader& reader, DrawFn& draw, void*& user);

// Called after the draw path returned: the game's target after the real draw, as the third picture. A call that did not run does nothing.
void End(Call& call);

// The copy of a depth-stencil state the extra draw binds: the same tests and operations, with depth writes OFF and the stencil write mask 0.
D3D11_DEPTH_STENCIL_DESC ShadowDepthStencilDesc(const D3D11_DEPTH_STENCIL_DESC& in);

// What a session did, for the tests and the capture: extra draws issued, draws kept, draws skipped for their coverage, kept with the coverage unknown,
// declines per reason.
uint32_t Runs();
uint32_t Kept();
uint32_t SkippedEmpty();
uint32_t SkippedSmall();
uint32_t KeptUnknown();
uint32_t Declines(Decline reason);
// The occlusion queries that count a draw's pixels can be switched off (a test of the "coverage unknown" outcome); on by default.
void SetCoverageQueryForTest(bool on);
// Forgets the session's counters and the once-a-session log gate (a test starts every case from the same state).
void ResetForTest();
} // namespace ext::ffxiv::engine::ssr::ab
