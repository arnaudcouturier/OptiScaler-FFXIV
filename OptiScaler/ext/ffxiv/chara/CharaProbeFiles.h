#pragma once
// File-side logic of the character preview probe (CharaProbe.cpp, CharaProbeDump.cpp): converting the pixels of a read-back
// texture into an RGB image and a separate alpha image, planning and writing a dump folder under the output budget, the plain-text
// state file, and the live command channel (the command file and the result file).
//
// No D3D and no COM. Everything that touches the disk is wrapped so that no exception leaves a function: the callers run on the
// game's draw thread. Times are passed in, so the tests do not depend on the clock.

#include "ext/ffxiv/chara/CharaProbeRules.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace ext::ffxiv::chara::probe
{
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------------------------------------------------

// A path as UTF-8 text for log lines and files. (fs::path::string() converts through the ANSI code page and throws for names
// it cannot represent, such as a game folder with Japanese characters.)
inline std::string Utf8(const fs::path& path)
{
    const std::wstring& wide = path.native();
    if (wide.empty())
        return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
        return "?";
    std::string text(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), text.data(), bytes, nullptr, nullptr);
    return text;
}

// "yyyyMMdd-HHmmss": the name of a dump folder.
inline std::string FormatStamp(unsigned year, unsigned month, unsigned day, unsigned hour, unsigned minute, unsigned second)
{
    char text[40];
    std::snprintf(text, sizeof(text), "%04u%02u%02u-%02u%02u%02u", year, month, day, hour, minute, second);
    return text;
}

// "yyyy-MM-dd HH:mm:ss.mmm": the start of every line of the result file.
inline std::string FormatLogTime(unsigned year, unsigned month, unsigned day, unsigned hour, unsigned minute, unsigned second,
                                 unsigned millisecond)
{
    char text[48];
    std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u:%02u.%03u", year, month, day, hour, minute, second, millisecond);
    return text;
}

inline std::string StampNow()
{
    SYSTEMTIME t {};
    GetLocalTime(&t);
    return FormatStamp(t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

inline std::string LogTimeNow()
{
    SYSTEMTIME t {};
    GetLocalTime(&t);
    return FormatLogTime(t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
}

inline std::string SizeText(uint64_t bytes)
{
    char text[48];
    if (bytes >= 1024ull * 1024)
        std::snprintf(text, sizeof(text), "%.1f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else if (bytes >= 1024)
        std::snprintf(text, sizeof(text), "%.1f KiB", static_cast<double>(bytes) / 1024.0);
    else
        std::snprintf(text, sizeof(text), "%llu bytes", static_cast<unsigned long long>(bytes));
    return text;
}

// ---------------------------------------------------------------------------------------------------------------------
// state.txt: plain text, one "key: value" per line
// ---------------------------------------------------------------------------------------------------------------------

class StateText
{
  public:
    // Keys keep letters, digits and ". _ -" (anything else becomes "_"); control characters in a value become spaces, so a value
    // never breaks the one-line-per-key layout.
    void Add(std::string_view key, std::string_view value)
    {
        for (char c : key)
        {
            const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
            _text.push_back(plain ? c : '_');
        }
        _text += ": ";
        for (char c : value)
            _text.push_back((static_cast<unsigned char>(c) < 0x20 || c == 0x7F) ? ' ' : c);
        _text.push_back('\n');
    }

    void Add(std::string_view key, uint64_t value) { Add(key, std::to_string(value)); }
    void AddBool(std::string_view key, bool value) { Add(key, value ? "yes" : "no"); }

    const std::string& Str() const { return _text; }

  private:
    std::string _text;
};

// ---------------------------------------------------------------------------------------------------------------------
// Pixels
// ---------------------------------------------------------------------------------------------------------------------

enum class PixelLayout : uint8_t
{
    Unsupported,
    Rgba, // R8G8B8A8_TYPELESS / _UNORM / _UNORM_SRGB
    Bgra, // B8G8R8A8_TYPELESS / _UNORM / _UNORM_SRGB
    Rgba16F, // R16G16B16A16_FLOAT (the preview NR result): 8 bytes per pixel, values clamped to 0..1 and scaled to 8 bits (no gamma change)
};

constexpr size_t BytesPerPixel(PixelLayout layout) { return layout == PixelLayout::Rgba16F ? 8 : 4; }

// DXGI_FORMAT values: 27 28 29 R8G8B8A8, 90 87 91 B8G8R8A8, 10 R16G16B16A16_FLOAT.
constexpr PixelLayout LayoutOfFormat(uint32_t format)
{
    switch (format)
    {
    case 27:
    case 28:
    case 29: return PixelLayout::Rgba;
    case 90:
    case 87:
    case 91: return PixelLayout::Bgra;
    case 10: return PixelLayout::Rgba16F;
    default: return PixelLayout::Unsupported;
    }
}

// Half-precision float to a clamped 8-bit value (NaN and negative give 0, values of 1 or more give 255).
constexpr uint8_t HalfToByte(uint16_t h)
{
    const uint32_t sign = h >> 15;
    const uint32_t exponent = (h >> 10) & 0x1F;
    const uint32_t mantissa = h & 0x3FF;
    if (sign != 0 || (exponent == 0x1F && mantissa != 0)) // negative or NaN
        return 0;
    if (exponent == 0x1F) // +infinity
        return 255;
    float value = 0.0f;
    if (exponent == 0)
        value = static_cast<float>(mantissa) / 1024.0f / 16384.0f; // subnormal: mantissa * 2^-24
    else
    {
        value = 1.0f + static_cast<float>(mantissa) / 1024.0f;
        int e = static_cast<int>(exponent) - 15;
        while (e > 0) { value *= 2.0f; --e; }
        while (e < 0) { value *= 0.5f; ++e; }
    }
    if (value >= 1.0f)
        return 255;
    return static_cast<uint8_t>(value * 255.0f + 0.5f);
}

struct Planes
{
    std::vector<uint8_t> rgb;   // width * height * 3, R G B per pixel
    std::vector<uint8_t> alpha; // width * height
};

// Converts `height` rows of `width` 4-byte pixels (rows `rowPitch` bytes apart) into an RGB plane and an alpha plane. False (and
// `out` untouched) for an unsupported format, an invalid size or a buffer that is too small.
inline bool ConvertPixels(uint32_t format, uint32_t width, uint32_t height, const uint8_t* data, size_t dataSize, size_t rowPitch,
                          Planes& out)
{
    const PixelLayout layout = LayoutOfFormat(format);
    if (layout == PixelLayout::Unsupported || data == nullptr || width == 0 || height == 0 || width > kMaxPngDimension ||
        height > kMaxPngDimension)
        return false;
    const size_t rowBytes = static_cast<size_t>(width) * BytesPerPixel(layout);
    if (rowPitch < rowBytes || dataSize < static_cast<size_t>(height - 1) * rowPitch + rowBytes)
        return false;

    const size_t redAt = layout == PixelLayout::Bgra ? 2 : 0;
    const size_t blueAt = layout == PixelLayout::Bgra ? 0 : 2;
    Planes planes;
    planes.rgb.resize(static_cast<size_t>(width) * height * 3);
    planes.alpha.resize(static_cast<size_t>(width) * height);
    for (uint32_t y = 0; y < height; ++y)
    {
        const uint8_t* in = data + static_cast<size_t>(y) * rowPitch;
        uint8_t* rgb = planes.rgb.data() + static_cast<size_t>(y) * width * 3;
        uint8_t* alpha = planes.alpha.data() + static_cast<size_t>(y) * width;
        if (layout == PixelLayout::Rgba16F)
        {
            for (uint32_t x = 0; x < width; ++x, in += 8)
            {
                const auto half = [in](size_t channel) { return static_cast<uint16_t>(in[channel * 2] | (in[channel * 2 + 1] << 8)); };
                rgb[x * 3 + 0] = HalfToByte(half(0));
                rgb[x * 3 + 1] = HalfToByte(half(1));
                rgb[x * 3 + 2] = HalfToByte(half(2));
                alpha[x] = HalfToByte(half(3));
            }
            continue;
        }
        for (uint32_t x = 0; x < width; ++x, in += 4)
        {
            rgb[x * 3 + 0] = in[redAt];
            rgb[x * 3 + 1] = in[1];
            rgb[x * 3 + 2] = in[blueAt];
            alpha[x] = in[3];
        }
    }
    out = std::move(planes);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// A dump folder
// ---------------------------------------------------------------------------------------------------------------------

// How old a preview image is, in words for the state file. A texture that holds the preview is copied exactly as it is: when the window
// is closed, or the renderer idle, it still holds the last picture, and a file named native.png would pass it off as the current one.
// `lastRendered`: the tick of the last draw that rendered into a preview texture or was geometry of the preview renderer (0: none seen
// since the probe was switched on); `lastSampled`: the tick of the last draw that sampled this texture (0: none).
inline constexpr uint64_t kStaleImageMs = 10000;

inline std::string ImageAgeText(uint64_t now, uint64_t lastRendered, uint64_t lastSampled)
{
    std::string text;
    bool stale = false;
    if (lastRendered != 0 && now >= lastRendered)
    {
        const uint64_t ago = now - lastRendered;
        text = "the preview renderer last rendered " + std::to_string(ago) + " ms ago";
        stale = ago > kStaleImageMs;
    }
    else
    {
        text = "the preview renderer has not been seen rendering since the probe was switched on";
        stale = true;
    }
    if (lastSampled != 0 && now >= lastSampled)
        text += ", a draw last sampled this texture " + std::to_string(now - lastSampled) + " ms ago";
    if (stale)
        text += " (older than " + std::to_string(kStaleImageMs / 1000) + " s or unknown: probably not the picture on screen now)";
    return text;
}

// One image of a dump: the pixels as read back (tightly packed rows) or the reason there are none.
struct DumpImage
{
    std::string name; // "native", "native-3", "aa", "dlss": the file names are <name>.png and alpha-<name>.png
    bool present = false;
    std::string note; // why there is no picture (when !present)
    std::string age;  // how old the picture is (images that copy a texture which may no longer be updated); empty: not known
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    std::string formatText; // for the state file, e.g. "B8G8R8A8_UNORM(87)"
    size_t rowPitch = 0;
    std::vector<uint8_t> data;
};

struct PlannedFile
{
    std::string fileName;
    uint64_t size = 0;
    size_t image = static_cast<size_t>(-1); // index into the images; -1 for state.txt
    bool alpha = false;
};

struct DumpPlan
{
    std::vector<PlannedFile> files; // state.txt is last
    std::string stateText;          // the text given plus one line per image
    uint64_t bytes = 0;
};

// Which files a dump would write and their exact sizes. Images that are absent or cannot be converted get a line in the state text
// instead of files; state.txt is always planned.
inline DumpPlan MakePlan(const std::vector<DumpImage>& images, const std::string& stateText)
{
    DumpPlan plan;
    std::string lines;
    for (size_t i = 0; i < images.size(); ++i)
    {
        const DumpImage& img = images[i];
        const std::string key = "image." + img.name + ": ";
        if (!img.present)
        {
            lines += key + "not saved, " + (img.note.empty() ? std::string("no image available") : img.note) + "\n";
            continue;
        }
        if (LayoutOfFormat(img.format) == PixelLayout::Unsupported)
        {
            lines += key + "not saved, format " + img.formatText +
                     " is not supported (only B8G8R8A8, R8G8B8A8 and R16G16B16A16_FLOAT formats are converted)\n";
            continue;
        }
        const uint64_t rgb = PngSize(img.width, img.height, 3);
        const uint64_t alpha = PngSize(img.width, img.height, 1);
        if (rgb == 0 || alpha == 0)
        {
            lines += key + "not saved, the size " + std::to_string(img.width) + "x" + std::to_string(img.height) + " is not valid\n";
            continue;
        }
        plan.files.push_back({ img.name + ".png", rgb, i, false });
        plan.files.push_back({ "alpha-" + img.name + ".png", alpha, i, true });
        plan.bytes += rgb + alpha;
        lines += key + std::to_string(img.width) + "x" + std::to_string(img.height) + " " + img.formatText + ", saved as " + img.name +
                 ".png and alpha-" + img.name + ".png" + (img.age.empty() ? std::string() : "; " + img.age) + "\n";
    }
    plan.stateText = stateText + lines;
    plan.files.push_back({ "state.txt", plan.stateText.size(), static_cast<size_t>(-1), false });
    plan.bytes += plan.stateText.size();
    return plan;
}

// Creates <root>\<stamp>, or <root>\<stamp>-2, -3, ... when that exists. Empty path on failure.
inline fs::path CreateUniqueFolder(const fs::path& root, std::string_view stamp)
{
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec)
        return {};
    for (int n = 1; n <= 99; ++n)
    {
        const std::string name = n == 1 ? std::string(stamp) : std::string(stamp) + "-" + std::to_string(n);
        const fs::path folder = root / name;
        ec.clear();
        if (fs::create_directory(folder, ec))
            return folder;
        if (ec)
            return {};
    }
    return {};
}

inline bool WriteFileBytes(const fs::path& path, const void* data, size_t size)
{
    try
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;
        file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        file.flush();
        return file.good();
    }
    catch (...)
    {
        return false;
    }
}

struct DumpOutcome
{
    enum class Result : uint8_t
    {
        Written, // at least state.txt was written
        Refused, // the session budget does not allow the dump: nothing was written
        Failed,  // nothing could be written
    };
    Result result = Result::Failed;
    std::string folder;          // UTF-8 path of the folder (empty when none was made)
    uint32_t files = 0;          // files written
    uint32_t plannedFiles = 0;
    uint64_t bytes = 0;          // bytes written
    std::string message;         // one line for the result file and the menu
};

// Writes a dump into a new folder under `root`: all the PNG pairs of the present images that can be converted, then state.txt (the
// state text plus one line per image). The budget is checked for the whole set before anything is written (all or nothing: a refused
// dump creates no folder and no file); what was written is added to it. Never throws.
inline DumpOutcome WriteDump(const fs::path& root, std::string_view stamp, const std::vector<DumpImage>& images, const std::string& stateText,
                             OutputBudget& budget)
{
    DumpOutcome outcome;
    try
    {
        const DumpPlan plan = MakePlan(images, stateText);
        outcome.plannedFiles = static_cast<uint32_t>(plan.files.size());
        if (!budget.Fits(outcome.plannedFiles, plan.bytes))
        {
            outcome.result = DumpOutcome::Result::Refused;
            outcome.message = "refused, the output budget of this session would be exceeded (" + std::to_string(outcome.plannedFiles) +
                              " files, " + SizeText(plan.bytes) + " needed; " + std::to_string(budget.files) + " of " +
                              std::to_string(kMaxFiles) + " files and " + SizeText(budget.bytes) + " of " + SizeText(kMaxBytes) + " used)";
            return outcome;
        }

        const fs::path folder = CreateUniqueFolder(root, stamp);
        if (folder.empty())
        {
            outcome.message = "failed, the folder could not be created under " + Utf8(root);
            return outcome;
        }
        outcome.folder = Utf8(folder);

        Planes planes;
        size_t planesFor = static_cast<size_t>(-1);
        std::vector<uint8_t> encoded;
        for (const PlannedFile& file : plan.files)
        {
            const fs::path path = folder / file.fileName;
            bool ok = false;
            uint64_t size = 0;
            if (file.image == static_cast<size_t>(-1))
            {
                ok = WriteFileBytes(path, plan.stateText.data(), plan.stateText.size());
                size = plan.stateText.size();
            }
            else
            {
                const DumpImage& img = images[file.image];
                if (planesFor != file.image)
                {
                    planesFor = file.image;
                    if (!ConvertPixels(img.format, img.width, img.height, img.data.data(), img.data.size(), img.rowPitch, planes))
                    {
                        planes = Planes {};
                        planesFor = static_cast<size_t>(-1);
                    }
                }
                encoded.clear();
                if (planesFor == file.image)
                {
                    const bool done = file.alpha ? EncodePng(img.width, img.height, 1, planes.alpha.data(), encoded)
                                                 : EncodePng(img.width, img.height, 3, planes.rgb.data(), encoded);
                    ok = done && WriteFileBytes(path, encoded.data(), encoded.size());
                    size = encoded.size();
                }
            }
            if (ok)
            {
                ++outcome.files;
                outcome.bytes += size;
            }
        }
        budget.Add(outcome.files, outcome.bytes);
        if (outcome.files == 0)
        {
            outcome.message = "failed, no file could be written in " + outcome.folder;
            return outcome;
        }
        outcome.result = DumpOutcome::Result::Written;
        outcome.message = "ok, " + std::to_string(outcome.files) + " of " + std::to_string(outcome.plannedFiles) + " files (" +
                          SizeText(outcome.bytes) + ") written to " + outcome.folder;
    }
    catch (...)
    {
        outcome.result = outcome.files != 0 ? DumpOutcome::Result::Written : DumpOutcome::Result::Failed;
        outcome.message = "failed, an error stopped the dump after " + std::to_string(outcome.files) + " files";
    }
    return outcome;
}

} // namespace ext::ffxiv::chara::probe
