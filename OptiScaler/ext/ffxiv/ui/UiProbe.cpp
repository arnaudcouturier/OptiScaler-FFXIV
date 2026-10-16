#include "pch.h"

#include "ext/ffxiv/ui/UiProbe.h"

#include "ext/core/ProcessMemory.h"

#include <psapi.h>

#include <algorithm>
#include <cstring>
#include <cstdio>

namespace ext::ffxiv::ui::probe
{
namespace
{
std::string Narrow(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string Lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string Hex(uintptr_t v)
{
    char b[32];
    snprintf(b, sizeof b, "0x%llX", static_cast<unsigned long long>(v));
    return b;
}

HMODULE OwnModule()
{
    static const HMODULE own = []
    {
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&OwnModule), &m);
        return m;
    }();
    return own;
}

struct Owner
{
    HMODULE module = nullptr;
    std::string path;
    uintptr_t offset = 0;
};

bool OwnerOf(uintptr_t address, Owner& out)
{
    HMODULE m = nullptr;
    if (address == 0 || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                            reinterpret_cast<LPCWSTR>(address), &m) || !m)
        return false;
    wchar_t path[2048] {};
    if (!GetModuleFileNameW(m, path, 2048))
        return false;
    out.module = m;
    out.path = Narrow(path);
    out.offset = address - reinterpret_cast<uintptr_t>(m);
    return true;
}

std::string BaseName(const std::string& path)
{
    const auto at = path.find_last_of("\\/");
    return at == std::string::npos ? path : path.substr(at + 1);
}

// [ReShade] [Dalamud] [OptiScaler] [Streamline]: by file name, and for ReShade also by its add-on export.
std::string Flags(HMODULE module, const std::string& path)
{
    const std::string lower = Lower(BaseName(path));
    std::string f;
    if (lower.find("reshade") != std::string::npos ||
        (module && GetProcAddress(module, "ReShadeRegisterAddon") != nullptr))
        f += " [ReShade]";
    if (lower.find("dalamud") != std::string::npos)
        f += " [Dalamud]";
    if (module && module == OwnModule())
        f += " [OptiScaler]";
    if (lower.rfind("sl.", 0) == 0)
        f += " [Streamline]";
    return f;
}

bool Read(uintptr_t address, void* out, size_t size) { return ext::core::ReadProcessBytes(address, out, size); }
} // namespace

std::vector<ModuleInfo> Modules()
{
    std::vector<ModuleInfo> out;
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed))
        return out;
    const size_t count = (std::min<size_t>)(needed / sizeof(HMODULE), 1024);
    for (size_t i = 0; i < count; ++i)
    {
        ModuleInfo m;
        m.base = reinterpret_cast<uintptr_t>(mods[i]);
        MODULEINFO info {};
        if (K32GetModuleInformation(GetCurrentProcess(), mods[i], &info, sizeof info))
            m.size = info.SizeOfImage;
        wchar_t path[2048] {};
        if (GetModuleFileNameW(mods[i], path, 2048))
            m.path = Narrow(path);
        out.push_back(std::move(m));
    }
    return out; // EnumProcessModules lists modules in load order
}

std::string Describe(uintptr_t address)
{
    Owner o;
    if (OwnerOf(address, o))
        return BaseName(o.path) + "+" + Hex(o.offset) + Flags(o.module, o.path) + "  (" + o.path + ")";

    MEMORY_BASIC_INFORMATION mbi {};
    if (address && VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof mbi))
    {
        char b[200];
        snprintf(b, sizeof b, "<no module: region %s size %llu state 0x%lX type 0x%lX protect 0x%lX>",
                 Hex(reinterpret_cast<uintptr_t>(mbi.AllocationBase)).c_str(), static_cast<unsigned long long>(mbi.RegionSize),
                 mbi.State, mbi.Type, mbi.Protect);
        return b;
    }
    return "<unreadable address>";
}

std::string ModulesText(std::vector<ModuleInfo>& previous)
{
    const bool first = previous.empty();
    const auto now = Modules();
    std::string out = first ? "== process modules in load order (start of the run) ==\n"
                            : "== modules loaded since the previous listing ==\n";
    size_t listed = 0;
    for (size_t i = 0; i < now.size(); ++i)
    {
        const bool known = std::ranges::any_of(previous, [&](const ModuleInfo& p) { return p.base == now[i].base; });
        if (known)
            continue;
        ++listed;
        char head[64];
        snprintf(head, sizeof head, "%03zu  %s  %8u  ", i + 1, Hex(now[i].base).c_str(), now[i].size);
        const HMODULE m = reinterpret_cast<HMODULE>(now[i].base);
        out += std::string(head) + now[i].path + Flags(m, now[i].path) + "\n";
    }
    if (!first && listed == 0)
        out += "(none)\n";
    previous = now;
    return out;
}

std::string StackText(uint64_t frame)
{
    void* frames[48] {};
    const USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
    char head[128];
    snprintf(head, sizeof head, "== Present call chain, frame %llu, thread %lu ==\n", static_cast<unsigned long long>(frame),
             GetCurrentThreadId());
    std::string out = head;

    int lastOwn = -1;
    std::vector<std::string> flagsAbove;
    for (USHORT i = 0; i < n; ++i)
    {
        const uintptr_t a = reinterpret_cast<uintptr_t>(frames[i]);
        char idx[24];
        snprintf(idx, sizeof idx, "  #%02u  %s  ", i, Hex(a).c_str());
        out += idx + Describe(a) + "\n";
        Owner o;
        if (OwnerOf(a, o) && o.module == OwnModule())
            lastOwn = i;
    }
    // Callers of the bridge's Present are the frames after the last OptiScaler frame.
    bool reshadeOuter = false, dalamudOuter = false;
    for (int i = lastOwn + 1; i < static_cast<int>(n); ++i)
    {
        Owner o;
        if (!OwnerOf(reinterpret_cast<uintptr_t>(frames[i]), o))
            continue;
        const std::string f = Flags(o.module, o.path);
        reshadeOuter |= f.find("[ReShade]") != std::string::npos;
        dalamudOuter |= f.find("[Dalamud]") != std::string::npos;
    }
    out += std::string("# inference: ReShade frame above the bridge Present on this call chain: ") +
           (reshadeOuter ? "YES (ReShade is OUTER: it calls the bridge's Present)" : "no") + "\n";
    out += std::string("# inference: Dalamud frame above the bridge Present on this call chain: ") +
           (dalamudOuter ? "YES" : "no") + "\n";
    return out;
}

uintptr_t DecodeJump(uintptr_t address)
{
    uint8_t b[16] {};
    if (!Read(address, b, sizeof b))
        return 0;
    if (b[0] == 0xE9) // jmp rel32
    {
        int32_t rel = 0;
        std::memcpy(&rel, b + 1, 4);
        return address + 5 + static_cast<intptr_t>(rel);
    }
    if (b[0] == 0xEB) // jmp rel8
        return address + 2 + static_cast<intptr_t>(static_cast<int8_t>(b[1]));
    if (b[0] == 0xFF && b[1] == 0x25) // jmp [rip+disp32]
    {
        int32_t disp = 0;
        std::memcpy(&disp, b + 2, 4);
        uintptr_t target = 0;
        return Read(address + 6 + static_cast<intptr_t>(disp), &target, sizeof target) ? target : 0;
    }
    if (b[0] == 0x48 && b[1] == 0xB8 && b[10] == 0xFF && b[11] == 0xE0) // mov rax, imm64 ; jmp rax
    {
        uintptr_t target = 0;
        std::memcpy(&target, b + 2, 8);
        return target;
    }
    if (b[0] == 0x49 && b[1] == 0xBB && b[10] == 0x41 && b[11] == 0xFF && b[12] == 0xE3) // mov r11, imm64 ; jmp r11
    {
        uintptr_t target = 0;
        std::memcpy(&target, b + 2, 8);
        return target;
    }
    return 0;
}
} // namespace ext::ffxiv::ui::probe
