// The on-disk format of an NR frame-replay set.
//
// A replay set is what is needed to run the NR compute passes of one frame again without the game: for every
// dispatch the constants as they were bound, the contents of every resource the pass could read or write, and the
// views that were bound on them. Replaying the inputs on another adapter and comparing the outputs bit for bit is
// how a later change to a shader is checked against an earlier build.
//
// This file is the pure side: the data model, the text manifest, and the hashing and row packing the images share.
// It includes nothing from Direct3D and nothing from the rest of the product, so the writer in the game and the
// replay tests read and write exactly the same bytes.
//
// Layout of a set (<dir> is one replay folder):
//   <dir>/manifest.txt        this format
//   <dir>/images/img_NNNN.raw one image per distinct content, tightly packed rows (the readback's row padding is
//                             never stored: it holds garbage, which would make hashes and compares unstable)
//
// The manifest is text, one record per line, "kind key=value key=value ...", values without spaces:
//   optiscaler-nr-replay 2
//   frames 2
//   frameinfo frame= dispatches= skipped= boundariesSkipped=      (one per frame: dispatches the pass made, dispatches left out)
//   image id= file= width= height= format= srcRowPitch= rowBytes= bytes= hash=
//   dispatch frame= index= kind= shader= shaderHash= groups=x,y,z constants=<512 hex digits>
//   res local= width= height= resourceFormat= copyFormat= mips= arraySize= before= after=     (belongs to the dispatch above)
//   bind type=srv|uav slot= res= view= dim=2d|2darray mip= slice= slices= mips= state=         (belongs to the dispatch above)
//   boundary frame= name= image= resourceFormat=
//   end
// The views the passes bind always span the whole resource: mip 0, every array slice (a shader view sees every mip).
// The sub-rectangles a pass works on (the guides' base offsets and extents) are not part of the view; they travel in
// the constants bytes, so replaying the constants replays the sub-rectangle. Only subresource 0 of a resource is stored.
// A dispatch the recorder could not record is not silently absent: the pass's dispatch index still advances, so the indices of a
// frame show a gap, and frameinfo says how many dispatches (and boundary pictures) were left out. A set with a gap, a left-out
// dispatch or an empty frame is incomplete and a replay of it fails (see CheckComplete). Every dispatch lists all of t0..t5 and
// u0..u1 exactly once; a slot a mode does not use is bound to a resource the pass chose.
// "after=" and "before=" are image ids, or "-" for none. Formats are DXGI_FORMAT numbers. "format" of an image and
// "copyFormat" of a resource are the format the pixels were copied out in (a typeless resource is copied as the typed
// member of its family); "resourceFormat" is what the game declared; "view" is the format the shader view was created
// with. Unknown keys are ignored, a missing required key is an error, and a manifest of another version is refused.

#pragma once

#include <dxgiformat.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace ext::nr::replay
{
inline constexpr int kManifestVersion = 2;
inline constexpr const char* kManifestMagic = "optiscaler-nr-replay";
inline constexpr size_t kConstantBytes = 256;
inline constexpr uint32_t kNoImage = 0xFFFFFFFFu;
inline constexpr uint32_t kSrvSlots = 6;
inline constexpr uint32_t kUavSlots = 2;

// D3D12_RESOURCE_STATES values the passes bind in, kept as numbers so this file needs no D3D12 header.
inline constexpr uint32_t kStateNonPixelShaderResource = 0x40;
inline constexpr uint32_t kStateUnorderedAccess = 0x8;
inline constexpr uint32_t kStateCopySource = 0x800;

struct Image
{
    uint32_t id = 0;
    std::string file;        // relative to the set folder
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;     // DXGI_FORMAT the pixels were copied out in
    uint32_t srcRowPitch = 0; // the readback row pitch (information only; the file is tight)
    uint32_t rowBytes = 0;   // bytes of one row in the file
    uint64_t bytes = 0;      // rowBytes * height
    uint64_t hash = 0;       // HashRows of the tight content
};

// One distinct D3D12 resource a dispatch touched. Two slots bound to the same resource share one entry, which is
// how a replay keeps the aliasing (an unused slot is bound to the source frame; the second output may be the first).
struct Resource
{
    uint32_t local = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t resourceFormat = 0;
    uint32_t copyFormat = 0;
    uint32_t mips = 1;
    uint32_t arraySize = 1;
    uint32_t before = kNoImage; // contents before the dispatch
    uint32_t after = kNoImage;  // contents after it; set for a resource bound as an output
};

struct Binding
{
    bool uav = false;
    uint32_t slot = 0;
    uint32_t res = 0;        // Resource::local
    uint32_t viewFormat = 0; // the format the view was created with (the typed view over a typeless resource)
    bool array = false;
    uint32_t mip = 0;        // UAV mip slice / SRV most detailed mip
    uint32_t firstSlice = 0;
    uint32_t slices = 1;
    uint32_t mips = 1;       // SRV mip count
    uint32_t state = 0;      // the state the resource was assumed to be in for this binding
};

struct Dispatch
{
    uint32_t frame = 0;
    uint32_t index = 0;
    std::string kind;
    std::string shader;
    uint64_t shaderHash = 0;
    uint32_t groups[3] = { 1, 1, 1 };
    std::array<uint8_t, kConstantBytes> constants {};
    std::vector<Resource> resources;
    std::vector<Binding> bindings;
};

// A named picture at a boundary of the pass: what the model returned, the Output Scaling legs, the untouched game
// frame and the final edited frame. Informational for a replay (the model is not replayed).
struct Boundary
{
    uint32_t frame = 0;
    std::string name;
    uint32_t image = kNoImage;
    uint32_t resourceFormat = 0;
};

// What the recorder knows about one recorded frame: the number of dispatches the pass issued (recorded or not), the number it
// could not record and the number of boundary pictures it could not take.
struct FrameInfo
{
    uint32_t frame = 0;
    uint32_t dispatches = 0;
    uint32_t skipped = 0;
    uint32_t boundariesSkipped = 0;
};

struct Manifest
{
    int version = kManifestVersion;
    uint32_t frames = 0;
    std::vector<FrameInfo> frameInfos;
    std::vector<Image> images;
    std::vector<Dispatch> dispatches;
    std::vector<Boundary> boundaries;
};

// ---- hashing and rows ----------------------------------------------------------------------------------------------

inline uint64_t Fnv1a64(const void* data, size_t size, uint64_t seed = 0xcbf29ce484222325ull)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < size; ++i)
    {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

// Hash of the tight content of an image: the row length and row count are part of it, so the same bytes read as a
// different shape do not collide. The padding between rows of a readback is never read.
inline uint64_t HashRows(const uint8_t* src, uint32_t srcPitch, uint32_t rowBytes, uint32_t rows)
{
    uint64_t h = Fnv1a64(&rowBytes, sizeof(rowBytes));
    h = Fnv1a64(&rows, sizeof(rows), h);
    for (uint32_t y = 0; y < rows; ++y)
        h = Fnv1a64(src + static_cast<size_t>(y) * srcPitch, rowBytes, h);
    return h;
}

inline std::vector<uint8_t> PackRows(const uint8_t* src, uint32_t srcPitch, uint32_t rowBytes, uint32_t rows)
{
    std::vector<uint8_t> out(static_cast<size_t>(rowBytes) * rows);
    for (uint32_t y = 0; y < rows; ++y)
        std::memcpy(out.data() + static_cast<size_t>(y) * rowBytes, src + static_cast<size_t>(y) * srcPitch, rowBytes);
    return out;
}

inline std::string Hex(const uint8_t* data, size_t size)
{
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(size * 2);
    for (size_t i = 0; i < size; ++i)
    {
        s.push_back(digits[data[i] >> 4]);
        s.push_back(digits[data[i] & 15]);
    }
    return s;
}

inline std::string Hex64(uint64_t v)
{
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

inline bool FromHex(const std::string& text, uint8_t* out, size_t size)
{
    if (text.size() != size * 2)
        return false;
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    for (size_t i = 0; i < size; ++i)
    {
        const int hi = nibble(text[i * 2]), lo = nibble(text[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = static_cast<uint8_t>(hi * 16 + lo);
    }
    return true;
}

inline bool FromHex64(const std::string& text, uint64_t& out)
{
    uint8_t b[8];
    if (!FromHex(text, b, 8))
        return false;
    out = 0;
    for (int i = 0; i < 8; ++i)
        out = (out << 8) | b[i];
    return true;
}

// The format a resource is copied out (and back in) in. A copy needs a fully typed format, and a placed footprint
// carrying a typeless one makes CopyTextureRegion invalid, which removes the device. The depth formats that have a
// stencil plane are left alone on purpose: the footprint of their depth plane comes from GetCopyableFootprints.
inline uint32_t CopyFormatFor(uint32_t f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R32G32B32_TYPELESS: return DXGI_FORMAT_R32G32B32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R8G8_TYPELESS: return DXGI_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
    default: return f;
    }
}

// Formats whose first plane is depth: copied through the footprint of subresource 0 as the device reports it. The two
// "X8" names are the depth-plane view formats; the guide copies NR makes of a depth buffer are declared with them.
inline bool IsPlanarDepthFormat(uint32_t f)
{
    return f == DXGI_FORMAT_R24G8_TYPELESS || f == DXGI_FORMAT_R32G8X24_TYPELESS ||
           f == DXGI_FORMAT_D24_UNORM_S8_UINT || f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
           f == DXGI_FORMAT_R24_UNORM_X8_TYPELESS || f == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
}

// Any format a depth buffer can be declared with. Such a resource is created with the depth-stencil flag (never the
// unordered-access flag) and is only ever read by the passes.
inline bool IsDepthResourceFormat(uint32_t f)
{
    return IsPlanarDepthFormat(f) || f == DXGI_FORMAT_D32_FLOAT || f == DXGI_FORMAT_D16_UNORM;
}

// ---- writer --------------------------------------------------------------------------------------------------------

inline std::string ImageRef(uint32_t id)
{
    return id == kNoImage ? std::string("-") : std::to_string(id);
}

inline std::string WriteManifest(const Manifest& m)
{
    std::ostringstream o;
    o << kManifestMagic << ' ' << m.version << '\n';
    o << "frames " << m.frames << '\n';

    for (const FrameInfo& f : m.frameInfos)
        o << "frameinfo frame=" << f.frame << " dispatches=" << f.dispatches << " skipped=" << f.skipped
          << " boundariesSkipped=" << f.boundariesSkipped << '\n';

    for (const Image& i : m.images)
        o << "image id=" << i.id << " file=" << i.file << " width=" << i.width << " height=" << i.height
          << " format=" << i.format << " srcRowPitch=" << i.srcRowPitch << " rowBytes=" << i.rowBytes
          << " bytes=" << i.bytes << " hash=" << Hex64(i.hash) << '\n';

    for (const Dispatch& d : m.dispatches)
    {
        o << "dispatch frame=" << d.frame << " index=" << d.index << " kind=" << d.kind << " shader=" << d.shader
          << " shaderHash=" << Hex64(d.shaderHash) << " groups=" << d.groups[0] << ',' << d.groups[1] << ',' << d.groups[2]
          << " constants=" << Hex(d.constants.data(), d.constants.size()) << '\n';

        for (const Resource& r : d.resources)
            o << "res local=" << r.local << " width=" << r.width << " height=" << r.height
              << " resourceFormat=" << r.resourceFormat << " copyFormat=" << r.copyFormat << " mips=" << r.mips
              << " arraySize=" << r.arraySize << " before=" << ImageRef(r.before) << " after=" << ImageRef(r.after) << '\n';

        for (const Binding& b : d.bindings)
            o << "bind type=" << (b.uav ? "uav" : "srv") << " slot=" << b.slot << " res=" << b.res << " view=" << b.viewFormat
              << " dim=" << (b.array ? "2darray" : "2d") << " mip=" << b.mip << " slice=" << b.firstSlice
              << " slices=" << b.slices << " mips=" << b.mips << " state=" << b.state << '\n';
    }

    for (const Boundary& b : m.boundaries)
        o << "boundary frame=" << b.frame << " name=" << b.name << " image=" << ImageRef(b.image)
          << " resourceFormat=" << b.resourceFormat << '\n';

    o << "end\n";
    return o.str();
}

// ---- reader --------------------------------------------------------------------------------------------------------

struct ParseResult
{
    bool ok = false;
    std::string error;
    Manifest manifest;
};

namespace detail
{
using Fields = std::map<std::string, std::string>;

inline Fields SplitFields(const std::string& line, size_t from)
{
    Fields f;
    size_t i = from;
    while (i < line.size())
    {
        while (i < line.size() && line[i] == ' ')
            ++i;
        size_t j = i;
        while (j < line.size() && line[j] != ' ')
            ++j;
        if (j > i)
        {
            const std::string token = line.substr(i, j - i);
            const size_t eq = token.find('=');
            if (eq != std::string::npos)
                f[token.substr(0, eq)] = token.substr(eq + 1);
        }
        i = j;
    }
    return f;
}

inline bool ToU32(const std::string& s, uint32_t& out)
{
    if (s.empty() || s.size() > 10)
        return false;
    uint64_t v = 0;
    for (char c : s)
    {
        if (c < '0' || c > '9')
            return false;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    if (v > 0xFFFFFFFFull)
        return false;
    out = static_cast<uint32_t>(v);
    return true;
}

inline bool ToU64(const std::string& s, uint64_t& out)
{
    if (s.empty() || s.size() > 20)
        return false;
    uint64_t v = 0;
    for (char c : s)
    {
        if (c < '0' || c > '9')
            return false;
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (v > (0xFFFFFFFFFFFFFFFFull - digit) / 10)
            return false;
        v = v * 10 + digit;
    }
    out = v;
    return true;
}

struct Reader
{
    const Fields& f;
    int line;
    std::string error;

    bool Fail(const std::string& what)
    {
        if (error.empty())
            error = "line " + std::to_string(line) + ": " + what;
        return false;
    }
    const std::string* Get(const char* key)
    {
        const auto it = f.find(key);
        if (it == f.end())
        {
            Fail(std::string("missing ") + key);
            return nullptr;
        }
        return &it->second;
    }
    bool U32(const char* key, uint32_t& out)
    {
        const std::string* s = Get(key);
        if (!s)
            return false;
        if (!ToU32(*s, out))
            return Fail(std::string("bad number for ") + key + ": " + *s);
        return true;
    }
    bool U64(const char* key, uint64_t& out)
    {
        const std::string* s = Get(key);
        if (!s)
            return false;
        if (!ToU64(*s, out))
            return Fail(std::string("bad number for ") + key + ": " + *s);
        return true;
    }
    bool H64(const char* key, uint64_t& out)
    {
        const std::string* s = Get(key);
        if (!s)
            return false;
        if (!FromHex64(*s, out))
            return Fail(std::string("bad hash for ") + key + ": " + *s);
        return true;
    }
    bool Text(const char* key, std::string& out)
    {
        const std::string* s = Get(key);
        if (!s)
            return false;
        out = *s;
        return true;
    }
    bool ImageId(const char* key, uint32_t& out)
    {
        const std::string* s = Get(key);
        if (!s)
            return false;
        if (*s == "-")
        {
            out = kNoImage;
            return true;
        }
        return U32(key, out);
    }
};
} // namespace detail

// A file name from a manifest is relative and stays inside the set folder: no drive, no leading separator, no ".." part.
inline bool IsConfinedPath(const std::string& file)
{
    if (file.empty() || file[0] == '/' || file[0] == '\\' || file.find(':') != std::string::npos)
        return false;
    size_t i = 0;
    while (i <= file.size())
    {
        size_t j = file.find_first_of("/\\", i);
        if (j == std::string::npos)
            j = file.size();
        const std::string part = file.substr(i, j - i);
        if (part == ".." || part == ".")
            return false;
        i = j + 1;
    }
    return true;
}

inline std::string Validate(const Manifest& m)
{
    std::map<uint32_t, const Image*> images;
    for (const Image& i : m.images)
    {
        if (!images.emplace(i.id, &i).second)
            return "duplicate image id " + std::to_string(i.id);
        if (!IsConfinedPath(i.file))
            return "image " + std::to_string(i.id) + ": the file name " + i.file + " is not inside the set folder";
        if (i.rowBytes == 0 || i.height == 0 || i.width == 0)
            return "image " + std::to_string(i.id) + " has an empty shape";
        if (i.bytes != static_cast<uint64_t>(i.rowBytes) * i.height)
            return "image " + std::to_string(i.id) + ": bytes does not equal rowBytes * height";
        if (i.srcRowPitch < i.rowBytes)
            return "image " + std::to_string(i.id) + ": srcRowPitch is smaller than rowBytes";
    }

    const auto known = [&](uint32_t id) { return id == kNoImage || images.count(id) != 0; };

    std::map<uint32_t, const FrameInfo*> infos;
    for (const FrameInfo& f : m.frameInfos)
    {
        if (f.frame >= m.frames)
            return "frameinfo for frame " + std::to_string(f.frame) + ", but the set has " + std::to_string(m.frames) + " frames";
        if (!infos.emplace(f.frame, &f).second)
            return "duplicate frameinfo for frame " + std::to_string(f.frame);
    }

    std::set<std::pair<uint32_t, uint32_t>> seen;
    for (const Dispatch& d : m.dispatches)
    {
        const std::string where = "dispatch " + std::to_string(d.frame) + ":" + std::to_string(d.index);
        if (!seen.emplace(d.frame, d.index).second)
            return where + ": duplicate dispatch (same frame and index)";
        if (d.frame >= m.frames)
            return where + ": the set has " + std::to_string(m.frames) + " frames";
        const auto info = infos.find(d.frame);
        if (info != infos.end() && d.index >= info->second->dispatches)
            return where + ": the index is past the " + std::to_string(info->second->dispatches) + " dispatches the frame had";

        std::map<uint32_t, const Resource*> res;
        for (const Resource& r : d.resources)
        {
            if (!res.emplace(r.local, &r).second)
                return where + ": duplicate resource " + std::to_string(r.local);
            if (!known(r.before) || !known(r.after))
                return where + ": resource " + std::to_string(r.local) + " names an unknown image";
            if (r.width == 0 || r.height == 0)
                return where + ": resource " + std::to_string(r.local) + " has an empty shape";
            if (r.copyFormat != CopyFormatFor(r.resourceFormat))
                return where + ": resource " + std::to_string(r.local) + ": copyFormat " + std::to_string(r.copyFormat) +
                       " is not the copy format of resourceFormat " + std::to_string(r.resourceFormat);
            for (const uint32_t id : { r.before, r.after })
            {
                if (id == kNoImage)
                    continue;
                const Image& im = *images[id];
                if (im.format != r.copyFormat)
                    return where + ": resource " + std::to_string(r.local) + ": image " + std::to_string(id) + " has format " +
                           std::to_string(im.format) + ", the resource copies as " + std::to_string(r.copyFormat);
                if (im.width != r.width || im.height != r.height)
                    return where + ": resource " + std::to_string(r.local) + ": image " + std::to_string(id) + " is " +
                           std::to_string(im.width) + "x" + std::to_string(im.height) + ", the resource is " + std::to_string(r.width) +
                           "x" + std::to_string(r.height);
            }
        }

        // every dispatch binds each of t0..t5 and u0..u1 once
        std::set<std::pair<bool, uint32_t>> slots;
        for (const Binding& b : d.bindings)
        {
            if (res.count(b.res) == 0)
                return where + ": binding " + (b.uav ? "u" : "t") + std::to_string(b.slot) + " names an unknown resource";
            if (b.slot >= (b.uav ? kUavSlots : kSrvSlots))
                return where + ": binding slot " + std::to_string(b.slot) + " is out of range";
            if (!slots.emplace(b.uav, b.slot).second)
                return where + ": " + (b.uav ? "u" : "t") + std::to_string(b.slot) + " is bound twice";
            if (b.uav && res[b.res]->after == kNoImage)
                return where + ": output u" + std::to_string(b.slot) + " has no image after the dispatch";
        }
        for (uint32_t s = 0; s < kSrvSlots; ++s)
            if (slots.count({ false, s }) == 0)
                return where + ": t" + std::to_string(s) + " is not bound";
        for (uint32_t s = 0; s < kUavSlots; ++s)
            if (slots.count({ true, s }) == 0)
                return where + ": u" + std::to_string(s) + " is not bound";
    }

    for (const Boundary& b : m.boundaries)
    {
        if (!known(b.image))
            return "boundary " + b.name + " names an unknown image";
        if (b.image != kNoImage && images[b.image]->format != CopyFormatFor(b.resourceFormat))
            return "boundary " + b.name + ": the image has format " + std::to_string(images[b.image]->format) +
                   ", the resource copies as " + std::to_string(CopyFormatFor(b.resourceFormat));
    }

    return {};
}

// Whether the set holds everything the pass did in the frames it covers. Returns one line per problem, empty when complete:
// a frame the recorder did not describe, a dispatch or boundary picture the recorder had to leave out, an index missing from
// a frame (the sequence has a gap), or a frame with no dispatch at all. A set like that cannot stand for the frame.
inline std::vector<std::string> CheckComplete(const Manifest& m)
{
    std::vector<std::string> problems;
    if (m.frames == 0)
        problems.push_back("the set has no frames");
    std::map<uint32_t, const FrameInfo*> infos;
    for (const FrameInfo& f : m.frameInfos)
        infos[f.frame] = &f;
    for (uint32_t frame = 0; frame < m.frames; ++frame)
    {
        const std::string name = "frame " + std::to_string(frame);
        const auto info = infos.find(frame);
        if (info == infos.end())
            problems.push_back(name + ": the manifest does not say how many dispatches it had");
        else
        {
            if (info->second->skipped > 0)
                problems.push_back(name + ": " + std::to_string(info->second->skipped) + " dispatch(es) were left out of the capture");
            if (info->second->boundariesSkipped > 0)
                problems.push_back(name + ": " + std::to_string(info->second->boundariesSkipped) + " boundary picture(s) were left out of the capture");
        }
        std::set<uint32_t> present;
        for (const Dispatch& d : m.dispatches)
            if (d.frame == frame)
                present.insert(d.index);
        if (present.empty())
            problems.push_back(name + " has no dispatches");
        if (info != infos.end())
        {
            std::string missing;
            uint32_t count = 0;
            for (uint32_t i = 0; i < info->second->dispatches; ++i)
                if (present.count(i) == 0 && ++count <= 8)
                    missing += (missing.empty() ? "" : ", ") + std::to_string(i);
            if (count > 0)
                problems.push_back(name + ": dispatch index " + missing + (count > 8 ? " and more" : "") + " missing (" + std::to_string(count) +
                                   " of " + std::to_string(info->second->dispatches) + ")");
        }
    }
    return problems;
}

inline ParseResult ParseManifest(const std::string& text)
{
    ParseResult out;
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    bool sawMagic = false, sawEnd = false;
    Dispatch* current = nullptr;

    while (std::getline(in, line))
    {
        ++lineNo;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;

        const size_t space = line.find(' ');
        const std::string head = line.substr(0, space);
        const std::string rest = space == std::string::npos ? std::string() : line.substr(space + 1);

        if (!sawMagic)
        {
            if (head != kManifestMagic)
            {
                out.error = "line " + std::to_string(lineNo) + ": not a replay manifest";
                return out;
            }
            uint32_t version = 0;
            if (!detail::ToU32(rest, version))
            {
                out.error = "line " + std::to_string(lineNo) + ": bad manifest version";
                return out;
            }
            if (static_cast<int>(version) != kManifestVersion)
            {
                out.error = "unsupported manifest version " + std::to_string(version) + " (this reader reads " +
                            std::to_string(kManifestVersion) + ")";
                return out;
            }
            out.manifest.version = static_cast<int>(version);
            sawMagic = true;
            continue;
        }

        if (head == "end")
        {
            sawEnd = true;
            break;
        }

        if (head == "frames")
        {
            if (!detail::ToU32(rest, out.manifest.frames))
            {
                out.error = "line " + std::to_string(lineNo) + ": bad frame count";
                return out;
            }
            continue;
        }

        const detail::Fields fields = detail::SplitFields(line, space == std::string::npos ? line.size() : space + 1);
        detail::Reader r { fields, lineNo, {} };
        bool ok = true;

        if (head == "frameinfo")
        {
            FrameInfo f;
            ok = r.U32("frame", f.frame) && r.U32("dispatches", f.dispatches) && r.U32("skipped", f.skipped) &&
                 r.U32("boundariesSkipped", f.boundariesSkipped);
            if (ok)
                out.manifest.frameInfos.push_back(f);
        }
        else if (head == "image")
        {
            Image i;
            ok = r.U32("id", i.id) && r.Text("file", i.file) && r.U32("width", i.width) && r.U32("height", i.height) &&
                 r.U32("format", i.format) && r.U32("srcRowPitch", i.srcRowPitch) && r.U32("rowBytes", i.rowBytes) &&
                 r.U64("bytes", i.bytes) && r.H64("hash", i.hash);
            if (ok)
                out.manifest.images.push_back(i);
        }
        else if (head == "dispatch")
        {
            Dispatch d;
            std::string groups, constants;
            ok = r.U32("frame", d.frame) && r.U32("index", d.index) && r.Text("kind", d.kind) && r.Text("shader", d.shader) &&
                 r.H64("shaderHash", d.shaderHash) && r.Text("groups", groups) && r.Text("constants", constants);
            if (ok)
            {
                // exactly three unsigned numbers separated by commas
                uint32_t parts[3] = {};
                size_t at = 0;
                int found = 0;
                while (found < 3 && ok)
                {
                    const size_t comma = groups.find(',', at);
                    const bool last = found == 2;
                    if ((comma == std::string::npos) != last || !detail::ToU32(groups.substr(at, last ? std::string::npos : comma - at), parts[found]))
                        ok = r.Fail("bad groups: " + groups);
                    at = comma == std::string::npos ? groups.size() : comma + 1;
                    ++found;
                }
                if (ok)
                    for (int i = 0; i < 3; ++i)
                        d.groups[i] = parts[i];
            }
            if (ok && !FromHex(constants, d.constants.data(), d.constants.size()))
                ok = r.Fail("constants must be " + std::to_string(kConstantBytes * 2) + " lowercase hex digits");
            if (ok)
            {
                out.manifest.dispatches.push_back(d);
                current = &out.manifest.dispatches.back();
            }
        }
        else if (head == "res" || head == "bind")
        {
            if (current == nullptr)
                ok = r.Fail(head + " before any dispatch");
            else if (head == "res")
            {
                Resource x;
                ok = r.U32("local", x.local) && r.U32("width", x.width) && r.U32("height", x.height) &&
                     r.U32("resourceFormat", x.resourceFormat) && r.U32("copyFormat", x.copyFormat) && r.U32("mips", x.mips) &&
                     r.U32("arraySize", x.arraySize) && r.ImageId("before", x.before) && r.ImageId("after", x.after);
                if (ok)
                    current->resources.push_back(x);
            }
            else
            {
                Binding b;
                std::string type, dim;
                ok = r.Text("type", type) && r.U32("slot", b.slot) && r.U32("res", b.res) && r.U32("view", b.viewFormat) &&
                     r.Text("dim", dim) && r.U32("mip", b.mip) && r.U32("slice", b.firstSlice) && r.U32("slices", b.slices) &&
                     r.U32("mips", b.mips) && r.U32("state", b.state);
                if (ok && type != "srv" && type != "uav")
                    ok = r.Fail("bad binding type: " + type);
                if (ok && dim != "2d" && dim != "2darray")
                    ok = r.Fail("bad view dimension: " + dim);
                if (ok)
                {
                    b.uav = type == "uav";
                    b.array = dim == "2darray";
                    current->bindings.push_back(b);
                }
            }
        }
        else if (head == "boundary")
        {
            Boundary b;
            ok = r.U32("frame", b.frame) && r.Text("name", b.name) && r.ImageId("image", b.image) &&
                 r.U32("resourceFormat", b.resourceFormat);
            if (ok)
                out.manifest.boundaries.push_back(b);
        }
        else
        {
            ok = r.Fail("unknown record: " + head);
        }

        if (!ok)
        {
            out.error = r.error;
            return out;
        }
    }

    if (!sawMagic)
    {
        out.error = "empty manifest";
        return out;
    }
    if (!sawEnd)
    {
        out.error = "manifest is truncated (no end record)";
        return out;
    }

    out.error = Validate(out.manifest);
    out.ok = out.error.empty();
    return out;
}
} // namespace ext::nr::replay
