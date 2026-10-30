// The Classic-enlargement sharpening shader end to end on the Direct3D 12 software adapter, with a replay
// set the test writes itself. No capture, no game and no product build are needed: the shader is compiled
// from source at run time, so the dispatches below run the real sharpen pass over small images.
//
// What it proves:
//  - each sharpening dispatch gives identical bytes when replayed twice (the property the replay scheme rests on);
//  - strength 0 is a copy: the output is the input to the byte, whatever the picture;
//  - the frame's own alpha is kept at every strength;
//  - a flat field is untouched by full strength (there is nothing to sharpen);
//  - a uniform negative picture clamps to zero rather than sharpening;
//  - a step edge moves at 0.4 and moves further at 1.0 (the strength orders the edit).
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include "ext/nr/diag/NrReplayEngine.h"
#include "ext/nr/pass/DlssNr_Common.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

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

ComPtr<ID3DBlob> Compile(const wchar_t* file)
{
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompileFromFile(file, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "CSMain", "cs_5_0",
                                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr))
    {
        std::printf("sharpen shader did not compile: %s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return nullptr;
    }
    return code;
}

uint16_t FloatToHalf(float f)
{
    // exact for the values the test uses (multiples of 1/64 below 4, and their negatives)
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

// one half-float pixel from four floats; the alpha channel carries a checker so a lost alpha shows
std::vector<uint8_t> Rgba16F(uint32_t w, uint32_t h, float r, float g, float b, float aEven, float aOdd)
{
    std::vector<uint16_t> px(static_cast<size_t>(w) * h * 4);
    const uint16_t hr = FloatToHalf(r), hg = FloatToHalf(g), hb = FloatToHalf(b);
    const uint16_t haEven = FloatToHalf(aEven), haOdd = FloatToHalf(aOdd);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
        {
            uint16_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = hr;
            p[1] = hg;
            p[2] = hb;
            p[3] = ((x + y) & 1) == 0 ? haEven : haOdd;
        }
    return Bytes(px);
}

// a vertical step edge: dark on the left, bright on the right
std::vector<uint8_t> StepEdge(uint32_t w, uint32_t h, float dark, float bright, float aEven, float aOdd)
{
    std::vector<uint16_t> px(static_cast<size_t>(w) * h * 4);
    const uint16_t hd = FloatToHalf(dark), hb = FloatToHalf(bright);
    const uint16_t haEven = FloatToHalf(aEven), haOdd = FloatToHalf(aOdd);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
        {
            uint16_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            const uint16_t v = x < w / 2 ? hd : hb;
            p[0] = p[1] = p[2] = v;
            p[3] = ((x + y) & 1) == 0 ? haEven : haOdd;
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
        m.frames = 1;
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

    Dispatch& Add(uint32_t frame, uint32_t index, const char* kind, const char* shader, const void* constants, size_t size,
                  uint32_t gx, uint32_t gy)
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

void BindAllSrv(Dispatch& d, uint32_t res, uint32_t view)
{
    for (uint32_t s = 0; s < kSrvSlots; ++s)
        d.bindings.push_back(SetWriter::Srv(s, res, view));
}

constexpr uint32_t W = 40, H = 24; // not multiples of the 8x8 group: the edges are partial groups
constexpr uint32_t F16 = DXGI_FORMAT_R16G16B16A16_FLOAT;

DlssNrConstants SharpenConstants(float strength)
{
    DlssNrConstants c {};
    c.Mode = DlssNrMode_ClassicSharpen;
    c.Width = W;
    c.Height = H;
    c.TransferStrength = strength; // the slot the shader reads as the sharpen amount
    return c;
}

// one sharpen dispatch over the given picture, with the pass's own kind and shader names
void AddSharpen(SetWriter& w, uint32_t index, const std::vector<uint8_t>& picture, float strength)
{
    const uint32_t row16 = W * 8;
    const auto c = SharpenConstants(strength);
    Dispatch& d = w.Add(0, index, "classic_sharpen", "classic_sharpen", &c, sizeof(c), (W + 7) / 8, (H + 7) / 8);
    d.resources = { SetWriter::Res(0, W, H, F16, w.Image_(picture, W, H, F16, row16)), SetWriter::Res(1, W, H, F16, kNoImage) };
    d.resources[1].after = w.Image_(picture, W, H, F16, row16);
    BindAllSrv(d, 0, F16);
    d.bindings.push_back(SetWriter::Uav(0, 1, F16));
    d.bindings.push_back(SetWriter::Uav(1, 1, F16));
}

std::map<uint32_t, Output> RunOnce(Replayer& replayer, const SetFiles& set, const Dispatch& d, const void* code, size_t size)
{
    std::map<uint32_t, Output> out;
    std::string error;
    if (!replayer.Run(set, d, code, size, out, error))
    {
        std::printf("    dispatch 0:%u (strength %.2f) did not run: %s\n", d.index,
                    *reinterpret_cast<const float*>(&d.constants[offsetof(DlssNrConstants, TransferStrength)]), error.c_str());
        out.clear();
    }
    return out;
}

// the alpha channel of every output pixel is the input alpha: the filter sharpens light, not coverage
void CheckAlphaKept(const std::vector<uint8_t>& in, const std::map<uint32_t, Output>& out, const char* what)
{
    if (out.count(0) == 0)
    {
        CHECK(false);
        return;
    }
    const auto& bytes = out.at(0).bytes;
    CHECK(bytes.size() == in.size());
    bool kept = bytes.size() == in.size();
    for (size_t i = 6; kept && i < bytes.size(); i += 8)
        kept = bytes[i] == in[i] && bytes[i + 1] == in[i + 1];
    if (!kept)
        std::printf("    %s: alpha channel changed\n", what);
    CHECK(kept);
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // a crash must not swallow the output so far
    if (argc != 2)
    {
        std::printf("pass the staged-shaders directory\n");
        return 2;
    }
    const fs::path shaders = fs::path(argv[1]) / L"shaders";
    const ComPtr<ID3DBlob> sharpen = Compile((shaders / L"dlssnr_classic_sharpen.hlsl").c_str());
    if (!sharpen)
        return 2;

    Replayer replayer;
    std::string error;
    if (!replayer.Init(error))
    {
        std::printf("no software adapter: %s\n", error.c_str());
        return 2;
    }

    const fs::path dir = fs::temp_directory_path() / "optiscaler-nr-classic-sharpen-synth";
    SetWriter w(dir);
    const std::vector<uint8_t> edge = StepEdge(W, H, 0.25f, 0.75f, 0.5f, 1.0f);
    const std::vector<uint8_t> flat = Rgba16F(W, H, 0.25f, 0.25f, 0.25f, 0.5f, 1.0f);
    const std::vector<uint8_t> negative = Rgba16F(W, H, -0.5f, -0.5f, -0.5f, 1.0f, 1.0f);
    AddSharpen(w, 0, edge, 0.0f);
    AddSharpen(w, 1, edge, 0.4f);
    AddSharpen(w, 2, edge, 1.0f);
    AddSharpen(w, 3, flat, 1.0f);
    AddSharpen(w, 4, negative, 1.0f);
    w.Save();

    SetFiles set;
    CHECK(LoadSet(dir, set, error));
    CHECK(set.manifest.dispatches.size() == 5);
    const void* code = sharpen->GetBufferPointer();
    const size_t codeSize = sharpen->GetBufferSize();

    // every dispatch replays to identical bytes: the property the replay scheme rests on
    std::vector<std::map<uint32_t, Output>> first, second;
    for (const Dispatch& d : set.manifest.dispatches)
    {
        first.push_back(RunOnce(replayer, set, d, code, codeSize));
        second.push_back(RunOnce(replayer, set, d, code, codeSize));
    }
    for (size_t i = 0; i < first.size(); ++i)
    {
        CHECK(!first[i].empty() && first[i].count(0) == 1 && !first[i][0].bytes.empty());
        CHECK(first[i][0].bytes == second[i][0].bytes);
    }
    if (failures != 0)
    {
        fs::remove_all(dir);
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("    determinism (two runs each, bit exact): classic_sharpen=identical\n");

    // strength 0 is a copy of the edge picture, to the byte
    CHECK(first[0][0].bytes == edge);
    CheckAlphaKept(edge, first[0], "strength 0");

    // full strength on a flat field has nothing to sharpen: the picture is untouched
    CHECK(first[3][0].bytes == flat);
    CheckAlphaKept(flat, first[3], "flat field at full strength");

    // a uniform negative picture clamps to zero rather than sharpening, keeping its alpha
    {
        const auto& bytes = first[4][0].bytes;
        CHECK(bytes.size() == negative.size());
        bool zero = bytes.size() == negative.size();
        for (size_t i = 0; zero && i < bytes.size(); i += 8)
        {
            uint16_t r, g, b;
            std::memcpy(&r, &bytes[i], 2);
            std::memcpy(&g, &bytes[i + 2], 2);
            std::memcpy(&b, &bytes[i + 4], 2);
            zero = r == 0 && g == 0 && b == 0;
        }
        if (!zero)
            std::printf("    negative input was not clamped to zero\n");
        CHECK(zero);
        CheckAlphaKept(negative, first[4], "negative input");
    }

    // the step edge moves at 0.4 and moves further at 1.0: the strength orders the edit
    CHECK(first[1][0].bytes != edge && first[2][0].bytes != edge);
    CHECK(first[1][0].bytes != first[0][0].bytes && first[2][0].bytes != first[0][0].bytes);
    CHECK(first[1][0].bytes != first[2][0].bytes);
    CheckAlphaKept(edge, first[1], "strength 0.4");
    CheckAlphaKept(edge, first[2], "strength 1.0");

    fs::remove_all(dir);
    if (failures != 0)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("nr_classic_sharpen_synth: all checks passed\n");
    return 0;
}
