#pragma once
// Pure rules of the preview DLSS diagnostics (CharaDiag.cpp): what may be written to disk and when, the file format, the
// statistics of the motion guide and the rate limiter of the trace lines. Nothing here touches D3D or the file system.
//
// Everything that writes files is off unless [FFXIV] PreviewGuideCapture is on, and is capped: at most 12 files and
// 100 MiB per session, checked BEFORE a write.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace ext::ffxiv::chara::diag
{
inline constexpr uint32_t kMaxFiles = 12;
inline constexpr uint64_t kMaxBytes = 100ull * 1024 * 1024;
inline constexpr uint64_t kMaxPixels = 0x200000;        // a preview larger than this is not dumped
inline constexpr uint64_t kSecondPhaseAfter = 120;      // evaluations before the second dump
inline constexpr uint64_t kPhaseTimeoutMs = 5000;       // a phase that never finished is armed again after this
inline constexpr uint32_t kHeaderBytes = 12;            // width, height, DXGI format (little-endian uint32 each)
inline constexpr uint32_t kFilesPerDump = 6;

enum class Role : uint8_t
{
    Native,   // the image the game produced (finisher output)
    Input,    // the colour input of the upscaler
    Dlss,     // the upscaler output (RGBA16F, 2x)
    Composed, // the upscaled colour with the native alpha (RGBA8, 2x)
    Depth,    // R32_FLOAT
    Motion,   // RG16F, the signed motion after the camera offset was removed
};

inline constexpr std::array<Role, kFilesPerDump> kRoles { Role::Native, Role::Input, Role::Dlss, Role::Composed, Role::Depth, Role::Motion };

constexpr const char* RoleName(Role r)
{
    switch (r)
    {
    case Role::Native: return "native";
    case Role::Input: return "input";
    case Role::Dlss: return "dlss";
    case Role::Composed: return "composed";
    case Role::Depth: return "depth";
    case Role::Motion: return "motion";
    }
    return "";
}

// Bytes per pixel and the scale of the role's image relative to the render size.
constexpr uint32_t RoleBytesPerPixel(Role r)
{
    switch (r)
    {
    case Role::Dlss: return 8;
    default: return 4;
    }
}
constexpr uint32_t RoleScale(Role r) { return (r == Role::Dlss || r == Role::Composed) ? 2u : 1u; }

// Size of one file: the header plus the packed rows.
constexpr uint64_t FileBytes(Role r, uint32_t renderWidth, uint32_t renderHeight)
{
    const uint64_t w = static_cast<uint64_t>(renderWidth) * RoleScale(r);
    const uint64_t h = static_cast<uint64_t>(renderHeight) * RoleScale(r);
    return kHeaderBytes + w * h * RoleBytesPerPixel(r);
}

constexpr uint64_t DumpBytes(uint32_t renderWidth, uint32_t renderHeight)
{
    uint64_t total = 0;
    for (Role r : kRoles)
        total += FileBytes(r, renderWidth, renderHeight);
    return total;
}

// The per-session budget. A dump is written only when all of it fits, so a session never ends up with a partial set.
struct Budget
{
    uint32_t files = 0;
    uint64_t bytes = 0;

    constexpr bool Fits(uint32_t addFiles, uint64_t addBytes) const
    {
        return files + addFiles <= kMaxFiles && addBytes <= kMaxBytes && bytes <= kMaxBytes - addBytes;
    }
    constexpr void Add(uint32_t addFiles, uint64_t addBytes)
    {
        files += addFiles;
        bytes += addBytes;
    }
};

// Whether a preview of this size is dumped at all.
constexpr bool SizeAllowed(uint32_t renderWidth, uint32_t renderHeight)
{
    return renderWidth != 0 && renderHeight != 0 && static_cast<uint64_t>(renderWidth) * renderHeight <= kMaxPixels;
}

// Two dumps per session: the first evaluation, and the first evaluation after 120 have passed.
class PhaseGate
{
  public:
    // Returns the phase (0 or 1) to capture now, or -1. `evaluations` is the number of successful evaluations including
    // the one that just finished.
    int Next(uint64_t evaluations, uint64_t nowMs)
    {
        for (int phase = 0; phase < 2; ++phase)
        {
            if (_done & (1u << phase))
                continue;
            if (phase == 1 && evaluations <= kSecondPhaseAfter)
                return -1;
            if (_started & (1u << phase))
            {
                // started but never finished: arm it again after the timeout
                if (nowMs >= _startedAt[phase] && nowMs - _startedAt[phase] < kPhaseTimeoutMs)
                    return -1;
            }
            return phase;
        }
        return -1;
    }

    void Started(int phase, uint64_t nowMs)
    {
        _started |= 1u << phase;
        _startedAt[static_cast<size_t>(phase)] = nowMs;
    }
    void Finished(int phase) { _done |= 1u << phase; }
    bool Done() const { return _done == 3u; }

  private:
    uint32_t _started = 0;
    uint32_t _done = 0;
    std::array<uint64_t, 2> _startedAt {};
};

// IEEE half to float (RGBA16F / RG16F readback).
inline float HalfToFloat(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1u;
    const uint32_t exponent = (h >> 10) & 0x1Fu;
    const uint32_t mantissa = h & 0x3FFu;
    uint32_t bits;
    if (exponent == 0)
    {
        if (mantissa == 0)
            bits = sign << 31;
        else
        {
            uint32_t e = 127 - 15 + 1;
            uint32_t m = mantissa;
            while ((m & 0x400u) == 0)
            {
                m <<= 1;
                --e;
            }
            bits = (sign << 31) | (e << 23) | ((m & 0x3FFu) << 13);
        }
    }
    else if (exponent == 31)
        bits = (sign << 31) | 0x7F800000u | (mantissa << 13);
    else
        bits = (sign << 31) | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// Range of the motion guide.
struct MotionStats
{
    float minX = 0.0f, maxX = 0.0f, minY = 0.0f, maxY = 0.0f;
    uint64_t total = 0;
    uint64_t nonZero = 0;
    uint64_t invalid = 0; // NaN or infinite components

    void Add(float x, float y)
    {
        ++total;
        if (!std::isfinite(x) || !std::isfinite(y))
        {
            ++invalid;
            return;
        }
        if (total - invalid == 1)
        {
            minX = maxX = x;
            minY = maxY = y;
        }
        minX = x < minX ? x : minX;
        maxX = x > maxX ? x : maxX;
        minY = y < minY ? y : minY;
        maxY = y > maxY ? y : maxY;
        if (x != 0.0f || y != 0.0f)
            ++nonZero;
    }
};

// Trace lines: one per category per interval, and a cap on the total, so a trace never floods the log.
class TraceLimiter
{
  public:
    static constexpr size_t kCategories = 8;
    static constexpr uint64_t kIntervalMs = 500;
    static constexpr uint32_t kMaxLines = 400;

    bool Allow(size_t category, uint64_t nowMs)
    {
        if (category >= kCategories || _lines >= kMaxLines)
            return false;
        uint64_t& last = _last[category];
        if (_seen[category] && nowMs >= last && nowMs - last < kIntervalMs)
            return false;
        _seen[category] = true;
        last = nowMs;
        ++_lines;
        return true;
    }
    uint32_t Lines() const { return _lines; }

  private:
    std::array<uint64_t, kCategories> _last {};
    std::array<bool, kCategories> _seen {};
    uint32_t _lines = 0;
};
} // namespace ext::ffxiv::chara::diag
