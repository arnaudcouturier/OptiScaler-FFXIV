#pragma once
// LiveImage: the running process's own executable image, read through ext::core::ReadProcessBytes (a safe ReadProcessMemory on our own
// process: an unmapped or protected page returns false instead of faulting). The same ImageView interface the offline DenseImage
// implements, so the resolver is one piece of code in the game and in the harness.
//
// Windows only. The product creates one once, from the startup at the first device hook (ResolveStartup.cpp).

#include "ImageView.h"
#include "../../core/ProcessMemory.h"

#include <memory>

namespace ext::ffxiv::resolve
{
class LiveImage : public ImageView
{
  public:
    // The module that created the process (the game executable). nullptr when its headers do not validate.
    static std::unique_ptr<LiveImage> OpenMainModule() { return Open(reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr))); }

    // The image mapped at `base`.
    static std::unique_ptr<LiveImage> Open(std::uintptr_t base)
    {
        if (base == 0)
            return nullptr;
        auto img = std::unique_ptr<LiveImage>(new LiveImage(base));
        std::vector<std::uint8_t> headers(0x1000);
        if (!ext::core::ReadProcessBytes(base, headers.data(), headers.size()) || !ParsePe(headers.data(), headers.size(), img->_info))
            return nullptr;
        return img;
    }

    bool Read(std::uint32_t rva, void* out, std::size_t bytes) const override
    {
        if (bytes == 0 || !_info.valid || !_info.IsMapped(rva, bytes))
            return false;
        return ext::core::ReadProcessBytes(_base + rva, out, bytes);
    }

    std::uintptr_t Base() const { return _base; }

  private:
    explicit LiveImage(std::uintptr_t base) : _base(base) {}
    std::uintptr_t _base;
};
} // namespace ext::ffxiv::resolve
