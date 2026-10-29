// The NR frame-replay machinery end to end on the Direct3D 12 software adapter, with a replay set the test writes itself.
// No capture, no game and no product build are needed: the shaders are compiled from source at run time, so the dispatch
// kinds below run the real NR compute passes (encode, downsample, resolve, multipass feedback, meter, guide match, the residual
// modes and the three spatial stages), plus two
// small probe shaders whose output is known to the pixel.
//
// What it proves:
//  - each dispatch kind gives identical bytes when replayed twice (the property the whole scheme rests on);
//  - the root signature, typeless resources with typed views, and depth planes round-trip through the copy and upload paths;
//  - a run with nothing blessed fails loudly; blessing writes references and provenance; comparing afterwards passes;
//  - a changed constant, or a changed shader, is reported with the dispatch, the first differing pixel, a PSNR, a picture
//    and whether the shader differs from the blessed one; a changed adapter says so and fails;
//  - blessing one kind leaves the others' references alone; a tolerance applies only to the kind it names;
//  - a damaged image file is refused;
//  - a capture that left something out (a dispatch, an index, a whole frame) is refused before anything runs, in compare and in
//    bless mode alike;
//  - a blessing is strict: every write is checked, a kind that matches nothing is refused, a partial blessing keeps the records of
//    the kinds it did not touch, a banner says what was blessed, an adapter nobody could identify is refused;
//  - a GPU timeout abandons the rest of the set and names the dispatch; a shader buffer reused for another shader is not mistaken
//    for the old one; the folder of difference pictures is emptied at the start of a run.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include "ext/nr/diag/NrReplayEngine.h"
#include "ext/nr/pass/DlssNr_Common.h"
#include "ext/nr/spatial/DlssNr_Spatial.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>

using namespace ext::nr::replay;
namespace fs = std::filesystem;

namespace
{
int failures = 0;

void Check(bool ok, const char* what, int line)
{
    if (!ok)
    {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

bool Contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

bool AnyLineContains(const RunReport& r, const std::string& part)
{
    for (const auto& l : r.lines)
        if (Contains(l, part))
            return true;
    return false;
}

void Dump(const RunReport& r)
{
    for (const auto& l : r.lines)
        std::printf("    %s\n", l.c_str());
}

// ---- shaders -------------------------------------------------------------------------------------------------------

ComPtr<ID3DBlob> Compile(const wchar_t* file, const char* source, const char* name, const D3D_SHADER_MACRO* defines = nullptr)
{
    ComPtr<ID3DBlob> code, errors;
    HRESULT hr;
    if (file != nullptr)
        hr = D3DCompileFromFile(file, defines, D3D_COMPILE_STANDARD_FILE_INCLUDE, "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    else
        hr = D3DCompile(source, std::strlen(source), name, nullptr, nullptr, "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr))
    {
        std::printf("shader %s did not compile: %s\n", name, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return nullptr;
    }
    return code;
}

// A probe with a known answer: copies the source, and adds gDelta to red at one pixel.
const char* kProbe = R"(
cbuffer P : register(b0) { uint gW; uint gH; float gDelta; uint gPx; uint gPy; }
Texture2D<float4> t0 : register(t0);
RWTexture2D<float4> u0 : register(u0);
[numthreads(8,8,1)] void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gW || id.y >= gH) return;
    float4 v = t0.Load(int3(id.xy, 0));
    if (id.x == gPx && id.y == gPy) v.x += gDelta;
    u0[id.xy] = v;
})";

// The same probe after a "shader edit": every pixel also moves in green.
const char* kProbeEdited = R"(
cbuffer P : register(b0) { uint gW; uint gH; float gDelta; uint gPx; uint gPy; }
Texture2D<float4> t0 : register(t0);
RWTexture2D<float4> u0 : register(u0);
[numthreads(8,8,1)] void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gW || id.y >= gH) return;
    float4 v = t0.Load(int3(id.xy, 0));
    if (id.x == gPx && id.y == gPy) v.x += gDelta;
    v.y += 0.0625;
    u0[id.xy] = v;
})";

// The same edit with another amount: the compiled shader has exactly the size of the one above and differs only in a constant.
const char* kProbeEditedAgain = R"(
cbuffer P : register(b0) { uint gW; uint gH; float gDelta; uint gPx; uint gPy; }
Texture2D<float4> t0 : register(t0);
RWTexture2D<float4> u0 : register(u0);
[numthreads(8,8,1)] void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gW || id.y >= gH) return;
    float4 v = t0.Load(int3(id.xy, 0));
    if (id.x == gPx && id.y == gPy) v.x += gDelta;
    v.y += 0.125;
    u0[id.xy] = v;
})";

// Reads the first channel of whatever view it is given (a depth plane) and writes it out as a float.
const char* kDepthProbe = R"(
cbuffer P : register(b0) { uint gW; uint gH; }
Texture2D<float4> t0 : register(t0);
RWTexture2D<float> u0 : register(u0);
[numthreads(8,8,1)] void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gW || id.y >= gH) return;
    u0[id.xy] = t0.Load(int3(id.xy, 0)).x;
})";

struct ProbeConstants
{
    uint32_t w, h;
    float delta;
    uint32_t px, py;
};

// ---- building a set ------------------------------------------------------------------------------------------------

uint16_t FloatToHalf(float f)
{
    // exact for the values the test uses (multiples of 1/64 below 4)
    if (f == 0.0f)
        return 0;
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    const int exp = static_cast<int>((bits >> 23) & 255) - 127 + 15;
    const uint32_t man = (bits >> 13) & 1023;
    return static_cast<uint16_t>(((bits >> 16) & 0x8000) | (static_cast<uint32_t>(exp) << 10) | man);
}

template <class T> std::vector<uint8_t> Bytes(const std::vector<T>& v)
{
    std::vector<uint8_t> out(v.size() * sizeof(T));
    std::memcpy(out.data(), v.data(), out.size());
    return out;
}

// pseudo random but deterministic, in [0, 2) in steps of 1/64
std::vector<uint8_t> Rgba16F(uint32_t w, uint32_t h, uint32_t seed)
{
    std::vector<uint16_t> px(static_cast<size_t>(w) * h * 4);
    uint32_t s = seed * 2654435761u + 12345u;
    for (auto& v : px)
    {
        s = s * 1664525u + 1013904223u;
        v = FloatToHalf(static_cast<float>((s >> 16) & 127) / 64.0f);
    }
    return Bytes(px);
}

std::vector<uint8_t> Floats(uint32_t count, uint32_t seed, float scale)
{
    std::vector<float> px(count);
    uint32_t s = seed * 2654435761u + 777u;
    for (auto& v : px)
    {
        s = s * 1664525u + 1013904223u;
        v = static_cast<float>((s >> 16) & 255) / 256.0f * scale;
    }
    return Bytes(px);
}

struct SetWriter
{
    fs::path dir;
    Manifest m;

    explicit SetWriter(const fs::path& d) : dir(d)
    {
        fs::remove_all(dir);
        fs::create_directories(dir / "images");
        m.frames = 2;
    }

    uint32_t Image_(const std::vector<uint8_t>& tight, uint32_t w, uint32_t h, uint32_t format, uint32_t rowBytes)
    {
        Image i;
        i.id = static_cast<uint32_t>(m.images.size());
        char name[64];
        std::snprintf(name, sizeof(name), "images/img_%04u.raw", i.id);
        i.file = name;
        i.width = w;
        i.height = h;
        i.format = format;
        i.rowBytes = rowBytes;
        i.srcRowPitch = (rowBytes + 255) & ~255u;
        i.bytes = tight.size();
        i.hash = HashRows(tight.data(), rowBytes, rowBytes, h);
        WriteFile(dir / i.file, tight.data(), tight.size());
        m.images.push_back(i);
        return i.id;
    }

    static Resource Res(uint32_t local, uint32_t w, uint32_t h, uint32_t resFormat, uint32_t before)
    {
        Resource r;
        r.local = local;
        r.width = w;
        r.height = h;
        r.resourceFormat = resFormat;
        r.copyFormat = CopyFormatFor(resFormat);
        r.before = before;
        return r;
    }

    static Binding Srv(uint32_t slot, uint32_t res, uint32_t view)
    {
        Binding b;
        b.slot = slot;
        b.res = res;
        b.viewFormat = view;
        b.state = kStateNonPixelShaderResource;
        return b;
    }

    static Binding Uav(uint32_t slot, uint32_t res, uint32_t view)
    {
        Binding b;
        b.uav = true;
        b.slot = slot;
        b.res = res;
        b.viewFormat = view;
        b.state = kStateUnorderedAccess;
        return b;
    }

    Dispatch& Add(uint32_t frame, uint32_t index, const char* kind, const char* shader, const void* constants, size_t size, uint32_t gx, uint32_t gy)
    {
        Dispatch d;
        d.frame = frame;
        d.index = index;
        d.kind = kind;
        d.shader = shader;
        d.groups[0] = gx;
        d.groups[1] = gy;
        std::memcpy(d.constants.data(), constants, size);
        m.dispatches.push_back(d);
        return m.dispatches.back();
    }

    // A recorder that left nothing out: every frame has the dispatches it was given. Tests that want a damaged set change
    // m.frameInfos or m.dispatches before saving.
    void Save()
    {
        if (m.frameInfos.empty())
            for (uint32_t frame = 0; frame < m.frames; ++frame)
            {
                FrameInfo f;
                f.frame = frame;
                for (const Dispatch& d : m.dispatches)
                    if (d.frame == frame)
                        ++f.dispatches;
                m.frameInfos.push_back(f);
            }
        WriteText(dir / "manifest.txt", WriteManifest(m));
    }
};

// All six shader slots bound to one resource, as the pass does for slots a mode does not use.
void BindAllSrv(Dispatch& d, uint32_t res, uint32_t view)
{
    for (uint32_t s = 0; s < kSrvSlots; ++s)
        d.bindings.push_back(SetWriter::Srv(s, res, view));
}

constexpr uint32_t W = 40, H = 24; // not multiples of the 8x8 group: the edges are partial groups
constexpr uint32_t F16 = DXGI_FORMAT_R16G16B16A16_FLOAT;

DlssNrConstants Base(uint32_t mode, uint32_t w, uint32_t h)
{
    DlssNrConstants c {};
    c.Mode = mode;
    c.Width = w;
    c.Height = h;
    c.WhitePoint = 1.0f;
    c.TransferStrength = 1.0f;
    c.ColourStrength = 1.0f;
    c.MaxRatio = 8.0f;
    c.ApplyModel = 1;
    c.CompareZoom = 1.0f;
    return c;
}

// The synthetic frame: the dispatch kinds of an NR frame, on small images.
void BuildSet(SetWriter& w)
{
    const uint32_t row16 = W * 8;

    // 0 encode: the game frame (a typeless resource, viewed as float) -> proxy and untouched copy
    {
        const auto c = Base(DlssNrMode_Encode, W, H);
        Dispatch& d = w.Add(0, 0, "encode", "dlssnr", &c, sizeof(c), (W + 7) / 8, (H + 7) / 8);
        d.resources = { SetWriter::Res(0, W, H, DXGI_FORMAT_R16G16B16A16_TYPELESS, w.Image_(Rgba16F(W, H, 1), W, H, F16, row16)),
                        SetWriter::Res(1, W, H, F16, kNoImage), SetWriter::Res(2, W, H, F16, kNoImage) };
        d.resources[1].after = w.Image_(Rgba16F(W, H, 91), W, H, F16, row16); // stands in for what the game's GPU wrote
        d.resources[2].after = w.Image_(Rgba16F(W, H, 92), W, H, F16, row16);
        BindAllSrv(d, 0, F16);
        d.bindings.push_back(SetWriter::Uav(0, 1, F16));
        d.bindings.push_back(SetWriter::Uav(1, 2, F16));
    }
    // 1 downsample: the proxy -> a half-size proxy
    {
        const auto c = Base(DlssNrMode_Downsample, W / 2, H / 2);
        Dispatch& d = w.Add(0, 1, "downsample", "dlssnr", &c, sizeof(c), (W / 2 + 7) / 8, (H / 2 + 7) / 8);
        d.resources = { SetWriter::Res(0, W, H, F16, w.Image_(Rgba16F(W, H, 2), W, H, F16, row16)), SetWriter::Res(1, W / 2, H / 2, F16, kNoImage) };
        d.resources[1].after = w.Image_(Rgba16F(W / 2, H / 2, 93), W / 2, H / 2, F16, (W / 2) * 8);
        BindAllSrv(d, 0, F16);
        d.bindings.push_back(SetWriter::Uav(0, 1, F16));
        d.bindings.push_back(SetWriter::Uav(1, 1, F16)); // the second output slot aliases the first
    }
    // 2 resolve: proxy + the model's answer + the untouched frame (+ motion) -> the edited frame
    {
        auto c = Base(DlssNrMode_Resolve, W, H);
        c.GuideWidth = W;
        c.GuideHeight = H;
        c.MvScaleX = 1.0f;
        c.MvScaleY = 1.0f;
        c.Passthrough = 0;
        Dispatch& d = w.Add(0, 2, "resolve", "dlssnr", &c, sizeof(c), (W + 7) / 8, (H + 7) / 8);
        d.resources = { SetWriter::Res(0, W, H, F16, w.Image_(Rgba16F(W, H, 3), W, H, F16, row16)),
                        SetWriter::Res(1, W, H, F16, w.Image_(Rgba16F(W, H, 4), W, H, F16, row16)),
                        SetWriter::Res(2, W, H, F16, w.Image_(Rgba16F(W, H, 5), W, H, F16, row16)),
                        SetWriter::Res(3, W, H, DXGI_FORMAT_R32G32_FLOAT, w.Image_(Floats(W * H * 2, 6, 0.01f), W, H, DXGI_FORMAT_R32G32_FLOAT, W * 8)),
                        SetWriter::Res(4, W, H, F16, kNoImage) };
        d.resources[4].after = w.Image_(Rgba16F(W, H, 94), W, H, F16, row16);
        d.bindings = { SetWriter::Srv(0, 0, F16), SetWriter::Srv(1, 1, F16), SetWriter::Srv(2, 2, F16),
                       SetWriter::Srv(3, 3, DXGI_FORMAT_R32G32_FLOAT), SetWriter::Srv(4, 0, F16), SetWriter::Srv(5, 0, F16),
                       SetWriter::Uav(0, 4, F16), SetWriter::Uav(1, 4, F16) };
    }
    // 3 multipass feedback blend
    {
        auto c = Base(DlssNrMode_MultipassFeedback, W, H);
        c.PassFeedback = 0.5f;
        Dispatch& d = w.Add(0, 3, "feedback", "dlssnr", &c, sizeof(c), (W + 7) / 8, (H + 7) / 8);
        d.resources = { SetWriter::Res(0, W, H, F16, w.Image_(Rgba16F(W, H, 7), W, H, F16, row16)),
                        SetWriter::Res(1, W, H, F16, w.Image_(Rgba16F(W, H, 8), W, H, F16, row16)), SetWriter::Res(2, W, H, F16, kNoImage) };
        d.resources[2].after = w.Image_(Rgba16F(W, H, 95), W, H, F16, row16);
        d.bindings = { SetWriter::Srv(0, 0, F16), SetWriter::Srv(1, 1, F16), SetWriter::Srv(2, 0, F16), SetWriter::Srv(3, 0, F16),
                       SetWriter::Srv(4, 0, F16), SetWriter::Srv(5, 0, F16), SetWriter::Uav(0, 2, F16), SetWriter::Uav(1, 2, F16) };
    }
    // 4 meter: one thread copies the game's exposure into tile 0
    {
        auto c = Base(DlssNrMode_Meter, 1, 1);
        Dispatch& d = w.Add(0, 4, "meter", "dlssnr", &c, sizeof(c), 1, 1);
        d.resources = { SetWriter::Res(0, W, H, F16, w.Image_(Rgba16F(W, H, 9), W, H, F16, row16)),
                        SetWriter::Res(1, 1, 1, DXGI_FORMAT_R32_FLOAT, w.Image_(Floats(1, 10, 1.0f), 1, 1, DXGI_FORMAT_R32_FLOAT, 4)),
                        SetWriter::Res(2, W, H, F16, kNoImage) };
        d.resources[2].after = w.Image_(Rgba16F(W, H, 96), W, H, F16, row16);
        d.bindings = { SetWriter::Srv(0, 0, F16), SetWriter::Srv(1, 0, F16), SetWriter::Srv(2, 0, F16), SetWriter::Srv(3, 1, DXGI_FORMAT_R32_FLOAT),
                       SetWriter::Srv(4, 0, F16), SetWriter::Srv(5, 0, F16), SetWriter::Uav(0, 2, F16), SetWriter::Uav(1, 2, F16) };
    }
    // 5 guide match: depth and motion resampled to the working size, with their own shader
    {
        auto c = Base(DlssNrMode_ResizePrivateGuides, 20, 12);
        c.GuideWidth = W;
        c.GuideHeight = H;
        c.TransferStrength = static_cast<float>(W);
        c.ColourStrength = static_cast<float>(H);
        c.DebugView = 3;
        c.CompareMode = 2;
        Dispatch& d = w.Add(0, 5, "guide_match", "guide_match", &c, sizeof(c), 3, 2);
        d.resources = { SetWriter::Res(0, W, H, DXGI_FORMAT_R32_FLOAT, w.Image_(Floats(W * H, 11, 1.0f), W, H, DXGI_FORMAT_R32_FLOAT, W * 4)),
                        SetWriter::Res(1, W, H, DXGI_FORMAT_R32G32_FLOAT, w.Image_(Floats(W * H * 2, 12, 0.05f), W, H, DXGI_FORMAT_R32G32_FLOAT, W * 8)),
                        SetWriter::Res(2, 20, 12, DXGI_FORMAT_R32_FLOAT, kNoImage), SetWriter::Res(3, 20, 12, DXGI_FORMAT_R32G32_FLOAT, kNoImage) };
        d.resources[2].after = w.Image_(Floats(240, 97, 1.0f), 20, 12, DXGI_FORMAT_R32_FLOAT, 80);
        d.resources[3].after = w.Image_(Floats(480, 98, 1.0f), 20, 12, DXGI_FORMAT_R32G32_FLOAT, 160);
        d.bindings = { SetWriter::Srv(0, 0, DXGI_FORMAT_R32_FLOAT), SetWriter::Srv(1, 1, DXGI_FORMAT_R32G32_FLOAT),
                       SetWriter::Srv(2, 0, DXGI_FORMAT_R32_FLOAT), SetWriter::Srv(3, 0, DXGI_FORMAT_R32_FLOAT),
                       SetWriter::Srv(4, 0, DXGI_FORMAT_R32_FLOAT), SetWriter::Srv(5, 0, DXGI_FORMAT_R32_FLOAT),
                       SetWriter::Uav(0, 2, DXGI_FORMAT_R32_FLOAT), SetWriter::Uav(1, 3, DXGI_FORMAT_R32G32_FLOAT) };
    }
    // 6 a probe whose answer is known to the pixel (frame 1, the second consecutive frame)
    {
        ProbeConstants c { W, H, 0.5f, 5, 3 };
        Dispatch& d = w.Add(1, 0, "probe", "probe", &c, sizeof(c), (W + 7) / 8, (H + 7) / 8);
        d.resources = { SetWriter::Res(0, W, H, F16, w.Image_(Rgba16F(W, H, 20), W, H, F16, row16)), SetWriter::Res(1, W, H, F16, kNoImage) };
        d.resources[1].after = w.Image_(Rgba16F(W, H, 99), W, H, F16, row16);
        BindAllSrv(d, 0, F16);
        d.bindings.push_back(SetWriter::Uav(0, 1, F16));
        d.bindings.push_back(SetWriter::Uav(1, 1, F16));
    }
    // 7 the same resolve as dispatch 2 with another reversible mode, in the second frame
    {
        auto c = Base(DlssNrMode_Resolve, W, H);
        c.GuideWidth = W;
        c.GuideHeight = H;
        c.ReversibleMode = 5;
        c.Passthrough = 1;
        c.TransferStrength = 0.75f;
        Dispatch& d = w.Add(1, 1, "resolve", "dlssnr", &c, sizeof(c), (W + 7) / 8, (H + 7) / 8);
        d.resources = { SetWriter::Res(0, W, H, F16, w.Image_(Rgba16F(W, H, 13), W, H, F16, row16)),
                        SetWriter::Res(1, W, H, F16, w.Image_(Rgba16F(W, H, 14), W, H, F16, row16)),
                        SetWriter::Res(2, W, H, F16, w.Image_(Rgba16F(W, H, 15), W, H, F16, row16)), SetWriter::Res(3, W, H, F16, kNoImage) };
        d.resources[3].after = w.Image_(Rgba16F(W, H, 100), W, H, F16, row16);
        d.bindings = { SetWriter::Srv(0, 0, F16), SetWriter::Srv(1, 1, F16), SetWriter::Srv(2, 2, F16), SetWriter::Srv(3, 0, F16),
                       SetWriter::Srv(4, 0, F16), SetWriter::Srv(5, 0, F16), SetWriter::Uav(0, 3, F16), SetWriter::Uav(1, 3, F16) };
    }
    // 8..15 the residual modes of the same shader
    {
        struct
        {
            uint32_t mode;
            const char* kind;
        } modes[] = { { 5, "encode_residual" }, { 6, "apply_residual" }, { 7, "unit_exposure" }, { 8, "normalize_motion" },
                      { 9, "compose_motion" }, { 10, "apply_interpolated_residual" }, { 11, "zero_motion" }, { 12, "bound_residual" } };
        uint32_t index = 2;
        for (const auto& m : modes)
        {
            auto c = Base(m.mode, W, H);
            c.GuideWidth = W;
            c.GuideHeight = H;
            c.MvScaleX = 1.0f;
            c.MvScaleY = 1.0f;
            Dispatch& d = w.Add(1, index, m.kind, "dlssnr", &c, sizeof(c), (W + 7) / 8, (H + 7) / 8);
            d.resources = { SetWriter::Res(0, W, H, F16, w.Image_(Rgba16F(W, H, 30 + index), W, H, F16, row16)),
                            SetWriter::Res(1, W, H, F16, w.Image_(Rgba16F(W, H, 50 + index), W, H, F16, row16)), SetWriter::Res(2, W, H, F16, kNoImage) };
            d.resources[2].after = w.Image_(Rgba16F(W, H, 70 + index), W, H, F16, row16);
            d.bindings = { SetWriter::Srv(0, 0, F16), SetWriter::Srv(1, 1, F16), SetWriter::Srv(2, 0, F16), SetWriter::Srv(3, 1, F16),
                           SetWriter::Srv(4, 0, F16), SetWriter::Srv(5, 1, F16), SetWriter::Uav(0, 2, F16), SetWriter::Uav(1, 2, F16) };
            ++index;
        }
    }
    // 16..18 spatial compression: pack, guides, unpack (own shader, the guides stage compiled with its own define)
    {
        constexpr uint32_t NW = 160, NH = 96;
        DlssNr::Spatial::Settings s {};
        s.enabled = true;
        const auto layout = DlssNr::Spatial::Build(s, NW, NH, 1.0f);
        const DlssNr::GuideRegions regions { { 0, 0, NW, NH }, { 0, 0, NW, NH } };
        const uint32_t mw = layout.modelW, mh = layout.modelH, ow = layout.ordinaryW, oh = layout.ordinaryH;
        const auto groups = [](uint32_t n) { return (n + 7) / 8; };
        {
            const auto c = DlssNr::Spatial::MakeConstants(layout, 100, regions, 1.0f, 1.0f, NW, NH);
            Dispatch& d = w.Add(1, 10, "spatial_pack", "spatial", &c, sizeof(c), groups(mw), groups(mh));
            d.resources = { SetWriter::Res(0, NW, NH, F16, w.Image_(Rgba16F(NW, NH, 81), NW, NH, F16, NW * 8)), SetWriter::Res(1, mw, mh, F16, kNoImage) };
            d.resources[1].after = w.Image_(Rgba16F(mw, mh, 82), mw, mh, F16, mw * 8);
            BindAllSrv(d, 0, F16);
            d.bindings.push_back(SetWriter::Uav(0, 1, F16));
            d.bindings.push_back(SetWriter::Uav(1, 1, F16));
        }
        {
            const auto c = DlssNr::Spatial::MakeConstants(layout, 101, regions, 1.0f, 1.0f, NW, NH);
            Dispatch& d = w.Add(1, 11, "spatial_guides", "spatial_guides", &c, sizeof(c), groups(mw), groups(mh));
            d.resources = { SetWriter::Res(0, mw, mh, F16, w.Image_(Rgba16F(mw, mh, 83), mw, mh, F16, mw * 8)),
                            SetWriter::Res(1, NW, NH, DXGI_FORMAT_R32_FLOAT, w.Image_(Floats(NW * NH, 84, 1.0f), NW, NH, DXGI_FORMAT_R32_FLOAT, NW * 4)),
                            SetWriter::Res(2, NW, NH, DXGI_FORMAT_R32G32_FLOAT, w.Image_(Floats(NW * NH * 2, 85, 0.05f), NW, NH, DXGI_FORMAT_R32G32_FLOAT, NW * 8)),
                            SetWriter::Res(3, mw, mh, DXGI_FORMAT_R32_FLOAT, kNoImage), SetWriter::Res(4, mw, mh, DXGI_FORMAT_R32G32_FLOAT, kNoImage) };
            d.resources[3].after = w.Image_(Floats(mw * mh, 86, 1.0f), mw, mh, DXGI_FORMAT_R32_FLOAT, mw * 4);
            d.resources[4].after = w.Image_(Floats(mw * mh * 2, 87, 1.0f), mw, mh, DXGI_FORMAT_R32G32_FLOAT, mw * 8);
            d.bindings = { SetWriter::Srv(0, 0, F16), SetWriter::Srv(1, 1, DXGI_FORMAT_R32_FLOAT), SetWriter::Srv(2, 2, DXGI_FORMAT_R32G32_FLOAT),
                           SetWriter::Srv(3, 0, F16), SetWriter::Srv(4, 0, F16), SetWriter::Srv(5, 0, F16),
                           SetWriter::Uav(0, 3, DXGI_FORMAT_R32_FLOAT), SetWriter::Uav(1, 4, DXGI_FORMAT_R32G32_FLOAT) };
        }
        {
            const auto c = DlssNr::Spatial::MakeConstants(layout, 102, regions, 1.0f, 1.0f, NW, NH);
            Dispatch& d = w.Add(1, 12, "spatial_unpack", "spatial", &c, sizeof(c), groups(ow), groups(oh));
            d.resources = { SetWriter::Res(0, mw, mh, F16, w.Image_(Rgba16F(mw, mh, 88), mw, mh, F16, mw * 8)),
                            SetWriter::Res(1, mw, mh, F16, w.Image_(Rgba16F(mw, mh, 89), mw, mh, F16, mw * 8)),
                            SetWriter::Res(2, ow, oh, F16, kNoImage), SetWriter::Res(3, ow, oh, F16, kNoImage) };
            d.resources[2].after = w.Image_(Rgba16F(ow, oh, 90), ow, oh, F16, ow * 8);
            d.resources[3].after = w.Image_(Rgba16F(ow, oh, 91), ow, oh, F16, ow * 8);
            d.bindings = { SetWriter::Srv(0, 0, F16), SetWriter::Srv(1, 1, F16), SetWriter::Srv(2, 0, F16), SetWriter::Srv(3, 0, F16),
                           SetWriter::Srv(4, 0, F16), SetWriter::Srv(5, 0, F16), SetWriter::Uav(0, 2, F16), SetWriter::Uav(1, 3, F16) };
        }
    }
    w.Save();
}

constexpr size_t kDispatchesInSet = 19;

fs::path Temp(const char* name) { return fs::temp_directory_path() / (std::string("optiscaler-nr-replay-synthetic-") + name); }

struct Blobs
{
    ComPtr<ID3DBlob> dlssnr, guide, spatial, spatialGuides, probe, probeEdited, probeEditedAgain, depth;
    bool editedProbe = false;

    bool Lookup(const std::string& id, Blob& out) const
    {
        ID3DBlob* b = id == "dlssnr"           ? dlssnr.Get()
                      : id == "guide_match"    ? guide.Get()
                      : id == "spatial"        ? spatial.Get()
                      : id == "spatial_guides" ? spatialGuides.Get()
                      : id == "probe"          ? (editedProbe ? probeEdited.Get() : probe.Get())
                                               : nullptr;
        if (b == nullptr)
            return false;
        out = { b->GetBufferPointer(), b->GetBufferSize() };
        return true;
    }
};

RunOptions Options(const fs::path& set, Blobs& blobs)
{
    RunOptions o;
    o.setDir = set;
    o.outDir = set / "out";
    o.commit = "synthetic";
    o.date = "2026-10-29";
    o.warp = { "10.0.1.0", 0x1111 };
    o.shaders = [&blobs](const std::string& id, Blob& b) { return blobs.Lookup(id, b); };
    return o;
}

// ---- the checks ----------------------------------------------------------------------------------------------------

void TestDeterminism(Replayer& replayer, const SetFiles& set, const Blobs& blobs)
{
    std::map<std::string, bool> reproducible;
    for (const Dispatch& d : set.manifest.dispatches)
    {
        Blob blob;
        CHECK(blobs.Lookup(d.shader, blob));
        std::map<uint32_t, Output> a, b;
        std::string e1, e2;
        const bool ok = replayer.Run(set, d, blob.data, blob.size, a, e1) && replayer.Run(set, d, blob.data, blob.size, b, e2);
        if (!ok)
            std::printf("    dispatch %u:%u (%s) did not run: %s %s\n", d.frame, d.index, d.kind.c_str(), e1.c_str(), e2.c_str());
        CHECK(ok);
        bool same = ok && !a.empty() && a.size() == b.size();
        for (const auto& [slot, out] : a)
            same = same && b.count(slot) && b[slot].bytes == out.bytes && !out.bytes.empty();
        if (!same)
            std::printf("    dispatch %u:%u (%s) is not reproducible\n", d.frame, d.index, d.kind.c_str());
        CHECK(same);
        reproducible[d.kind] = same;
    }
    std::string kinds;
    for (const auto& [k, v] : reproducible)
        kinds += k + (v ? "=identical " : "=DIFFERENT ");
    std::printf("    determinism (two runs each, bit exact): %s\n", kinds.c_str());
}

void TestKnownAnswers(Replayer& replayer, const SetFiles& set, const Blobs& blobs)
{
    // the probe copies its source and adds 0.5 to red at (5, 3)
    const Dispatch* probe = nullptr;
    for (const Dispatch& d : set.manifest.dispatches)
        if (d.kind == "probe")
            probe = &d;
    CHECK(probe != nullptr);
    if (probe == nullptr)
        return;
    std::vector<uint8_t> src;
    std::string error;
    CHECK(ReadImage(set, probe->resources[0].before, src, error));
    Blob blob;
    blobs.Lookup("probe", blob);
    std::map<uint32_t, Output> out;
    CHECK(replayer.Run(set, *probe, blob.data, blob.size, out, error));
    CHECK(out.count(0) == 1);
    if (out.count(0) == 0)
        return;
    const Output& o = out[0];
    CHECK(o.width == W && o.height == H && o.format == F16 && o.rowBytes == W * 8 && o.bytes.size() == static_cast<size_t>(W) * H * 8);
    size_t differing = 0;
    for (size_t i = 0; i + 8 <= o.bytes.size(); i += 8)
        if (std::memcmp(&o.bytes[i], &src[i], 8) != 0)
        {
            ++differing;
            CHECK(i / 8 == 3u * W + 5u);
        }
    CHECK(differing == 1);
    uint16_t red;
    std::memcpy(&red, &o.bytes[(3 * W + 5) * 8], 2);
    uint16_t before;
    std::memcpy(&before, &src[(3 * W + 5) * 8], 2);
    CHECK(std::fabs(HalfToFloat(red) - HalfToFloat(before) - 0.5f) < 1e-6f);
}

// A depth plane through the same paths: created as a typeless depth resource, filled from the recorded image, viewed with
// the view format the manifest names, read by a shader, written out as a float.
void TestDepthPlanes(Replayer& replayer)
{
    ComPtr<ID3DBlob> shader = Compile(nullptr, kDepthProbe, "depth_probe");
    CHECK(shader != nullptr);
    if (!shader)
        return;
    struct Case
    {
        const char* name;
        DXGI_FORMAT resource, view;
    } cases[] = {
        { "R32_TYPELESS single-plane depth buffer", DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT },
        { "R32G8X24_TYPELESS depth+stencil", DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS },
        { "R24G8_TYPELESS depth+stencil", DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R24_UNORM_X8_TYPELESS },
    };
    for (const Case& c : cases)
    {
        SetWriter w(Temp("depth"));
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 16;
        desc.Height = 8;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = c.resource;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        Footprint fp;
        CHECK(QueryFootprint(replayer.Device(), desc, fp));
        const uint32_t rowBytes = static_cast<uint32_t>(fp.rowBytes);
        std::vector<uint8_t> pixels(static_cast<size_t>(rowBytes) * fp.rows);
        for (size_t i = 0; i < pixels.size(); ++i)
            pixels[i] = static_cast<uint8_t>(i * 7 + 3);
        if (c.resource == DXGI_FORMAT_R32_TYPELESS)
        {
            std::vector<float> f(16 * 8);
            for (size_t i = 0; i < f.size(); ++i)
                f[i] = static_cast<float>(i) / 128.0f;
            pixels = Bytes(f);
        }
        const uint32_t image = w.Image_(pixels, 16, 8, fp.copyFormat, rowBytes);
        struct { uint32_t w, h; } k { 16, 8 };
        Dispatch& d = w.Add(0, 0, "depth_probe", "depth_probe", &k, sizeof(k), 2, 1);
        d.resources = { SetWriter::Res(0, 16, 8, c.resource, image), SetWriter::Res(1, 16, 8, DXGI_FORMAT_R32_FLOAT, kNoImage) };
        d.resources[1].after = w.Image_(Floats(128, 1, 1.0f), 16, 8, DXGI_FORMAT_R32_FLOAT, 64);
        for (uint32_t slot = 0; slot < kSrvSlots; ++slot)
            d.bindings.push_back(SetWriter::Srv(slot, 0, c.view));
        d.bindings.push_back(SetWriter::Uav(0, 1, DXGI_FORMAT_R32_FLOAT));
        d.bindings.push_back(SetWriter::Uav(1, 1, DXGI_FORMAT_R32_FLOAT));
        w.Save();

        SetFiles set;
        std::string error;
        CHECK(LoadSet(w.dir, set, error));
        std::map<uint32_t, Output> out;
        const bool ran = replayer.Run(set, set.manifest.dispatches[0], shader->GetBufferPointer(), shader->GetBufferSize(), out, error);
        if (!ran)
            std::printf("    %s: %s\n", c.name, error.c_str());
        CHECK(ran);
        if (!ran || out.count(0) == 0)
            continue;
        std::printf("    %s: copy format %u, row bytes %u, view %u: replayed\n", c.name, fp.copyFormat, rowBytes, c.view);
        const float* result = reinterpret_cast<const float*>(out[0].bytes.data());
        if (c.resource == DXGI_FORMAT_R32_TYPELESS)
        {
            bool exact = true;
            for (size_t i = 0; i < 128; ++i)
                exact = exact && result[i] == static_cast<float>(i) / 128.0f;
            CHECK(exact);
        }
        else if (c.resource == DXGI_FORMAT_R32G8X24_TYPELESS)
        {
            // the depth plane is 32-bit float: the first texel of the image is what the shader sees at (0, 0)
            float expected;
            std::memcpy(&expected, pixels.data(), 4);
            CHECK(std::memcmp(&result[0], &expected, 4) == 0 || (std::isnan(expected) && std::isnan(result[0])));
        }
        else
        {
            // 24-bit unorm in the low bits of the first texel
            uint32_t texel;
            std::memcpy(&texel, pixels.data(), 4);
            const float expected = static_cast<float>(texel & 0xFFFFFFu) / 16777215.0f;
            CHECK(std::fabs(result[0] - expected) < 1e-6f);
        }
    }
    fs::remove_all(Temp("depth"));
}

void TestWorkflow(Replayer& replayer, Blobs& blobs)
{
    const fs::path dir = Temp("set");
    SetWriter w(dir);
    BuildSet(w);

    SetFiles set;
    std::string error;
    CHECK(LoadSet(dir, set, error));
    CHECK(set.manifest.dispatches.size() == kDispatchesInSet);

    TestDeterminism(replayer, set, blobs);
    TestKnownAnswers(replayer, set, blobs);

    const size_t total = set.manifest.dispatches.size();

    // 1. nothing blessed: a loud failure, not a pass and not a skip
    {
        RunReport r = RunReplaySet(Options(dir, blobs), replayer);
        CHECK(!r.ok && r.noBlessedSet);
        CHECK(AnyLineContains(r, "no blessed set"));
        CHECK(!fs::exists(dir / "reference" / "provenance.txt"));
    }

    // 2. bless everything
    {
        RunOptions o = Options(dir, blobs);
        o.bless = ParseBless("all");
        RunReport r = RunReplaySet(o, replayer);
        if (!r.ok)
            Dump(r);
        CHECK(r.ok && r.blessed == static_cast<int>(total) && r.matched == 0 && r.differing == 0);
        CHECK(fs::exists(dir / "reference" / "provenance.txt"));
        CHECK(fs::exists(dir / "reference" / ReferenceFileName(0, 2, 0)) && fs::exists(dir / "reference" / ReferenceFileName(1, 0, 0)));
        std::vector<uint8_t> text;
        ReadFile(dir / "reference" / "provenance.txt", text);
        Provenance p;
        CHECK(ParseProvenance(std::string(text.begin(), text.end()), p, error));
        CHECK(p.commit == "synthetic" && p.date == "2026-10-29" && p.warp.version == "10.0.1.0" && p.warp.hash == 0x1111);
        CHECK(p.dispatches.size() == total && p.dispatches[DispatchKey(1, 0)].kind == "probe");
        // the reference of an output is the tightly packed replay output
        std::vector<uint8_t> ref;
        ReadFile(dir / "reference" / ReferenceFileName(0, 0, 0), ref);
        CHECK(ref.size() == static_cast<size_t>(W) * H * 8);
    }

    // 3. compare: everything is identical
    {
        RunReport r = RunReplaySet(Options(dir, blobs), replayer);
        if (!r.ok)
            Dump(r);
        CHECK(r.ok && r.matched == static_cast<int>(total) && r.blessed == 0 && r.differing == 0 && r.missing == 0 && r.errors == 0);
        // the recorded GPU outputs here are random images: the informational comparison runs and never decides anything
        CHECK(r.comparedToCaptured > 0);
    }

    // 4. a changed constant: the first differing pixel is where the probe was changed
    {
        Manifest edited = set.manifest; // the same set with the probe's pixel moved
        ProbeConstants c { W, H, 0.5f, 9, 4 };
        for (Dispatch& d : edited.dispatches)
            if (d.kind == "probe")
                std::memcpy(d.constants.data(), &c, sizeof(c));
        WriteText(dir / "manifest.txt", WriteManifest(edited));
        RunReport r = RunReplaySet(Options(dir, blobs), replayer);
        CHECK(!r.ok && r.differing == 1 && r.matched == static_cast<int>(total) - 1);
        // the old pixel (5, 3) is now the same as the source, and (9, 4) is new: the first in row-major order is (9, 4) vs (5, 3)
        CHECK(AnyLineContains(r, "dispatch 1:0 kind=probe u0: differs from the blessed image; first differing pixel (5, 3), 2 pixels differ"));
        CHECK(AnyLineContains(r, "PSNR "));
        CHECK(AnyLineContains(r, "shader blob is the blessed one"));
        CHECK(AnyLineContains(r, "picture diff_f1_d0_u0.ppm"));
        CHECK(fs::exists(dir / "out" / "diff_f1_d0_u0.ppm"));
        WriteText(dir / "manifest.txt", WriteManifest(set.manifest)); // restore
    }

    // 5. a changed shader with the same inputs and constants: reported, with the shader named as the difference
    {
        blobs.editedProbe = true;
        RunReport r = RunReplaySet(Options(dir, blobs), replayer);
        CHECK(!r.ok && r.differing == 1);
        CHECK(AnyLineContains(r, "dispatch 1:0 kind=probe"));
        CHECK(AnyLineContains(r, "shader blob differs from the blessed one"));
        CHECK(AnyLineContains(r, "first differing pixel (0, 0)"));
        // a tolerance on another kind does not excuse it; one on this kind does, up to its size
        RunOptions o = Options(dir, blobs);
        o.tolerance["encode"] = 1.0;
        CHECK(!RunReplaySet(o, replayer).ok);
        o.tolerance["probe"] = 0.01; // the edit moves green by 0.0625
        CHECK(!RunReplaySet(o, replayer).ok);
        o.tolerance["probe"] = 0.5; // the edit moves green everywhere by 0.0625, within the allowance
        CHECK(RunReplaySet(o, replayer).ok);
        blobs.editedProbe = false;
    }

    // 6. a partial bless rewrites only the named kind, and still compares the rest
    {
        // damage the reference of a dispatch of another kind, and the shader of the probe
        const fs::path other = dir / "reference" / ReferenceFileName(0, 3, 0);
        std::vector<uint8_t> ref;
        ReadFile(other, ref);
        ref[10] ^= 0x40;
        WriteFile(other, ref.data(), ref.size());
        blobs.editedProbe = true;
        RunOptions o = Options(dir, blobs);
        o.bless = ParseBless("probe");
        RunReport r = RunReplaySet(o, replayer);
        CHECK(!r.ok); // the damaged feedback reference is still compared, and differs
        CHECK(r.blessed == 1 && r.differing == 1);
        CHECK(AnyLineContains(r, "dispatch 0:3 kind=feedback"));
        std::vector<uint8_t> after;
        ReadFile(other, after);
        CHECK(after == ref); // the other kind's file was not rewritten
        // the probe now matches the edited shader and its provenance says so
        RunReport again = RunReplaySet(Options(dir, blobs), replayer);
        CHECK(again.differing == 1 && again.matched == static_cast<int>(total) - 1 && again.errors == 0);
        CHECK(!AnyLineContains(again, "kind=probe"));
        std::vector<uint8_t> text;
        ReadFile(dir / "reference" / "provenance.txt", text);
        Provenance p;
        CHECK(ParseProvenance(std::string(text.begin(), text.end()), p, error));
        Blob edited, original;
        blobs.editedProbe = true;
        blobs.Lookup("probe", edited);
        blobs.editedProbe = false;
        blobs.Lookup("probe", original);
        CHECK(p.dispatches[DispatchKey(1, 0)].shaderHash == Fnv1a64(edited.data, edited.size));
        CHECK(p.dispatches[DispatchKey(1, 0)].shaderHash != Fnv1a64(original.data, original.size));
        CHECK(p.dispatches.size() == total); // the other kinds' lines were kept
        // repair everything for the next steps
        blobs.editedProbe = false;
        RunOptions all = Options(dir, blobs);
        all.bless = ParseBless("all");
        CHECK(RunReplaySet(all, replayer).ok);
    }

    // 7. a different adapter: refused with the message, even though every image is the same
    {
        RunOptions o = Options(dir, blobs);
        o.warp = { "10.0.2.0", 0x2222 };
        RunReport r = RunReplaySet(o, replayer);
        CHECK(!r.ok && r.warpChanged && r.dispatches == 0);
        CHECK(AnyLineContains(r, "WARP changed 10.0.1.0 (0000000000001111) -> 10.0.2.0 (0000000000002222): re-bless, not a shader regression"));
        // a partial bless on a different adapter is refused too, a full one replaces the provenance
        o.bless = ParseBless("probe");
        CHECK(RunReplaySet(o, replayer).warpChanged);
        o.bless = ParseBless("all");
        CHECK(RunReplaySet(o, replayer).ok);
        std::vector<uint8_t> text;
        ReadFile(dir / "reference" / "provenance.txt", text);
        Provenance p;
        CHECK(ParseProvenance(std::string(text.begin(), text.end()), p, error));
        CHECK(p.warp.version == "10.0.2.0");
        CHECK(RunReplaySet(Options(dir, blobs), replayer).warpChanged); // the default options carry the old adapter now
    }

    // 8. a dispatch without a blessed image
    {
        RunOptions o = Options(dir, blobs);
        o.warp = { "10.0.2.0", 0x2222 };
        fs::remove(dir / "reference" / ReferenceFileName(0, 1, 0));
        RunReport r = RunReplaySet(o, replayer);
        CHECK(!r.ok && r.missing == 1);
        CHECK(AnyLineContains(r, "dispatch 0:1 kind=downsample u0: no blessed image"));
    }

    // 9. a damaged image file is refused rather than replayed
    {
        RunOptions o = Options(dir, blobs);
        o.warp = { "10.0.2.0", 0x2222 };
        const fs::path image = dir / set.manifest.images[0].file;
        std::vector<uint8_t> bytes;
        ReadFile(image, bytes);
        bytes[5] ^= 1;
        WriteFile(image, bytes.data(), bytes.size());
        RunReport r = RunReplaySet(o, replayer);
        CHECK(!r.ok && r.errors >= 1);
        CHECK(AnyLineContains(r, "does not match its recorded hash"));
    }
    fs::remove_all(dir);
}

void TestBadSets(Replayer& replayer, Blobs& blobs)
{
    const fs::path dir = Temp("bad");
    fs::remove_all(dir);
    fs::create_directories(dir);
    RunReport r = RunReplaySet(Options(dir, blobs), replayer);
    CHECK(!r.ok && AnyLineContains(r, "no manifest.txt"));
    WriteText(dir / "manifest.txt", "optiscaler-nr-replay 1\nend\n"); // the format before the per-frame counts
    r = RunReplaySet(Options(dir, blobs), replayer);
    CHECK(!r.ok && AnyLineContains(r, "unsupported manifest version 1"));
    WriteText(dir / "manifest.txt", "optiscaler-nr-replay 3\nend\n");
    r = RunReplaySet(Options(dir, blobs), replayer);
    CHECK(!r.ok && AnyLineContains(r, "unsupported manifest version 3"));
    fs::remove_all(dir);

    // a dispatch naming a shader nobody supplied is an error, not a skip
    SetWriter w(dir);
    BuildSet(w);
    RunOptions o = Options(dir, blobs);
    o.bless = ParseBless("all");
    o.shaders = [](const std::string&, Blob&) { return false; };
    r = RunReplaySet(o, replayer);
    CHECK(!r.ok && r.errors == static_cast<int>(kDispatchesInSet) && AnyLineContains(r, "no shader blob"));
    fs::remove_all(dir);
}

// ---- small sets for the strictness checks ------------------------------------------------------------------------------

void AddProbe(SetWriter& w, uint32_t frame, uint32_t index, const char* kind, uint32_t seed, uint32_t px)
{
    const uint32_t row16 = W * 8;
    ProbeConstants c { W, H, 0.5f, px, 3 };
    Dispatch& d = w.Add(frame, index, kind, "probe", &c, sizeof(c), (W + 7) / 8, (H + 7) / 8);
    d.resources = { SetWriter::Res(0, W, H, F16, w.Image_(Rgba16F(W, H, seed), W, H, F16, row16)), SetWriter::Res(1, W, H, F16, kNoImage) };
    d.resources[1].after = w.Image_(Rgba16F(W, H, seed + 100), W, H, F16, row16);
    BindAllSrv(d, 0, F16);
    d.bindings.push_back(SetWriter::Uav(0, 1, F16));
    d.bindings.push_back(SetWriter::Uav(1, 1, F16));
}

// frame 0: probe_a, probe_b; frame 1: probe_a
void BuildMini(SetWriter& w)
{
    AddProbe(w, 0, 0, "probe_a", 1, 5);
    AddProbe(w, 0, 1, "probe_b", 2, 6);
    AddProbe(w, 1, 0, "probe_a", 3, 7);
    w.Save();
}

RunReport Run(const fs::path& dir, Blobs& blobs, Replayer& replayer, const char* bless = "")
{
    RunOptions o = Options(dir, blobs);
    o.bless = ParseBless(bless);
    return RunReplaySet(o, replayer);
}

// ---- 1. a capture that left something out is refused, in compare and in bless mode ----------------------------------------

void TestIncompleteSets(Replayer& replayer, Blobs& blobs)
{
    const fs::path dir = Temp("incomplete");
    const auto damaged = [&](const char* what, const std::function<void(Manifest&)>& change, const char* expectInLine) {
        SetWriter w(dir);
        BuildMini(w);
        Manifest m = w.m;
        change(m);
        WriteText(dir / "manifest.txt", WriteManifest(m));
        for (const char* bless : { "", "all" })
        {
            const RunReport r = Run(dir, blobs, replayer, bless);
            const bool good = !r.ok && r.incomplete && r.dispatches == 0 && !r.noBlessedSet && AnyLineContains(r, "the capture is incomplete") &&
                              AnyLineContains(r, expectInLine);
            if (!good)
            {
                std::printf("    %s (bless \"%s\"): expected an incomplete-capture failure mentioning \"%s\"\n", what, bless, expectInLine);
                Dump(r);
            }
            CHECK(good);
            CHECK(!fs::exists(dir / "reference")); // nothing is blessed from a damaged capture
        }
    };
    damaged("dispatches left out", [](Manifest& m) { m.frameInfos[0].skipped = 2; }, "frame 0: 2 dispatch(es) were left out");
    damaged("boundaries left out", [](Manifest& m) { m.frameInfos[1].boundariesSkipped = 1; }, "frame 1: 1 boundary picture(s) were left out");
    damaged("a gap in the indices", [](Manifest& m) { m.frameInfos[0].dispatches = 3; }, "frame 0: dispatch index 2 missing (1 of 3)");
    damaged("an index removed", [](Manifest& m) { m.dispatches.erase(m.dispatches.begin()); }, "frame 0: dispatch index 0 missing");
    damaged("an empty frame", [](Manifest& m) { m.dispatches.pop_back(); }, "frame 1 has no dispatches");
    damaged("no frame record", [](Manifest& m) { m.frameInfos.pop_back(); }, "frame 1: the manifest does not say how many dispatches");

    // the same index twice is not even a readable set
    {
        SetWriter w(dir);
        BuildMini(w);
        Manifest m = w.m;
        m.dispatches.push_back(m.dispatches[0]);
        WriteText(dir / "manifest.txt", WriteManifest(m));
        const RunReport r = Run(dir, blobs, replayer);
        CHECK(!r.ok && AnyLineContains(r, "duplicate dispatch"));
    }
    // and a complete one runs
    {
        SetWriter w(dir);
        BuildMini(w);
        RunReport r = Run(dir, blobs, replayer, "all");
        if (!r.ok)
            Dump(r);
        CHECK(r.ok && r.blessed == 3 && !r.incomplete);
    }
    fs::remove_all(dir);
}

// ---- 2. blessing is strict ---------------------------------------------------------------------------------------------------

void TestBlessIsStrict(Replayer& replayer, Blobs& blobs)
{
    const fs::path dir = Temp("strict");
    std::string error;

    // the banner and the record of each blessing
    {
        SetWriter w(dir);
        BuildMini(w);
        RunOptions first = Options(dir, blobs);
        first.bless = ParseBless("all");
        first.commit = "firstcommit";
        first.date = "2026-10-01";
        first.dirty = true;
        RunReport r = RunReplaySet(first, replayer);
        CHECK(r.ok && r.writeFailures == 0);
        CHECK(AnyLineContains(r, "BLESSED all at firstcommit (working tree has uncommitted changes)"));
        // a compare is quiet about it
        r = Run(dir, blobs, replayer);
        CHECK(r.ok && !AnyLineContains(r, "BLESSED"));

        // a partial blessing of one kind, by another commit on another day on a clean tree
        RunOptions second = Options(dir, blobs);
        second.bless = ParseBless("probe_b");
        second.commit = "secondcommit";
        second.date = "2026-10-02";
        second.dirty = false;
        r = RunReplaySet(second, replayer);
        CHECK(r.ok && r.blessed == 1);
        CHECK(AnyLineContains(r, "BLESSED probe_b at secondcommit") && !AnyLineContains(r, "uncommitted"));

        std::vector<uint8_t> text;
        ReadFile(dir / "reference" / "provenance.txt", text);
        Provenance p;
        CHECK(ParseProvenance(std::string(text.begin(), text.end()), p, error));
        CHECK(p.commit == "secondcommit" && p.date == "2026-10-02" && !p.dirty); // the last blessing
        CHECK(p.dispatches.size() == 3);
        // probe_a keeps the record of the blessing that wrote its images
        for (const char* key : { "0:0", "1:0" })
        {
            const ProvenanceEntry& e = p.dispatches[key];
            CHECK(e.kind == "probe_a" && e.commit == "firstcommit" && e.date == "2026-10-01" && e.dirty && e.warp.version == "10.0.1.0" && e.warp.hash == 0x1111);
        }
        const ProvenanceEntry& b = p.dispatches["0:1"];
        CHECK(b.kind == "probe_b" && b.commit == "secondcommit" && b.date == "2026-10-02" && !b.dirty);
    }

    // a kind that no dispatch has is reported by name (across the sets of the run)
    {
        const fs::path other = Temp("strict-other");
        SetWriter w(other);
        AddProbe(w, 0, 0, "only_here", 9, 8);
        w.m.frames = 1;
        w.Save();
        CHECK(BlessKindProblems(ParseBless("all"), { dir, other }).empty());
        CHECK(BlessKindProblems(ParseBless("probe_a,only_here"), { dir, other }).empty()); // one in each set
        CHECK(BlessKindProblems(ParseBless("probe_a"), { dir }).empty());
        const std::string problem = BlessKindProblems(ParseBless("probe_a,nosuchkind,alsonot"), { dir, other });
        CHECK(Contains(problem, "alsonot, nosuchkind") && !Contains(problem, "probe_a,") && Contains(problem, "Nothing was written"));
        fs::remove_all(other);
    }

    // a reference that cannot be written is a failure, and nothing is claimed as blessed
    {
        fs::remove_all(dir);
        SetWriter w(dir);
        BuildMini(w);
        WriteText(dir / "reference", "this is a file where the folder should be");
        const RunReport r = Run(dir, blobs, replayer, "all");
        CHECK(!r.ok && r.writeFailures >= 3 && r.blessed == 0);
        CHECK(AnyLineContains(r, "could not write the blessed image"));
        CHECK(!AnyLineContains(r, "BLESSED"));
    }
    // the provenance that cannot be written is a failure too
    {
        fs::remove_all(dir);
        SetWriter w(dir);
        BuildMini(w);
        CHECK(Run(dir, blobs, replayer, "all").ok);
        const fs::path provenance = dir / "reference" / "provenance.txt";
        const fs::perms writes = fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write; // Windows: read-only needs all three gone
        fs::permissions(provenance, writes, fs::perm_options::remove);
        const RunReport r = Run(dir, blobs, replayer, "probe_b");
        fs::permissions(provenance, writes, fs::perm_options::add);
        CHECK(!r.ok && r.writeFailures == 1);
        CHECK(AnyLineContains(r, "could not write") && AnyLineContains(r, "provenance.txt"));
        CHECK(!AnyLineContains(r, "BLESSED"));
    }

    // an adapter nobody could identify: nothing is replayed or written, whatever is asked
    {
        fs::remove_all(dir);
        SetWriter w(dir);
        BuildMini(w);
        for (const char* bless : { "", "all", "probe_a" })
        {
            RunOptions o = Options(dir, blobs);
            o.bless = ParseBless(bless);
            o.warp = GetWarpInfo(dir / "there-is-no-d3d10warp.dll");
            CHECK(!o.warp.Valid() && Contains(o.warp.error, "cannot read"));
            const RunReport r = RunReplaySet(o, replayer);
            CHECK(!r.ok && r.dispatches == 0 && AnyLineContains(r, "could not be identified"));
            CHECK(!fs::exists(dir / "reference"));
        }
        // and the real adapter is identified
        CHECK(GetWarpInfo().Valid());
    }
    fs::remove_all(dir);
}

// ---- 3. a GPU timeout abandons the set -------------------------------------------------------------------------------------

void TestTimeoutAbandonsTheSet(Blobs& blobs)
{
    CHECK(Replayer::kWaitMilliseconds >= 10u * 60u * 1000u); // WARP at 3840x2160 needs minutes, not a minute

    const fs::path dir = Temp("timeout");
    SetWriter w(dir);
    BuildMini(w);

    Replayer fresh;
    std::string error;
    CHECK(fresh.Init(error));
    fresh.SimulateTimeoutAtRun(2); // the second dispatch (0:1) is the one that "hangs"
    RunOptions o = Options(dir, blobs);
    o.bless = ParseBless("all");
    const RunReport r = RunReplaySet(o, fresh);
    if (!r.aborted)
        Dump(r);
    CHECK(!r.ok && r.aborted && fresh.Abandoned());
    CHECK(r.dispatches == 2 && r.blessed == 1); // 0:0 ran, 0:1 timed out, 1:0 was never replayed
    CHECK(AnyLineContains(r, "dispatch 0:1 kind=probe_b: the replay timed out on the GPU"));
    CHECK(AnyLineContains(r, "the replay stopped at dispatch 0:1 kind=probe_b"));
    CHECK(!fs::exists(dir / "reference" / ReferenceFileName(1, 0, 0)));
    CHECK(!AnyLineContains(r, "dispatch 1:0"));

    // nothing runs on an abandoned replayer
    SetFiles set;
    CHECK(LoadSet(dir, set, error));
    Blob blob;
    blobs.Lookup("probe", blob);
    std::map<uint32_t, Output> out;
    CHECK(!fresh.Run(set, set.manifest.dispatches[0], blob.data, blob.size, out, error) && Contains(error, "abandoned"));
    fs::remove_all(dir);
}

// ---- 4. the shader cache is keyed by content, the picture folder is emptied ----------------------------------------------

void TestShaderCacheAndOutputFolder(Replayer& replayer, Blobs& blobs)
{
    const fs::path dir = Temp("cache");
    SetWriter w(dir);
    BuildMini(w);
    SetFiles set;
    std::string error;
    CHECK(LoadSet(dir, set, error));
    const Dispatch& d = set.manifest.dispatches[0];

    const auto bytesOf = [&](const void* code, size_t size) {
        std::map<uint32_t, Output> out;
        const bool ran = replayer.Run(set, d, code, size, out, error);
        CHECK(ran && out.count(0) == 1);
        return ran && out.count(0) ? out[0].bytes : std::vector<uint8_t>();
    };

    // one buffer, refilled with another shader: the pipeline of the first must not be found at the same address
    const size_t sizeA = blobs.probe->GetBufferSize(), sizeB = blobs.probeEdited->GetBufferSize();
    std::vector<uint8_t> shared(std::max(sizeA, sizeB));
    std::memcpy(shared.data(), blobs.probe->GetBufferPointer(), sizeA);
    const std::vector<uint8_t> first = bytesOf(shared.data(), sizeA);
    std::memcpy(shared.data(), blobs.probeEdited->GetBufferPointer(), sizeB);
    const std::vector<uint8_t> second = bytesOf(shared.data(), sizeB);
    CHECK(!first.empty() && first != second);
    CHECK(second == bytesOf(blobs.probeEdited->GetBufferPointer(), sizeB));

    // two shaders of the same size that differ only in a constant, at different addresses
    const size_t sizeC = blobs.probeEditedAgain->GetBufferSize();
    CHECK(sizeB == sizeC);
    CHECK(std::memcmp(blobs.probeEdited->GetBufferPointer(), blobs.probeEditedAgain->GetBufferPointer(), sizeB) != 0);
    const std::vector<uint8_t> third = bytesOf(blobs.probeEditedAgain->GetBufferPointer(), sizeC);
    CHECK(third != second);
    // and the same address and size with different content
    std::vector<uint8_t> same(sizeB);
    std::memcpy(same.data(), blobs.probeEdited->GetBufferPointer(), sizeB);
    CHECK(bytesOf(same.data(), sizeB) == second);
    std::memcpy(same.data(), blobs.probeEditedAgain->GetBufferPointer(), sizeC);
    CHECK(bytesOf(same.data(), sizeC) == third);

    // the folder of difference pictures starts empty on every run
    {
        RunOptions o = Options(dir, blobs);
        o.bless = ParseBless("all");
        CHECK(RunReplaySet(o, replayer).ok);
        WriteText(dir / "out" / "diff_f9_d9_u0.ppm", "stale");
        WriteText(dir / "out" / "notes.txt", "stale");
        CHECK(RunReplaySet(Options(dir, blobs), replayer).ok);
        CHECK(!fs::exists(dir / "out" / "diff_f9_d9_u0.ppm") && !fs::exists(dir / "out" / "notes.txt"));
        // the folder is never one the set lives in
        for (const fs::path& bad : { dir, dir / "reference", dir / "images" })
        {
            RunOptions o2 = Options(dir, blobs);
            o2.outDir = bad;
            const RunReport r = RunReplaySet(o2, replayer);
            CHECK(!r.ok && r.dispatches == 0 && AnyLineContains(r, "difference pictures"));
        }
        CHECK(fs::exists(dir / "manifest.txt") && fs::exists(dir / "reference" / "provenance.txt") && fs::exists(dir / "images"));
    }
    fs::remove_all(dir);
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // a crash must not swallow the output so far
    if (argc != 2)
    {
        std::printf("pass the dlssnr.hlsl path\n");
        return 2;
    }
    fs::path shaders = fs::path(argv[1]).parent_path();

    Blobs blobs;
    blobs.dlssnr = Compile((shaders / L"dlssnr.hlsl").c_str(), nullptr, "dlssnr");
    blobs.guide = Compile((shaders / L"dlssnr_guide_match.hlsl").c_str(), nullptr, "guide_match");
    blobs.spatial = Compile((shaders / L"dlssnr_spatial.hlsl").c_str(), nullptr, "spatial");
    const D3D_SHADER_MACRO guides[] = { { "SPATIAL_GUIDES", "1" }, { nullptr, nullptr } };
    blobs.spatialGuides = Compile((shaders / L"dlssnr_spatial.hlsl").c_str(), nullptr, "spatial_guides", guides);
    blobs.probe = Compile(nullptr, kProbe, "probe");
    blobs.probeEdited = Compile(nullptr, kProbeEdited, "probe_edited");
    blobs.probeEditedAgain = Compile(nullptr, kProbeEditedAgain, "probe_edited_again");
    if (!blobs.dlssnr || !blobs.guide || !blobs.spatial || !blobs.spatialGuides || !blobs.probe || !blobs.probeEdited || !blobs.probeEditedAgain)
        return 2;

    Replayer replayer;
    std::string error;
    if (!replayer.Init(error))
    {
        std::printf("no software adapter: %s\n", error.c_str());
        return 2;
    }

    TestDepthPlanes(replayer);
    TestWorkflow(replayer, blobs);
    TestBadSets(replayer, blobs);
    TestIncompleteSets(replayer, blobs);
    TestBlessIsStrict(replayer, blobs);
    TestShaderCacheAndOutputFolder(replayer, blobs);
    TestTimeoutAbandonsTheSet(blobs);

    const WarpInfo warp = GetWarpInfo();
    CHECK(!warp.version.empty() && warp.hash != 0 && warp.Valid());
    std::printf("    adapter file version %s, hash %s\n", warp.version.c_str(), Hex64(warp.hash).c_str());

    std::printf(failures == 0 ? "nr_replay_synthetic: all checks passed\n" : "nr_replay_synthetic: %d check(s) failed\n", failures);
    return failures;
}
