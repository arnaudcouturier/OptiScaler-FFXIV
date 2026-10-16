#pragma once

// Which NVIDIA NR CUDA kernel modules this fork has verified, and for what.
//
// A module is identified by the exact size of the blob handed to NvAPI_D3D12_CreateCuModule plus its SHA-256 (the
// 64-character lowercase hex string that DlssNrNative.cpp logs as "NR reuse CUDA module: bytes=..., sha256=..."), so a
// row can be pasted straight from the log. Two independent permissions hang off a row:
//
//   reuseVerified   the ViT bottleneck reuse (VitEvery=2) may drop the cc_vit_1d_* run of this module. Only set after the
//                   safety invariants in DlssNrVitReuse.h were checked on a capture of exactly this module (see
//                   docs/nr-vit-reuse-profile-runbook.md).
//   hybridVerified  the FP8+NVFP4 hybrid FFN rewrite (Precision=4) may replace the FFN kernels of this module. That path
//                   hard-codes the 72-byte parameter layout and launch geometry of one module, so a row for a new model
//                   must leave this false unless the hybrid path itself was re-verified for it.
//
// Rows are data only: the lookup knows nothing about NvAPI, BCrypt or the GPU, so tests/ext/nr/nr_module_profiles.cpp runs
// on the host. An unknown module gets neither permission.

#include <cctype>
#include <cstddef>
#include <span>
#include <string_view>

namespace DlssNrModules
{
struct Flags
{
    bool reuse = false;  // DlssNrVitReuse::RoleRegistry entries of this module count as verified
    bool hybrid = false; // the hybrid FFN rewrite targets of this module are supported
};

struct Row
{
    unsigned bytes;
    std::string_view sha256; // 64 lowercase hex characters
    bool reuseVerified;
    bool hybridVerified;
    std::string_view note;
};

// The production table. Keep one row per module, lowercase sha, and say in `note` what was verified and on which capture.
inline constexpr Row kProfiles[] = {
    { 3202680u, "3fa6f076eecfbb6e19c378f84bbb7025cd805ed4de9a2f9c9b995eb34a7f3d76", true, true,
      "original verified module: ViT reuse and hybrid FFN rewrite" },
};

inline bool EqualsIgnoreCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;

    return true;
}

// Flags of the module with this size and SHA-256 hex digest. Pure; "unavailable" or any other non-matching digest gives
// neither permission.
inline Flags Classify(std::span<const Row> rows, unsigned bytes, std::string_view sha256Hex)
{
    for (const Row& row : rows)
        if (row.bytes == bytes && EqualsIgnoreCase(row.sha256, sha256Hex))
            return { row.reuseVerified, row.hybridVerified };

    return {};
}

inline Flags Classify(unsigned bytes, std::string_view sha256Hex)
{
    return Classify(std::span<const Row>(kProfiles), bytes, sha256Hex);
}

// Table hygiene, used by the test: every digest is 64 lowercase hex characters, and no (bytes, sha) pair repeats.
inline bool IsLowerHex64(std::string_view s)
{
    if (s.size() != 64)
        return false;

    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;

    return true;
}

inline bool WellFormed(std::span<const Row> rows)
{
    for (size_t i = 0; i < rows.size(); ++i)
    {
        if (rows[i].bytes == 0 || !IsLowerHex64(rows[i].sha256))
            return false;

        for (size_t j = i + 1; j < rows.size(); ++j)
            if (rows[i].bytes == rows[j].bytes && rows[i].sha256 == rows[j].sha256)
                return false;
    }

    return true;
}
} // namespace DlssNrModules
