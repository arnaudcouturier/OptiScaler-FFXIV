#pragma once
// S10 package A: the deterministic ReShade / Dalamud position probe of D1 (docs/HUDLESS.md sections 12 and 15, S10-PLAN.md
// section 2b). No pixel comparison: it names the modules that own the Present call chain and the Present entries of the
// swapchains. Everything it prints is evidence (addresses, module paths); the verdict lines are marked as inferences.

#include <cstdint>
#include <string>
#include <vector>

namespace ext::ffxiv::ui::probe
{
struct ModuleInfo
{
    uintptr_t base = 0;
    uint32_t size = 0;
    std::string path;
};

// Process modules in load order.
std::vector<ModuleInfo> Modules();

// "name.dll+0xOFFSET [flags]" for an address inside a module, "<private memory ...>" otherwise.
std::string Describe(uintptr_t address);

// Process module list for the start of the run (previous empty) or the modules added since `previous`.
std::string ModulesText(std::vector<ModuleInfo>& previous);

// Call chain of the current Present (CaptureStackBackTrace -> owning module of every frame).
std::string StackText(uint64_t frame);

// Owner of each swapchain object's vtable and of its Present / Present1 entries, with inline jump hooks followed.
std::string VtableText();

// Decodes one jump at `address` (E9, EB, FF 25, mov+jmp register forms). Returns the target or 0.
uintptr_t DecodeJump(uintptr_t address);
} // namespace ext::ffxiv::ui::probe
