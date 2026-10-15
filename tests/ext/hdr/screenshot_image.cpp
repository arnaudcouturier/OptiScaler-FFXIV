#include "ext/hdr/screenshot/Image.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>

namespace
{
using namespace ext::hdr::screenshot::image;

struct Png
{
    std::vector<std::pair<std::string, std::vector<BYTE>>> chunks;

    const std::vector<BYTE>* Find(const char* name) const
    {
        for (const auto& [type, data] : chunks)
            if (type == name) return &data;
        return nullptr;
    }
    size_t Count(const char* name) const
    {
        size_t count = 0;
        for (const auto& [type, data] : chunks)
            if (type == name) ++count;
        return count;
    }
};

Png ReadPng(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    assert(input);
    std::vector<BYTE> bytes((std::istreambuf_iterator<char>(input)), {});
    constexpr BYTE signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    assert(bytes.size() >= 33 && std::equal(std::begin(signature), std::end(signature), bytes.begin()));

    Png png;
    size_t offset = 8;
    while (offset < bytes.size())
    {
        assert(bytes.size() - offset >= 12);
        const size_t length = ReadBE(bytes.data() + offset);
        assert(length <= bytes.size() - offset - 12);
        const std::string type(reinterpret_cast<const char*>(bytes.data() + offset + 4), 4);
        const uint32_t expectedCrc = ReadBE(bytes.data() + offset + 8 + length);
        assert(Crc(bytes.data() + offset + 4, length + 4) == expectedCrc);
        png.chunks.emplace_back(type, std::vector<BYTE>(bytes.begin() + offset + 8, bytes.begin() + offset + 8 + length));
        offset += length + 12;
        if (type == "IEND") break;
    }
    assert(offset == bytes.size());
    assert(png.Count("IHDR") == 1 && png.Count("IEND") == 1);
    return png;
}

std::vector<BYTE> Decode(const std::filesystem::path& path, REFWICPixelFormatGUID format, UINT stride, UINT rows)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    ComPtr<IWICBitmapDecoder> decoder;
    Check(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder));
    ComPtr<IWICBitmapFrameDecode> frame;
    Check(decoder->GetFrame(0, &frame));
    ComPtr<IWICFormatConverter> converter;
    Check(factory->CreateFormatConverter(&converter));
    Check(converter->Initialize(frame.Get(), format, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom));
    std::vector<BYTE> pixels(size_t(stride) * rows);
    Check(converter->CopyPixels(nullptr, stride, UINT(pixels.size()), pixels.data()));
    return pixels;
}

struct TempPngs
{
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("optiscaler-hdr-png-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    TempPngs() { std::filesystem::create_directories(root); }
    ~TempPngs() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    std::filesystem::path File(const wchar_t* name) const { return root / name; }
};
}

int main()
{
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    assert(SUCCEEDED(initialized));
    struct ComUninit { ~ComUninit() { CoUninitialize(); } } com;
    TempPngs temp;

    // R=128, G=64, B=32, A=255 in ReShade's RGBA8 source.
    const uint32_t sdrPixel = 0xff204080;
    const auto sdrPath = temp.File(L"sdr.png");
    SavePng(sdrPath, &sdrPixel, 1, 1, 4, false, DXGI_FORMAT_R8G8B8A8_UNORM);
    const Png sdr = ReadPng(sdrPath);
    const auto* sdrHeader = sdr.Find("IHDR");
    assert(sdrHeader && sdrHeader->size() == 13 && (*sdrHeader)[8] == 8 && (*sdrHeader)[9] == 2);
    const auto* srgb = sdr.Find("sRGB");
    assert(srgb && *srgb == std::vector<BYTE>{0});
    assert(sdr.Count("cICP") == 0 && sdr.Count("iCCP") == 0 && sdr.Count("IDAT") > 0);
    const auto sdrDecoded = Decode(sdrPath, GUID_WICPixelFormat24bppBGR, 3, 1);
    assert(sdrDecoded == (std::vector<BYTE>{0x20, 0x40, 0x80}));

    // R=1023, G=512, B=0, A=3 in R10G10B10A2; output is 16-bit RGB.
    const uint32_t hdrPixel = 1023u | (512u << 10) | (3u << 30);
    const auto hdrPath = temp.File(L"hdr.png");
    SavePng(hdrPath, &hdrPixel, 1, 1, 4, true);
    const Png hdr = ReadPng(hdrPath);
    const auto* hdrHeader = hdr.Find("IHDR");
    assert(hdrHeader && hdrHeader->size() == 13 && (*hdrHeader)[8] == 16 && (*hdrHeader)[9] == 2);
    const auto* cicp = hdr.Find("cICP");
    assert(cicp && *cicp == (std::vector<BYTE>{9, 16, 0, 1}));
    assert(hdr.Count("sRGB") == 0 && hdr.Count("iCCP") == 0 && hdr.Count("IDAT") > 0);
    const auto hdrDecoded = Decode(hdrPath, GUID_WICPixelFormat48bppRGB, 6, 1);
    std::array<uint16_t, 3> hdrChannels{};
    memcpy(hdrChannels.data(), hdrDecoded.data(), sizeof(hdrChannels));
    assert((hdrChannels == std::array<uint16_t, 3>{65535, 32800, 0}));

    const auto iccPath = temp.File(L"hdr-icc-only.png");
    SavePng(iccPath, &hdrPixel, 1, 1, 4, true, DXGI_FORMAT_R10G10B10A2_UNORM, true);
    const Png icc = ReadPng(iccPath);
    const auto* profile = icc.Find("iCCP");
    constexpr char profileName[] = "RGB_D65_202_Rel_PeQ";
    assert(profile && profile->size() > sizeof(profileName) + 1);
    assert(std::equal(std::begin(profileName), std::end(profileName), profile->begin()));
    assert((*profile)[sizeof(profileName)] == 0);
    assert(icc.Count("cICP") == 0 && icc.Count("sRGB") == 0 && icc.Count("IDAT") > 0);

    std::puts("PASS: SDR sRGB and HDR 16-bit PNGs carry valid CRCs and the expected cICP or ICC-only metadata");
    return 0;
}
