#include "pch.h"

#include "ext/ffxiv/relight/Inject.h"

#include "ext/ffxiv/relight/shaders/CameraRegisters.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstring>
#include <ctime>
#include <format>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

// Contact shadows: the one-frame dump of the developer check ([FFXIV] ContactShadowsDump, ini only, off by default). At the first sun light
// draw that runs after the delay it writes, from that one frame and nothing else: the engine's sun mask before the pass, the half-resolution
// term, the mask copy the draw sampled, the draw's depth, the camera matrices and the sun vector - and a text file with the sizes and the
// keys. All of them came out of the same GPU frame, so a comparison of them has no run-to-run noise, which is the thing two benchmark runs
// could not give (docs/notes/contact-shadows.md says why).
//
// Nothing here runs unless the key is set and the pass has run: the check is one integer comparison per light draw, and the readbacks are
// staging copies on the frame the key asks for, once per session. Files go to OptiScaler\contact-dump\<date-time>\ beside the DLL, never
// into the game's own folders.
namespace ext::ffxiv::relight
{
namespace
{
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

// The folder the DLL lives in, which is where the fork writes everything it reports.
fs::path OptiScalerDir()
{
    wchar_t path[32768] {};
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&OptiScalerDir), &module) ||
        !GetModuleFileNameW(module, path, 32768))
        return {};
    return fs::path(path).parent_path() / "OptiScaler";
}

fs::path MakeDumpFolder()
{
    const fs::path base = OptiScalerDir();
    if (base.empty())
        return {};
    std::tm local {};
    const std::time_t now = std::time(nullptr);
    if (localtime_s(&local, &now) != 0)
        return {};
    char id[32] {};
    if (std::strftime(id, sizeof id, "%Y%m%d-%H%M%S", &local) == 0)
        return {};
    const fs::path dir = base / kDumpFolder / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec || !fs::is_directory(dir, ec))
        return {};
    return dir;
}

bool WriteBytes(const fs::path& path, const void* data, size_t bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    file.write(static_cast<const char*>(data), std::streamsize(bytes));
    file.flush();
    return file.good();
}

bool WriteText(const fs::path& path, const std::string& text) { return WriteBytes(path, text.data(), text.size()); }

// The bytes per pixel of the formats a dump writes, from the format the resource has.
uint32_t PixelBytes(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UINT: return 1;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    // The depth buffer of the shipped build is a typeless 24-bit depth in a 32-bit surface (measured: format 44).
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_D32_FLOAT: return 4;
    default: return 0;
    }
}

// The pixels of a bound texture, tightly packed, through a staging copy the resource's own description builds: a typeless depth surface keeps
// its typeless format in the copy, so the raw bytes arrive without a format conversion.
bool ReadTexture(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11ShaderResourceView* view, const fs::path& path,
                 std::string& note)
{
    if (view == nullptr)
    {
        note += " (not bound)";
        return false;
    }
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (resource)
        resource.As(&texture);
    if (!texture)
    {
        note += " (not a 2D texture)";
        return false;
    }
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    const uint32_t bytesPerPixel = PixelBytes(desc.Format);
    if (bytesPerPixel == 0)
    {
        note += " (format " + std::to_string(uint32_t(desc.Format)) + " not written)";
        return false;
    }
    D3D11_TEXTURE2D_DESC staging = desc;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> copy;
    if (FAILED(device->CreateTexture2D(&staging, nullptr, &copy)) || !copy)
    {
        note += " (the staging copy could not be created)";
        return false;
    }
    context->CopyResource(copy.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE map {};
    if (FAILED(context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &map)) || map.pData == nullptr)
    {
        note += " (the staging copy could not be mapped)";
        return false;
    }
    std::vector<unsigned char> pixels(size_t(desc.Width) * desc.Height * bytesPerPixel);
    const auto* rows = static_cast<const unsigned char*>(map.pData);
    for (uint32_t y = 0; y < desc.Height; ++y)
        std::memcpy(pixels.data() + size_t(y) * desc.Width * bytesPerPixel, rows + size_t(y) * map.RowPitch,
                    size_t(desc.Width) * bytesPerPixel);
    context->Unmap(copy.Get(), 0);
    if (!WriteBytes(path, pixels.data(), pixels.size()))
    {
        note += " (the write failed)";
        return false;
    }
    note += std::format(" {}x{} format {} ({} bytes)", desc.Width, desc.Height, uint32_t(desc.Format), pixels.size());
    return true;
}

// A byte range of a constant buffer, the way the pass copies it.
bool ReadBuffer(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Buffer* buffer, uint32_t offset, uint32_t bytes,
                const fs::path& path, std::string& note)
{
    if (buffer == nullptr)
    {
        note += " (not bound)";
        return false;
    }
    D3D11_BUFFER_DESC desc {};
    buffer->GetDesc(&desc);
    if (desc.ByteWidth < offset + bytes)
    {
        note += " (too small)";
        return false;
    }
    D3D11_BUFFER_DESC staging = desc;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;
    ComPtr<ID3D11Buffer> copy;
    if (FAILED(device->CreateBuffer(&staging, nullptr, &copy)) || !copy)
    {
        note += " (the staging copy could not be created)";
        return false;
    }
    D3D11_BOX box {};
    box.left = offset;
    box.right = offset + bytes;
    box.bottom = 1;
    box.back = 1;
    context->CopySubresourceRegion(copy.Get(), 0, 0, 0, 0, buffer, 0, &box);
    D3D11_MAPPED_SUBRESOURCE map {};
    if (FAILED(context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &map)) || map.pData == nullptr)
    {
        note += " (the staging copy could not be mapped)";
        return false;
    }
    const bool written = WriteBytes(path, map.pData, bytes);
    context->Unmap(copy.Get(), 0);
    if (!written)
    {
        note += " (the write failed)";
        return false;
    }
    note += std::format(" {} bytes", bytes);
    return true;
}
} // namespace

namespace
{
// The bytecode of the mask-declaring shaders, kept from creation while the one-frame dump is armed (ContactShadowsRules.h's kMaxKeptShaders): one
// copy per CRC, bounded in count and in bytes. Shader creation runs on any thread and the dump reads on the render thread, so both take this
// mutex; the storage lives here, with the only reader, so Inject.h - which the draw hook includes - carries no lock.
struct KeptShader
{
    uint32_t crc = 0;
    bool alternateName = false; // the shader declares `g_SamplerShadowMask`, this build's light draw, not `g_SamplerShadow`
    Layout layout;
    std::vector<uint8_t> code;
};

struct Kept
{
    std::mutex mutex;
    std::vector<KeptShader> shaders;
    size_t bytes = 0;
};

Kept& K()
{
    static Kept* kept = new Kept; // never destroyed: no destructor may run under the loader lock
    return *kept;
}
} // namespace

void KeepShaderBytecode(uint32_t crc, const void* code, size_t size, const Layout& layout)
{
    if (code == nullptr || size == 0 || size > kMaxKeptShaderBytes || crc == 0)
        return;
    Kept& kept = K();
    std::lock_guard lock(kept.mutex);
    if (kept.shaders.size() >= kMaxKeptShaders || kept.bytes + size > kMaxKeptBytes)
        return;
    for (const KeptShader& shader : kept.shaders)
        if (shader.crc == crc)
            return;
    KeptShader shader;
    shader.crc = crc;
    shader.layout = layout;
    shader.alternateName = MentionsName(code, size, kMaskSamplerAlternate);
    const auto* bytes = static_cast<const uint8_t*>(code);
    shader.code.assign(bytes, bytes + size);
    kept.bytes += size;
    kept.shaders.push_back(std::move(shader));
}

void ForgetKeptShaders()
{
    Kept& kept = K();
    std::lock_guard lock(kept.mutex);
    kept.shaders.clear();
    kept.bytes = 0;
}

namespace
{
// The copy the dump writes: the list under the same lock, so a shader creation on another thread cannot grow the vector under the loop.
std::vector<KeptShader> KeptShaderCode()
{
    Kept& kept = K();
    std::lock_guard lock(kept.mutex);
    return kept.shaders;
}
} // namespace

void DumpFrame(ID3D11DeviceContext* context, State& state, const Inputs& inputs, const Pass& pass)
{
    if (context == nullptr || pass.CopyView() == nullptr || pass.TermView() == nullptr)
        return;
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device)
        return;
    const fs::path dir = MakeDumpFolder();
    if (dir.empty())
    {
        LOG_WARN("Contact shadows: the dump folder could not be created; the frame was not written");
        return;
    }

    std::string maskBefore, term, maskAfter, depth, camera, sun;
    // The mask's files carry its own format, and their names say which one: an R8_UNORM mask is one byte per texel, a B8G8R8A8_UNORM mask four,
    // and the mask's meaning (which level is sunlit) is what this dump exists to settle (ContactShadowsRules.h).
    const bool maskR8 = IsMaskR8(inputs.maskFormat);
    const bool okMaskBefore = ReadTexture(device.Get(), context, inputs.mask, dir / MaskDumpFile(maskR8, /*before*/ true), maskBefore);
    const bool okTerm = ReadTexture(device.Get(), context, pass.TermView(), dir / kDumpTermFile, term);
    const bool okMaskAfter = ReadTexture(device.Get(), context, pass.CopyView(), dir / MaskDumpFile(maskR8, /*before*/ false), maskAfter);
    const bool okDepth = ReadTexture(device.Get(), context, inputs.depth, dir / kDumpDepthFile, depth);
    const bool okCamera = ReadBuffer(device.Get(), context, inputs.cameraParam, 0, CS_CAMERA_CHECK_BYTES, dir / kDumpCameraFile, camera);
    const bool okSun = ReadBuffer(device.Get(), context, inputs.lightParam, 0x10, 16, dir / kDumpSunFile, sun);
    (void)okSun;

    // The mask-declaring shaders' bytecode, kept from creation (KeptShaderCode): the light shader is created while the game loads, so this is
    // the only place its bytecode can come from, and which channel of the mask it reads is the other half of the mask's meaning.
    std::string shaders;
    for (const KeptShader& kept : KeptShaderCode())
    {
        const std::string name = std::format("shader-{:08X}.dxbc", kept.crc);
        const bool written = WriteBytes(dir / name, kept.code.data(), kept.code.size());
        shaders += std::format("\n  {} ({} bytes{}) declaration {}, mask slot {}, depth slot {}, g_LightParam slot {}, g_CameraParameter "
                               "slot {}, {} of its four names",
                               name, kept.code.size(), written ? "" : ", the write failed",
                               kept.alternateName ? "g_SamplerShadowMask" : "g_SamplerShadow", kept.layout.maskSlot,
                               kept.layout.depthSlot, kept.layout.lightParamSlot, kept.layout.cameraParamSlot,
                               uint32_t((kept.layout.found & 1u) != 0) + uint32_t((kept.layout.found & 2u) != 0) +
                                   uint32_t((kept.layout.found & 4u) != 0) + uint32_t((kept.layout.found & 8u) != 0));
    }
    if (shaders.empty())
        shaders = " none: no shader declared a mask name while the dump was armed";

    std::string facts = std::format(
        "contact shadows: one frame, at the sun light draw that ran after {} s\n"
        "mask (the engine's, before the pass):{}\n"
        "term (half resolution):{}\n"
        "mask copy (what the draw sampled):{}\n"
        "depth:{}\n"
        "camera (g_CameraParameter +0x000..0x{:03X}):{}\n"
        "sun (g_LightParam +0x10):{}\n"
        "window: origin ({:.2f}, {:.2f}) extent ({:.2f}, {:.2f}), mask {}x{} format {}\n"
        "parameters: length {:.2f} m, strength {:.2f}, {} step(s), sun in {} space, sign {}\n"
        "sun vector at the first light draw: ({:.4f}, {:.4f}, {:.4f})\n"
        "the mask's meaning is not measured for this build: which level is sunlit is the benchmark build's convention (1 = sunlit, 0 = fully\n"
        "shadowed), and mask-before above carries the engine's own texels, {} byte(s) per texel, so the convention and the channel the light draw\n"
        "reads can be read from this folder - the shaders below are the ones that declare a mask name, kept from creation\n"
        "shaders kept from creation:{}\n",
        state.dumpDelaySeconds, maskBefore, term, maskAfter, depth, CS_CAMERA_CHECK_BYTES, camera, sun, double(inputs.window.x),
        double(inputs.window.y), double(inputs.window.width), double(inputs.window.height), inputs.maskWidth, inputs.maskHeight,
        inputs.maskFormat, double(inputs.params.length), double(inputs.params.strength), inputs.params.steps,
        inputs.params.space == SunSpace::View ? "view" : "world", inputs.params.sign == SunSign::Toward ? "toward the sun" : "the way the light travels",
        double(state.lastSun[0]), double(state.lastSun[1]), double(state.lastSun[2]), PixelBytes(DXGI_FORMAT(inputs.maskFormat)), shaders);
    if (!okMaskBefore || !okTerm || !okMaskAfter || !okDepth || !okCamera)
        facts += "one or more files above carry a note in brackets: they were not written\n";
    if (WriteText(dir / kDumpFactsFile, facts))
        LOG_INFO("Contact shadows: dump written to {} (mask before, term, mask copy, depth, camera, sun, the mask shaders' bytecode, facts.txt)",
                 dir.string());
    else
        LOG_WARN("Contact shadows: the dump's facts.txt could not be written");
    state.dumpWritten = true;
}
} // namespace ext::ffxiv::relight
