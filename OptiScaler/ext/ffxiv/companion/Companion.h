#pragma once
#include <d3d11.h>

namespace ext::ffxiv::companion
{
using IndexedOriginal = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawCall = void(*)(void*);
struct CaptureCallbacks
{
    void (*tagShader)(ID3D11PixelShader*, const void*, SIZE_T) = nullptr;
    void (*tagPixel)(ID3D11PixelShader*, const void*, SIZE_T) = nullptr;
    void (*tagLayout)(ID3D11InputLayout*, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T) = nullptr;
    void (*observeDraw)(ID3D11DeviceContext*) = nullptr;
    bool (*interceptVisibilityIndexed)(ID3D11DeviceContext*, UINT, UINT, INT, IndexedOriginal) = nullptr;
    void (*aroundIndexed)(ID3D11DeviceContext*, UINT, UINT, DrawCall, void*) = nullptr;
};

bool Install(ID3D11Device* device) noexcept;
void Unhook() noexcept;
void AfterSharedCopy(ID3D11DeviceContext* context, ID3D11Texture2D* target) noexcept;
void DrawSourceMarkers(ID3D11DeviceContext* context, ID3D11Texture2D* target);
void DrawSettings();
bool DiagnosticsEnabled();
bool IsInstalled() noexcept;
CaptureCallbacks GetCaptureCallbacks() noexcept;
}
