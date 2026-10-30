// Comparing a replay with the blessed images, and the bookkeeping of what was blessed. Pure: no Direct3D, no product code.
//
// What the replay is checked against is never a picture the game's GPU produced. It is the output of replaying the same
// captured inputs on a software adapter with the shaders of the build that was blessed. Hardware and software adapters
// round differently, so a comparison against hardware output could never be bit exact; two software runs of the same
// shader on the same inputs are, so any difference is a change in a shader, in the constants, or in the adapter itself.
// The provenance file records the adapter's version so the last case can be told apart from the first two.
//
// A blessed set is one folder, <set>/reference/, with one raw file per output of each dispatch and a provenance file:
//   provenance.txt                    the build and the adapter of the last blessing, and for every dispatch the shader, build,
//                                     date and adapter of the blessing that wrote its images
//   f<frame>_d<index>_u<slot>.raw     the tightly packed rows of that output
// Re-blessing a kind of dispatch rewrites only that kind's files and its lines of the provenance, so the other kinds keep the
// record of the blessing that made them.

#pragma once

#include "ext/nr/diag/NrReplayFormat.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>

namespace ext::nr::replay
{
// ---- pixels --------------------------------------------------------------------------------------------------------

struct PixelInfo
{
    enum class Kind { Unknown, Float32, Float16, Unorm8, Unorm16, Unorm10Packed };
    Kind kind = Kind::Unknown;
    uint32_t bytes = 0;    // per pixel
    uint32_t channels = 0; // decoded
    bool bgra = false;
};

inline PixelInfo PixelInfoFor(uint32_t format)
{
    using K = PixelInfo::Kind;
    switch (format)
    {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return { K::Float32, 16, 4 };
    case DXGI_FORMAT_R32G32_FLOAT: return { K::Float32, 8, 2 };
    case DXGI_FORMAT_R32_FLOAT: return { K::Float32, 4, 1 };
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return { K::Float32, 4, 1 };
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return { K::Float16, 8, 4 };
    case DXGI_FORMAT_R16G16_FLOAT: return { K::Float16, 4, 2 };
    case DXGI_FORMAT_R16_FLOAT: return { K::Float16, 2, 1 };
    case DXGI_FORMAT_R16G16B16A16_UNORM: return { K::Unorm16, 8, 4 };
    case DXGI_FORMAT_R16_UNORM: return { K::Unorm16, 2, 1 };
    case DXGI_FORMAT_R8G8B8A8_UNORM: return { K::Unorm8, 4, 4 };
    case DXGI_FORMAT_B8G8R8A8_UNORM: return { K::Unorm8, 4, 4, true };
    case DXGI_FORMAT_R8G8_UNORM: return { K::Unorm8, 2, 2 };
    case DXGI_FORMAT_R8_UNORM: return { K::Unorm8, 1, 1 };
    case DXGI_FORMAT_R10G10B10A2_UNORM: return { K::Unorm10Packed, 4, 4 };
    default: return {};
    }
}

inline float HalfToFloat(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, man = h & 1023;
    float v;
    if (exp == 0)
        v = std::ldexp(static_cast<float>(man), -24);
    else if (exp == 31)
        return man == 0 ? (sign ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity())
                        : std::numeric_limits<float>::quiet_NaN();
    else
        v = std::ldexp(static_cast<float>(man + 1024), static_cast<int>(exp) - 25);
    return sign ? -v : v;
}

// Decodes one pixel into up to four floats; unused components are left at zero.
inline void DecodePixel(const PixelInfo& info, const uint8_t* p, float out[4])
{
    using K = PixelInfo::Kind;
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    switch (info.kind)
    {
    case K::Float32:
        for (uint32_t c = 0; c < info.channels; ++c)
            std::memcpy(&out[c], p + c * 4, 4);
        break;
    case K::Float16:
        for (uint32_t c = 0; c < info.channels; ++c)
        {
            uint16_t h;
            std::memcpy(&h, p + c * 2, 2);
            out[c] = HalfToFloat(h);
        }
        break;
    case K::Unorm16:
        for (uint32_t c = 0; c < info.channels; ++c)
        {
            uint16_t u;
            std::memcpy(&u, p + c * 2, 2);
            out[c] = static_cast<float>(u) / 65535.0f;
        }
        break;
    case K::Unorm8:
        for (uint32_t c = 0; c < info.channels; ++c)
            out[c] = static_cast<float>(p[c]) / 255.0f;
        if (info.bgra)
            std::swap(out[0], out[2]);
        break;
    case K::Unorm10Packed:
    {
        uint32_t v;
        std::memcpy(&v, p, 4);
        out[0] = static_cast<float>(v & 1023u) / 1023.0f;
        out[1] = static_cast<float>((v >> 10) & 1023u) / 1023.0f;
        out[2] = static_cast<float>((v >> 20) & 1023u) / 1023.0f;
        out[3] = static_cast<float>(v >> 30) / 3.0f;
        break;
    }
    default: break;
    }
}

// ---- comparison ----------------------------------------------------------------------------------------------------

// The peak value PSNR is measured against. 1.0 for every format: the passes work in a normalised range, and a float image
// that goes above it only scores lower, which is the safe direction for a regression check.
inline constexpr double kPsnrPeak = 1.0;

struct Diff
{
    bool sameShape = true;   // both images have the size the caller said
    bool identical = false;  // every byte equal
    uint64_t differingPixels = 0;
    uint32_t firstX = 0;     // the first differing pixel in row-major order
    uint32_t firstY = 0;
    double maxAbs = 0.0;     // largest component error, in decoded units
    double mse = 0.0;        // mean over all decoded components
    double psnr = 0.0;       // dB; +inf when identical; meaningful only when psnrValid
    bool psnrValid = false;  // the format could be decoded
};

inline double PsnrFromMse(double mse, double peak = kPsnrPeak)
{
    if (mse <= 0.0)
        return std::numeric_limits<double>::infinity();
    return 10.0 * std::log10(peak * peak / mse);
}

// Compares two tightly packed images byte for byte. The PSNR and the largest error are computed on the decoded values
// when the format is known; they never decide the result.
inline Diff CompareImages(const std::vector<uint8_t>& ref, const std::vector<uint8_t>& got, uint32_t width, uint32_t height,
                          uint32_t rowBytes, uint32_t format)
{
    Diff d;
    const uint64_t expected = static_cast<uint64_t>(rowBytes) * height;
    if (ref.size() != expected || got.size() != expected || width == 0 || height == 0 || rowBytes == 0)
    {
        d.sameShape = false;
        return d;
    }

    // Pixel size from the row when the format is not one that can be decoded.
    const PixelInfo info = PixelInfoFor(format);
    const uint32_t bpp = info.bytes != 0 ? info.bytes : std::max<uint32_t>(1, rowBytes / width);

    bool first = true;
    double sumSq = 0.0;
    uint64_t components = 0;
    for (uint32_t y = 0; y < height; ++y)
    {
        const uint8_t* a = ref.data() + static_cast<size_t>(y) * rowBytes;
        const uint8_t* b = got.data() + static_cast<size_t>(y) * rowBytes;
        const uint32_t pixelsInRow = rowBytes / bpp;
        for (uint32_t x = 0; x < pixelsInRow; ++x)
        {
            const bool same = std::memcmp(a + static_cast<size_t>(x) * bpp, b + static_cast<size_t>(x) * bpp, bpp) == 0;
            if (!same)
            {
                ++d.differingPixels;
                if (first)
                {
                    first = false;
                    d.firstX = x;
                    d.firstY = y;
                }
            }
            if (info.kind != PixelInfo::Kind::Unknown && !same)
            {
                float ca[4], cb[4];
                DecodePixel(info, a + static_cast<size_t>(x) * bpp, ca);
                DecodePixel(info, b + static_cast<size_t>(x) * bpp, cb);
                for (uint32_t c = 0; c < info.channels; ++c)
                {
                    // A value that is not finite on either side counts as an error of 1, whatever it was.
                    const double e = (std::isfinite(ca[c]) && std::isfinite(cb[c])) ? std::fabs(static_cast<double>(ca[c]) - cb[c]) : 1.0;
                    d.maxAbs = std::max(d.maxAbs, e);
                    sumSq += e * e;
                }
            }
        }
        // bytes past the last whole pixel of the row (never for the formats used)
        const uint32_t tail = rowBytes - pixelsInRow * bpp;
        if (tail != 0 && std::memcmp(a + pixelsInRow * bpp, b + pixelsInRow * bpp, tail) != 0 && first)
        {
            first = false;
            d.firstX = pixelsInRow;
            d.firstY = y;
            ++d.differingPixels;
        }
    }

    d.identical = d.differingPixels == 0;
    if (info.kind != PixelInfo::Kind::Unknown)
    {
        components = static_cast<uint64_t>(width) * height * info.channels;
        d.mse = components != 0 ? sumSq / static_cast<double>(components) : 0.0;
        d.psnr = PsnrFromMse(d.mse);
        d.psnrValid = true;
    }
    return d;
}

// A picture of where two images differ, as a binary PPM: red, green and blue show the error of the first three components,
// scaled so the largest error in the image is full scale, with a floor so that any differing pixel is visible.
inline bool WriteDiffPpm(const std::filesystem::path& path, const std::vector<uint8_t>& ref, const std::vector<uint8_t>& got,
                         uint32_t width, uint32_t height, uint32_t rowBytes, uint32_t format, const Diff& diff)
{
    if (!diff.sameShape || ref.size() != got.size() || ref.size() != static_cast<size_t>(rowBytes) * height)
        return false;
    const PixelInfo info = PixelInfoFor(format);
    const uint32_t bpp = info.bytes != 0 ? info.bytes : std::max<uint32_t>(1, rowBytes / width);
    const double scale = diff.maxAbs > 0.0 ? 255.0 / diff.maxAbs : 0.0;

    std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3, 0);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width && static_cast<size_t>(x + 1) * bpp <= rowBytes; ++x)
        {
            const uint8_t* a = ref.data() + static_cast<size_t>(y) * rowBytes + static_cast<size_t>(x) * bpp;
            const uint8_t* b = got.data() + static_cast<size_t>(y) * rowBytes + static_cast<size_t>(x) * bpp;
            if (std::memcmp(a, b, bpp) == 0)
                continue;
            uint8_t* out = &rgb[(static_cast<size_t>(y) * width + x) * 3];
            if (info.kind == PixelInfo::Kind::Unknown)
            {
                out[0] = out[1] = out[2] = 255;
                continue;
            }
            float ca[4], cb[4];
            DecodePixel(info, a, ca);
            DecodePixel(info, b, cb);
            bool any = false;
            for (uint32_t c = 0; c < 3 && c < info.channels; ++c)
            {
                const double e = (std::isfinite(ca[c]) && std::isfinite(cb[c])) ? std::fabs(static_cast<double>(ca[c]) - cb[c]) : 1.0;
                out[c] = static_cast<uint8_t>(std::min(255.0, std::max(e * scale, e > 0.0 ? 64.0 : 0.0)));
                any = any || e > 0.0;
            }
            if (!any)
                out[0] = 64; // differs only in a component not shown (alpha, or a bit pattern that decodes equal)
        }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;
    f << "P6\n" << width << ' ' << height << "\n255\n";
    f.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    return static_cast<bool>(f);
}

// ---- bless -----------------------------------------------------------------------------------------------------------

// Which kinds of dispatch to write references for. "all" is every kind; otherwise a comma separated list of kinds.
struct BlessSpec
{
    bool all = false;
    std::set<std::string> kinds;
    bool Any() const { return all || !kinds.empty(); }
    bool Covers(const std::string& kind) const { return all || kinds.count(kind) != 0; }
};

inline BlessSpec ParseBless(const std::string& text)
{
    BlessSpec spec;
    size_t i = 0;
    while (i <= text.size())
    {
        size_t j = text.find(',', i);
        if (j == std::string::npos)
            j = text.size();
        std::string item = text.substr(i, j - i);
        while (!item.empty() && item.front() == ' ')
            item.erase(item.begin());
        while (!item.empty() && item.back() == ' ')
            item.pop_back();
        if (item == "all")
            spec.all = true;
        else if (!item.empty())
            spec.kinds.insert(item);
        i = j + 1;
    }
    return spec;
}

struct WarpInfo
{
    std::string version; // the adapter's file version, for example 10.0.26100.1
    uint64_t hash = 0;   // Fnv1a64 of its file
    std::string error;   // why the adapter's file could not be read; empty when it was

    // An adapter that could not be identified is never a match for anything, not even for itself.
    bool Valid() const { return error.empty() && !version.empty() && version != "unknown" && hash != 0; }
};

inline bool SameWarp(const WarpInfo& a, const WarpInfo& b)
{
    return a.Valid() && b.Valid() && a.version == b.version && a.hash == b.hash;
}

// The software adapter Windows ships ("Microsoft Basic Render Driver"): the only adapter a replay may run on.
inline bool AdapterIsWarp(uint32_t vendorId, uint32_t deviceId)
{
    return vendorId == 0x1414 && deviceId == 0x8c;
}

inline std::string WarpChangedMessage(const WarpInfo& blessed, const WarpInfo& now)
{
    return "WARP changed " + blessed.version + " (" + Hex64(blessed.hash) + ") -> " + now.version + " (" + Hex64(now.hash) +
           "): re-bless, not a shader regression";
}

inline std::string DispatchKey(uint32_t frame, uint32_t index)
{
    return std::to_string(frame) + ":" + std::to_string(index);
}

// What one blessing recorded about a dispatch. The commit, date and adapter are those of the blessing that wrote this
// dispatch's images, so a partial blessing leaves the other kinds' records as they were.
struct ProvenanceEntry
{
    std::string kind;
    uint64_t shaderHash = 0; // the blob that was replayed when this dispatch was blessed
    std::string commit;
    std::string date;
    bool dirty = false;      // the working tree had uncommitted changes
    WarpInfo warp;
};

struct Provenance
{
    std::string commit;
    std::string date;
    bool dirty = false;
    WarpInfo warp;
    std::map<std::string, ProvenanceEntry> dispatches; // DispatchKey -> entry
};

inline constexpr const char* kProvenanceMagic = "optiscaler-nr-replay-reference";
inline constexpr int kProvenanceVersion = 2;

inline std::string OrUnknown(const std::string& s)
{
    return s.empty() ? std::string("unknown") : s;
}

inline std::string WriteProvenance(const Provenance& p)
{
    std::ostringstream o;
    o << kProvenanceMagic << ' ' << kProvenanceVersion << '\n';
    o << "commit " << OrUnknown(p.commit) << '\n';
    o << "dirty " << (p.dirty ? 1 : 0) << '\n';
    o << "date " << OrUnknown(p.date) << '\n';
    o << "warp version=" << OrUnknown(p.warp.version) << " hash=" << Hex64(p.warp.hash) << '\n';
    for (const auto& [key, e] : p.dispatches)
    {
        const size_t colon = key.find(':');
        o << "dispatch frame=" << key.substr(0, colon) << " index=" << key.substr(colon + 1) << " kind=" << e.kind
          << " shaderHash=" << Hex64(e.shaderHash) << " commit=" << OrUnknown(e.commit) << " dirty=" << (e.dirty ? 1 : 0)
          << " date=" << OrUnknown(e.date) << " warpVersion=" << OrUnknown(e.warp.version) << " warpHash=" << Hex64(e.warp.hash) << '\n';
    }
    o << "end\n";
    return o.str();
}

inline bool ParseProvenance(const std::string& text, Provenance& out, std::string& error)
{
    std::istringstream in(text);
    std::string line;
    bool magic = false, end = false;
    Provenance p;
    int lineNo = 0;
    while (std::getline(in, line))
    {
        ++lineNo;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        const size_t space = line.find(' ');
        const std::string head = line.substr(0, space);
        const std::string rest = space == std::string::npos ? std::string() : line.substr(space + 1);
        if (!magic)
        {
            uint32_t version = 0;
            if (head != kProvenanceMagic || !detail::ToU32(rest, version))
            {
                error = "not a provenance file";
                return false;
            }
            if (static_cast<int>(version) != kProvenanceVersion)
            {
                error = "unsupported provenance version " + std::to_string(version);
                return false;
            }
            magic = true;
        }
        else if (head == "end")
        {
            end = true;
            break;
        }
        else if (head == "commit")
            p.commit = rest;
        else if (head == "date")
            p.date = rest;
        else if (head == "dirty")
            p.dirty = rest == "1";
        else if (head == "warp")
        {
            const auto f = detail::SplitFields(line, space + 1);
            const auto v = f.find("version"), h = f.find("hash");
            if (v == f.end() || h == f.end() || !FromHex64(h->second, p.warp.hash))
            {
                error = "line " + std::to_string(lineNo) + ": bad warp record";
                return false;
            }
            p.warp.version = v->second;
        }
        else if (head == "dispatch")
        {
            const auto f = detail::SplitFields(line, space + 1);
            detail::Reader r { f, lineNo, {} };
            uint32_t frame = 0, index = 0;
            ProvenanceEntry e;
            uint32_t dirty = 0;
            if (!(r.U32("frame", frame) && r.U32("index", index) && r.Text("kind", e.kind) && r.H64("shaderHash", e.shaderHash) &&
                  r.Text("commit", e.commit) && r.U32("dirty", dirty) && r.Text("date", e.date) && r.Text("warpVersion", e.warp.version) &&
                  r.H64("warpHash", e.warp.hash)))
            {
                error = r.error;
                return false;
            }
            e.dirty = dirty != 0;
            p.dispatches[DispatchKey(frame, index)] = e;
        }
    }
    if (!magic || !end)
    {
        error = magic ? "provenance is truncated (no end record)" : "empty provenance";
        return false;
    }
    out = p;
    return true;
}

inline std::string ReferenceFileName(uint32_t frame, uint32_t index, uint32_t slot)
{
    return "f" + std::to_string(frame) + "_d" + std::to_string(index) + "_u" + std::to_string(slot) + ".raw";
}

// What one dispatch needs done.
enum class Verdict
{
    Blessed,       // a reference was written
    Match,         // every output equals its reference
    Differs,       // an output differs
    NoReference,   // nothing blessed for this dispatch and it is not being blessed now
};

// A blessed set exists when the reference folder holds a provenance file.
inline bool HasBlessedSet(const std::filesystem::path& referenceDir)
{
    std::error_code ec;
    return std::filesystem::exists(referenceDir / "provenance.txt", ec);
}

// Whether a dispatch is blessed in this run: its kind is covered by the request.
inline bool BlessesNow(const BlessSpec& spec, const std::string& kind)
{
    return spec.Covers(kind);
}

// Whether the replay may run as a comparison against what was blessed: the adapter must be the one the set was blessed
// with, unless the run is blessing every kind (which replaces the whole provenance).
// An adapter that could not be identified is refused whatever the request says: "all" replaces a blessing made on an older
// version of the same adapter, it does not vouch for an adapter nobody could name.
inline bool AdapterAcceptable(const Provenance& blessed, const WarpInfo& now, const BlessSpec& spec)
{
    return now.Valid() && (spec.all || SameWarp(blessed.warp, now));
}

// ---- the bless request in the environment ---------------------------------------------------------------------------------

// A blessing rewrites what every later run is compared with, so a request left in the environment must not do it silently.
// The request (OPTISCALER_NR_REPLAY_BLESS) only takes effect together with the confirmation (OPTISCALER_NR_REPLAY_BLESS_OK=1);
// without it nothing is written and the run ends with kExitBlessRefused, which no passing compare ever returns.
inline constexpr int kExitBlessRefused = 3;

enum class RunMode { Compare, Bless, Refused };

inline RunMode DecideRunMode(const std::string& blessRequest, const std::string& blessConfirm)
{
    if (blessRequest.empty())
        return RunMode::Compare;
    return blessConfirm == "1" ? RunMode::Bless : RunMode::Refused;
}

// The kinds a request names that no dispatch of the given sets has. A kind may be missing from one set and present in another
// (not every scene runs the spatial stages), so the sets are looked at together.
inline std::vector<std::string> UnmatchedBlessKinds(const BlessSpec& spec, const std::vector<const Manifest*>& sets)
{
    std::set<std::string> present;
    for (const Manifest* m : sets)
        for (const Dispatch& d : m->dispatches)
            present.insert(d.kind);
    std::vector<std::string> out;
    for (const std::string& k : spec.kinds)
        if (present.count(k) == 0)
            out.push_back(k);
    return out;
}

// The line that makes a blessing impossible to mistake for a compare.
inline std::string BlessBanner(const BlessSpec& spec, const std::string& commit, bool dirty)
{
    std::string what;
    if (spec.all)
        what = "all";
    for (const std::string& k : spec.kinds)
        what += (what.empty() ? "" : ",") + k;
    return "BLESSED " + what + " at " + OrUnknown(commit) + (dirty ? " (working tree has uncommitted changes)" : "");
}

// Running git in the repository a blessing names: `git -C <repo> <args>`. The repo path is quoted so a path with
// spaces works. A path that cannot survive the quoting (empty, or holding a quote, a percent sign or a line break,
// which would break out of the quoting or be expanded by the shell) gives an empty command; the caller then records
// an unknown commit and a dirty tree. A trailing separator is dropped first: it would escape the closing quote when
// the child parses its command line.
inline std::string GitCommand(const std::string& repo, const std::string& args)
{
    if (repo.empty() || repo.find_first_of("\"%\r\n") != std::string::npos)
        return {};
    std::string path = repo;
    while (path.size() > 1 && (path.back() == '\\' || path.back() == '/'))
        path.pop_back();
    return "git -C \"" + path + "\" " + args;
}

// The commit recorded in the provenance of a blessing is one word: the per-dispatch records are space-separated
// key=value pairs, so a commit holding whitespace would be cut short there while the top-level record keeps it whole.
// Empty (the variable is not set) is fine: the commit the repository is on is recorded.
inline std::string CommitOverrideProblem(const std::string& override_)
{
    if (override_.empty() || override_.find_first_of(" \t\r\n") == std::string::npos)
        return {};
    return "OPTISCALER_NR_REPLAY_COMMIT=\"" + override_ +
           "\" holds whitespace; the provenance records the commit as one word (leave it unset to record the commit the repository is on).";
}

// The capture side decides whether a replay set is being recorded at this moment: the key is on, a capture was asked for,
// and fewer frames than asked for are recorded so far. Nothing is recorded, and nothing is read back, otherwise.
inline bool ReplayRecordingWanted(bool keyEnabled, bool requested, uint32_t recorded, uint32_t wanted)
{
    return keyEnabled && requested && recorded < wanted;
}
} // namespace ext::nr::replay
