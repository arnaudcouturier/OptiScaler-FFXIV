#pragma once
// Signed motion for the isolated DLSS path: a private float target that receives the G-buffer pixel shader's fourth
// colour output, produced by issuing a G-buffer draw a second time. The game's own motion output is 8-bit and unsigned.
// Rules: CharaMotionRules.h. Everything here runs on the thread that draws on the immediate context.

#include <d3d11.h>

#include "ext/ffxiv/chara/CharaDraw.h"
#include "ext/ffxiv/chara/CharaMotionRules.h"

namespace ext::ffxiv::chara::motion
{
enum class Outcome : uint8_t
{
    NotReplayed, // the rules refused: the caller runs the native draw as usual
    Replayed,    // the replay ran and every state was restored
};

struct ReplayResult
{
    Outcome outcome = Outcome::NotReplayed;
    Skip skip = Skip::None;
};

// The private target at `width` x `height` (R16G16B16A16_FLOAT, render target and shader resource). Creates it, or
// recreates it when the size changed, and clears it to zero. False when it cannot be provided.
bool EnsureTarget(ID3D11Device* device, uint32_t width, uint32_t height);
// Clears the target to zero (the feature's own clear: the hooks ignore it).
void ClearTarget(ID3D11DeviceContext* context);
ID3D11ShaderResourceView* TargetSrv();
ID3D11Texture2D* TargetTexture();

// Issues `draw` once more into the private target when the rules allow it (the native draw is not run here). The caller
// has already set the viewport the draw should use. Never leaves pipeline state changed.
ReplayResult Replay(const DrawView& view, DrawCall draw);

// Whether the depth state bound now compares with GREATER / GREATER_EQUAL (a reversed depth buffer).
bool BoundDepthIsReversed(ID3D11DeviceContext* context);

// Query bookkeeping (hooks).
void OnQueryBegin(ID3D11Asynchronous* query);
void OnQueryEnd(ID3D11Asynchronous* query);
void OnFrameEnd();
void MarkQueriesBlind(); // the module (re)started: queries opened before it are unknown until the next frame end or 100 ms of maintenance
void OnMaintain(uint64_t nowMs);

uint64_t ReplayCount();
uint64_t SkipCount(Skip reason);

void Release();
} // namespace ext::ffxiv::chara::motion
