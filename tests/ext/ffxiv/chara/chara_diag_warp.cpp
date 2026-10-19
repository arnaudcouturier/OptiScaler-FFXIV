// The preview DLSS diagnostics on D3D11 WARP: the real CharaDiag.cpp stages six images and a camera buffer, reads them back
// without waiting and writes the files. Off by default, capped at 12 files, two dumps only.
#include "check.h"
#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/chara/CharaDiag.h"
#include "ext/ffxiv/chara/CharaDiagRules.h"

#include <DirectXPackedVector.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::chara;
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

ComPtr<ID3D11Texture2D> Make(DXGI_FORMAT format, UINT w, UINT h, const std::vector<uint8_t>& data, UINT bpp)
{
    D3D11_TEXTURE2D_DESC td {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init { data.data(), w * bpp, 0 };
    ComPtr<ID3D11Texture2D> t;
    Ok(g_device->CreateTexture2D(&td, &init, &t), "texture");
    return t;
}

std::vector<uint8_t> Pattern(size_t bytes, uint8_t seed)
{
    std::vector<uint8_t> v(bytes);
    for (size_t i = 0; i < bytes; ++i)
        v[i] = static_cast<uint8_t>(seed + i * 7);
    return v;
}

std::vector<uint8_t> ReadFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Pumps until the job is written or a second passes.
void PumpUntil(const fs::path& file)
{
    for (int i = 0; i < 200 && !fs::exists(file); ++i)
    {
        diag::Pump(g_context.Get(), 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
} // namespace

int main()
{
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    if (!Ok(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &g_device, nullptr, &g_context),
            "D3D11CreateDevice(WARP)"))
        return 1;

    const fs::path dir = fs::temp_directory_path() / "chara_diag_warp_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    diag::SetDirectoryForTest(dir);

    constexpr UINT W = 4, H = 4;
    const auto nativeData = Pattern(W * H * 4, 1);
    const auto inputData = Pattern(W * H * 4, 2);
    const auto dlssData = Pattern(W * 2 * H * 2 * 8, 3);
    const auto composedData = Pattern(W * 2 * H * 2 * 4, 4);
    const auto depthData = Pattern(W * H * 4, 5);
    std::vector<uint8_t> motionData(W * H * 4, 0);
    for (UINT p = 0; p < W * H; ++p)
    {
        const uint16_t x = DirectX::PackedVector::XMConvertFloatToHalf(p == 1 ? 1.5f : 0.0f);
        const uint16_t y = DirectX::PackedVector::XMConvertFloatToHalf(p == 1 ? -0.5f : 0.0f);
        std::memcpy(&motionData[p * 4], &x, 2);
        std::memcpy(&motionData[p * 4 + 2], &y, 2);
    }

    auto native = Make(DXGI_FORMAT_B8G8R8A8_UNORM, W, H, nativeData, 4);
    auto input = Make(DXGI_FORMAT_R8G8B8A8_UNORM, W, H, inputData, 4);
    auto dlss = Make(DXGI_FORMAT_R16G16B16A16_FLOAT, W * 2, H * 2, dlssData, 8);
    auto composed = Make(DXGI_FORMAT_R8G8B8A8_UNORM, W * 2, H * 2, composedData, 4);
    auto depth = Make(DXGI_FORMAT_R32_FLOAT, W, H, depthData, 4);
    auto motion = Make(DXGI_FORMAT_R16G16_FLOAT, W, H, motionData, 4);

    float camera[59 * 4] {};
    camera[58 * 4 + 2] = 0.5f;
    camera[58 * 4 + 3] = -0.25f;
    D3D11_BUFFER_DESC bd {};
    bd.ByteWidth = sizeof(camera);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init { camera, 0, 0 };
    ComPtr<ID3D11Buffer> cameraBuffer;
    Ok(g_device->CreateBuffer(&bd, &init, &cameraBuffer), "camera buffer");

    diag::Request request;
    request.native = native.Get();
    request.input = input.Get();
    request.dlss = dlss.Get();
    request.composed = composed.Get();
    request.depth = depth.Get();
    request.motion = motion.Get();
    request.camera = cameraBuffer.Get();
    request.width = W;
    request.height = H;
    request.jitterX = 0.25f;
    request.jitterY = -0.125f;

    // Off by default: nothing is staged, nothing written.
    CHECK(!diag::CaptureEnabled());
    CHECK(!diag::Capture(g_context.Get(), request, 1, 1000));
    CHECK(!fs::exists(dir));

    ext::Cfg().ffxiv.chara.previewGuideCapture = true;
    CHECK(diag::CaptureEnabled());

    // A mismatching image (wrong size for its role) is refused.
    {
        diag::Request bad = request;
        bad.dlss = native.Get();
        CHECK(!diag::Capture(g_context.Get(), bad, 1, 1000));
    }

    // First dump.
    CHECK(diag::Capture(g_context.Get(), request, 1, 1000));
    CHECK(!diag::Capture(g_context.Get(), request, 2, 1001)); // one dump in flight at a time
    PumpUntil(dir / "1-motion.bin");

    const std::array<const char*, 6> names { "native", "input", "dlss", "composed", "depth", "motion" };
    const std::array<const std::vector<uint8_t>*, 6> data { &nativeData, &inputData, &dlssData, &composedData, &depthData, &motionData };
    const std::array<UINT, 6> widths { W, W, W * 2, W * 2, W, W };
    const std::array<UINT, 6> heights { H, H, H * 2, H * 2, H, H };
    const std::array<UINT, 6> formats { DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT };
    for (size_t i = 0; i < 6; ++i)
    {
        const auto bytes = ReadFile(dir / (std::string("1-") + names[i] + ".bin"));
        CHECK(bytes.size() == 12 + data[i]->size());
        if (bytes.size() != 12 + data[i]->size())
            continue;
        uint32_t header[3];
        std::memcpy(header, bytes.data(), 12);
        CHECK(header[0] == widths[i] && header[1] == heights[i] && header[2] == formats[i]);
        CHECK(std::memcmp(bytes.data() + 12, data[i]->data(), data[i]->size()) == 0); // the same-frame content, row packed
    }
    CHECK(diag::FilesWritten() == 6);
    CHECK(g_testLastLog.find("Camera[58].zw (0.5, -0.25)") != std::string::npos);
    CHECK(g_testLastLog.find("nonzero 1/16") != std::string::npos);
    CHECK(g_testLastLog.find("x [0, 1.5] y [-0.5, 0]") != std::string::npos);

    // Typed from the sizes above, not from the rules: 4x4 images are 16 pixels, 8x8 images are 64. Per file the header is 12
    // bytes: native 64, input 64, dlss 64 * 8, composed 64 * 4, depth 64, motion 64, so 1024 + 72.
    CHECK(diag::BytesWritten() == 1096);

    // The second phase waits for more than 120 evaluations.
    CHECK(!diag::Capture(g_context.Get(), request, 50, 2000));
    CHECK(!diag::Capture(g_context.Get(), request, 120, 2000));

    // The colour given to the upscaler is the NR before DLSS image (RGBA16F, 8 bytes per pixel) in the second dump: the rows
    // and the size budget follow the real format of each image, not the usual one of its role.
    const auto inputHalfData = Pattern(W * H * 8, 9);
    auto inputHalf = Make(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, inputHalfData, 8);
    request.input = inputHalf.Get();
    CHECK(diag::Capture(g_context.Get(), request, 121, 2000));
    PumpUntil(dir / "2-motion.bin");
    CHECK(diag::FilesWritten() == 12);
    {
        const auto bytes = ReadFile(dir / "2-input.bin");
        CHECK(bytes.size() == 12 + inputHalfData.size()); // 16 pixels * 8 bytes: not the 4 byte rows of an RGBA8 input
        if (bytes.size() == 12 + inputHalfData.size())
        {
            uint32_t header[3];
            std::memcpy(header, bytes.data(), 12);
            CHECK(header[0] == W && header[1] == H && header[2] == DXGI_FORMAT_R16G16B16A16_FLOAT);
            CHECK(std::memcmp(bytes.data() + 12, inputHalfData.data(), inputHalfData.size()) == 0); // whole rows, in order
        }
        CHECK(ReadFile(dir / "2-native.bin").size() == 12 + nativeData.size()); // the other roles keep their own sizes
        CHECK(ReadFile(dir / "2-dlss.bin").size() == 12 + dlssData.size());
    }
    CHECK(diag::BytesWritten() == 1096 + 1160); // the second dump is 1160: the 8 byte input adds 64 bytes (16 pixels * 4) to 1096

    // Two dumps only, and never more than 12 files.
    CHECK(!diag::Capture(g_context.Get(), request, 500, 3000));
    CHECK(!fs::exists(dir / "3-native.bin"));

    // Disabling the key stops everything again.
    ext::Cfg().ffxiv.chara.previewGuideCapture = false;
    CHECK(!diag::Capture(g_context.Get(), request, 900, 4000));

    // Trace lines need their own key and are rate limited.
    CHECK(!diag::TraceEnabled());
    const int before = g_testLogLines;
    diag::Trace(diag::TraceCategory::Finish, 10, "x");
    CHECK(g_testLogLines == before);
    ext::Cfg().ffxiv.chara.previewNrTrace = true;
    diag::Trace(diag::TraceCategory::Finish, 10, "first");
    diag::Trace(diag::TraceCategory::Finish, 11, "second");
    diag::Trace(diag::TraceCategory::Finish, 600, "third");
    CHECK(g_testLogLines == before + 2);

    diag::Release();
    fs::remove_all(dir, ec);
    return TEST_RESULT("chara_diag_warp");
}
