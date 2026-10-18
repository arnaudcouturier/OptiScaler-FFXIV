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
#include <map>
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
    // nr: R16G16B16A16_FLOAT, 3x2, the preview NR result: 8 bytes per pixel, half values (0.5 -> 128, 0.875 -> 223, 0.125 -> 32, 0.25 -> 64,
    // 1.0 and 2.0 -> 255, -1.0 and NaN -> 0). The row copy of Pump must use 8 bytes per pixel (a 4-byte row would cut every row in half).
    constexpr UINT RW = 3, RH = 2;
    const uint16_t halves[RW * RH][4] = {
        { 0x3800, 0x3B00, 0x3000, 0x3C00 }, // 0.5   0.875 0.125 1.0
        { 0x4000, 0xBC00, 0x7E00, 0x3400 }, // 2.0  -1.0   NaN   0.25
        { 0x0000, 0x3C00, 0x3800, 0x3800 }, // 0     1.0   0.5   0.5
        { 0x3400, 0x3400, 0x3400, 0x3400 }, // 0.25 x4
        { 0x3C00, 0x0000, 0x3C00, 0x0000 }, // 1.0   0     1.0   0
        { 0x3B00, 0x3800, 0x3000, 0x3B00 }, // 0.875 0.5   0.125 0.875
    };
    std::vector<uint8_t> nrData(sizeof(halves));
    std::memcpy(nrData.data(), halves, sizeof(halves));
    auto nrImage = Make(DXGI_FORMAT_R16G16B16A16_FLOAT, RW, RH, nrData, 8);
    const std::vector<uint8_t> nrRgb = { 128, 223, 32, 255, 0, 0, 0, 255, 128, 64, 64, 64, 255, 0, 255, 223, 128, 32 };
    const std::vector<uint8_t> nrAlpha = { 255, 64, 128, 64, 0, 223 };
    // an unsupported format
    const auto floatData = Pattern(2 * 2 * 8, 3);
    auto floating = Make(DXGI_FORMAT_R16G16_FLOAT, 2, 2, floatData, 8);
    CHECK(native && aa && nrImage && floating);

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
    sources.push_back({ "nr", nrImage.Get(), "" });
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
    CHECK(outcome->files == 7); // native pair, aa pair, nr pair, state.txt
    CHECK(!dump::InFlight());
    CHECK(!dump::Pump(g_context.Get(), 1002).has_value());

    const fs::path folder = fs::path(outcome->folder);
    CHECK(fs::is_directory(folder) && folder.parent_path() == root);
    CHECK(CountEntries(folder) == 7);
    CHECK(ReadAll(folder / "native.png") == Png(NW, NH, 3, nativeRgb));
    CHECK(ReadAll(folder / "alpha-native.png") == Png(NW, NH, 1, nativeAlpha));
    CHECK(ReadAll(folder / "aa.png") == Png(4, 4, 3, aaRgb));
    CHECK(ReadAll(folder / "alpha-aa.png") == Png(4, 4, 1, aaAlpha));
    CHECK(ReadAll(folder / "nr.png") == Png(RW, RH, 3, nrRgb));
    CHECK(ReadAll(folder / "alpha-nr.png") == Png(RW, RH, 1, nrAlpha));
    const std::string state = ReadText(folder / "state.txt");
    CHECK(state.rfind("probe.on: yes\n", 0) == 0);
    CHECK(state.find("image.native: 5x3 B8G8R8A8_UNORM(87), saved as native.png and alpha-native.png\n") != std::string::npos);
    CHECK(state.find("image.aa: 4x4 R8G8B8A8_UNORM_SRGB(29), saved as aa.png and alpha-aa.png\n") != std::string::npos);
    CHECK(state.find("image.dlss: not saved, the DLSS path has no evaluated preview image\n") != std::string::npos);
    CHECK(state.find("image.extra: not saved, format R16G16_FLOAT(34) is not supported") != std::string::npos);

    dump::Status status = dump::GetStatus();
    CHECK(status.dumps == 1 && status.files == 7 && status.refusals == 0 && !status.inFlight);
    CHECK(status.lastFolder == outcome->folder && status.lastMessage == outcome->message);

    // ---- a predicate set by the game: the dump says so instead of copying under it (REVIEW-CHARA-2 L1) ---------------------------------
    {
        CHECK(!dump::PredicationActive(g_context.Get()) && !dump::PredicationActive(nullptr));
        D3D11_QUERY_DESC qd {};
        qd.Query = D3D11_QUERY_OCCLUSION_PREDICATE;
        ComPtr<ID3D11Predicate> predicate;
        if (Ok(g_device->CreatePredicate(&qd, &predicate), "predicate"))
        {
            g_context->SetPredication(predicate.Get(), FALSE);
            CHECK(dump::PredicationActive(g_context.Get()));
            g_context->SetPredication(nullptr, FALSE);
            CHECK(!dump::PredicationActive(g_context.Get()));
        }
    }

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
        {
            // the case must exist: a WARP without 4x multisampling would silently skip it, which is not a pass
            std::printf("FAIL: the multisampled texture could not be created\n");
            ++g_failures;
        }
    }

    // ---- Release drops a dump in flight; a dump the GPU never finishes is dropped after 10 s ---------------------------------------
    CHECK(dump::Begin(g_context.Get(), sources, "", 5000, why));
    dump::Release();
    CHECK(!dump::InFlight() && !dump::Pump(g_context.Get(), 5001).has_value());
    // The give-up needs a GPU that never finishes; WARP finishes at once, so the query result is replaced by the test hook.
    CHECK(dump::Begin(g_context.Get(), sources, "", 6000, why));
    {
        dump::TestHooks hooks;
        hooks.queryDone = [](ID3D11DeviceContext*, ID3D11Query*) -> HRESULT { return S_FALSE; };
        dump::SetHooksForTest(hooks);
        CHECK(!dump::Pump(g_context.Get(), 6000 + 5000).has_value());       // not finished, not late: waiting
        CHECK(!dump::Pump(g_context.Get(), 6000 + 10000).has_value());      // exactly 10 s is not "more than 10 s"
        auto late = dump::Pump(g_context.Get(), 6000 + 10001);
        CHECK(late && late->result == DumpOutcome::Result::Failed && late->message.find("10 seconds") != std::string::npos);
        CHECK(!dump::InFlight());
        dump::SetHooksForTest({});
    }

    // ---- the GPU side of the pump: a late pump of a finished dump, a map that is still busy, a row pitch that is not tight (REVIEW-CHARA-2 L4) ----
    {
        // L4: the copies finished on the GPU but nobody pumped for a long time (a loading screen, a minimised window): the dump is
        // written, not reported as "the GPU did not finish".
        CHECK(dump::Begin(g_context.Get(), sources, "late: yes\n", 20000, why));
        // (the hook waits until the real GPU work is done, which models a GPU that finished long ago)
        dump::TestHooks waitForGpu;
        waitForGpu.queryDone = [](ID3D11DeviceContext* ctx, ID3D11Query* q) -> HRESULT
        {
            for (int i = 0; i < 400; ++i)
            {
                if (ctx->GetData(q, nullptr, 0, 0) == S_OK)
                    return S_OK;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return S_FALSE;
        };
        dump::SetHooksForTest(waitForGpu);
        const auto late = PumpToEnd(20000 + 60000 - 1); // PumpToEnd pumps at startMs + 1: a minute after the dump began
        dump::SetHooksForTest({});
        CHECK(late && late->result == DumpOutcome::Result::Written && late->files == 7);
        if (late)
            CHECK(ReadAll(fs::path(late->folder) / "native.png") == Png(NW, NH, 3, nativeRgb));

        // a busy map: the first pumps find the second image busy; everything mapped so far is unmapped again (a map is never left open),
        // nothing is written, and the pump that finds everything free writes correct files.
        struct Counters
        {
            int callInPump = 0, busyPumps = 0, mapped = 0, unmapped = 0, busyAnswers = 0;
        };
        static Counters c;
        c = Counters {};
        c.busyPumps = 2;
        dump::TestHooks hooks;
        hooks.map = [](ID3D11DeviceContext* ctx, ID3D11Resource* res, D3D11_MAPPED_SUBRESOURCE* m) -> HRESULT
        {
            if (++c.callInPump == 2 && c.busyPumps > 0)
            {
                --c.busyPumps;
                ++c.busyAnswers;
                return DXGI_ERROR_WAS_STILL_DRAWING;
            }
            const HRESULT hr = ctx->Map(res, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, m);
            if (SUCCEEDED(hr))
                ++c.mapped;
            return hr;
        };
        hooks.queryDone = [](ID3D11DeviceContext* ctx, ID3D11Query* q) -> HRESULT // waits until the real GPU work is done: the maps then succeed at once
        {
            for (int i = 0; i < 400; ++i)
            {
                if (ctx->GetData(q, nullptr, 0, 0) == S_OK)
                    return S_OK;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return S_FALSE;
        };
        hooks.unmap = [](ID3D11DeviceContext* ctx, ID3D11Resource* res)
        {
            ++c.unmapped;
            ctx->Unmap(res, 0);
        };
        dump::SetHooksForTest(hooks);
        std::vector<dump::Source> two;
        two.push_back({ "native", native.Get(), "" });
        two.push_back({ "aa", aa.Get(), "" });
        CHECK(dump::Begin(g_context.Get(), two, "busy: test\n", 30000, why));
        for (int pump = 0; pump < 2; ++pump)
        {
            c.callInPump = 0;
            CHECK(!dump::Pump(g_context.Get(), 30001 + pump).has_value()); // busy: no outcome, retried later
            CHECK(dump::InFlight());
            CHECK(c.unmapped == c.mapped);                                   // the image mapped before the busy one was unmapped again
        }
        CHECK(c.busyAnswers == 2 && c.mapped == 2);
        c.callInPump = 0;
        const auto done = dump::Pump(g_context.Get(), 30003);
        CHECK(done && done->result == DumpOutcome::Result::Written && done->files == 5);
        CHECK(c.unmapped == c.mapped && c.mapped == 4); // two images mapped and unmapped by the last pump
        if (done)
        {
            CHECK(ReadAll(fs::path(done->folder) / "native.png") == Png(NW, NH, 3, nativeRgb));
            CHECK(ReadAll(fs::path(done->folder) / "aa.png") == Png(4, 4, 3, aaRgb));
        }

        // a busy map that never frees up: late, the dump is given up
        c = Counters {};
        c.busyPumps = 1000;
        CHECK(dump::Begin(g_context.Get(), two, "", 40000, why));
        c.callInPump = 0;
        CHECK(!dump::Pump(g_context.Get(), 40100).has_value());
        c.callInPump = 0;
        const auto gaveUp = dump::Pump(g_context.Get(), 40000 + 10001);
        CHECK(gaveUp && gaveUp->result == DumpOutcome::Result::Failed && !dump::InFlight() && c.unmapped == c.mapped);

        // a row pitch wider than the row (what a GPU gives for most sizes; WARP's is tight): the rows are copied with the mapped pitch, the
        // padding is not part of the picture. The hook hands out a buffer with 16 junk bytes after every row.
        static std::map<ID3D11Resource*, std::vector<uint8_t>> padded; // one buffer per mapped image: the rows are copied out after all maps
        hooks.map = [](ID3D11DeviceContext* ctx, ID3D11Resource* res, D3D11_MAPPED_SUBRESOURCE* m) -> HRESULT
        {
            const HRESULT hr = ctx->Map(res, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, m);
            if (FAILED(hr))
                return hr;
            ComPtr<ID3D11Texture2D> texture;
            res->QueryInterface(IID_PPV_ARGS(&texture));
            D3D11_TEXTURE2D_DESC d {};
            texture->GetDesc(&d);
            const UINT bytesPerPixel = d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
            const UINT row = d.Width * bytesPerPixel;
            const UINT pitch = row + 16;
            std::vector<uint8_t>& buffer = padded[res];
            buffer.assign(static_cast<size_t>(pitch) * d.Height, 0xEE);
            for (UINT y = 0; y < d.Height; ++y)
                std::memcpy(buffer.data() + static_cast<size_t>(y) * pitch, static_cast<const uint8_t*>(m->pData) + static_cast<size_t>(y) * m->RowPitch, row);
            m->pData = buffer.data();
            m->RowPitch = pitch;
            return hr;
        };
        hooks.unmap = nullptr;
        dump::SetHooksForTest(hooks);
        std::vector<dump::Source> mixed;
        mixed.push_back({ "native", native.Get(), "" });
        mixed.push_back({ "nr", nrImage.Get(), "" });
        CHECK(dump::Begin(g_context.Get(), mixed, "pitch: test\n", 50000, why));
        const auto wide = PumpToEnd(50000);
        CHECK(wide && wide->result == DumpOutcome::Result::Written && wide->files == 5);
        if (wide)
        {
            CHECK(ReadAll(fs::path(wide->folder) / "native.png") == Png(NW, NH, 3, nativeRgb));
            CHECK(ReadAll(fs::path(wide->folder) / "alpha-native.png") == Png(NW, NH, 1, nativeAlpha));
            CHECK(ReadAll(fs::path(wide->folder) / "nr.png") == Png(RW, RH, 3, nrRgb));
            CHECK(ReadAll(fs::path(wide->folder) / "alpha-nr.png") == Png(RW, RH, 1, nrAlpha));
        }
        dump::SetHooksForTest({});
    }

    // ---- state only -------------------------------------------------------------------------------------------------------------
    {
        const DumpOutcome st = dump::WriteStateOnly("key: value\n");
        CHECK(st.result == DumpOutcome::Result::Written && st.files == 1);
        CHECK(ReadText(fs::path(st.folder) / "state.txt") == "key: value\n");
    }

    // ---- the session budget: dumps of 7 files each until 40 files are used, then refused with nothing written -----------------------
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
    CHECK(written == 5); // 5 x 7 = 35 files; the sixth would be 42
    CHECK(last.result == DumpOutcome::Result::Refused && last.files == 0 && last.folder.empty());
    CHECK(last.message.find("refused") == 0);
    CHECK(CountEntries(root) == 5); // no folder for the refused dump
    status = dump::GetStatus();
    CHECK(status.dumps == 5 && status.refusals == 1 && status.files == 35);
    CHECK(status.lastMessage == last.message);
    CHECK(!dump::InFlight());

    dump::Release();
    fs::remove_all(root, ec);
    return TEST_RESULT("chara_probe_dump_warp");
}
