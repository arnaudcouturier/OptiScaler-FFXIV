// The preview image dump on D3D11 WARP: the real CharaProbeDump.cpp stages copies of known textures, waits for an event query,
// maps without waiting, converts to PNG and writes the folder. One dump in flight at a time; the session budget refuses the dump that
// would go over; absent, unsupported and multisampled images get a line in state.txt and never a crash.
#include "check.h"
#include "pch.h"

#include "ext/ffxiv/chara/CharaProbeDump.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::chara::probe;
namespace fs = std::filesystem;

namespace
{
ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;

bool Ok(HRESULT hr, const char* what)
{
    if (SUCCEEDED(hr))
        return true;
    std::printf("FAIL: %s (0x%08lX)\n", what, static_cast<unsigned long>(hr));
    ++g_failures;
    return false;
}

std::vector<uint8_t> Pattern(size_t bytes, uint8_t seed)
{
    std::vector<uint8_t> v(bytes);
    for (size_t i = 0; i < bytes; ++i)
        v[i] = static_cast<uint8_t>(seed + i * 7);
    return v;
}

ComPtr<ID3D11Texture2D> Make(DXGI_FORMAT format, UINT w, UINT h, const std::vector<uint8_t>& mip0, UINT bpp, UINT mips = 1,
                             const std::vector<uint8_t>* mip1 = nullptr)
{
    D3D11_TEXTURE2D_DESC td {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = mips;
    td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init[2] {};
    init[0] = { mip0.data(), w * bpp, 0 };
    if (mips > 1 && mip1)
        init[1] = { mip1->data(), (w / 2) * bpp, 0 };
    ComPtr<ID3D11Texture2D> t;
    Ok(g_device->CreateTexture2D(&td, init, &t), "texture");
    return t;
}

std::vector<uint8_t> ReadAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string ReadText(const fs::path& p)
{
    const auto b = ReadAll(p);
    return std::string(b.begin(), b.end());
}

size_t CountEntries(const fs::path& dir)
{
    size_t n = 0;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        ++n;
    return n;
}

// Pumps (as the probe does about every 100 ms) until the dump has an outcome or two seconds pass.
std::optional<DumpOutcome> PumpToEnd(uint64_t startMs)
{
    for (int i = 0; i < 400; ++i)
    {
        if (auto outcome = dump::Pump(g_context.Get(), startMs + 1))
            return outcome;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return std::nullopt;
}

std::vector<uint8_t> Png(uint32_t w, uint32_t h, uint32_t channels, const std::vector<uint8_t>& pixels)
{
    std::vector<uint8_t> out;
    EncodePng(w, h, channels, pixels.data(), out);
    return out;
}
} // namespace

int main()
{
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    if (!Ok(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &g_device, nullptr, &g_context),
            "D3D11CreateDevice(WARP)"))
        return 1;

    const fs::path root = fs::temp_directory_path() / "chara_probe_dump_warp_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    dump::SetOutputRootForTest(root);
    dump::ResetSessionForTest();

    // native: B8G8R8A8, 5x3, with a second mip level (the dump must take mip 0 of a texture that has mips)
    constexpr UINT NW = 5, NH = 3;
    const auto nativeData = Pattern(NW * NH * 4, 1);
    const auto nativeMip1 = Pattern(2 * 1 * 4, 99);
    auto native = Make(DXGI_FORMAT_B8G8R8A8_UNORM, NW, NH, nativeData, 4, 2, &nativeMip1);
    // aa: R8G8B8A8_UNORM_SRGB, 4x4
    const auto aaData = Pattern(4 * 4 * 4, 5);
    auto aa = Make(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 4, 4, aaData, 4);
    // an unsupported format
    const auto floatData = Pattern(2 * 2 * 8, 3);
    auto floating = Make(DXGI_FORMAT_R16G16B16A16_FLOAT, 2, 2, floatData, 8);
    CHECK(native && aa && floating);

    // expected pictures
    std::vector<uint8_t> nativeRgb, nativeAlpha, aaRgb, aaAlpha;
    for (size_t i = 0; i < NW * NH; ++i)
    {
        const uint8_t* p = nativeData.data() + i * 4; // B G R A
        nativeRgb.insert(nativeRgb.end(), { p[2], p[1], p[0] });
        nativeAlpha.push_back(p[3]);
    }
    for (size_t i = 0; i < 16; ++i)
    {
        const uint8_t* p = aaData.data() + i * 4; // R G B A
        aaRgb.insert(aaRgb.end(), { p[0], p[1], p[2] });
        aaAlpha.push_back(p[3]);
    }

    // ---- one dump -----------------------------------------------------------------------------------------------------
    std::vector<dump::Source> sources;
    sources.push_back({ "native", native.Get(), "" });
    sources.push_back({ "aa", aa.Get(), "" });
    sources.push_back({ "dlss", nullptr, "the DLSS path has no evaluated preview image" });
    sources.push_back({ "extra", floating.Get(), "" });

    std::string why;
    CHECK(!dump::InFlight());
    CHECK(!dump::Begin(nullptr, sources, "x: y\n", 1000, why));
    CHECK(dump::Begin(g_context.Get(), sources, "probe.on: yes\n", 1000, why));
    CHECK(dump::InFlight());
    CHECK(!dump::Begin(g_context.Get(), sources, "second: dump\n", 1001, why) && why == "a dump is already in progress");
    CHECK(!fs::exists(root)); // nothing is written before the GPU is done and a pump saw it

    auto outcome = PumpToEnd(1000);
    CHECK(outcome.has_value());
    if (!outcome)
        return 1;
    CHECK(outcome->result == DumpOutcome::Result::Written);
    CHECK(outcome->files == 5); // native pair, aa pair, state.txt
    CHECK(!dump::InFlight());
    CHECK(!dump::Pump(g_context.Get(), 1002).has_value());

    const fs::path folder = fs::path(outcome->folder);
    CHECK(fs::is_directory(folder) && folder.parent_path() == root);
    CHECK(CountEntries(folder) == 5);
    CHECK(ReadAll(folder / "native.png") == Png(NW, NH, 3, nativeRgb));
    CHECK(ReadAll(folder / "alpha-native.png") == Png(NW, NH, 1, nativeAlpha));
    CHECK(ReadAll(folder / "aa.png") == Png(4, 4, 3, aaRgb));
    CHECK(ReadAll(folder / "alpha-aa.png") == Png(4, 4, 1, aaAlpha));
    const std::string state = ReadText(folder / "state.txt");
    CHECK(state.rfind("probe.on: yes\n", 0) == 0);
    CHECK(state.find("image.native: 5x3 B8G8R8A8_UNORM(87), saved as native.png and alpha-native.png\n") != std::string::npos);
    CHECK(state.find("image.aa: 4x4 R8G8B8A8_UNORM_SRGB(29), saved as aa.png and alpha-aa.png\n") != std::string::npos);
    CHECK(state.find("image.dlss: not saved, the DLSS path has no evaluated preview image\n") != std::string::npos);
    CHECK(state.find("image.extra: not saved, format R16G16B16A16_FLOAT(10) is not supported") != std::string::npos);

    dump::Status status = dump::GetStatus();
    CHECK(status.dumps == 1 && status.files == 5 && status.refusals == 0 && !status.inFlight);
    CHECK(status.lastFolder == outcome->folder && status.lastMessage == outcome->message);

    // ---- the pixel content is the content at copy time (not at pump time) -------------------------------------------------------
    // (a staging copy is a snapshot: the source can be overwritten right after Begin)
    {
        D3D11_TEXTURE2D_DESC td {};
        td.Width = td.Height = 2;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const std::vector<uint8_t> before = Pattern(16, 20);
        const std::vector<uint8_t> after = Pattern(16, 200);
        D3D11_SUBRESOURCE_DATA init { before.data(), 8, 0 };
        ComPtr<ID3D11Texture2D> t;
        Ok(g_device->CreateTexture2D(&td, &init, &t), "snapshot texture");
        std::vector<dump::Source> one { { "snap", t.Get(), "" } };
        CHECK(dump::Begin(g_context.Get(), one, "", 3000, why));
        g_context->UpdateSubresource(t.Get(), 0, nullptr, after.data(), 8, 0);
        auto o = PumpToEnd(3000);
        CHECK(o && o->result == DumpOutcome::Result::Written);
        if (o)
        {
            std::vector<uint8_t> rgb, alpha;
            for (size_t i = 0; i < 4; ++i)
            {
                rgb.insert(rgb.end(), { before[i * 4], before[i * 4 + 1], before[i * 4 + 2] });
                alpha.push_back(before[i * 4 + 3]);
            }
            CHECK(ReadAll(fs::path(o->folder) / "snap.png") == Png(2, 2, 3, rgb));
            CHECK(ReadAll(fs::path(o->folder) / "alpha-snap.png") == Png(2, 2, 1, alpha));
        }
    }

    // ---- a multisampled texture gets a line, not a crash ---------------------------------------------------------------------
    {
        D3D11_TEXTURE2D_DESC td {};
        td.Width = td.Height = 8;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 4;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> msaa;
        UINT quality = 0;
        if (SUCCEEDED(g_device->CheckMultisampleQualityLevels(td.Format, 4, &quality)) && quality > 0 &&
            SUCCEEDED(g_device->CreateTexture2D(&td, nullptr, &msaa)))
        {
            std::vector<dump::Source> one { { "msaa", msaa.Get(), "" } };
            CHECK(dump::Begin(g_context.Get(), one, "", 4000, why));
            auto o = PumpToEnd(4000);
            CHECK(o && o->result == DumpOutcome::Result::Written && o->files == 1);
            if (o)
                CHECK(ReadText(fs::path(o->folder) / "state.txt").find("image.msaa: not saved, the texture is multisampled (4 samples)") !=
                      std::string::npos);
        }
        else
            std::printf("note: WARP offers no 4x multisampling for B8G8R8A8; multisampled case skipped\n");
    }

    // ---- Release drops a dump in flight; a dump the GPU never finishes is dropped after 10 s ---------------------------------------
    CHECK(dump::Begin(g_context.Get(), sources, "", 5000, why));
    dump::Release();
    CHECK(!dump::InFlight() && !dump::Pump(g_context.Get(), 5001).has_value());
    CHECK(dump::Begin(g_context.Get(), sources, "", 6000, why));
    {
        auto late = dump::Pump(g_context.Get(), 6000 + 10001); // the time check comes before the query check
        CHECK(late && late->result == DumpOutcome::Result::Failed && late->message.find("10 seconds") != std::string::npos);
        CHECK(!dump::InFlight());
    }

    // ---- state only -------------------------------------------------------------------------------------------------------------
    {
        const DumpOutcome st = dump::WriteStateOnly("key: value\n");
        CHECK(st.result == DumpOutcome::Result::Written && st.files == 1);
        CHECK(ReadText(fs::path(st.folder) / "state.txt") == "key: value\n");
    }

    // ---- the session budget: dumps of 5 files each until 40 files are used, then refused with nothing written -----------------------
    dump::ResetSessionForTest();
    fs::remove_all(root, ec);
    int written = 0;
    DumpOutcome last;
    for (int i = 0; i < 12; ++i)
    {
        CHECK(dump::Begin(g_context.Get(), sources, "n: " + std::to_string(i) + "\n", 7000 + i, why));
        auto o = PumpToEnd(7000 + i);
        CHECK(o.has_value());
        if (!o)
            break;
        last = *o;
        if (o->result == DumpOutcome::Result::Written)
            ++written;
        else
            break;
    }
    CHECK(written == 8); // 8 x 5 = 40 files
    CHECK(last.result == DumpOutcome::Result::Refused && last.files == 0 && last.folder.empty());
    CHECK(last.message.find("refused") == 0);
    CHECK(CountEntries(root) == 8); // no folder for the refused dump
    status = dump::GetStatus();
    CHECK(status.dumps == 8 && status.refusals == 1 && status.files == 40);
    CHECK(status.lastMessage == last.message);
    CHECK(!dump::InFlight());

    dump::Release();
    fs::remove_all(root, ec);
    return TEST_RESULT("chara_probe_dump_warp");
}
