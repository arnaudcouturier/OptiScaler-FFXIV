// Byte widths of the input-layout element formats (ext/ffxiv/ui/PlateLayoutTag.h FormatBytes): the table the
// nameplate probe's layout tagging walks to resolve D3D11_APPEND_ALIGNED_ELEMENT offsets. Every format the moved
// function handles is pinned with its width: R32G32B32_FLOAT is 12 bytes and R32G32_FLOAT is in the 8-byte group.
#include "ext/ffxiv/ui/PlateLayoutTag.h"

#include <cstdio>

namespace
{
int g_failures = 0;
void CheckFormat(DXGI_FORMAT format, UINT want, const char* name)
{
    const UINT got = ext::ffxiv::ui::plate::FormatBytes(format);
    if (got != want)
    {
        ++g_failures;
        std::printf("FAIL: FormatBytes(%s) = %u, want %u\n", name, got, want);
    }
}
} // namespace

int main()
{
    CheckFormat(DXGI_FORMAT_R32G32B32A32_FLOAT, 16, "R32G32B32A32_FLOAT");
    CheckFormat(DXGI_FORMAT_R32G32B32_FLOAT, 12, "R32G32B32_FLOAT");
    CheckFormat(DXGI_FORMAT_R32G32_FLOAT, 8, "R32G32_FLOAT");
    CheckFormat(DXGI_FORMAT_R16G16B16A16_SINT, 8, "R16G16B16A16_SINT");
    CheckFormat(DXGI_FORMAT_R16G16B16A16_UINT, 8, "R16G16B16A16_UINT");
    CheckFormat(DXGI_FORMAT_R32_FLOAT, 4, "R32_FLOAT");
    CheckFormat(DXGI_FORMAT_R16G16_SINT, 4, "R16G16_SINT");
    CheckFormat(DXGI_FORMAT_R16G16_UINT, 4, "R16G16_UINT");
    CheckFormat(DXGI_FORMAT_R8G8B8A8_UNORM, 4, "R8G8B8A8_UNORM");
    CheckFormat(DXGI_FORMAT_R8G8B8A8_SINT, 4, "R8G8B8A8_SINT");
    CheckFormat(DXGI_FORMAT_R8G8B8A8_UINT, 4, "R8G8B8A8_UINT");
    CheckFormat(DXGI_FORMAT_UNKNOWN, 0, "UNKNOWN");
    if (g_failures)
    {
        std::printf("plate_layout_tag: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("PASS: input-layout element byte widths (R32G32B32_FLOAT is 12, R32G32_FLOAT is 8)");
    return 0;
}
