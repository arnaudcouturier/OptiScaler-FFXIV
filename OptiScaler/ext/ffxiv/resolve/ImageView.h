#pragma once
// The image the resolver reads: a section table plus "read bytes at an RVA". One interface, two implementations:
//   LiveImage   (LiveImage.h)  the process's own main module, read through ext::core::ReadProcessBytes
//   DenseImage  (DenseImage.h) a zero-filled in-memory image of the declared SizeOfImage, section-mapped from a file or built for a
//                              test (the CLI and every offline test)
// Both parse the same PE headers with ParsePe below, so the resolver sees identical section flags in the game and in the harness.
//
// Pure: no <windows.h> (the PE structures are read at their documented offsets), no logging, no global state.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "ResolveTypes.h"

namespace ext::ffxiv::resolve
{
inline constexpr std::uint32_t kScnCode = 0x00000020;
inline constexpr std::uint32_t kScnInitData = 0x00000040;
inline constexpr std::uint32_t kScnUninitData = 0x00000080;
inline constexpr std::uint32_t kScnDiscardable = 0x02000000;
inline constexpr std::uint32_t kScnExecute = 0x20000000;
inline constexpr std::uint32_t kScnRead = 0x40000000;
inline constexpr std::uint32_t kScnWrite = 0x80000000;
inline constexpr std::uint32_t kPageSize = 0x1000;
inline constexpr std::uint32_t kMaxImageBytes = 0x40000000; // a header claiming more than 1 GiB is refused

struct ImageSection
{
    char name[9] = {};
    std::uint32_t rva = 0;
    std::uint32_t virtualSize = 0; // 0 in the header = the raw size (what the loader does)
    std::uint32_t rawSize = 0;
    std::uint32_t characteristics = 0;

    bool Executable() const { return (characteristics & kScnExecute) != 0; }
    bool Writable() const { return (characteristics & kScnWrite) != 0; }
    bool Readable() const { return (characteristics & kScnRead) != 0; }
    bool Discardable() const { return (characteristics & kScnDiscardable) != 0; }
    bool NameIs(const char* n) const { return std::strncmp(name, n, 8) == 0; }
    std::uint32_t End() const { return rva + virtualSize; }
    // End of the part that is initialized in the file: above it (up to End()) the section is zero-filled bss.
    std::uint32_t InitializedEnd() const
    {
        if ((characteristics & kScnUninitData) != 0 && (characteristics & kScnInitData) == 0)
            return rva;
        return rva + std::min(rawSize, virtualSize);
    }
};

struct PeInfo
{
    bool valid = false;
    std::string error;
    std::uint32_t timeDateStamp = 0;
    std::uint32_t sizeOfImage = 0;
    std::uint32_t sizeOfHeaders = 0;
    std::uint64_t imageBase = 0;
    std::uint32_t exceptionRva = 0; // IMAGE_DIRECTORY_ENTRY_EXCEPTION (.pdata): the RUNTIME_FUNCTION table
    std::uint32_t exceptionSize = 0;
    std::vector<ImageSection> sections;
    // The mapped address ranges [lo, hi): the headers and every section rounded up to a page. A read must lie inside ONE range.
    std::vector<std::pair<std::uint32_t, std::uint64_t>> mapped;

    bool IsMapped(std::uint32_t rva, std::size_t bytes) const
    {
        const std::uint64_t end = static_cast<std::uint64_t>(rva) + bytes;
        for (const auto& r : mapped)
        {
            if (rva >= r.first && end <= r.second)
                return true;
        }
        return false;
    }
};

namespace detail
{
template <class T> inline bool Pull(const std::uint8_t* data, std::size_t size, std::size_t at, T& out)
{
    if (at > size || size - at < sizeof(T))
        return false;
    std::memcpy(&out, data + at, sizeof(T));
    return true;
}
inline std::uint64_t PageUp(std::uint64_t v) { return (v + kPageSize - 1) & ~static_cast<std::uint64_t>(kPageSize - 1); }
} // namespace detail

// Parses a copy of the module headers (at least SizeOfHeaders bytes, normally the first page). Validates MZ, the PE signature, the
// x64 machine and PE32+ magic, the section count (1..96), that the section table lies inside `size`, that every section lies inside
// SizeOfImage and that no two sections overlap. On failure `out.valid` is false and `out.error` says why.
inline bool ParsePe(const std::uint8_t* data, std::size_t size, PeInfo& out)
{
    out = PeInfo {};
    auto fail = [&out](const char* why)
    {
        out.valid = false;
        out.error = why;
        return false;
    };
    using namespace detail;

    std::uint16_t mz = 0;
    std::int32_t lfanew = 0;
    if (!Pull(data, size, 0, mz) || mz != 0x5a4d || !Pull(data, size, 0x3c, lfanew))
        return fail("not an MZ image");
    if (lfanew <= 0 || lfanew > 0x1000)
        return fail("bad e_lfanew");
    const std::size_t nt = static_cast<std::size_t>(lfanew);

    std::uint32_t sig = 0;
    std::uint16_t machine = 0, count = 0, optSize = 0, magic = 0;
    if (!Pull(data, size, nt, sig) || sig != 0x00004550)
        return fail("no PE signature");
    if (!Pull(data, size, nt + 4, machine) || machine != 0x8664)
        return fail("not an x64 image");
    if (!Pull(data, size, nt + 6, count) || count == 0 || count > 96)
        return fail("bad section count");
    if (!Pull(data, size, nt + 8, out.timeDateStamp) || !Pull(data, size, nt + 20, optSize))
        return fail("truncated file header");
    const std::size_t opt = nt + 24;
    if (!Pull(data, size, opt, magic) || magic != 0x20b)
        return fail("not a PE32+ image");
    if (!Pull(data, size, opt + 24, out.imageBase) || !Pull(data, size, opt + 56, out.sizeOfImage) ||
        !Pull(data, size, opt + 60, out.sizeOfHeaders))
        return fail("truncated optional header");
    if (out.sizeOfImage == 0 || out.sizeOfImage > kMaxImageBytes)
        return fail("bad SizeOfImage");
    if (optSize >= 112 + 4 * 8)
    {
        if (!Pull(data, size, opt + 112 + 3 * 8, out.exceptionRva) || !Pull(data, size, opt + 112 + 3 * 8 + 4, out.exceptionSize))
            return fail("truncated data directory");
    }

    const std::size_t table = opt + optSize;
    if (table > size || size - table < static_cast<std::size_t>(count) * 40)
        return fail("section table outside the headers");

    for (std::size_t i = 0; i < count; ++i)
    {
        const std::size_t at = table + i * 40;
        ImageSection s;
        std::memcpy(s.name, data + at, 8);
        s.name[8] = '\0';
        if (!Pull(data, size, at + 8, s.virtualSize) || !Pull(data, size, at + 12, s.rva) || !Pull(data, size, at + 16, s.rawSize) ||
            !Pull(data, size, at + 36, s.characteristics))
            return fail("truncated section header");
        if (s.virtualSize == 0)
            s.virtualSize = s.rawSize;
        if (static_cast<std::uint64_t>(s.rva) + s.virtualSize > out.sizeOfImage)
            return fail("a section lies outside SizeOfImage");
        for (const ImageSection& o : out.sections)
        {
            if (s.rva < o.End() && o.rva < s.End())
                return fail("sections overlap");
        }
        out.sections.push_back(s);
    }
    std::sort(out.sections.begin(), out.sections.end(), [](const ImageSection& a, const ImageSection& b) { return a.rva < b.rva; });

    // mapped ranges: headers, then every section rounded up to a page; merge ranges that touch
    std::vector<std::pair<std::uint32_t, std::uint64_t>> ranges;
    ranges.emplace_back(0u, std::min<std::uint64_t>(PageUp(std::max<std::uint32_t>(out.sizeOfHeaders, 1)), out.sizeOfImage));
    for (const ImageSection& s : out.sections)
        ranges.emplace_back(s.rva, std::min<std::uint64_t>(PageUp(static_cast<std::uint64_t>(s.rva) + s.virtualSize), out.sizeOfImage));
    std::sort(ranges.begin(), ranges.end());
    for (const auto& r : ranges)
    {
        if (!out.mapped.empty() && r.first <= out.mapped.back().second)
            out.mapped.back().second = std::max(out.mapped.back().second, r.second);
        else
            out.mapped.push_back(r);
    }
    out.valid = true;
    return true;
}

// The read side of the resolver. Implementations parse the headers once and keep the result in _info.
class ImageView
{
  public:
    virtual ~ImageView() = default;
    // Copies `bytes` bytes at `rva`; false when any byte lies outside the headers and the sections (nothing is partially returned
    // to the resolver as success).
    virtual bool Read(std::uint32_t rva, void* out, std::size_t bytes) const = 0;

    const PeInfo& Info() const { return _info; }
    bool Valid() const { return _info.valid; }
    std::uint32_t SizeOfImage() const { return _info.sizeOfImage; }
    std::uint32_t TimeDateStamp() const { return _info.timeDateStamp; }
    const std::vector<ImageSection>& Sections() const { return _info.sections; }

  protected:
    PeInfo _info;
};

// ---- Section classification --------------------------------------------------------------------------------------------

inline const ImageSection* SectionAt(const PeInfo& info, std::uint32_t rva)
{
    for (const ImageSection& s : info.sections)
    {
        if (rva >= s.rva && rva < s.End())
            return &s;
    }
    return nullptr;
}

// The SearchIn class a section belongs to (every section has at most one), or false.
inline bool SearchClassOf(const ImageSection& s, SearchIn& out)
{
    if (s.Executable())
    {
        out = SearchIn::Code;
        return true;
    }
    if (s.Writable())
    {
        out = SearchIn::InitData;
        return true;
    }
    if (s.Readable() && (s.characteristics & kScnInitData) != 0 && !s.Discardable() && !s.NameIs(".pdata") && !s.NameIs(".rsrc"))
    {
        out = SearchIn::ReadOnlyData;
        return true;
    }
    return false;
}

// The byte range [lo, hi) of the section that a search of class `cls` covers.
inline bool SearchRange(const ImageSection& s, SearchIn cls, std::uint32_t& lo, std::uint32_t& hi)
{
    SearchIn mine;
    if (!SearchClassOf(s, mine) || mine != cls)
        return false;
    lo = s.rva;
    hi = cls == SearchIn::InitData ? s.InitializedEnd() : s.End();
    return hi > lo;
}

inline bool InRegion(const PeInfo& info, RegionKind kind, std::uint32_t rva)
{
    const ImageSection* s = SectionAt(info, rva);
    if (s == nullptr)
        return false;
    switch (kind)
    {
    case RegionKind::Image:
        return true;
    case RegionKind::Code:
        return s->Executable();
    case RegionKind::ReadOnlyData:
    {
        SearchIn c;
        return SearchClassOf(*s, c) && c == SearchIn::ReadOnlyData;
    }
    case RegionKind::InitData:
        return s->Writable() && !s->Executable() && rva < s->InitializedEnd();
    case RegionKind::Bss:
        return !s->Executable() && (s->Writable() || (s->characteristics & kScnUninitData) != 0) && rva >= s->InitializedEnd();
    }
    return false;
}
} // namespace ext::ffxiv::resolve
