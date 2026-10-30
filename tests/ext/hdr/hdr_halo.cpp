// HUD halo in HDR output: the strips and properties P1-P10, run on "engines" that each turn pixels into nits:
//   model-legacy / model-law   the CPU reference model (hdr_halo_model.h) of the frozen legacy shader and of the continuous law
//   shader-legacy              the FROZEN compiled legacy shader (tests/ext/hdr/legacy), on D3D11 WARP
//   shader-law-off / shader-law-on   (only with HALO_LAW) the current Hdr10.hlsl with legacy composition on / off
// Quantisation: the shader writes 10-bit PQ, about 0.9 percent per code between 200 and 1000 nits; shader engines get a relative slack of
// 2 percent + 0.3 nit on every comparison, the model engines none.
// Usage: hdr_halo.exe [--png <folder>]   writes a false-colour log-brightness strip and a tone-mapped strip per engine and scenario.
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <Hdr10Legacy_Shader.h>
#ifdef HALO_LAW
#include <Hdr10_Shader.h>
#endif
#include "hdr_halo_model.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <random>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
using namespace halo;

namespace
{
int g_failed = 0;
void Fail(const std::string& what)
{
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failed;
}

// ---- PQ decode ---------------------------------------------------------------------------------------------------------
double PqNits(unsigned code)
{
    const double p = std::pow(code / 1023.0, 32.0 / 2523.0);
    return 10000.0 * std::pow(std::max(p - 3424.0 / 4096.0, 0.0) / (2413.0 / 128.0 - 2392.0 / 128.0 * p), 16384.0 / 2610.0);
}
V3 Bt2020Nits(unsigned word) { return { PqNits(word & 1023), PqNits((word >> 10) & 1023), PqNits((word >> 20) & 1023) }; }

// ---- WARP runner --------------------------------------------------------------------------------------------------------
struct Cb
{
    UINT w, h;
    float paper, peak, expansion, contrast, saturation, vibrance;
    UINT scene, reshadeHighlights, referenceSrgb, hasMask;
    float rect[4];
    UINT continuous; // zero = legacy composition: an old 64 byte constant buffer leaves it (and the next fields) at zero
    float uiExpansion;
    UINT pad[2];
};
static_assert(sizeof(Cb) == 80);

struct Gpu
{
    ComPtr<ID3D11Device> d;
    ComPtr<ID3D11DeviceContext> c;
    ComPtr<ID3D11ComputeShader> legacy, current;
    bool ok = false;

    Gpu()
    {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d, nullptr, &c)))
            return;
        if (FAILED(d->CreateComputeShader(Hdr10Legacy_cso, sizeof(Hdr10Legacy_cso), nullptr, &legacy)))
            return;
#ifdef HALO_LAW
        if (FAILED(d->CreateComputeShader(Hdr10_cso, sizeof(Hdr10_cso), nullptr, &current)))
            return;
#endif
        ok = true;
    }

    ComPtr<ID3D11ShaderResourceView> Tex(const std::vector<float>& rgba, UINT n)
    {
        D3D11_TEXTURE2D_DESC t {};
        t.Width = n;
        t.Height = t.MipLevels = t.ArraySize = t.SampleDesc.Count = 1;
        t.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data { rgba.data(), n * 16, 0 };
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> view;
        if (FAILED(d->CreateTexture2D(&t, &data, &tex)) || FAILED(d->CreateShaderResourceView(tex.Get(), nullptr, &view)))
            std::abort();
        return view;
    }

    // One dispatch per chunk of pixels; `cbBytes` is 64 for the frozen shader (its constant buffer) and 80 for the current one.
    std::vector<unsigned> Run(ID3D11ComputeShader* shader, Cb cb, UINT cbBytes, const std::vector<Pixel>& pixels)
    {
        std::vector<unsigned> out;
        out.reserve(pixels.size());
        for (size_t base = 0; base < pixels.size(); base += 8192)
        {
            const UINT n = static_cast<UINT>(std::min<size_t>(8192, pixels.size() - base));
            std::vector<float> src(n * 4, 1.f), scene(n * 4, 1.f), ref(n * 4, 1.f), mask(n * 4, 1.f);
            for (UINT i = 0; i < n; ++i)
            {
                const Pixel& p = pixels[base + i];
                for (int k = 0; k < 3; ++k)
                {
                    src[i * 4 + k] = static_cast<float>(p.final[k]);
                    scene[i * 4 + k] = static_cast<float>(p.scene[k]);
                    ref[i * 4 + k] = static_cast<float>(p.reference[k]);
                }
                mask[i * 4] = static_cast<float>(p.hasMask ? p.mask : 1.0);
            }
            cb.w = n;
            cb.h = 1;
            D3D11_BUFFER_DESC bd {};
            bd.ByteWidth = cbBytes;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA init { &cb, 0, 0 };
            ComPtr<ID3D11Buffer> cbuf;
            if (FAILED(d->CreateBuffer(&bd, &init, &cbuf)))
                std::abort();
            auto s = Tex(src, n), sc = Tex(scene, n), rf = Tex(ref, n), mk = Tex(mask, n);
            D3D11_TEXTURE2D_DESC t {};
            t.Width = n;
            t.Height = t.MipLevels = t.ArraySize = t.SampleDesc.Count = 1;
            t.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
            t.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            ComPtr<ID3D11Texture2D> dst, stage;
            ComPtr<ID3D11UnorderedAccessView> uav;
            if (FAILED(d->CreateTexture2D(&t, nullptr, &dst)) || FAILED(d->CreateUnorderedAccessView(dst.Get(), nullptr, &uav)))
                std::abort();
            t.BindFlags = 0;
            t.Usage = D3D11_USAGE_STAGING;
            t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(d->CreateTexture2D(&t, nullptr, &stage)))
                std::abort();
            ID3D11ShaderResourceView* views[4] { s.Get(), sc.Get(), rf.Get(), mk.Get() };
            c->CSSetShader(shader, nullptr, 0);
            c->CSSetShaderResources(0, 4, views);
            c->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
            c->CSSetConstantBuffers(0, 1, cbuf.GetAddressOf());
            c->Dispatch((n + 7) / 8, 1, 1);
            ID3D11UnorderedAccessView* none = nullptr;
            c->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
            ID3D11ShaderResourceView* noViews[4] {};
            c->CSSetShaderResources(0, 4, noViews);
            c->CopyResource(stage.Get(), dst.Get());
            D3D11_MAPPED_SUBRESOURCE m {};
            if (FAILED(c->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m)))
                std::abort();
            const unsigned* w = static_cast<const unsigned*>(m.pData);
            out.insert(out.end(), w, w + n);
            c->Unmap(stage.Get(), 0);
        }
        return out;
    }
};

Cb MakeCb(const Config& cfg, bool continuous, int scene = 1, int hasMask = 1)
{
    Cb cb {};
    cb.paper = static_cast<float>(cfg.paper);
    cb.peak = static_cast<float>(cfg.peak);
    cb.expansion = static_cast<float>(cfg.expansion);
    cb.contrast = static_cast<float>(cfg.contrast);
    cb.saturation = static_cast<float>(cfg.saturation);
    cb.vibrance = static_cast<float>(cfg.vibrance);
    cb.scene = static_cast<UINT>(scene);
    cb.reshadeHighlights = cfg.reshadeHighlights ? 1u : 0u;
    cb.referenceSrgb = cfg.referenceSrgb ? 1u : 0u;
    cb.hasMask = static_cast<UINT>(hasMask);
    cb.rect[2] = cb.rect[3] = 1;
    cb.continuous = continuous ? 1u : 0u;
    cb.uiExpansion = static_cast<float>(cfg.uiExpansion < 0 ? cfg.expansion : cfg.uiExpansion);
    return cb;
}

// ---- engines --------------------------------------------------------------------------------------------------------------
struct Engine
{
    std::string name;
    bool quantised = false;
    std::function<std::vector<V3>(const Config&, const std::vector<Pixel>&)> run;
    V3 One(const Config& c, const Pixel& p) const { return run(c, std::vector<Pixel> { p })[0]; }
    double Slack(double nits) const { return quantised ? 0.02 * nits + 0.3 : 1e-6 * nits + 1e-6; }
};

Engine ModelEngine(const char* name, bool continuous)
{
    Engine e;
    e.name = name;
    e.run = [continuous](const Config& c, const std::vector<Pixel>& px)
    {
        std::vector<V3> out;
        for (const Pixel& p : px)
            out.push_back(OutputStage(c, Compose(c, p, continuous)));
        return out;
    };
    return e;
}

Engine ShaderEngine(Gpu& gpu, const char* name, bool frozen, bool continuous)
{
    Engine e;
    e.name = name;
    e.quantised = true;
    e.run = [&gpu, frozen, continuous](const Config& c, const std::vector<Pixel>& px)
    {
        const auto words = gpu.Run(frozen ? gpu.legacy.Get() : gpu.current.Get(), MakeCb(c, continuous), frozen ? 64u : 80u, px);
        std::vector<V3> out;
        for (unsigned w : words)
            out.push_back(Bt2020Nits(w));
        return out;
    };
    return e;
}

// ---- the world rule, independent of the engines ---------------------------------------------------------------------------
V3 WorldRule(const Config& c, const Pixel& p) // MapScenePeak(S * max(scale, 1)) of the final pixel: what a pixel with no HUD shows
{
    const Inputs in = Prepare(c, p);
    V3 w;
    for (int k = 0; k < 3; ++k)
        w[k] = in.S[k] * std::max(in.scale[k], 1.0);
    return MapScenePeak(c, w);
}

struct Verdict
{
    bool ok = true;
    double worst = 0; // the largest violation, in the unit named in `what`
    std::string what;
    void Note(bool good, double amount, const std::string& label)
    {
        if (!good && (ok || amount > worst))
        {
            ok = false;
            worst = amount;
            what = label;
        }
    }
};

double Rel(double a, double b) { return std::fabs(a - b) / std::max(std::max(std::fabs(a), std::fabs(b)), 1.0); }

// ---- strips and properties --------------------------------------------------------------------------------------------
// S6 / P1: a ReShade grade g on the world (no HUD, no mask): the law must not read it as HUD.
Verdict P1WorldGrade(const Engine& eng)
{
    Verdict v;
    for (double contrast : { 1.0, 0.5, 1.5 })
    {
        Config cfg;
        cfg.contrast = contrast;
        std::vector<Pixel> px;
        std::vector<std::string> label;
        for (double s : { 1.0, 2.0, 4.0 })
            for (double r : { 0.1, 0.25, 0.4, 0.55, 0.7, 0.8, 0.9, 0.95 })
                for (double g : { 0.8, 0.9, 1.0, 1.1, 1.2 })
                {
                    px.push_back(Gray(Sat(r * g), r, s, 1.0, false));
                    label.push_back("contrast " + std::to_string(contrast) + " scale " + std::to_string(s) + " ref " + std::to_string(r) + " grade " + std::to_string(g));
                }
        const auto out = eng.run(cfg, px);
        for (size_t i = 0; i < px.size(); ++i)
        {
            const double w = WorldRule(cfg, px[i])[0], e = out[i][0];
            v.Note(std::fabs(e - w) <= 0.12 * w + 0.5 + eng.Slack(w), std::fabs(e - w) / std::max(w, 0.5), label[i]);
        }
    }
    return v;
}

// P2: an opaque tagged pixel (t = 0) is the old HUD rule for any background and any scale.
Verdict P2Opaque(const Engine& eng)
{
    Verdict v;
    std::mt19937 rng(21);
    std::uniform_real_distribution<double> u(0, 1);
    for (double contrast : { 1.0, 0.7, 1.3 })
    {
        Config cfg;
        cfg.contrast = contrast;
        std::vector<Pixel> px;
        for (int i = 0; i < 4000; ++i)
        {
            Pixel p;
            const double ref = u(rng), sc = std::pow(10.0, 2 * u(rng));
            for (int k = 0; k < 3; ++k)
            {
                p.reference[k] = std::min(1.0, ref * (0.5 + u(rng)));
                p.scene[k] = p.reference[k] * std::pow(sc, 1 / 2.2);
                p.final[k] = u(rng);
            }
            p.mask = 0;
            px.push_back(p);
        }
        const auto out = eng.run(cfg, px);
        for (size_t i = 0; i < px.size(); ++i)
        {
            const V3 want = OutputStage(cfg, LegacyCompose(cfg, px[i])); // t = 0: the HUD rule of the old shader
            if (getenv("HALO_DEBUG") && std::fabs(out[i][0] - want[0]) > 0.3 * want[0] && eng.quantised)
            {
                const Pixel& p = px[i];
                std::printf("DEBUG pixel %zu final %.4f %.4f %.4f ref %.4f %.4f %.4f scene %.4f %.4f %.4f mask %.3f contrast %.2f: shader %.2f %.2f %.2f model %.2f %.2f %.2f\n", i, p.final[0], p.final[1], p.final[2],
                            p.reference[0], p.reference[1], p.reference[2], p.scene[0], p.scene[1], p.scene[2], p.mask, cfg.contrast, out[i][0], out[i][1], out[i][2], want[0], want[1], want[2]);
            }
            for (int k = 0; k < 3; ++k)
                v.Note(std::fabs(out[i][k] - want[k]) <= 0.01 * want[k] + 0.05 + eng.Slack(want[k]), Rel(out[i][k], want[k]),
                       "opaque pixel " + std::to_string(i));
        }
    }
    return v;
}

// The tagged white feather of R1: world ref, scale, mask t from 1 down to 0, final = t * ref + (1 - t) * 1 (encoded).
std::vector<Pixel> Feather(double ref, double scale, double hud, const std::vector<double>& ts)
{
    std::vector<Pixel> px;
    for (double t : ts)
        px.push_back(Gray(t * ref + (1 - t) * hud, ref, scale, t));
    return px;
}

// S4 / P3: glow tail alpha 1/255..10/255 over bright sky (rim R1): the faint tail must not move the world pixel by more than 10 percent.
Verdict S4GlowTail(const Engine& eng)
{
    Verdict v;
    Config cfg;
    for (double scale : { 1.0, 2.0, 4.0 })
        for (double ref : { 0.8, 0.92 })
        {
            std::vector<double> ts;
            for (int k = 0; k <= 10; ++k)
                ts.push_back(1 - k / 255.0);
            const auto px = Feather(ref, scale, 1.0, ts);
            const auto out = eng.run(cfg, px);
            const double world = out[0][0];
            for (size_t i = 0; i < px.size(); ++i)
                v.Note(std::fabs(out[i][0] - world) <= 0.10 * world + eng.Slack(world), std::fabs(out[i][0] - world) / world,
                       "scale " + std::to_string(scale) + " ref " + std::to_string(ref) + " alpha " + std::to_string(i) + "/255");
        }
    return v;
}

// S10 / P3: a tagged feather (t swept from 1 down) AND an untagged glow on the same pixels: no step at the t = 1 seam.
Verdict S10Seam(const Engine& eng)
{
    Verdict v;
    Config cfg;
    // (reference, glow): a small glow the grade tolerance absorbs, and a large glow over a dark world that it cannot (HUD light at t = 1)
    const double cases[][2] = { { 0.8, 0.0 }, { 0.8, 0.06 }, { 0.4, 0.35 } };
    for (double scale : { 1.0, 4.0 })
        for (const auto& cs : cases)
        {
            const double ref = cs[0], glow = cs[1];
            std::vector<Pixel> px;
            for (int k = 0; k <= 25; ++k)
            {
                const double t = 1 - k / 255.0;
                px.push_back(Gray(Sat(t * ref + (1 - t) * 1.0 + glow), ref, scale, t));
            }
            const auto out = eng.run(cfg, px);
            // Smoothness: the feather itself is a steep ramp (hundreds of nits over ~25 steps), so the bound is relative to the strip: no single
            // step may exceed 4 percent of the strip's peak or 4 times the mean step, whichever is larger. The old shader steps by 50-150 percent in ONE
            // step (about 25 times the mean step).
            double top = 0, total = 0;
            for (size_t i = 0; i < out.size(); ++i)
            {
                top = std::max(top, out[i][0]);
                if (i)
                    total += std::fabs(out[i][0] - out[i - 1][0]);
            }
            const double bound = std::max(0.04 * top, 4.0 * total / double(out.size() - 1));
            for (size_t i = 1; i < px.size(); ++i)
                v.Note(std::fabs(out[i][0] - out[i - 1][0]) <= bound + eng.Slack(out[i][0]) * 0.5, std::fabs(out[i][0] - out[i - 1][0]) / out[0][0],
                       "scale " + std::to_string(scale) + " ref " + std::to_string(ref) + " glow " + std::to_string(glow) + " step " + std::to_string(i));
        }
    return v;
}

// P4: a tagged anti-aliased ramp stays between the world side and the HUD side and moves one way.
Verdict P4Envelope(const Engine& eng)
{
    Verdict v;
    Config cfg;
    for (double scale : { 1.0, 2.0, 4.0 })
        for (double ref : { 0.3, 0.6, 0.8, 0.92 })
            for (double hud : { 1.0, 0.7, 0.35, 0.0 })
            {
                std::vector<double> ts;
                for (int k = 0; k <= 255; k += 5)
                    ts.push_back(1 - k / 255.0);
                ts.push_back(0);
                const auto px = Feather(ref, scale, hud, ts);
                const auto out = eng.run(cfg, px);
                const double ew = out.front()[0], eh = out.back()[0];
                if (std::fabs(ew - eh) < 0.1 * std::max(ew, eh))
                    continue; // both sides alike: there is no envelope to stay inside
                const double hi = std::max(ew, eh), lo = std::min(ew, eh), dir = eh >= ew ? 1 : -1;
                double previous = ew;
                for (size_t i = 0; i < out.size(); ++i)
                {
                    const double e = out[i][0];
                    const std::string label = "scale " + std::to_string(scale) + " ref " + std::to_string(ref) + " hud " + std::to_string(hud) + " step " + std::to_string(i);
                    v.Note(e <= hi * 1.07 + eng.Slack(hi) && e >= lo * 0.93 - eng.Slack(lo), std::max((e - hi) / hi, (lo - e) / std::max(lo, 1.0)), "outside the envelope: " + label);
                    v.Note((e - previous) * dir >= -(0.02 * hi + eng.Slack(hi)), -(e - previous) * dir / hi, "not monotone: " + label);
                    previous = e;
                }
            }
    return v;
}

// P5: bounded and finite on the extremes.
Verdict P5Bounded(const Engine& eng)
{
    Verdict v;
    Config cfg;
    std::vector<Pixel> px;
    for (double f : { 0.0, 1e-6, 0.5, 1.0 })
        for (double r : { 0.0, 1e-6, 0.5, 1.0 })
            for (double scene : { 0.0, 1e-9, 1.0, 1e4 })
                for (double t : { 0.0, 1 / 255.0, 0.5, 254 / 255.0, 1.0 })
                {
                    Pixel p;
                    for (int k = 0; k < 3; ++k)
                    {
                        p.final[k] = f;
                        p.reference[k] = r;
                        p.scene[k] = scene;
                    }
                    p.mask = t;
                    px.push_back(p);
                }
    const auto out = eng.run(cfg, px);
    for (size_t i = 0; i < px.size(); ++i)
        for (int k = 0; k < 3; ++k)
            v.Note(std::isfinite(out[i][k]) && out[i][k] >= -0.5 && out[i][k] <= cfg.peak * 1.05, std::isfinite(out[i][k]) ? out[i][k] : 1e9, "pixel " + std::to_string(i));
    return v;
}

// P6: an error in the reference moves the output by as much as the error, not by the scene scale (the gain is held: scene and reference move together).
Verdict P6ReferenceError(const Engine& eng)
{
    Verdict v;
    Config cfg;
    for (double scale : { 1.0, 4.0 })
        for (double r : { 0.3, 0.6, 0.9 })
            for (double err : { -0.06, 0.06 })
            {
                Pixel p = Gray(r, r, scale, 1.0, false), q = p;
                for (int k = 0; k < 3; ++k)
                {
                    q.reference[k] = Sat(r * (1 + err));
                    q.scene[k] = p.scene[k] * (1 + err);
                }
                const double e0 = eng.One(cfg, p)[0], e1 = eng.One(cfg, q)[0];
                v.Note(std::fabs(e1 - e0) <= 0.08 * e0 + eng.Slack(e0), std::fabs(e1 - e0) / e0, "scale " + std::to_string(scale) + " ref " + std::to_string(r) + " err " + std::to_string(err));
            }
    return v;
}

// P7: a coloured HUD over a neutral world keeps its channel order, neutral stays neutral.
Verdict P7Hue(const Engine& eng)
{
    Verdict v;
    Config cfg;
    for (double scale : { 1.0, 4.0 })
        for (int k = 0; k <= 20; ++k)
        {
            const double a = k / 20.0;
            Pixel p;
            const V3 hud { 1.0, 0.5, 0.0 }, sky { 0.55, 0.55, 0.55 };
            for (int c = 0; c < 3; ++c)
            {
                p.reference[c] = sky[c];
                p.scene[c] = sky[c] * std::pow(scale, 1 / 2.2);
                p.final[c] = (1 - a) * sky[c] + a * hud[c];
            }
            p.mask = 1 - a; // tagged
            const V3 e = eng.One(cfg, p);
            v.Note(e[0] >= e[1] * 0.98 - eng.Slack(e[1]) && e[1] >= e[2] * 0.98 - eng.Slack(e[2]), 1.0, "orange keeps r >= g >= b, alpha " + std::to_string(a));
            Pixel n = p;
            for (int c = 0; c < 3; ++c)
                n.final[c] = (1 - a) * sky[c] + a * 0.9;
            const V3 g = eng.One(cfg, n);
            v.Note(std::fabs(g[0] - g[1]) <= eng.Slack(g[0]) * 0.5 + 0.02 * g[0] && std::fabs(g[1] - g[2]) <= eng.Slack(g[0]) * 0.5 + 0.02 * g[0], std::fabs(g[0] - g[2]) / g[0], "neutral stays neutral, alpha " + std::to_string(a));
        }
    return v;
}

// P8 / S7: an additive glow (no mask, final above the reference) adds light and never subtracts.
Verdict P8Additive(const Engine& eng)
{
    Verdict v;
    Config cfg;
    for (double scale : { 1.0, 4.0 })
        for (double ref : { 0.3, 0.6, 0.9 })
        {
            std::vector<Pixel> px;
            for (int k = 0; k <= 40; ++k)
                px.push_back(Gray(Sat(ref + k / 40.0 * 0.4), ref, scale, 1.0, false));
            const auto out = eng.run(cfg, px);
            for (size_t i = 1; i < out.size(); ++i)
                v.Note(out[i][0] >= out[i - 1][0] - (0.02 * out[i - 1][0] + eng.Slack(out[i][0])), (out[i - 1][0] - out[i][0]) / out[i - 1][0], "scale " + std::to_string(scale) + " ref " + std::to_string(ref) + " glow " + std::to_string(i));
        }
    return v;
}

// S1 / S12 / P9: an untagged white anti-aliasing ramp (brightening) and a black shadow ramp (darkening), t = 1 and no mask: monotone, bounded.
Verdict P9UntaggedRamps(const Engine& eng)
{
    Verdict v;
    Config cfg;
    for (double scale : { 1.0, 4.0, 10.0 })
        for (double ref : { 0.15, 0.5, 0.8, 0.92 })
            for (int dark = 0; dark < 2; ++dark)
            {
                std::vector<Pixel> px;
                for (int k = 0; k <= 32; ++k)
                {
                    const double a = k / 32.0;
                    px.push_back(Gray(dark ? ref * (1 - a) : ref + (1 - ref) * a, ref, scale, 1.0, false));
                }
                const auto out = eng.run(cfg, px);
                for (size_t i = 1; i < out.size(); ++i)
                {
                    const double step = (out[i][0] - out[i - 1][0]) * (dark ? -1 : 1);
                    v.Note(step >= -(0.02 * std::max(out[i][0], out[i - 1][0]) + eng.Slack(out[i][0])), -step / std::max(out[i - 1][0], 1.0),
                           std::string(dark ? "darkening" : "brightening") + " ramp, scale " + std::to_string(scale) + " ref " + std::to_string(ref) + " step " + std::to_string(i));
                }
            }
    return v;
}

// S2: 1-pixel white and dark lines over sky with a lightly blurred reference (the resampled reference wobbles by +-0.02
// around the sky): a thin white line must not be lifted by more than the full-white interface factor over its local
// background (the legacy colour band hands the line to the interface curve: about 5x over sky at scale 1, measured),
// and a thin dark line must track the frame\'s own dimming within a factor of two either way.
Verdict S2SinglePixelLines(const Engine& eng)
{
    Verdict v;
    Config cfg;
    for (double scale : { 1.0, 4.0 })
        for (double sky : { 0.8, 0.92 })
        {
            std::vector<Pixel> px;
            for (int x = 0; x < 9; ++x)
            {
                const double blur = sky + ((x % 2 == 0) ? -0.02 : 0.02);
                double final = sky;
                if (x == 2)
                    final = 1.0; // 1-pixel white line
                else if (x == 6)
                    final = 0.2; // 1-pixel dark line
                px.push_back(Gray(final, Sat(blur), scale, 1.0, false));
            }
            const auto out = eng.run(cfg, px);
            const double bg = (out[0][0] + out[1][0] + out[3][0] + out[4][0] + out[5][0] + out[7][0] + out[8][0]) / 7.0;
            v.Note(out[2][0] >= bg * 0.98 - eng.Slack(bg) && out[2][0] <= bg * 2.2 + eng.Slack(bg), out[2][0] / bg,
                   "white line over sky " + std::to_string(sky) + " scale " + std::to_string(scale));
            const Inputs bgIn = Prepare(cfg, px[0]);
            const Inputs lnIn = Prepare(cfg, px[6]);
            const double sdrRatio = Luma(bgIn.S0) / std::max(Luma(lnIn.S0), 1e-3);
            const double outRatio = out[0][0] / std::max(out[6][0], 1e-3);
            v.Note(outRatio >= sdrRatio * 0.5 - eng.Slack(out[0][0]) && outRatio <= sdrRatio * 2.0 + eng.Slack(out[0][0]), outRatio / sdrRatio,
                   "dark line over sky " + std::to_string(sky) + " scale " + std::to_string(scale));
        }
    return v;
}

// S3: a black shadow, tagged and untagged, alpha 0..0.6 over sky: the pixel follows the SDR frame's own dimming (the world rule on the dimmed pixel).
Verdict S3Shadow(const Engine& eng)
{
    Verdict v;
    Config cfg;
    for (int tagged = 0; tagged < 2; ++tagged)
        for (double scale : { 1.0, 4.0 })
        {
            std::vector<Pixel> px;
            for (int k = 0; k <= 12; ++k)
            {
                const double a = k * 0.05;
                px.push_back(Gray(0.9 * (1 - a), 0.9, scale, tagged ? 1 - a : 1.0, tagged != 0));
            }
            const auto out = eng.run(cfg, px);
            for (size_t i = 0; i < px.size(); ++i)
            {
                const double want = WorldRule(cfg, px[i])[0];
                v.Note(std::fabs(out[i][0] - want) <= 0.10 * want + 0.5 + eng.Slack(want), std::fabs(out[i][0] - want) / std::max(want, 0.5),
                       std::string(tagged ? "tagged" : "untagged") + " shadow, scale " + std::to_string(scale) + " alpha " + std::to_string(i * 0.05));
            }
        }
    return v;
}

// S5: a fullscreen fade 0..100 percent (the whole frame darkens, nothing is HUD): the world rule on the dimmed pixel at every level.
Verdict S5Fade(const Engine& eng)
{
    Verdict v;
    Config cfg;
    for (double scale : { 1.0, 4.0 })
        for (double ref : { 0.5, 0.92 })
        {
            std::vector<Pixel> px;
            for (int k = 0; k <= 20; ++k)
                px.push_back(Gray(ref * (1 - k / 20.0), ref, scale, 1.0, false));
            const auto out = eng.run(cfg, px);
            for (size_t i = 0; i < px.size(); ++i)
            {
                const double want = WorldRule(cfg, px[i])[0];
                v.Note(std::fabs(out[i][0] - want) <= 0.12 * want + 0.5 + eng.Slack(want), std::fabs(out[i][0] - want) / std::max(want, 0.5),
                       "scale " + std::to_string(scale) + " ref " + std::to_string(ref) + " fade " + std::to_string(i * 5) + "%");
            }
        }
    return v;
}

// S11 / P10: sharp world edges against a reference that was resampled 1.5x / 1.7x / 2x (a ramp 3 / 3.4 / 4 px wide), no HUD:
// every pixel within max(12 percent, 5 nits) of what the old shader shows on the same input.
Verdict P10WorldDetail(const Engine& eng)
{
    Verdict v;
    Config cfg;
    for (double width : { 3.0, 3.4, 4.0 })
        for (double hi : { 0.9, 0.6 })
        {
            std::vector<Pixel> px;
            for (int x = -6; x <= 6; ++x)
            {
                const double u = Sat((x + width / 2) / width), refEnc = 0.3 + (hi - 0.3) * u, gain = 1 + 3 * u;
                Pixel p = Gray(x < 0 ? 0.3 : hi, refEnc, gain, 1.0, false);
                px.push_back(p);
            }
            const auto out = eng.run(cfg, px);
            for (size_t i = 0; i < px.size(); ++i)
            {
                const double old = LegacyCompose(cfg, px[i])[0];
                v.Note(std::fabs(out[i][0] - old) <= std::max(0.12 * old, 5.0) + eng.Slack(old), std::fabs(out[i][0] - old) / std::max(old, 5.0),
                       "ramp " + std::to_string(width) + " edge " + std::to_string(hi) + " x " + std::to_string(static_cast<int>(i) - 6));
            }
        }
    return v;
}

struct Prop
{
    const char* id;
    const char* what;
    Verdict (*run)(const Engine&);
    bool ring; // a ring strip: the frozen legacy shader must FAIL it
};
const Prop kProps[] = {
    { "P1  S6   world under a ReShade grade 0.8-1.2", "", &P1WorldGrade, false },
    { "P2       opaque tagged pixel is the HUD rule", "", &P2Opaque, false },
    { "P3  S4   faint glow tail (alpha 1-10/255) over bright sky", "", &S4GlowTail, true },
    { "P3  S10  tagged feather + untagged glow, t sweeps from 1", "", &S10Seam, true },
    { "P4       tagged ramp inside the envelope, monotone", "", &P4Envelope, false },
    { "P5       bounded and finite on extremes", "", &P5Bounded, false },
    { "P6       6 percent reference error", "", &P6ReferenceError, false },
    { "P7  S9   hue order and neutral", "", &P7Hue, false },
    { "P8  S7   additive glow never subtracts", "", &P8Additive, false },
    { "P9  S1   untagged ramps monotone", "", &P9UntaggedRamps, true },
    { "S2   1-pixel lines over sky, blurred reference", "", &S2SinglePixelLines, true },
    { "    S3   black shadow follows the SDR dimming", "", &S3Shadow, true },
    { "    S5   fullscreen fade follows the SDR dimming", "", &S5Fade, true },
    { "P10 S11  sharp world edges, resampled reference", "", &P10WorldDetail, false },
};

std::map<std::string, std::map<std::string, Verdict>> g_results; // engine -> property -> verdict

void RunAll(const Engine& eng)
{
    for (const Prop& p : kProps)
    {
        Verdict v = p.run(eng);
        g_results[eng.name][p.id] = v;
        std::printf("  %-8s %-62s %s%s\n", eng.name.c_str(), p.id, v.ok ? "pass" : "FAIL", v.ok ? "" : ("  worst " + std::to_string(v.worst * 100).substr(0, 6) + "%: " + v.what).c_str());
    }
}

// The frozen-shader regression numbers (config 203/1100/0.5): the halo, measured on the compiled legacy blob.
void StudyNumbers(const Engine& legacy)
{
    Config cfg;
    auto closeTo = [&](const char* label, double got, double want, double tol)
    {
        if (std::fabs(got - want) > tol * want + 0.5)
            Fail(std::string("study number ") + label + ": got " + std::to_string(got) + ", want " + std::to_string(want));
    };
    auto feather = [&](double ref, double scale, double t) { return legacy.One(cfg, Gray(t * ref + (1 - t) * 1.0, ref, scale, t))[0]; };
    closeTo("R1 t=1", feather(0.92, 1, 1.0), 168.0, 0.03);
    closeTo("R1 t=0.99", feather(0.92, 1, 0.99), 425.3, 0.03);
    closeTo("R1 t=0.98", feather(0.92, 1, 0.98), 426.3, 0.03);
    closeTo("R1 t=0.5", feather(0.92, 1, 0.5), 468.7, 0.03);
    closeTo("R2 t=1", feather(0.92, 4, 1.0), 511.0, 0.03);
    closeTo("R2 t=0.99", feather(0.92, 4, 0.99), 764.8, 0.03);
    closeTo("R3 t=1", feather(0.5, 1, 1.0), 43.5, 0.03);
    closeTo("R3 t=0.99", feather(0.5, 1, 0.99), 45.0, 0.03);
    closeTo("C1 scale 1", legacy.One(cfg, Gray(1.0, 0.92, 1, 1.0, false))[0], 203, 0.03);
    closeTo("C1 scale 10", legacy.One(cfg, Gray(1.0, 0.92, 10, 1.0, false))[0], 805, 0.03);
    closeTo("C4 alpha 0.3", legacy.One(cfg, Gray(0.9 * 0.7, 0.9, 4, 1.0, false))[0], 91, 0.05);
    Config strong;
    strong.expansion = 0; // R5: with HighlightExpansion = 0 the same branch is continuous: proves the cause
    const double a = legacy.One(strong, Gray(0.92, 0.92, 1, 1.0))[0], b = legacy.One(strong, Gray(0.99 * 0.92 + 0.01, 0.92, 1, 0.99))[0];
    if (std::fabs(a - b) > 0.02 * a)
        Fail("R5: with expansion 0 the legacy branch must be continuous");
}

// The CPU model of the old shader must agree with the frozen blob (so the model can stand in for it in the properties above).
void ModelAgrees(const Engine& model, const Engine& shader, const char* what)
{
    std::mt19937 rng(5);
    std::uniform_real_distribution<double> u(0, 1);
    double worst = 0;
    for (int round = 0; round < 6; ++round)
    {
        Config cfg;
        cfg.contrast = round % 3 == 0 ? 1.0 : (round % 3 == 1 ? 0.7 : 1.3);
        cfg.reshadeHighlights = round >= 3;
        cfg.paper = round % 2 ? 240 : 203;
        cfg.peak = round % 2 ? 1015 : 1100;
        cfg.expansion = round % 2 ? 0.6 : 0.5;
        std::vector<Pixel> px;
        for (int i = 0; i < 3000; ++i)
        {
            Pixel p;
            const double sc = std::pow(10.0, 1.2 * u(rng));
            for (int k = 0; k < 3; ++k)
            {
                p.reference[k] = u(rng);
                p.scene[k] = p.reference[k] * std::pow(sc, 1 / 2.2);
                p.final[k] = (i % 3 == 0) ? p.reference[k] : (i % 3 == 1 ? Sat(p.reference[k] + 0.2 * (u(rng) - .5)) : u(rng));
            }
            p.mask = (i % 4 == 0) ? 0.0 : ((i % 4 == 1) ? std::floor(u(rng) * 256) / 255 : 1.0);
            px.push_back(p);
        }
        const auto a = model.run(cfg, px), b = shader.run(cfg, px);
        for (size_t i = 0; i < px.size(); ++i)
            for (int k = 0; k < 3; ++k)
            {
                const double e = std::fabs(a[i][k] - b[i][k]) - shader.Slack(a[i][k]) * 0.6;
                worst = std::max(worst, e / std::max(a[i][k], 5.0));
            }
    }
    std::printf("  %s: model and compiled shader agree within quantisation (worst excess %.2f%%)\n", what, worst * 100);
    if (worst > 0.0)
        Fail(std::string(what) + ": the model and the compiled shader disagree");
}

// The frozen oracle must stay the compiled shader of main 191c89b2: pin its bytes (FNV-1a) so an accidental
// edit of the fixture fails loudly instead of silently moving the legacy baseline.
void CheckFrozenFixture()
{
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof(Hdr10Legacy_cso); ++i)
    {
        hash ^= Hdr10Legacy_cso[i];
        hash *= 1099511628211ull;
    }
    constexpr uint64_t want = 0x06448cf310cb446bull; // FNV-1a of the Hdr10Legacy_cso bytes fxc emits
    char text[160];
    std::snprintf(text, sizeof text, "frozen legacy fixture changed: FNV-1a %016llx, want %016llx",
                  (unsigned long long)hash, (unsigned long long)want);
    if (hash != want)
        Fail(text);
    else
        std::printf("  frozen fixture: %zu bytes, hash %016llx\n", sizeof(Hdr10Legacy_cso),
                      (unsigned long long)hash);
}

// NaN or Inf in Scene or Source (FP16 sources can carry them): the continuous law must emit a pixel that is
// exactly as (non-)finite as the legacy composition, lane by lane, so the new branch adds no new failure mode.
// Compared after the output stage, which is what the shader emits: its BT.2020 matrix mixes all three lanes, so a
// NaN anywhere poisons the whole emitted pixel in both compositions alike. (Before that stage the two formulas
// contain a single-lane NaN differently through their shared scalars; that never reaches the screen.) Model only:
// what a UNORM store does with a NaN is hardware-defined, so the shaders are pinned only on finite inputs.
void NaNInfParity()
{
    const double specials[] = { std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                                -std::numeric_limits<double>::infinity() };
    int checked = 0;
    for (double special : specials)
        for (int which = 0; which < 3; ++which) // final, reference, scene
            for (double t : { 0.0, 0.5, 1.0 })
                for (int channels = 0; channels < 2; ++channels) // all three lanes, then lane 0 alone
                {
                    Pixel p = Gray(0.6, 0.5, 2.0, t);
                    V3& lane = which == 0 ? p.final : (which == 1 ? p.reference : p.scene);
                    for (int k = 0; k < 3 && (channels == 0 || k == 0); ++k)
                        lane[k] = special;
                    const Config cfg;
                    const V3 old = OutputStage(cfg, LegacyCompose(cfg, p)),
                             law = OutputStage(cfg, ContinuousCompose(cfg, p));
                    for (int k = 0; k < 3; ++k)
                    {
                        ++checked;
                        if (std::isfinite(old[k]) != std::isfinite(law[k]))
                        {
                            char msg[256];
                            std::snprintf(msg, sizeof msg, "NaN/Inf parity: %g in %s lanes %s t %.2f lane %d: legacy %g, law %g",
                                          special, which == 0 ? "final" : (which == 1 ? "reference" : "scene"),
                                          channels == 0 ? "all" : "lane 0", t, k, old[k], law[k]);
                            Fail(msg);
                        }
                    }
                }
    std::printf("  NaN/Inf parity: %d lanes, the continuous law is as (non-)finite as the legacy composition\n",
                  checked);
}

#ifdef HALO_LAW
// With LegacyComposition = true (the default) the current shader must produce EXACTLY the words of the frozen shader of main.
void LegacyIsByteForByte(Gpu& gpu)
{
    std::mt19937 rng(77);
    std::uniform_real_distribution<double> u(0, 1);
    size_t checked = 0, differing = 0;
    for (int round = 0; round < 48; ++round)
    {
        Config cfg;
        cfg.contrast = (round % 3 == 0) ? 1.0 : ((round % 3 == 1) ? 0.5 : 1.5);
        cfg.reshadeHighlights = (round / 3) % 2;
        cfg.referenceSrgb = (round / 6) % 2;
        cfg.paper = (round / 12) % 2 ? 240 : 203;
        cfg.peak = (round / 12) % 2 ? 1015 : 1100;
        cfg.expansion = (round % 4 == 0) ? 0.0 : 0.5;
        const int scene = round % 2; // SceneMode 1 and 0 (the default SDR-expansion path)
        const int hasMask = (round / 2) % 2;
        const int grade = (round / 4) % 2; // default grading, then a non-default saturation and vibrance
        cfg.saturation = grade ? 1.3 : 1.0;
        cfg.vibrance = grade ? 0.5 : 0.0;
        const int uiCase = (round / 8) % 3; // auto, 0.9, 0: the legacy composition must ignore UiExpansion
        cfg.uiExpansion = uiCase == 0 ? -1.0 : (uiCase == 1 ? 0.9 : 0.0);
        std::vector<Pixel> px;
        const double masks[] = { 0.0, 1 / 255.0, 128 / 255.0, 254 / 255.0, 1.0 };
        for (int i = 0; i < 4000; ++i)
        {
            Pixel p;
            const double sc = std::pow(10.0, 2.5 * u(rng));
            for (int k = 0; k < 3; ++k)
            {
                p.reference[k] = (i % 7 == 0) ? 0.0 : u(rng);
                p.scene[k] = p.reference[k] * std::pow(sc, 1 / 2.2) * (i % 11 == 0 ? 0.0 : 1.0);
                p.final[k] = (i % 3 == 0) ? p.reference[k] : (i % 3 == 1 ? Sat(p.reference[k] + 0.3 * (u(rng) - .5)) : u(rng));
            }
            p.mask = hasMask ? masks[i % 5] : 1.0;
            p.hasMask = hasMask != 0;
            px.push_back(p);
        }
        const auto frozen = gpu.Run(gpu.legacy.Get(), MakeCb(cfg, false, scene, hasMask), 64, px);
        const auto current = gpu.Run(gpu.current.Get(), MakeCb(cfg, false, scene, hasMask), 80, px);
        // an old 64 byte constant buffer (the layout every existing test and tool uses) must also select the legacy path
        const auto shortBuffer = gpu.Run(gpu.current.Get(), MakeCb(cfg, true, scene, hasMask), 64, px);
        for (size_t i = 0; i < px.size(); ++i)
        {
            ++checked;
            if (frozen[i] != current[i] || frozen[i] != shortBuffer[i])
                ++differing;
        }
    }
    std::printf("  legacy path: %zu pixels over 48 configurations, %zu differ from the frozen shader of main\n", checked, differing);
    if (differing)
        Fail("LegacyComposition = true is not byte-for-byte the shader of main");
}
#endif
// ---- PNG dump (hdr_halo.exe --png <folder>): two images per strip (the legacy shader and the law): a false-colour
// log-brightness map and a plain tone-mapped copy, viewable on any SDR screen. A minimal writer (stored deflate
// blocks, no dependency); each strip is one row of pixels repeated over 32 rows so a 1-pixel line stays visible.
namespace dump
{
uint32_t Crc(const unsigned char* data, size_t n)
{
    static uint32_t tab[256] = {};
    static bool built = false;
    if (!built)
    {
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            tab[i] = c;
        }
        built = true;
    }
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; ++i)
        c = tab[(c ^ data[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}
void PutU32(std::vector<unsigned char>& out, uint32_t v)
{
    out.push_back(static_cast<unsigned char>((v >> 24) & 0xff));
    out.push_back(static_cast<unsigned char>((v >> 16) & 0xff));
    out.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
    out.push_back(static_cast<unsigned char>(v & 0xff));
}
void Chunk(std::vector<unsigned char>& out, const char* type, const unsigned char* data, size_t n)
{
    PutU32(out, static_cast<uint32_t>(n));
    const size_t at = out.size();
    for (int k = 0; k < 4; ++k)
        out.push_back(static_cast<unsigned char>(type[k]));
    out.insert(out.end(), data, data + n);
    PutU32(out, Crc(out.data() + at, 4 + n));
}
void Colour(double nits, double peak, unsigned char* rgb, bool tone)
{
    if (tone)
    {
        const unsigned char g = static_cast<unsigned char>(255 * std::pow(std::min(nits / peak, 1.0), 1 / 2.2));
        rgb[0] = rgb[1] = rgb[2] = g;
        return;
    }
    static const double stops[][4] = { { 0.0, 0, 0, 32 }, { 0.25, 0, 64, 255 }, { 0.45, 0, 200, 255 }, { 0.6, 0, 200, 64 },
                                       { 0.75, 255, 220, 0 }, { 0.9, 255, 64, 0 }, { 1.0, 255, 255, 255 } };
    const double t = std::min(std::log10(nits + 1) / std::log10(peak + 1), 1.0);
    for (int s = 1; s < 7; ++s)
        if (t <= stops[s][0])
        {
            const double f = (t - stops[s - 1][0]) / (stops[s][0] - stops[s - 1][0]);
            for (int c = 0; c < 3; ++c)
                rgb[c] = static_cast<unsigned char>(stops[s - 1][c + 1] + f * (stops[s][c + 1] - stops[s - 1][c + 1]));
            return;
        }
}
bool Write(const std::string& path, const std::vector<V3>& nits, double peak, bool tone)
{
    const int w = static_cast<int>(nits.size()), h = 32;
    std::vector<unsigned char> raw;
    raw.reserve(static_cast<size_t>(3 * w + 1) * h);
    std::vector<unsigned char> px(3);
    for (int y = 0; y < h; ++y)
    {
        raw.push_back(0);
        for (const V3& e : nits)
        {
            Colour(e[0], peak, px.data(), tone);
            raw.insert(raw.end(), px.begin(), px.end());
        }
    }
    std::vector<unsigned char> zlib = { 0x78, 0x01 };
    for (size_t at = 0; at < raw.size();)
    {
        const size_t n = std::min<size_t>(65535, raw.size() - at);
        const bool last = at + n == raw.size();
        zlib.push_back(static_cast<unsigned char>(last ? 1 : 0));
        zlib.push_back(static_cast<unsigned char>(n & 0xff));
        zlib.push_back(static_cast<unsigned char>((n >> 8) & 0xff));
        zlib.push_back(static_cast<unsigned char>(~n & 0xff));
        zlib.push_back(static_cast<unsigned char>((~n >> 8) & 0xff));
        zlib.insert(zlib.end(), raw.begin() + at, raw.begin() + at + n);
        at += n;
    }
    uint32_t a = 1, b = 0;
    for (unsigned char byte : raw)
    {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    zlib.push_back(static_cast<unsigned char>((b >> 8) & 0xff));
    zlib.push_back(static_cast<unsigned char>(b & 0xff));
    zlib.push_back(static_cast<unsigned char>((a >> 8) & 0xff));
    zlib.push_back(static_cast<unsigned char>(a & 0xff));
    std::vector<unsigned char> png = { 137, 80, 78, 71, 13, 10, 26, 10 };
    unsigned char ihdr[13] = {};
    ihdr[0] = static_cast<unsigned char>((w >> 24) & 0xff);
    ihdr[1] = static_cast<unsigned char>((w >> 16) & 0xff);
    ihdr[2] = static_cast<unsigned char>((w >> 8) & 0xff);
    ihdr[3] = static_cast<unsigned char>(w & 0xff);
    ihdr[7] = 32;
    ihdr[8] = 8;
    ihdr[9] = 2;
    Chunk(png, "IHDR", ihdr, 13);
    Chunk(png, "IDAT", zlib.data(), zlib.size());
    Chunk(png, "IEND", nullptr, 0);
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f)
        return false;
    const size_t wrote = std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);
    return wrote == png.size();
}
void Strip(const std::string& dir, const std::string& name, const Engine& eng, const Config& cfg, const std::vector<Pixel>& px)
{
    const auto out = eng.run(cfg, px);
    if (!Write(dir + "\\" + name + "_" + eng.name + "_false.png", out, cfg.peak, false))
        Fail("cannot write " + name + " false-colour map");
    if (!Write(dir + "\\" + name + "_" + eng.name + "_tone.png", out, cfg.peak, true))
        Fail("cannot write " + name + " tone-mapped copy");
}
} // namespace dump
} // namespace

int main(int argc, char** argv)
{
    std::string pngDir;
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--png"))
            pngDir = argv[i + 1];
    Gpu gpu;
    if (!gpu.ok)
    {
        std::puts("FAIL: no D3D11 WARP device or the shader blobs do not load");
        return 1;
    }
    const Engine modelLegacy = ModelEngine("model-old", false), modelLaw = ModelEngine("model-law", true),
                 shaderLegacy = ShaderEngine(gpu, "shader-old", true, false);
    const Engine* newEng = &modelLaw;
    std::puts("[model fidelity]");
    ModelAgrees(modelLegacy, shaderLegacy, "old shader");
    CheckFrozenFixture();
    std::puts("[study numbers on the frozen shader of main]");
    StudyNumbers(shaderLegacy);
    NaNInfParity();
    std::puts("[properties]");
    RunAll(modelLaw);
    RunAll(shaderLegacy);
    RunAll(modelLegacy);
#ifdef HALO_LAW
    const Engine lawOff = ShaderEngine(gpu, "law-off", false, false), lawOn = ShaderEngine(gpu, "law-on", false, true);
    newEng = &lawOn;
    ModelAgrees(modelLaw, lawOn, "continuous law");
    LegacyIsByteForByte(gpu);
    RunAll(lawOff);
    RunAll(lawOn);
#endif

    if (!pngDir.empty())
    {
        CreateDirectoryA(pngDir.c_str(), nullptr);
        Config cfg;
        std::vector<double> tail;
        for (int k = 0; k <= 10; ++k)
            tail.push_back(1 - k / 255.0);
        dump::Strip(pngDir, "S4-glow-tail", shaderLegacy, cfg, Feather(0.92, 1.0, 1.0, tail));
        dump::Strip(pngDir, "S4-glow-tail", *newEng, cfg, Feather(0.92, 1.0, 1.0, tail));
        std::vector<Pixel> seam;
        for (int k = 0; k <= 25; ++k)
        {
            const double t = 1 - k / 255.0;
            seam.push_back(Gray(Sat(t * 0.8 + (1 - t) * 1.0), 0.8, 1.0, t));
        }
        dump::Strip(pngDir, "S10-seam", shaderLegacy, cfg, seam);
        dump::Strip(pngDir, "S10-seam", *newEng, cfg, seam);
        std::vector<Pixel> ramp;
        for (int k = 0; k <= 32; ++k)
            ramp.push_back(Gray(0.5 + 0.5 * (k / 32.0), 0.5, 4.0, 1.0, false));
        dump::Strip(pngDir, "S1-ramp", shaderLegacy, cfg, ramp);
        dump::Strip(pngDir, "S1-ramp", *newEng, cfg, ramp);
        std::vector<Pixel> shadow;
        for (int k = 0; k <= 12; ++k)
            shadow.push_back(Gray(0.9 * (1 - k * 0.05), 0.9, 4.0, 1 - k * 0.05, true));
        dump::Strip(pngDir, "S3-shadow", shaderLegacy, cfg, shadow);
        dump::Strip(pngDir, "S3-shadow", *newEng, cfg, shadow);
        std::vector<Pixel> fade;
        for (int k = 0; k <= 20; ++k)
            fade.push_back(Gray(0.92 * (1 - k / 20.0), 0.92, 4.0, 1.0, false));
        dump::Strip(pngDir, "S5-fade", shaderLegacy, cfg, fade);
        dump::Strip(pngDir, "S5-fade", *newEng, cfg, fade);
    }

    // expectations
    for (const Prop& p : kProps)
    {
        if (!g_results["model-law"][p.id].ok)
            Fail(std::string("the law (model) fails ") + p.id);
        if (p.ring && g_results["shader-old"][p.id].ok)
            Fail(std::string("the frozen old shader should show the halo in '") + p.id + "' but passes: the strip does not detect it");
        if (p.ring && g_results["model-old"][p.id].ok)
            Fail(std::string("the model of the old shader should show the halo in '") + p.id + "' but passes");
#ifdef HALO_LAW
        if (!g_results["law-on"][p.id].ok)
            Fail(std::string("the compiled law fails ") + p.id);
        if (g_results["law-off"][p.id].ok != g_results["shader-old"][p.id].ok)
            Fail(std::string("LegacyComposition = true differs from the old shader on ") + p.id);
#endif
    }
    if (g_failed)
    {
        std::printf("%d check(s) failed\n", g_failed);
        return 1;
    }
    std::puts("PASS: hdr_halo");
    return 0;
}
