#include "pch.h"

#include "ext/ffxiv/chara/CharaDlssApi.h"

// Placeholder for the isolated DLSS path: every entry point is inert, so a character preview always keeps the native image.
// The real implementation replaces this file (delete it, or the link fails on duplicate definitions) and keeps the
// signatures declared in CharaDlssApi.h.
namespace ext::ffxiv::chara::dlss
{
bool Implemented() { return false; }
bool Active() { return false; }
bool Around(const DrawView&, DrawCall) { return false; }
void OnCopy(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*, bool) {}
void OnClearRenderTarget(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]) {}
void OnQueryBegin(ID3D11DeviceContext*, ID3D11Asynchronous*) {}
void OnQueryEnd(ID3D11DeviceContext*, ID3D11Asynchronous*) {}
void OnVertexShaderCreated(ID3D11Device*, ID3D11VertexShader*, const void*, SIZE_T) {}
void OnFrameEnd() {}
void Maintain(uint64_t) {}
void Detach() {}
Counters GetCounters() { return {}; }
const char* StatusNote() { return "DLSS core: not in this build yet."; }
} // namespace ext::ffxiv::chara::dlss
