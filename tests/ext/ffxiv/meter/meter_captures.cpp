// Effect meter (METER): the acceptance on the user's own live captures. Local class: it needs the draw-diagnostic captures of the 2026-11-07 play session on this
// machine (Args[0] = the draw-diag folder), and runs the real MeterPass (MeterPass.cpp + Meter.hlsl) on the D3D11 WARP adapter - no GPU, no game.
//
// What it does. Every stage that ran in a capture frame left its input and its output as raw images (dump\stage-NN-name-in/out.bin, cropped to the stage's render
// window, in the texture's own format; an input that is the previous stage's output is named "same_as" in facts.txt). Each pair is fed to the pass exactly as the
// game would (the whole image is the stage's box), the frame's white point is the stage line's own "white=" word (the stages that name none use the captured
// measurement), and the GPU totals are compared with the CPU diff of the very same images (MeterRules.h ReferenceTotals, every texel):
//   - the visible share (> 8/255 of the region), the mean change and the region's coverage land within 1 % (absolute share; 0.25/255 of the mean) of the CPU diff;
//   - the GPU totals equal the CPU tap emulation (same geometry, hash and seed): the region and tap counts exactly.
// The table it prints is the acceptance table of the METER report (analysis\rc6-session\fixes\meter\ACCEPTANCE.md).
#include "check.h"

#include <windows.h>

#include "ext/ffxiv/diag/StageDumpFormat.h"
#include "ext/ffxiv/meter/MeterPass.h"
#include "ext/ffxiv/meter/MeterRules.h"

#include <d3d11.h>

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
namespace mt = ext::ffxiv::meter;
using mt::StageId;

namespace
{
float FromHalf(uint16_t value)
{
    const float sign = (value & 0x8000u) != 0 ? -1.0f : 1.0f;
    const uint32_t exponent = (value >> 10) & 0x1Fu;
    const uint32_t mantissa = value & 0x3FFu;
    if (exponent == 0)
        return sign * std::ldexp(float(mantissa), -24);
    if (exponent == 31)
        return mantissa == 0 ? sign * INFINITY : NAN;
    return sign * std::ldexp(float(1024 + mantissa), int(exponent) - 25);
}

bool ReadAll(const std::string& path, std::vector<uint8_t>& bytes)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

std::vector<std::string> Lines(const std::string& path)
{
    std::vector<std::string> lines;
    std::ifstream file(path);
    for (std::string line; std::getline(file, line);)
        lines.push_back(line);
    return lines;
}

struct Item
{
    std::string name; // stage-03-ao-in
    std::string file; // stage-03-ao-in.bin, or the same_as target's
    std::string format;
    uint32_t w = 0, h = 0, pitch = 0;
};

struct Image
{
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> bytes;
    uint32_t pitch = 0;
    std::vector<float> rgba; // what Load returns

    bool Decode()
    {
        rgba.assign(size_t(w) * h * 4, 0.0f);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                float* t = &rgba[(size_t(y) * w + x) * 4];
                const uint8_t* p = bytes.data() + size_t(y) * pitch;
                if (format == DXGI_FORMAT_R16G16B16A16_FLOAT)
                {
                    const uint16_t* h16 = reinterpret_cast<const uint16_t*>(p) + size_t(x) * 4;
                    for (int c = 0; c < 4; ++c)
                        t[c] = FromHalf(h16[c]);
                }
                else if (format == DXGI_FORMAT_B8G8R8A8_UNORM)
                {
                    const uint8_t* px = p + size_t(x) * 4;
                    t[0] = float(px[2]) / 255.0f;
                    t[1] = float(px[1]) / 255.0f;
                    t[2] = float(px[0]) / 255.0f;
                    t[3] = float(px[3]) / 255.0f;
                }
                else if (format == DXGI_FORMAT_R8_UNORM)
                {
                    t[0] = float(p[x]) / 255.0f;
                    t[3] = 1.0f;
                }
                else
                    return false;
            }
        return true;
    }
};

bool FormatOf(const std::string& name, DXGI_FORMAT& format)
{
    if (name == "R16G16B16A16_FLOAT")
        format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    else if (name == "B8G8R8A8_UNORM")
        format = DXGI_FORMAT_B8G8R8A8_UNORM;
    else if (name == "R8_UNORM")
        format = DXGI_FORMAT_R8_UNORM;
    else
        return false;
    return true;
}

struct Rows
{
    double share = 0, mean = 0, coverage = 0;
};
} // namespace

int main(int argc, char** argv)
{
    const std::string root = argc > 1 ? argv[1] : "";
    if (root.empty())
    {
        std::printf("meter_captures: no draw-diag folder given\n");
        return 1;
    }
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)))
    {
        std::printf("meter_captures: no WARP device\n");
        return 1;
    }

    const char* captures[] { "20261107-164135", "20261107-164337-2", "20261107-164414-3" };
    uint32_t pairsChecked = 0;
    std::printf("%-18s %-16s %-9s %7s | %8s %8s %8s | %8s %8s %8s | %s\n", "capture", "stage", "format", "white", "gpu.shr%", "gpu.mean", "gpu.cov%", "cpu.shr%", "cpu.mean",
                "cpu.cov%", "taps(gpu/cpu)");
    for (const char* capture : captures)
    {
        const std::string dir = root + "/" + capture + "/dump";
        const std::vector<std::string> facts = Lines(dir + "/facts.txt");
        if (facts.empty())
        {
            std::printf("meter_captures: %s has no facts.txt\n", capture);
            CHECK(false);
            continue;
        }
        // the image files: "stage-03-ao-in.bin,B8G8R8A8_UNORM,1920x1080,8294400,7680,..." and "# stage-image stage-10-dof-in same_as=stage-09-gi-out"
        std::map<std::string, Item> items;
        std::map<std::string, std::string> sameAs;
        std::map<std::string, std::string> textureOf; // "# stage-image <name> ... texture=3840x2160": the texture the stage image was cropped from
        std::map<std::string, float> whiteOf; // stage name (ao, gi, ...) -> the stage line's white
        for (const std::string& line : facts)
        {
            if (line.rfind("stage-", 0) == 0 && line.find(".bin,") != std::string::npos)
            {
                std::stringstream ss(line);
                std::string file, format, size, bytes, pitch;
                std::getline(ss, file, ',');
                std::getline(ss, format, ',');
                std::getline(ss, size, ',');
                std::getline(ss, bytes, ',');
                std::getline(ss, pitch, ',');
                Item item;
                item.file = file;
                item.name = file.substr(0, file.size() - 4);
                item.format = format;
                item.w = uint32_t(std::atoi(size.c_str()));
                item.h = uint32_t(std::atoi(size.substr(size.find('x') + 1).c_str()));
                item.pitch = uint32_t(std::atoi(pitch.c_str()));
                items[item.name] = item;
            }
            else if (line.rfind("# stage-image ", 0) == 0)
            {
                std::stringstream ss(line.substr(14));
                std::string name, token;
                ss >> name;
                while (ss >> token)
                {
                    if (token.rfind("same_as=", 0) == 0)
                        sameAs[name] = token.substr(8);
                    else if (token.rfind("texture=", 0) == 0)
                        textureOf[name] = token.substr(8);
                }
            }
            else if (line.rfind("# stage ", 0) == 0)
            {
                // "# stage 09 gi: ran | ... | values rays=4 ... white=0.838 fade=1.00 ..."
                const size_t colon = line.find(':');
                const size_t nameStart = line.find(' ', 8);
                if (colon == std::string::npos || nameStart == std::string::npos)
                    continue;
                const std::string name = line.substr(nameStart + 1, colon - nameStart - 1);
                const size_t w = line.find("white=");
                if (w != std::string::npos)
                    whiteOf[name] = float(std::atof(line.c_str() + w + 6));
            }
        }
        // every stage that has an "out" image gets measured against its "in" (the file, or the "same_as" file)
        for (auto& [name, outItem] : items)
        {
            const size_t dash = name.rfind('-');
            if (dash == std::string::npos || name.substr(dash + 1) != "out")
                continue;
            const std::string base = name.substr(0, dash);                // stage-03-ao
            const std::string stageName = base.substr(base.find('-', 6) + 1); // ao  (after "stage-NN-")
            std::string inName = base + "-in";
            if (sameAs.count(inName) != 0)
                inName = sameAs[inName];
            if (items.count(inName) == 0)
            {
                std::printf("%-18s %-16s (no input image: skipped)\n", capture, stageName.c_str());
                continue;
            }
            const Item& inItem = items[inName];
            // The stage's input and output must come from textures of ONE size: the live pass refuses a pair that differs ("never measured"), so a capture that shows a pair of
            // two sizes would mean the meter is blind to that stage in the game.
            CHECK(textureOf.count(inName) == 1 && textureOf.count(name) == 1 && textureOf[inName] == textureOf[name]);
            // the stage id from the registry's file stem
            StageId id = StageId::Count;
            for (uint32_t i = 0; i < mt::kEffectCount; ++i)
                if (stageName == ext::ffxiv::diag::kStageTable[i].name)
                    id = StageId(i);
            if (id == StageId::Count || !mt::InfoOf(id).metered)
                continue;

            Image a, b;
            DXGI_FORMAT fa, fb;
            if (!FormatOf(inItem.format, fa) || !FormatOf(outItem.format, fb) || fa != fb)
            {
                std::printf("%-18s %-16s (format %s / %s: skipped)\n", capture, stageName.c_str(), inItem.format.c_str(), outItem.format.c_str());
                continue;
            }
            a.format = b.format = fa;
            a.w = inItem.w;
            a.h = inItem.h;
            a.pitch = inItem.pitch;
            b.w = outItem.w;
            b.h = outItem.h;
            b.pitch = outItem.pitch;
            if (!ReadAll(dir + "/" + inItem.file, a.bytes) || !ReadAll(dir + "/" + outItem.file, b.bytes) || a.w != b.w || a.h != b.h || !a.Decode() || !b.Decode())
            {
                std::printf("%-18s %-16s (could not read the images)\n", capture, stageName.c_str());
                CHECK(false);
                continue;
            }

            const auto upload = [&](const Image& image) {
                D3D11_TEXTURE2D_DESC desc {};
                desc.Width = image.w;
                desc.Height = image.h;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = image.format;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA initial { image.bytes.data(), image.pitch, 0 };
                ComPtr<ID3D11Texture2D> texture;
                ComPtr<ID3D11ShaderResourceView> view;
                CHECK(SUCCEEDED(device->CreateTexture2D(&desc, &initial, &texture)));
                if (texture)
                    CHECK(SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &view)));
                return view;
            };
            auto inView = upload(a);
            auto outView = upload(b);
            const float white = whiteOf.count(stageName) != 0 && whiteOf[stageName] > 0.0f ? whiteOf[stageName] : mt::kFallbackWhite;

            mt::Pass pass;
            mt::Request request;
            request.id = id;
            request.in = inView.Get();
            request.out = outView.Get();
            request.white = white;
            request.seed = 7;
            mt::Result result;
            bool got = pass.Start(context.Get(), request);
            for (int i = 0; got && i < 4000; ++i)
            {
                if (pass.Collect(context.Get(), result))
                    break;
                if (i == 3999)
                    got = false;
                context->Flush();
                Sleep(1);
            }
            CHECK(got);
            if (!got)
            {
                std::printf("%-18s %-16s (the pass did not run: %s)\n", capture, stageName.c_str(), pass.Failure() != nullptr ? pass.Failure() : "no result");
                continue;
            }
            const mt::Params params = mt::ParamsOf(id, white);
            const auto fetch = [&](uint32_t x, uint32_t y, float* in, float* out) {
                std::memcpy(in, &a.rgba[(size_t(y) * a.w + x) * 4], 16);
                std::memcpy(out, &b.rgba[(size_t(y) * b.w + x) * 4], 16);
            };
            const mt::Totals full = mt::ReferenceTotals(a.w, a.h, params, fetch);
            const mt::Totals taps = mt::TapTotals(a.w, a.h, result.geometry, 7, params, fetch);
            std::printf("%-18s %-16s %-9s %7.3f | %8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f | %.0f/%.0f\n", capture, stageName.c_str(), inItem.format.c_str(), double(params.white),
                        result.totals.Share() * 100.0, result.totals.Mean(), result.totals.Coverage() * 100.0, full.Share() * 100.0, full.Mean(), full.Coverage() * 100.0,
                        result.totals.taps, taps.taps);
            // within 1 % (absolute share) of the CPU diff of the very same images; the mean within a quarter level plus 1 % of itself; the coverage within 1 %
            CHECK(std::fabs(result.totals.Share() - full.Share()) <= 0.01);
            CHECK(std::fabs(result.totals.Mean() - full.Mean()) <= 0.25 + 0.01 * full.Mean()); // a quarter level plus 1 % of the mean (the sparse regions of the reflections carry big values)
            CHECK(std::fabs(result.totals.Coverage() - full.Coverage()) <= 0.01);
            // and the GPU equals the CPU tap emulation: the tap and region counts exactly
            CHECK(result.totals.taps == taps.taps);
            CHECK(result.totals.region == taps.region);
            ++pairsChecked;
        }
    }
    std::printf("meter_captures: %u stage pairs checked\n", pairsChecked);
    CHECK(pairsChecked >= 10); // the three captures hold at least ten metered stage pairs between them
    return Finish("meter_captures");
}
