#include "../../../OptiScaler/ext/ffxiv/companion/CompanionCallbackMap.h"
#include <cassert>
#include <cstdio>

// Mirrors the shape of Companion.h's CaptureCallbacks and FfxivLightingCapture.h's CompanionCallbacks without
// including either (both pull in d3d11.h; the lighting header also pulls detours.h, FfxivLightingScan.h and
// the HDR scene headers). Two distinct struct types stand in for the two real, independently-defined structs.
struct Src
{
    void* tagShader = nullptr;
    void* tagPixel = nullptr;
    void* tagLayout = nullptr;
    void* observeDraw = nullptr;
    void* interceptVisibilityIndexed = nullptr;
    void* aroundIndexed = nullptr;
};
struct Dst
{
    void* tagShader = nullptr;
    void* tagPixel = nullptr;
    void* tagLayout = nullptr;
    void* observeDraw = nullptr;
    void* interceptVisibilityIndexed = nullptr;
    void* aroundIndexed = nullptr;
};

int main()
{
    int a, b, c, d, e, f; // six distinct addresses; a swapped/misordered field assignment fails immediately.
    Src src;
    src.tagShader = &a;
    src.tagPixel = &b;
    src.tagLayout = &c;
    src.observeDraw = &d;
    src.interceptVisibilityIndexed = &e;
    src.aroundIndexed = &f;

    Dst dst = ext::ffxiv::companion::MapCaptureCallbacks<Dst>(src);
    assert(dst.tagShader == &a);
    assert(dst.tagPixel == &b);
    assert(dst.tagLayout == &c);
    assert(dst.observeDraw == &d);
    assert(dst.interceptVisibilityIndexed == &e);
    assert(dst.aroundIndexed == &f);

    // Null-default source (the inert default both real structs use) maps to an all-null destination.
    Dst inert = ext::ffxiv::companion::MapCaptureCallbacks<Dst>(Src {});
    assert(!inert.tagShader && !inert.tagPixel && !inert.tagLayout && !inert.observeDraw &&
           !inert.interceptVisibilityIndexed && !inert.aroundIndexed);

    puts("PASS: all six capture callbacks map field-by-field across distinct struct types; inert default stays null");
}
