#pragma once
// Enclosing-function lookup through the x64 exception directory (.pdata): the RUNTIME_FUNCTION table lists every function that has an
// unwind entry, sorted by start address. This gives a real function start for an anchor that sits inside a function body, so the seam
// does not depend on a fixed offset back to the entry.
//
// Two facts measured on the FFXIV executable:
//   * 42% of the 173,212 entries are CHAINED (UNW_FLAG_CHAININFO): MSVC splits a function into several ranges and each secondary range
//     points back at its parent. The function start is found by following the chain to an entry that is not chained (at most 6 hops
//     in the file, 64 allowed here; a loop is refused).
//   * Leaf functions that allocate no stack and call nothing have NO entry at all (the CharaView accessor, the camera getter). That is
//     its own outcome, NoUnwindEntry, never confused with "a different function".
//
// Pure: reads through ImageView, no state.

#include "ImageView.h"

namespace ext::ffxiv::resolve
{
enum class FunctionStartStatus : std::uint8_t
{
    Found,         // `start` is the entry of the function that contains the address
    NoUnwindEntry, // the address is in no RUNTIME_FUNCTION range (a leaf function, padding, no exception directory)
    Unreadable,    // the table, an unwind block or a chain could not be read, or the chain loops: nothing may be concluded
};

struct FunctionStartResult
{
    FunctionStartStatus status = FunctionStartStatus::NoUnwindEntry;
    std::uint32_t start = 0;
};

namespace detail
{
struct RuntimeFunction
{
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    std::uint32_t unwind = 0;
};

inline bool ReadRuntimeFunction(const ImageView& image, std::uint32_t rva, RuntimeFunction& out)
{
    std::uint32_t v[3] = {};
    if (!image.Read(rva, v, sizeof(v)))
        return false;
    out = { v[0], v[1], v[2] };
    return true;
}
} // namespace detail

inline FunctionStartResult FunctionStart(const ImageView& image, std::uint32_t rva)
{
    using detail::ReadRuntimeFunction;
    using detail::RuntimeFunction;
    FunctionStartResult result;
    const PeInfo& info = image.Info();
    if (!info.valid || info.exceptionRva == 0 || info.exceptionSize < 12)
        return result;
    // an exception directory that is not inside the image is unusable (and would overflow the entry address arithmetic below)
    if (static_cast<std::uint64_t>(info.exceptionRva) + info.exceptionSize > info.sizeOfImage)
    {
        result.status = FunctionStartStatus::Unreadable;
        return result;
    }

    // binary search for the last entry whose begin address is <= rva
    const std::uint32_t count = info.exceptionSize / 12;
    std::uint32_t lo = 0, hi = count; // [lo, hi)
    while (lo < hi)
    {
        const std::uint32_t mid = lo + (hi - lo) / 2;
        RuntimeFunction e;
        if (!ReadRuntimeFunction(image, info.exceptionRva + mid * 12, e))
        {
            result.status = FunctionStartStatus::Unreadable;
            return result;
        }
        if (e.begin <= rva)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return result;

    RuntimeFunction cur;
    if (!ReadRuntimeFunction(image, info.exceptionRva + (lo - 1) * 12, cur))
    {
        result.status = FunctionStartStatus::Unreadable;
        return result;
    }
    if (rva >= cur.end)
        return result; // in the gap after this function: no entry covers the address

    // follow UNW_FLAG_CHAININFO to the parent entry until an entry is not chained
    constexpr int kMaxHops = 64;
    for (int hop = 0; hop <= kMaxHops; ++hop)
    {
        if ((cur.unwind & 1) != 0) // an indirect entry: not produced by the toolchain that built the game, not supported
        {
            result.status = FunctionStartStatus::Unreadable;
            return result;
        }
        std::uint8_t header[4] = {};
        if (!image.Read(cur.unwind, header, sizeof(header)))
        {
            result.status = FunctionStartStatus::Unreadable;
            return result;
        }
        const std::uint8_t flags = static_cast<std::uint8_t>(header[0] >> 3);
        if ((flags & 4) == 0) // not chained: this entry's begin address is the function start
        {
            result.status = FunctionStartStatus::Found;
            result.start = cur.begin;
            return result;
        }
        const std::uint32_t codes = header[2];
        const std::uint32_t parentAt = cur.unwind + 4 + ((codes + 1u) & ~1u) * 2;
        RuntimeFunction parent;
        if (!ReadRuntimeFunction(image, parentAt, parent) || parent.begin >= parent.end)
        {
            result.status = FunctionStartStatus::Unreadable;
            return result;
        }
        cur = parent;
    }
    result.status = FunctionStartStatus::Unreadable; // more hops than any real function has: a loop
    return result;
}
} // namespace ext::ffxiv::resolve
