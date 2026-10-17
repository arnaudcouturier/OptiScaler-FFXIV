#pragma once
// The interface between the preview dispatcher and the isolated-DLSS implementation. The dispatcher calls these functions
// and nothing else of the DLSS code; the implementation lives behind them (CharaDlssStub.cpp until the DLSS path exists:
// the implementation replaces that file and keeps these signatures).
//
// Contract:
//  * All functions run on the thread that issues draws on the immediate context, except GetCounters/StatusNote (menu).
//  * Around is called for every draw of the immediate context while [FFXIV] PreviewDLSS is on and Active() is true, after
//    the dispatcher classified it (view.kind) and before anything else ran. Draws that are certainly not preview draws (no
//    role on the pixel shader and neither render target nor depth texture in the table) are not offered; the rest are,
//    also when view.kind is Ignore. It returns true when it ran the draw (it called
//    `draw` natively once, plus at most one replay of the same draw under ReplayScope: the callable is the bare original
//    call); false makes the dispatcher call `draw` itself. It must never leave pipeline state changed.
//  * The pass bookkeeping (view.pass) belongs to the dispatcher: the pass starts at the first accepted geometry draw
//    (view.passStarted is true for exactly that draw) and ends at a rejected finisher or at the end of the frame.
//    For a finisher, view.finisherAccepted is the verdict of the finisher check, evaluated before the draw from the state
//    the draw runs with; the dispatcher ends the pass itself when it is false, after Around returned.
//  * A replay of a native draw by another module (the HDR mask replays) never reaches Around: the dispatcher skips it.
//  * The event hooks below are called only while Active() is true. They run inside the game's own calls: keep them cheap.

#include <d3d11.h>

#include "ext/ffxiv/chara/CharaDraw.h"

namespace ext::ffxiv::chara::dlss
{
struct Counters
{
    uint32_t activeSlots = 0;     // slots that own an upscaler feature
    uint64_t evaluations = 0;     // successful evaluations
    uint64_t fallbacks = 0;       // renders that kept the native image
};

// True when the DLSS path is compiled in (false in the placeholder implementation).
bool Implemented();

// Implemented, switched on in the config and not failed for this session.
bool Active();

bool Around(const DrawView& view, DrawCall draw);

// CopyResource / CopySubresourceRegion on the immediate context, after the native copy ran (`full` = whole subresource 0).
void OnCopy(ID3D11DeviceContext* context, ID3D11Resource* dest, ID3D11Resource* source, bool full);

// ClearRenderTargetView on the immediate context, after the native clear ran.
void OnClearRenderTarget(ID3D11DeviceContext* context, ID3D11RenderTargetView* view, const FLOAT color[4]);

// Begin / End on the immediate context (query bookkeeping: a replayed draw must not run inside an open occlusion query).
void OnQueryBegin(ID3D11DeviceContext* context, ID3D11Asynchronous* query);
void OnQueryEnd(ID3D11DeviceContext* context, ID3D11Asynchronous* query);

// A vertex shader was created (bytecode available for the call only).
void OnVertexShaderCreated(ID3D11Device* device, ID3D11VertexShader* shader, const void* bytecode, SIZE_T size);

// Once per presented frame (when the host calls the frame callback), on the draw thread.
void OnFrameEnd();

// At least every 100 ms of draw activity, on the draw thread (idle cleanup when no frame callback arrives).
void Maintain(uint64_t nowMs);

// The hooks are being removed: release every GPU object.
void Detach();

Counters GetCounters();

// One line for the menu while the DLSS path is not fully available (empty when there is nothing to say).
const char* StatusNote();
} // namespace ext::ffxiv::chara::dlss
