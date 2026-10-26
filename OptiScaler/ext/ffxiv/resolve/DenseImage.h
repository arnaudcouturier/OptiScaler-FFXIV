#pragma once
// DenseImage: the in-memory image of an executable for tests and the CLI. A zero-filled buffer of the declared SizeOfImage with the
// headers and every section copied at its RVA, exactly like the loader maps a file (min(rawSize, virtualSize) bytes per section; the
// rest, including all bss, is zero). Read returns false only outside the headers and the page-rounded sections (see PeInfo::IsMapped).
//
// This is the dense reader the matcher needs: a sparse map reader makes every scan end as "unreadable" or unique by construction.
// Header-only; nothing in the product instantiates it.

#include "ImageView.h"

#include <fstream>
#include <functional>
#include <memory>

namespace ext::ffxiv::resolve
{
class DenseImage : public ImageView
{
  public:
    struct SectionDef
    {
        const char* name;
        std::uint32_t rva;
        std::uint32_t virtualSize;
        std::uint32_t rawSize;
        std::uint32_t characteristics;
    };

    // Common section flag sets.
    static constexpr std::uint32_t kText = kScnCode | kScnExecute | kScnRead;
    static constexpr std::uint32_t kRdata = kScnInitData | kScnRead;
    static constexpr std::uint32_t kData = kScnInitData | kScnRead | kScnWrite;

    // Maps a PE file (the whole file in memory). nullptr + *error when the headers are not a valid x64 PE or a section's raw data lies
    // outside the file.
    static std::unique_ptr<DenseImage> FromFileBytes(const std::vector<std::uint8_t>& file, std::string* error = nullptr)
    {
        auto img = std::unique_ptr<DenseImage>(new DenseImage());
        const std::size_t headerBytes = std::min<std::size_t>(file.size(), 0x4000);
        if (!ParsePe(file.data(), headerBytes, img->_info))
        {
            if (error != nullptr)
                *error = img->_info.error;
            return nullptr;
        }
        img->_bytes.assign(img->_info.sizeOfImage, 0);
        const std::size_t hdr = std::min<std::size_t>({ file.size(), img->_info.sizeOfHeaders, img->_bytes.size() });
        std::memcpy(img->_bytes.data(), file.data(), hdr);

        for (const ImageSection& s : img->_info.sections)
        {
            std::uint32_t ptr = 0;
            if (!FilePointerOf(file, s, ptr))
                continue;
            const std::uint64_t want = std::min(s.rawSize, s.virtualSize);
            if (want == 0)
                continue;
            if (ptr > file.size() || file.size() - ptr < want)
            {
                if (error != nullptr)
                    *error = "a section's raw data lies outside the file";
                return nullptr;
            }
            std::memcpy(img->_bytes.data() + s.rva, file.data() + ptr, static_cast<std::size_t>(want));
        }
        return img;
    }

    static std::unique_ptr<DenseImage> FromFile(const std::string& path, std::string* error = nullptr)
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in)
        {
            if (error != nullptr)
                *error = "cannot open " + path;
            return nullptr;
        }
        const std::streamsize size = in.tellg();
        if (size <= 0 || size > static_cast<std::streamsize>(kMaxImageBytes))
        {
            if (error != nullptr)
                *error = "bad file size";
            return nullptr;
        }
        std::vector<std::uint8_t> file(static_cast<std::size_t>(size));
        in.seekg(0);
        if (!in.read(reinterpret_cast<char*>(file.data()), size))
        {
            if (error != nullptr)
                *error = "cannot read " + path;
            return nullptr;
        }
        return FromFileBytes(file, error);
    }

    // A synthetic image with valid PE headers, the given sections and an optional exception directory. Section contents start as zero:
    // fill them with Put. Returns nullptr when the headers do not validate (overlapping or out-of-range sections).
    static std::unique_ptr<DenseImage> Synthetic(std::uint32_t timeDateStamp, std::uint32_t sizeOfImage, const std::vector<SectionDef>& defs,
                                                 std::uint32_t exceptionRva = 0, std::uint32_t exceptionSize = 0)
    {
        std::vector<std::uint8_t> h(0x400, 0);
        auto put = [&h](std::size_t at, auto v) { std::memcpy(h.data() + at, &v, sizeof(v)); };
        put(0, std::uint16_t(0x5a4d));
        put(0x3c, std::int32_t(0x80));
        put(0x80, std::uint32_t(0x4550));
        put(0x84, std::uint16_t(0x8664));
        put(0x86, std::uint16_t(defs.size()));
        put(0x88, timeDateStamp);
        put(0x94, std::uint16_t(0xf0)); // SizeOfOptionalHeader
        const std::size_t opt = 0x98;
        put(opt, std::uint16_t(0x20b));
        put(opt + 24, std::uint64_t(0x140000000ull));
        put(opt + 56, sizeOfImage);
        put(opt + 60, std::uint32_t(0x400));
        put(opt + 112 + 3 * 8, exceptionRva);
        put(opt + 112 + 3 * 8 + 4, exceptionSize);
        std::size_t at = opt + 0xf0;
        std::uint32_t fileCursor = 0x400;
        for (const SectionDef& d : defs)
        {
            if (at + 40 > h.size())
                return nullptr;
            std::memcpy(h.data() + at, d.name, std::min<std::size_t>(std::strlen(d.name), 8));
            put(at + 8, d.virtualSize);
            put(at + 12, d.rva);
            put(at + 16, d.rawSize);
            put(at + 20, fileCursor); // pointer to raw data: only meaningful to FromFileBytes
            put(at + 36, d.characteristics);
            fileCursor += d.rawSize;
            at += 40;
        }
        auto img = std::unique_ptr<DenseImage>(new DenseImage());
        if (!ParsePe(h.data(), h.size(), img->_info))
            return nullptr;
        img->_bytes.assign(img->_info.sizeOfImage, 0);
        std::memcpy(img->_bytes.data(), h.data(), h.size());
        return img;
    }

    bool Read(std::uint32_t rva, void* out, std::size_t bytes) const override
    {
        if (readObserver)
            readObserver(rva, bytes);
        if (bytes == 0 || !_info.valid || !_info.IsMapped(rva, bytes))
            return false;
        std::memcpy(out, _bytes.data() + rva, bytes);
        return true;
    }

    // Test setup: write bytes into the mapped image (false when the target is not mapped). Not part of the read interface.
    bool Put(std::uint32_t rva, const void* data, std::size_t bytes)
    {
        if (bytes == 0 || !_info.IsMapped(rva, bytes))
            return false;
        std::memcpy(_bytes.data() + rva, data, bytes);
        return true;
    }
    bool Put(std::uint32_t rva, const std::vector<std::uint8_t>& bytes) { return Put(rva, bytes.data(), bytes.size()); }

    // Called for every Read (before it is validated): tests use it to prove that no read leaves a section.
    std::function<void(std::uint32_t, std::size_t)> readObserver {};

    std::vector<std::uint8_t>& Bytes() { return _bytes; }

  private:
    DenseImage() = default;

    // PointerToRawData of a section: parsed from the header copy (ParsePe does not keep it).
    static bool FilePointerOf(const std::vector<std::uint8_t>& file, const ImageSection& s, std::uint32_t& ptr)
    {
        std::int32_t lfanew = 0;
        std::uint16_t count = 0, optSize = 0;
        if (!detail::Pull(file.data(), file.size(), 0x3c, lfanew) || lfanew <= 0)
            return false;
        const std::size_t nt = static_cast<std::size_t>(lfanew);
        if (!detail::Pull(file.data(), file.size(), nt + 6, count) || !detail::Pull(file.data(), file.size(), nt + 20, optSize))
            return false;
        const std::size_t table = nt + 24 + optSize;
        for (std::size_t i = 0; i < count; ++i)
        {
            std::uint32_t rva = 0, raw = 0;
            if (!detail::Pull(file.data(), file.size(), table + i * 40 + 12, rva) || !detail::Pull(file.data(), file.size(), table + i * 40 + 20, raw))
                return false;
            if (rva == s.rva)
            {
                ptr = raw;
                return true;
            }
        }
        return false;
    }

    std::vector<std::uint8_t> _bytes;
};
} // namespace ext::ffxiv::resolve
