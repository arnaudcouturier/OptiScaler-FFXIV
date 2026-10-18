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
//    (view.passStarted is true for exactly that draw) and ends at a rejected finisher or at the end of the frame. An accepted
//    finisher closes the render: the next prepass or G-buffer draw starts a new pass (a deliberate deviation from the
//    specification, where two renders of one frame share a pass). A geometry draw whose target is not the pass's size does not
//    belong to the pass and is left native.
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

// The SR path or preview NR can do work for a draw now (wanted, not latched, gate open). The dispatcher routes a draw to this plumbing
// only while this is true; Active() is also true while a latched or switched-off path only drains its objects.
bool CanRun();

// The SR path itself may do work (wanted, not latched, gate open) or still holds objects. The SR-only hooks (copy, clear, query)
// check this instead of Active(), which is also true for preview NR alone.
bool SrAvailable();

// Preview slots still hold GPU objects (images, NGX features), whatever the config says now. The dispatcher's gate and its
// maintenance stay on while this is true, so the objects are released after PreviewDLSS is switched off.
bool HoldsGpuObjects();

// Called for every draw the dispatcher classified while [FFXIV] PreviewDLSS is on, also when Active() is false: it keeps the
// NGX-device comparison current, which is what lets an unavailable path become available again.
void Probe(const DrawView& view);

// D3D11 NGX was initialised on `device` / is shutting down (from the NGX entry points, any thread). Identity only: the device
// pointer is not kept for later calls.
void OnNgxInitialised(ID3D11Device* device);
void OnNgxShutdown();

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

// The composed 2x image (the DLSS result with the native alpha) of the most recently evaluated preview slot, or null when that slot is
// not live any more (released, failed, not evaluated). Borrowed: valid until the slot is released; draw thread only. Used by the
// preview image dump.
ID3D11Texture2D* ComposedTexture();

// One line for the menu while the DLSS path is not fully available (empty when there is nothing to say).
const char* StatusNote();
} // namespace ext::ffxiv::chara::dlss
