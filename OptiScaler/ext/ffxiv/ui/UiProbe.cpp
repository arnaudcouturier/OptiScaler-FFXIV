#include "pch.h"

#include "ext/ffxiv/ui/UiProbe.h"

#include "ext/core/ProcessMemory.h"

#include <State.h>

#include <psapi.h>

#include <algorithm>
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

namespace
{
std::string Chain(const char* label, uintptr_t entry)
{
    std::string out = std::string("    ") + label + ": entry " + Hex(entry) + "  " + Describe(entry) + "\n";
    uintptr_t at = entry;
    for (int hop = 1; hop <= 3; ++hop)
    {
        const uintptr_t next = DecodeJump(at);
        if (!next)
            break;
        out += "      inline jump hop " + std::to_string(hop) + " -> " + Hex(next) + "  " + Describe(next) + "\n";
        at = next;
    }
    return out;
}

void OneChain(std::string& out, const char* name, IUnknown* object)
{
    out += std::string("  ") + name + ": ";
    if (!object)
    {
        out += "(null)\n";
        return;
    }
    out += Hex(reinterpret_cast<uintptr_t>(object)) + "\n";

    auto vtableOf = [](IUnknown* o, uintptr_t& vt)
    {
        return Read(reinterpret_cast<uintptr_t>(o), &vt, sizeof vt) && vt != 0;
    };

    uintptr_t vt = 0;
    if (!vtableOf(object, vt))
    {
        out += "    vtable unreadable\n";
        return;
    }
    out += "    vtable " + Hex(vt) + "  " + Describe(vt) + "\n";
    uintptr_t slot8 = 0;
    if (Read(vt + 8 * sizeof(uintptr_t), &slot8, sizeof slot8))
        out += Chain("Present  (slot 8)", slot8);

    // Present1 exists only on IDXGISwapChain1: ask for that interface first (its vtable pointer may differ).
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1)
    {
        uintptr_t vt1 = 0;
        uintptr_t slot22 = 0;
        if (vtableOf(sc1, vt1) && Read(vt1 + 22 * sizeof(uintptr_t), &slot22, sizeof slot22))
        {
            if (vt1 != vt)
                out += "    IDXGISwapChain1 vtable " + Hex(vt1) + "  " + Describe(vt1) + "\n";
            out += Chain("Present1 (slot 22)", slot22);
        }
        sc1->Release();
    }
    else
        out += "    Present1: interface not supported by this object\n";
}
} // namespace

std::string VtableText()
{
    auto& st = State::Instance();
    std::string out = "== swapchain vtable owners (who runs when this object's Present is called) ==\n";
    out += "# slot 8 = IDXGISwapChain::Present, slot 22 = IDXGISwapChain1::Present1. An inline patch of the function leaves the\n"
           "# slot pointing into its original module and shows up as an 'inline jump hop' line instead.\n";
    OneChain(out, "real D3D11 swapchain (State::currentRealSwapchain)", st.currentRealSwapchain);
    OneChain(out, "D3D12 frame-generation swapchain (State::currentFGSwapchain)", st.currentFGSwapchain);
    OneChain(out, "swapchain the game uses (State::currentSwapchain)", st.currentSwapchain);
    if (st.currentWrappedSwapchain != st.currentSwapchain)
        OneChain(out, "wrapped swapchain (State::currentWrappedSwapchain)", st.currentWrappedSwapchain);

    // Backbuffer identity: the bridge copies GetBuffer(GetCurrentBackBufferIndex()); the detector compares against it.
    if (auto* chain = st.currentRealSwapchain)
    {
        Microsoft::WRL::ComPtr<IDXGISwapChain3> c3;
        UINT index = 0;
        const bool has3 = SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&c3))) && c3;
        if (has3)
            index = c3->GetCurrentBackBufferIndex();
        Microsoft::WRL::ComPtr<ID3D11Texture2D> b0, bi;
        const HRESULT r0 = chain->GetBuffer(0, IID_PPV_ARGS(&b0));
        const HRESULT ri = chain->GetBuffer(index, IID_PPV_ARGS(&bi));
        DXGI_SWAP_CHAIN_DESC d {};
        chain->GetDesc(&d);
        char b[400];
        snprintf(b, sizeof b,
                 "  backbuffer: IDXGISwapChain3=%s currentIndex=%u GetBuffer(0)=%s hr=0x%08lX GetBuffer(index)=%s hr=0x%08lX same=%s "
                 "buffers=%u swapEffect=%d format=%u %ux%u\n",
                 has3 ? "yes" : "no", index, Hex(reinterpret_cast<uintptr_t>(b0.Get())).c_str(), static_cast<unsigned long>(r0),
                 Hex(reinterpret_cast<uintptr_t>(bi.Get())).c_str(), static_cast<unsigned long>(ri),
                 b0.Get() == bi.Get() ? "yes" : "NO", d.BufferCount, static_cast<int>(d.SwapEffect),
                 static_cast<unsigned>(d.BufferDesc.Format), d.BufferDesc.Width, d.BufferDesc.Height);
        out += b;
    }
    return out;
}
} // namespace ext::ffxiv::ui::probe
