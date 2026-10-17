#pragma once
// The character preview renderer's render-target table: 20 entries (output images, G-buffer, transparent G-buffer,
// depth candidates), each a native texture pointer and its size, read from the game's memory.
//
// This header holds the pure parts (they take byte spans and a reader callback, so they are tested on synthetic data):
// the code-pattern scan, the PE section lookup, the manager parse and the lookup. CharaTable.cpp reads the real process.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include <windows.h>

namespace ext::ffxiv::chara::table
{
inline constexpr size_t kEntries = 20;

// Entry roles by index.
constexpr const char* EntryRole(size_t i)
{
    return i < 8 ? "output" : i < 13 ? "GBuffer" : i < 18 ? "transparent GBuffer" : i < 20 ? "depth candidate" : "invalid";
}

struct Entry
{
    uintptr_t resource = 0; // native ID3D11Texture2D*
    uint32_t width = 0;
    uint32_t height = 0;

    bool Valid() const { return resource != 0 && width != 0 && height != 0; }
};

struct Snapshot
{
    std::array<Entry, kEntries> entries {};
    uint64_t tick = 0;
    bool ready = false; // the manager was read (the entries may still all be empty)

    bool AnyValid() const
    {
        for (const auto& e : entries)
        {
            if (e.Valid())
                return true;
        }
        return false;
    }

    size_t ValidCount() const
    {
        size_t n = 0;
        for (const auto& e : entries)
            n += e.Valid() ? 1 : 0;
        return n;
    }
};

// ---- Layout --------------------------------------------------------------------------------------------------------

struct Layout
{
    uint32_t managerBytes = 0x400;
    uint32_t targetsOffset = 0x2c8;
    uint32_t transparentOffset = 0x378;
    uint32_t depthOffset0 = 0x360;
    uint32_t depthOffset1 = 0x368;
    uint32_t entryBytes = 0x70;
    uint32_t resourceOffset = 0x68;
    uint32_t widthOffset = 0x40;
    uint32_t heightOffset = 0x44;
    uint32_t maxExtent = 0x4000;
};

// Offset of entry i's pointer inside the manager; 0 for an index outside 0..19.
constexpr uint32_t EntryPointerOffset(const Layout& l, size_t i)
{
    if (i < 13)
        return l.targetsOffset + 8u * static_cast<uint32_t>(i);
    if (i < 18)
        return l.transparentOffset + 8u * static_cast<uint32_t>(i - 13);
    if (i == 18)
        return l.depthOffset0;
    if (i == 19)
        return l.depthOffset1;
    return 0;
}

// Reads the table from a copy of the manager. `readEntry(address, out, bytes)` reads game memory (false = unreadable).
// A null pointer, an unreadable entry object, a null resource or a width/height of 0 or above maxExtent leaves that
// entry empty (all zero).
template <class ReadFn> Snapshot ParseManager(const uint8_t* manager, size_t managerSize, const Layout& l, ReadFn&& readEntry)
{
    Snapshot s;
    s.ready = true;

    std::array<uint8_t, 0x200> object {};
    const size_t bytes = l.entryBytes < object.size() ? l.entryBytes : object.size();
    if (l.resourceOffset + 8 > bytes || l.widthOffset + 4 > bytes || l.heightOffset + 4 > bytes)
        return s;

    for (size_t i = 0; i < kEntries; ++i)
    {
        const uint32_t off = EntryPointerOffset(l, i);
        if (off == 0 || off + 8 > managerSize)
            continue;

        uint64_t pointer = 0;
        std::memcpy(&pointer, manager + off, sizeof(pointer));
        if (pointer == 0 || !readEntry(static_cast<uintptr_t>(pointer), object.data(), bytes))
            continue;

        uint64_t resource = 0;
        uint32_t w = 0, h = 0;
        std::memcpy(&resource, object.data() + l.resourceOffset, sizeof(resource));
        std::memcpy(&w, object.data() + l.widthOffset, sizeof(w));
        std::memcpy(&h, object.data() + l.heightOffset, sizeof(h));
        if (resource == 0 || w == 0 || h == 0 || w > l.maxExtent || h > l.maxExtent)
            continue;

        s.entries[i] = { static_cast<uintptr_t>(resource), w, h };
    }
    return s;
}

// ---- Lookup --------------------------------------------------------------------------------------------------------

// Index of the entry holding exactly (resource, width, height); -1 when none. An empty table never matches.
inline int Lookup(const Snapshot& s, uintptr_t resource, uint32_t width, uint32_t height)
{
    if (resource == 0)
        return -1;
    for (size_t i = 0; i < kEntries; ++i)
    {
        const Entry& e = s.entries[i];
        if (e.resource == resource && e.width == width && e.height == height)
            return static_cast<int>(i);
    }
    return -1;
}

// Cheap pre-filter: index of the first entry with this pointer (size not compared); -1 when none.
inline int IndexOfPointer(const Snapshot& s, uintptr_t resource)
{
    if (resource == 0)
        return -1;
    for (size_t i = 0; i < kEntries; ++i)
    {
        if (s.entries[i].resource == resource)
            return static_cast<int>(i);
    }
    return -1;
}

// ---- Cache ---------------------------------------------------------------------------------------------------------

inline constexpr uint64_t kRefreshMs = 100;

// Holds a snapshot and re-reads it when it is older than kRefreshMs. `load()` returns a Snapshot.
class Cache
{
  public:
    template <class LoadFn> const Snapshot& Get(uint64_t nowMs, LoadFn&& load)
    {
        if (!_has || nowMs < _snapshot.tick || nowMs - _snapshot.tick >= kRefreshMs)
        {
            _snapshot = load();
            _snapshot.tick = nowMs;
            _has = true;
        }
        return _snapshot;
    }

    void Reset() { _has = false; }

  private:
    Snapshot _snapshot;
    bool _has = false;
};

// ---- Code pattern scan ---------------------------------------------------------------------------------------------

struct ScanResult
{
    size_t hits = 0;         // number of matches; counting stops at 2
    size_t firstOffset = 0;  // offset of the first match inside the scanned region
    size_t secondOffset = 0; // offset of the second match (valid when hits >= 2)
};

// True when data[0, n) matches `pattern` (mask byte 0 = wildcard).
inline bool MatchesAt(const uint8_t* data, const uint8_t* pattern, const uint8_t* mask, size_t n)
{
    for (size_t k = 0; k < n; ++k)
    {
        if (mask[k] && data[k] != pattern[k])
            return false;
    }
    return true;
}

// Counts matches of `pattern` whose START lies in data[0, startLimit), the data holding `size` bytes. Stops at two.
inline ScanResult ScanStarts(const uint8_t* data, size_t size, size_t startLimit, const uint8_t* pattern,
                             const uint8_t* mask, size_t n, size_t baseOffset)
{
    ScanResult r;
    for (size_t i = 0; i < startLimit && i + n <= size; ++i)
    {
        if (!MatchesAt(data + i, pattern, mask, n))
            continue;

        if (r.hits == 0)
            r.firstOffset = baseOffset + i;
        else
            r.secondOffset = baseOffset + i;
        if (++r.hits >= 2)
            break;
    }
    return r;
}

// Counts matches of `pattern` in data[0, size).
inline ScanResult ScanSpan(const uint8_t* data, size_t size, const uint8_t* pattern, const uint8_t* mask, size_t n)
{
    return ScanStarts(data, size, size, pattern, mask, n, 0);
}

// Scans a region of `regionSize` bytes read in windows of `chunk` bytes: `read(offset, out, bytes)` fills `out` (false =
// unreadable, which sets `unreadable`). Each window also holds n - 1 bytes of the next one, so a match that straddles a
// boundary is found exactly once. `scratch` must hold chunk + n - 1 bytes.
template <class ReadFn>
ScanResult ScanRegion(ReadFn&& read, size_t regionSize, const uint8_t* pattern, const uint8_t* mask, size_t n,
                      size_t chunk, uint8_t* scratch, bool& unreadable)
{
    ScanResult total;
    unreadable = false;
    if (n == 0 || chunk == 0 || regionSize < n)
        return total;

    for (size_t start = 0; start + n <= regionSize; start += chunk)
    {
        const size_t want = chunk + n - 1;
        const size_t len = (regionSize - start) < want ? (regionSize - start) : want;
        if (!read(start, scratch, len))
        {
            unreadable = true;
            return total;
        }

        const ScanResult r = ScanStarts(scratch, len, chunk, pattern, mask, n, start);
        for (size_t k = 0; k < r.hits; ++k)
        {
            if (total.hits == 0)
                total.firstOffset = k == 0 ? r.firstOffset : r.secondOffset;
            else
                total.secondOffset = k == 0 ? r.firstOffset : r.secondOffset;
            if (++total.hits >= 2)
                return total;
        }
    }
    return total;
}

// RVA a RIP-relative load points at: the instruction starts at `matchRva`, is `instructionLength` bytes long and ends in
// disp32.
constexpr int64_t RipTargetRva(uint32_t matchRva, uint32_t instructionLength, int32_t disp32)
{
    return static_cast<int64_t>(matchRva) + static_cast<int64_t>(instructionLength) + static_cast<int64_t>(disp32);
}

// ---- PE headers ----------------------------------------------------------------------------------------------------

struct Section
{
    uint32_t rva = 0;
    uint32_t size = 0; // VirtualSize
};

// Finds a section by name in a copy of the module headers. Validates MZ, the PE signature, the 64-bit optional header
// magic and the section count (<= 0x60), and that the section table lies inside `size` bytes.
inline bool FindSection(const uint8_t* image, size_t size, const char* name, Section& out)
{
    if (size < sizeof(IMAGE_DOS_HEADER))
        return false;

    IMAGE_DOS_HEADER dos {};
    std::memcpy(&dos, image, sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
        static_cast<size_t>(dos.e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > size)
        return false;

    IMAGE_NT_HEADERS64 nt {};
    std::memcpy(&nt, image + dos.e_lfanew, sizeof(nt));
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return false;

    const size_t count = nt.FileHeader.NumberOfSections;
    if (count == 0 || count > 0x60)
        return false;

    const size_t tableOffset = static_cast<size_t>(dos.e_lfanew) + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
                               nt.FileHeader.SizeOfOptionalHeader;
    if (tableOffset + count * sizeof(IMAGE_SECTION_HEADER) > size)
        return false;

    for (size_t i = 0; i < count; ++i)
    {
        IMAGE_SECTION_HEADER sh {};
        std::memcpy(&sh, image + tableOffset + i * sizeof(sh), sizeof(sh));
        if (std::strncmp(reinterpret_cast<const char*>(sh.Name), name, IMAGE_SIZEOF_SHORT_NAME) == 0)
        {
            out = { sh.VirtualAddress, sh.Misc.VirtualSize };
            return out.size != 0;
        }
    }
    return false;
}

// ---- Process side (CharaTable.cpp) ---------------------------------------------------------------------------------

enum class AccessorState : uint8_t
{
    Unresolved,
    Ready,
    Inert, // the build or the accessor could not be verified: the feature stays off for this session
};

struct Resolution
{
    AccessorState state = AccessorState::Unresolved;
    uintptr_t managerPointerAddress = 0; // address of the global that holds the manager
    uint32_t accessorRva = 0;
    uint32_t managerPointerRva = 0;
    std::string reason; // why Inert
};

// Verifies the build and the accessor once (a PE check, a read-only scan of the executable's .text, a cross-check with the
// documented RVAs) and returns the result. Safe to call from any thread. The first call logs one line.
const Resolution& Resolve();

// True once Resolve() succeeded.
bool Ready();

// Why the feature is off for this session; empty while it is not (or not yet known to be) off.
const char* InertReason();

// The table as of at most kRefreshMs ago (thread-local cache). A not-ready, empty snapshot when the accessor is not
// ready; a ready snapshot with no valid entries while the manager does not exist yet (not latched: re-read each time the
// cache expires).
const Snapshot& Current(uint64_t nowMs);

// Logs the entries (one line each for the valid ones, one summary line), tagged with `when`.
void LogTable(const char* when, const Snapshot& s);
} // namespace ext::ffxiv::chara::table
