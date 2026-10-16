// S10-A: the ReShade / Dalamud position probe (probe.txt) on the test process itself: module list in load order, module
// ownership of an address, the inline-hook jump decoder, the call chain text, the module diff.
#include "check.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "ext/ffxiv/ui/UiProbe.h"

#include <cstring>
#include <string>
#include <vector>

using namespace ext::ffxiv::ui::probe;

namespace
{
bool Contains(const std::string& text, const char* needle)
{
    std::string a = text, b = needle;
    for (auto& c : a)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : b)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return a.find(b) != std::string::npos;
}

int Marker() { return 42; }

void Modules_()
{
    const auto mods = Modules();
    CHECK(mods.size() > 3);
    if (mods.empty())
        return;
    CHECK(Contains(mods.front().path, "ui_probe.exe")); // load order: the executable first
    bool kernel = false, ntdll = false;
    for (const auto& m : mods)
    {
        kernel |= Contains(m.path, "kernel32.dll");
        ntdll |= Contains(m.path, "ntdll.dll");
        CHECK(m.base != 0);
    }
    CHECK(kernel && ntdll);

    // The module diff: the first listing names everything, a second one only what was loaded in between.
    std::vector<ModuleInfo> previous;
    const std::string first = ModulesText(previous);
    CHECK(Contains(first, "load order"));
    CHECK(Contains(first, "ui_probe.exe"));
    CHECK(!previous.empty());
    const std::string none = ModulesText(previous);
    CHECK(Contains(none, "(none)"));
    HMODULE extra = LoadLibraryW(L"winmm.dll");
    if (extra)
    {
        const std::string added = ModulesText(previous);
        CHECK(Contains(added, "winmm.dll"));
        CHECK(!Contains(added, "ui_probe.exe"));
    }
}

void Describe_()
{
    const std::string own = Describe(reinterpret_cast<uintptr_t>(&Marker));
    CHECK(Contains(own, "ui_probe.exe+0x"));
    CHECK(Contains(own, "[OptiScaler]")); // UiProbe.cpp is linked into this exe, so the exe is the 'own module' of the probe

    const std::string k32 = Describe(reinterpret_cast<uintptr_t>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetTickCount64")));
    CHECK(Contains(k32, "kernel32") || Contains(k32, "kernelbase"));

    void* region = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    CHECK(region != nullptr);
    if (region)
    {
        const std::string priv = Describe(reinterpret_cast<uintptr_t>(region));
        CHECK(Contains(priv, "<no module"));
        VirtualFree(region, 0, MEM_RELEASE);
    }
    CHECK(Contains(Describe(0), "unreadable"));
}

void Jumps()
{
    // Each pattern is placed in an executable buffer; the decoder reads it with ReadProcessMemory like the product.
    auto* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    CHECK(code != nullptr);
    if (!code)
        return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(code);

    // E9 rel32
    code[0] = 0xE9;
    const int32_t rel = 0x100;
    std::memcpy(code + 1, &rel, 4);
    CHECK_EQ(DecodeJump(base), base + 5 + 0x100);

    // E9 backwards
    const int32_t back = -0x10;
    std::memcpy(code + 1, &back, 4);
    CHECK_EQ(DecodeJump(base), base + 5 - 0x10);

    // EB rel8
    code[0x20] = 0xEB;
    code[0x21] = 0x10;
    CHECK_EQ(DecodeJump(base + 0x20), base + 0x20 + 2 + 0x10);

    // FF 25 disp32: jmp [rip+disp]
    code[0x40] = 0xFF;
    code[0x41] = 0x25;
    const int32_t disp = 0x30; // the pointer slot sits at base + 0x40 + 6 + 0x30
    std::memcpy(code + 0x42, &disp, 4);
    const uintptr_t target = 0x00007FF612345678ull;
    std::memcpy(code + 0x40 + 6 + 0x30, &target, 8);
    CHECK_EQ(DecodeJump(base + 0x40), target);

    // mov rax, imm64 ; jmp rax
    const uint8_t movRax[] { 0x48, 0xB8, 0x78, 0x56, 0x34, 0x12, 0xF6, 0x7F, 0x00, 0x00, 0xFF, 0xE0 };
    std::memcpy(code + 0x100, movRax, sizeof movRax);
    CHECK_EQ(DecodeJump(base + 0x100), 0x00007FF612345678ull);

    // mov r11, imm64 ; jmp r11
    const uint8_t movR11[] { 0x49, 0xBB, 0x78, 0x56, 0x34, 0x12, 0xF6, 0x7F, 0x00, 0x00, 0x41, 0xFF, 0xE3 };
    std::memcpy(code + 0x120, movR11, sizeof movR11);
    CHECK_EQ(DecodeJump(base + 0x120), 0x00007FF612345678ull);

    // not a jump, and unreadable memory
    code[0x140] = 0x90;
    CHECK_EQ(DecodeJump(base + 0x140), 0u);
    CHECK_EQ(DecodeJump(0), 0u);
    CHECK_EQ(DecodeJump(8), 0u);

    VirtualFree(code, 0, MEM_RELEASE);
}

void Stack()
{
    const std::string text = StackText(7);
    CHECK(Contains(text, "frame 7"));
    CHECK(Contains(text, "#00"));
    CHECK(Contains(text, "ui_probe.exe+0x"));
    CHECK(Contains(text, "# inference: ReShade frame above the bridge Present on this call chain: no"));
    CHECK(Contains(text, "# inference: Dalamud frame above"));
}
} // namespace

int main()
{
    Modules_();
    Describe_();
    Jumps();
    Stack();
    return Finish("ui_probe");
}
