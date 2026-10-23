// MANUAL: runs the preset guards (ext/fg/dlssg/PresetGuards.h) on a real nvngx_dlssg.dll read from disk and mapped by its
// section table. Usage: set OPTISCALER_DLSSG_DLL to the DLL and run the built exe (the recipe says how). Prints every located RVA
// and the file's SHA-256. For the two DLLs this was developed on it also asserts the RVAs found with a decompiler (310.9.1.0
// and 310.8.0); for any other DLL it passes when the guards locate everything and fails with the reason otherwise, so it is the
// check to run on a new runtime before trusting the live switch on it.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "fg_preset_harness.h"

#include "ext/fg/dlssg/PresetGuards.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace ext::fg::preset::guards;

namespace
{
std::string Sha256Hex(const std::vector<uint8_t>& bytes)
{
    uint8_t digest[32] {};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), digest, 32) != 0)
        return {};
    char out[65] {};
    for (int i = 0; i < 32; ++i)
        std::snprintf(out + i * 2, 3, "%02x", digest[i]);
    return out;
}

struct Expect
{
    const char* sha;
    const char* what;
    uint32_t reader, readFn, create, defaults, copy, site0, site1, global;
};
// RVAs read with a decompiler (REA, Ghidra 12.1.4) for 310.9.1.0 and the Streamline 2.14.16 build of 310.9.16; the structural scan for 310.8.0 and the model-store build.
const Expect kExpect[] = {
    { "cadee1aa6db3907babdc3285da2aebc4553d92b89c38ba3eae8abd8bba1bea72", "310.9.16 model-store build (scanner; reader, create and copy equal the addresses in the in-game log)", 0x14000, 0x344F0, 0x36330, 0x34BD0, 0x35BE0, 0x36A10, 0x36A95, 0x66AA20 },
    { "8d59d49b36410dd6b62fef2503a5871d347415bd85d943fa0848563a95dd50c7", "310.9.16 (Streamline 2.14.16 SDK)", 0x13FF0, 0x344D0, 0x36310, 0x34BB0, 0x35BC0, 0x369F0, 0x36A75, 0x66AA20 },
    { "5d5cbf14d2727d47f93fd10bf77bd91708ae122482a6f86fd564971641ebd47b", "310.8.0 (structural scan, cross-checked with a second implementation)", 0x13E80, 0x33E80, 0x35B70, 0x34530, 0x35460, 0x3624E, 0x362D5, 0x6679E0 },
    { "ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82", "310.9.1.0", 0x13EF0, 0x340E0, 0x35DD0, 0x34790, 0x356C0, 0x364AE, 0x36535, 0x6689E0 },
};
// The built-in Ada unlock (ext/fg/dlssg/MfgUnlock.cpp) rewrites a few bytes of this DLL in memory before anything here runs.
// Replays its two 310.9 byte patches on the mapped image and returns how many landed (0 on a DLL of another shape).
int ApplyAdaUnlockPatches(std::vector<uint8_t>& image, std::vector<std::pair<size_t, size_t>>& written)
{
    int applied = 0;
    auto find = [&](const std::vector<int>& pattern) -> size_t
    {
        size_t found = SIZE_MAX;
        for (size_t i = 0; i + pattern.size() <= image.size(); ++i)
        {
            bool ok = true;
            for (size_t k = 0; k < pattern.size() && ok; ++k)
                ok = pattern[k] < 0 || image[i + k] == static_cast<uint8_t>(pattern[k]);
            if (ok)
            {
                if (found != SIZE_MAX)
                    return SIZE_MAX - 1; // not unique
                found = i;
            }
        }
        return found;
    };
    // advertise: cmp ebp,1B0h / jl rel32 / mov edi,5  -> the jl (6 bytes) becomes a nop
    if (const size_t at = find({ 0x81, 0xFD, 0xB0, 0x01, 0x00, 0x00, 0x0F, 0x8C, -1, -1, -1, -1, 0xBF, 0x05, 0x00, 0x00, 0x00 }); at < SIZE_MAX - 1)
    {
        const uint8_t nop[] = { 0x0F, 0x1F, 0x44, 0x00, 0x00, 0x90 };
        std::memcpy(&image[at + 6], nop, 6);
        written.push_back({ at + 6, 6 });
        ++applied;
    }
    // validate: cmp eax,1B0h / setae al -> mov al,1 / nop
    if (const size_t at = find({ 0x3D, 0xB0, 0x01, 0x00, 0x00, 0x0F, 0x93, 0xC0 }); at < SIZE_MAX - 1)
    {
        const uint8_t always[] = { 0xB0, 0x01, 0x90 };
        std::memcpy(&image[at + 5], always, 3);
        written.push_back({ at + 5, 3 });
        ++applied;
    }
    return applied;
}
} // namespace

// The render-preset table of the runtime (EndpointConfiguration::ApplyRenderPreset = Located::readFn). The stores are 16-bit engaged-optional
// values into the endpoint configuration: `mov word [reg+0x54], 0x100/0x101` is the UI recomposition override (off/on), `mov word [reg+0x68],
// 0x100/0x101` the FG Lite override (off/on). Read in nvngx_dlssg.dll 310.9.16 (sha 8d59d49b, model-store build cadee1aa):
//   Preset C: +0x54 = 0x100 directly followed by +0x68 = 0x101      (recomposition OFF, FG Lite ON)
//   Preset A: +0x54 = 0x100                                          (recomposition OFF)         Preset B: +0x54 = 0x101 (recomposition ON)
//   common to A and B: +0x68 = 0x100 (FG Lite OFF)                   private-flag overrides: +0x54 = 0x100 and +0x68 = 0x100 once each
// so the function holds three +0x54 = 0x100, one +0x54 = 0x101, two +0x68 = 0x100 and one +0x68 = 0x101 stores. The recomposition rule
// (ext/fg/dlssg/UiRecompositionRule.h: A and C drop it, Auto and B keep it) rests on this table. 310.9.1.0 and 310.8.0 have no Preset C and no
// +0x68 stores at all: one +0x54 = 0x100 store (A). Any other build must show one of the two shapes, else the recipe fails: a runtime that
// changed the table would otherwise be paired with the wrong rule silently.
struct StoreScan
{
    int ui0 = 0, ui1 = 0, lite0 = 0, lite1 = 0; // +0x54 = 0x100, +0x54 = 0x101, +0x68 = 0x100, +0x68 = 0x101
    int cPairs = 0;                            // a +0x54 = 0x100 store immediately followed by a +0x68 = 0x101 store
};

StoreScan ScanPresetStores(const std::vector<uint8_t>& image, uint32_t from, uint32_t to)
{
    StoreScan scan;
    int previous = 0; // kind of the previous store: 1 = ui0
    uint32_t previousEnd = 0;
    for (uint32_t at = from; at + 7 <= to && at + 9 <= image.size(); ++at)
    {
        uint32_t p = at;
        if (image[p++] != 0x66)
            continue;
        if ((image[p] & 0xF0) == 0x40) // REX
            ++p;
        if (image[p++] != 0xC7)
            continue;
        const uint8_t modrm = image[p++];
        if ((modrm & 0xF8) != 0x40 || (modrm & 7) == 4) // [reg+disp8], no SIB
            continue;
        const uint8_t disp = image[p++];
        const uint8_t lo = image[p++], hi = image[p++];
        if ((disp != 0x54 && disp != 0x68) || hi != 0x01 || lo > 1)
            continue;
        const int kind = disp == 0x54 ? (lo ? 2 : 1) : (lo ? 4 : 3);
        if (kind == 1)
            ++scan.ui0;
        else if (kind == 2)
            ++scan.ui1;
        else if (kind == 3)
            ++scan.lite0;
        else
            ++scan.lite1;
        if (kind == 4 && previous == 1 && previousEnd == at)
            ++scan.cPairs;
        previous = kind;
        previousEnd = p;
        at = p - 1;
    }
    return scan;
}

int main()
{
    const char* path = std::getenv("OPTISCALER_DLSSG_DLL");
    if (path == nullptr || *path == 0)
    {
        std::puts("fg_preset_guards_dll: set OPTISCALER_DLSSG_DLL to the path of an nvngx_dlssg.dll");
        return 2;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        std::printf("cannot open %s\n", path);
        return 2;
    }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (file.size() < 0x400 || file[0] != 'M' || file[1] != 'Z')
    {
        std::puts("not a PE file");
        return 2;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    {
        std::puts("not a 64-bit PE file");
        return 2;
    }
    std::vector<uint8_t> image(nt->OptionalHeader.SizeOfImage, 0);
    std::memcpy(image.data(), file.data(), std::min<size_t>(nt->OptionalHeader.SizeOfHeaders, file.size()));
    std::vector<Section> sections;
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        const uint32_t raw = sec[i].SizeOfRawData, vsz = sec[i].Misc.VirtualSize;
        if (sec[i].PointerToRawData + static_cast<size_t>(raw) <= file.size() && sec[i].VirtualAddress + static_cast<size_t>(std::min(raw, vsz)) <= image.size())
            std::memcpy(image.data() + sec[i].VirtualAddress, file.data() + sec[i].PointerToRawData, std::min(raw, vsz));
        sections.push_back({ sec[i].VirtualAddress, vsz, (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 });
    }
    const auto& exc = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    const ImageView view { image.data(), image.size(), sections.data(), sections.size(), exc.VirtualAddress, exc.Size };

    const std::string sha = Sha256Hex(file);
    const Located l = Locate(view);
    std::printf("file %s\nsha256 %s (%s)\n", path, sha.c_str(), KnownHashLabel(sha) ? KnownHashLabel(sha) : "not in the known list");
    std::printf("guards: %s\n", WhyText(l.why));
    if (l.why == Why::Ok)
    {
        std::printf("reader %X readFn %X create %X defaults %X copy %X sites %X %X global %X\n", l.reader, l.readFn, l.create, l.defaults,
                    l.copy, l.copySites[0], l.copySites[1], l.sharedGlobal);
        if (l.ctxInSlot)
            std::printf("context argument: kept at create+%X (RVA %X) in the frame slot [rsp+%X], loaded back at RVA %X; %u instructions in between checked, %u decoded in create\n",
                        l.ctxSaveAt - l.create, l.ctxSaveAt, static_cast<unsigned>(l.ctxSlot), l.ctxReloadAt, l.ctxChecked, l.createInsns);
        else
            std::printf("context argument: kept at create+%X (RVA %X) in register %u, loaded back at RVA %X; %u instructions in between checked, %u decoded in create\n",
                        l.ctxSaveAt - l.create, l.ctxSaveAt, static_cast<unsigned>(l.ctxReg), l.ctxReloadAt, l.ctxChecked, l.createInsns);
    }
    else if (l.failAt != 0)
        std::printf("the check stopped at RVA %X\n", l.failAt);
    CHECK(l.why == Why::Ok);

    for (const auto& e : kExpect)
        if (sha == e.sha)
        {
            std::printf("expecting the RVAs found by hand for %s\n", e.what);
            CHECK(l.reader == e.reader && l.readFn == e.readFn && l.create == e.create);
            CHECK(l.defaults == e.defaults && l.copy == e.copy);
            CHECK(l.copySites[0] == e.site0 && l.copySites[1] == e.site1 && l.sharedGlobal == e.global);
        }
    // The preset table behind the UI recomposition rule (see ScanPresetStores).
    if (l.why == Why::Ok && l.defaults > l.readFn)
    {
        const StoreScan scan = ScanPresetStores(image, l.readFn, l.defaults);
        const bool full = scan.ui0 == 3 && scan.ui1 == 1 && scan.lite0 == 2 && scan.lite1 == 1 && scan.cPairs == 1;
        const bool aOnly = scan.ui0 == 1 && scan.ui1 == 0 && scan.lite0 == 0 && scan.lite1 == 0 && scan.cPairs == 0;
        std::printf("preset table check (readFn %X..%X): +0x54=0x100 x%d, +0x54=0x101 x%d, +0x68=0x100 x%d, +0x68=0x101 x%d, C pair (recomposition off + FG Lite on) x%d -> %s\n",
                    l.readFn, l.defaults, scan.ui0, scan.ui1, scan.lite0, scan.lite1, scan.cPairs,
                    full ? "the 310.9.16 table: A and C turn the recomposition off, B turns it on"
                         : aOnly ? "the older shape: Preset A only, no Preset C" : "UNKNOWN TABLE: review the recomposition rule before trusting this build");
        CHECK(full || aOnly);
        for (const auto& e : kExpect)
            if (sha == e.sha && std::strstr(e.what, "310.9.16") != nullptr)
                CHECK(full);
    }
    // Coexistence with the Ada unlock: its patches must not disturb any guarded byte, so the same RVAs come out afterwards.
    if (l.why == Why::Ok)
    {
        std::vector<uint8_t> patched = image;
        std::vector<std::pair<size_t, size_t>> written;
        const int landed = ApplyAdaUnlockPatches(patched, written);
        const ImageView after { patched.data(), patched.size(), sections.data(), sections.size(), exc.VirtualAddress, exc.Size };
        const Located m = Locate(after);
        std::printf("with the Ada unlock patches applied (%d of 2 landed): guards %s\n", landed, WhyText(m.why));
        // The hooks overwrite the first 16 bytes of the reader, create and copy functions (Detours): none of the unlock's bytes may be there.
        for (const auto& [at, len] : written)
        {
            bool clear = true;
            for (const uint32_t entry : { l.reader, l.create, l.copy })
                clear = clear && !(at < static_cast<size_t>(entry) + 16 && static_cast<size_t>(entry) < at + len);
            std::printf("Ada unlock patch writes %zu bytes at RVA %zX: %s\n", len, at, clear ? "clear of the hook entry bytes (reader, create, copy)" : "OVERLAPS A HOOK ENTRY");
            CHECK(clear);
        }
        CHECK(m.why == Why::Ok);
        CHECK(m.reader == l.reader && m.readFn == l.readFn && m.create == l.create && m.defaults == l.defaults && m.copy == l.copy);
        CHECK(m.copySites[0] == l.copySites[0] && m.copySites[1] == l.copySites[1] && m.sharedGlobal == l.sharedGlobal);
    }
    if (g_failures)
    {
        std::printf("fg_preset_guards_dll: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("fg_preset_guards_dll: passed");
    return 0;
}
