#pragma once
// Shared helpers of the resolver tests: hex strings, a small synthetic image and a log capture sink.
#include "../chara/check.h"
#include "ext/ffxiv/resolve/DenseImage.h"
#include "ext/ffxiv/resolve/Pattern.h"
#include "ext/ffxiv/resolve/Scan.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// A failed REQUIRE records the failure and leaves the test function (later lines index into what it guards).
#define REQUIRE(cond) \
    do \
    { \
        if (!(cond)) \
        { \
            CHECK(cond); \
            return; \
        } \
    } while (0)

namespace rt
{
using namespace ext::ffxiv::resolve;
using ext::ffxiv::GameBuild;

inline std::vector<std::uint8_t> FromHex(const std::string& text)
{
    std::vector<std::uint8_t> out;
    int hi = -1;
    for (char c : text)
    {
        int v = -1;
        if (c >= '0' && c <= '9')
            v = c - '0';
        else if (c >= 'a' && c <= 'f')
            v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            v = c - 'A' + 10;
        if (v < 0)
            continue;
        if (hi < 0)
            hi = v;
        else
        {
            out.push_back(static_cast<std::uint8_t>(hi * 16 + v));
            hi = -1;
        }
    }
    return out;
}

inline std::string ToHex(const std::vector<std::uint8_t>& bytes)
{
    static const char* d = "0123456789abcdef";
    std::string s;
    for (std::uint8_t b : bytes)
    {
        s.push_back(d[b >> 4]);
        s.push_back(d[b & 15]);
    }
    return s;
}

// The small synthetic layout used by most tests.
//   .text   0x1000..0x5000  executable
//   .rdata  0x6000..0x8000  read-only data
//   .data   0x8000..0xB000  writable; initialized part 0x8000..0x9000, bss 0x9000..0xB000
//   .pdata  0xB000..0xC000  (the exception directory, empty unless a test fills it)
inline constexpr std::uint32_t kTextRva = 0x1000, kTextSize = 0x4000;
inline constexpr std::uint32_t kRdataRva = 0x6000, kRdataSize = 0x2000;
inline constexpr std::uint32_t kDataRva = 0x8000, kDataSize = 0x3000, kDataRaw = 0x1000;
inline constexpr std::uint32_t kPdataRva = 0xB000, kPdataSize = 0x1000;
inline constexpr std::uint32_t kImageSize = 0xC000;
inline constexpr std::uint32_t kBssRva = 0x9000;

inline std::unique_ptr<DenseImage> MakeImage(std::uint32_t stamp = 0x11112222, std::uint32_t exceptionBytes = 0)
{
    auto img = DenseImage::Synthetic(stamp, kImageSize,
                                     { { ".text", kTextRva, kTextSize, kTextSize, DenseImage::kText },
                                       { ".rdata", kRdataRva, kRdataSize, kRdataSize, DenseImage::kRdata },
                                       { ".data", kDataRva, kDataSize, kDataRaw, DenseImage::kData },
                                       { ".pdata", kPdataRva, kPdataSize, kPdataSize, DenseImage::kRdata } },
                                     kPdataRva, exceptionBytes);
    if (!img)
        return nullptr;
    // filler that never starts a typical pattern (0x48, 0xe8, 0x8b ...): 0x90..0x94
    auto& b = img->Bytes();
    for (std::uint32_t i = kTextRva; i < kTextRva + kTextSize; ++i)
        b[i] = static_cast<std::uint8_t>(0x90 + (i * 7) % 5);
    return img;
}

inline void Plant(DenseImage& img, std::uint32_t rva, const std::string& hex)
{
    const auto bytes = FromHex(hex);
    if (!img.Put(rva, bytes))
    {
        std::printf("FAIL: could not plant at 0x%x\n", rva);
        ++g_failures;
    }
}

// .pdata helpers: RUNTIME_FUNCTION entry `index` at the start of .pdata, and an UNWIND_INFO block (flags 4 = UNW_FLAG_CHAININFO, followed by the
// parent RUNTIME_FUNCTION after the unwind codes, which are padded to an even count).
inline void PutRuntimeFunction(DenseImage& img, std::uint32_t index, std::uint32_t begin, std::uint32_t end, std::uint32_t unwind)
{
    const std::uint32_t e[3] = { begin, end, unwind };
    if (!img.Put(kPdataRva + index * 12, e, sizeof(e)))
    {
        std::printf("FAIL: could not write RUNTIME_FUNCTION %u\n", index);
        ++g_failures;
    }
}

inline void PutUnwind(DenseImage& img, std::uint32_t rva, std::uint8_t flags, std::uint8_t codes, std::uint32_t parentBegin = 0, std::uint32_t parentEnd = 0,
                      std::uint32_t parentUnwind = 0)
{
    std::vector<std::uint8_t> b = { static_cast<std::uint8_t>(1 | (flags << 3)), 0, codes, 0 };
    b.resize(4 + ((codes + 1u) & ~1u) * 2, 0);
    if ((flags & 4) != 0)
    {
        const std::uint32_t parent[3] = { parentBegin, parentEnd, parentUnwind };
        const auto* p = reinterpret_cast<const std::uint8_t*>(parent);
        b.insert(b.end(), p, p + sizeof(parent));
    }
    if (!img.Put(rva, b))
    {
        std::printf("FAIL: could not write UNWIND_INFO at 0x%x\n", rva);
        ++g_failures;
    }
}

struct LogCapture
{
    std::vector<std::pair<ext::ffxiv::resolve::LogLevel, std::string>> lines;
    ext::ffxiv::resolve::LogSink Sink()
    {
        return [this](ext::ffxiv::resolve::LogLevel l, const std::string& s) { lines.emplace_back(l, s); };
    }
};
} // namespace rt
