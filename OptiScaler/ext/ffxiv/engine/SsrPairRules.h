#pragma once

#include <cstdint>
#include <string>

namespace ext::ffxiv::engine::ssr::pair
{
inline constexpr uint64_t kByteBudget = 512ull * 1024 * 1024;
inline constexpr uint64_t kPresentTimeout = 1200;
inline constexpr char kManifestHeader[] = "file,frameToken,moduleRun,width,height,dxgiFormat,rowBytes\n";

inline constexpr bool Fits(uint64_t held, uint64_t addition) noexcept
{
    return addition != 0 && addition <= kByteBudget && held <= kByteBudget - addition;
}

inline constexpr bool TimedOut(uint64_t start, uint64_t now) noexcept
{
    return now > start && now - start > kPresentTimeout;
}

inline constexpr bool IsPairSlot(uint32_t slot) noexcept { return slot < 2; }

inline std::string FileName(uint32_t slot, const std::string& name)
{
    return "frame" + std::to_string(slot + 1) + "_" + name;
}
} // namespace ext::ffxiv::engine::ssr::pair
