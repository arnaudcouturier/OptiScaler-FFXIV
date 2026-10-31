// Source pins for the live output switch. The pure tests cannot see an upstream site that moved, so this test reads the
// sites (comments stripped, whitespace collapsed) and pins the exact statements the switch's rules depend on:
//
//   * the swapchain hook installs one class at a time and returns at once when it has one (so a switch has to unhook);
//   * the ten vtable slots and the extra lines for one provider class (so the rebind covers the same set);
//   * the release hook's pass-through condition (the switch clears the global pointer so it always takes this path);
//   * the two special branches for a last release while the pointer still names the chain: one swallows the release,
//     the other deletes the bridge. Neither may run during a switch;
//   * the bridge's own release order, which the chain teardown copies;
//   * the bridge present's early out, which is where a detached bridge answers instead of failing the game;
//   * the pure switch headers stay pure and never load or probe a provider inside the preflight.
//
// A pin that stops matching means a site moved: re-derive the rule from the new code, do not loosen the pin.
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

static int g_failed = 0;
#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                        \
            ++g_failed;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace
{
std::string Read(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Comments out, string literals kept, every run of whitespace one space.
std::string Normalize(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '"')
        {
            out += s[i++];
            while (i < s.size() && s[i] != '"')
            {
                if (s[i] == '\\' && i + 1 < s.size())
                    out += s[i++];
                out += s[i++];
            }
            out += '"';
        }
        else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/')
        {
            while (i < s.size() && s[i] != '\n')
                ++i;
            if (out.empty() || out.back() != ' ')
                out += ' ';
        }
        else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*')
        {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/'))
                ++i;
            ++i;
            if (out.empty() || out.back() != ' ')
                out += ' ';
        }
        else if (std::isspace(static_cast<unsigned char>(s[i])))
        {
            if (out.empty() || out.back() != ' ')
                out += ' ';
        }
        else
        {
            out += s[i];
        }
    }
    return out;
}

void Pin(const std::string& text, const char* needle, int line)
{
    if (text.find(needle) == std::string::npos)
    {
        std::printf("FAIL line %d: missing source pin: %s\n", line, needle);
        ++g_failed;
    }
}

void Absent(const std::string& text, const char* needle, int line)
{
    if (text.find(needle) != std::string::npos)
    {
        std::printf("FAIL line %d: unexpected source text: %s\n", line, needle);
        ++g_failed;
    }
}
// How often a pinned statement occurs: a body that loses its prologue must not pass because another body still has one.
size_t CountOf(const std::string& haystack, const std::string& needle)
{
    size_t count = 0;
    for (size_t at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + needle.size()))
        ++count;
    return count;
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    const std::string fgHooks = Normalize(Read(repo + "/OptiScaler/hooks/FG_Hooks.cpp"));
    const std::string bridge = Normalize(Read(repo + "/OptiScaler/with_dx12/dx11_with_dx12_sc.cpp"));
    const std::string feature = Normalize(Read(repo + "/OptiScaler/framegen/IFGFeature_Dx12.cpp"));
    const std::string ffx = Normalize(Read(repo + "/OptiScaler/framegen/ffx/FSRFG_Dx12.cpp"));
    const std::string xess = Normalize(Read(repo + "/OptiScaler/framegen/xefg/XeFG_Dx12.cpp"));
    const std::string sequence = Normalize(Read(repo + "/OptiScaler/ext/fg/outswitch/SwitchSequence.h"));
    const std::string policy = Normalize(Read(repo + "/OptiScaler/ext/fg/outswitch/SwitchPolicy.h"));
    const std::string lifetime = Normalize(Read(repo + "/OptiScaler/ext/fg/outswitch/ChainLifetime.h"));
    const std::string probation = Normalize(Read(repo + "/OptiScaler/ext/fg/outswitch/ProbationWatch.h"));
    const std::string probe = Normalize(Read(repo + "/OptiScaler/ext/fg/outswitch/Probe.cpp"));
    const std::string bridgeHdr = Normalize(Read(repo + "/OptiScaler/ext/hdr/BridgeHdr.cpp"));
    const std::string probeHeader = Normalize(Read(repo + "/OptiScaler/ext/fg/outswitch/Probe.h"));
    const std::string registry = Normalize(Read(repo + "/OptiScaler/ext/fg/outswitch/BridgeRegistry.h"));
    const std::string statusBox = Normalize(Read(repo + "/OptiScaler/ext/fg/outswitch/StatusBox.h"));
    const std::string hookLife = Normalize(Read(repo + "/OptiScaler/ext/fg/outswitch/HookLifecycle.h"));
    const std::string contract = Normalize(Read(repo + "/OptiScaler/ext/fg/outswitch/SwitchOpsContract.h"));
    const std::string contractRaw = Read(repo + "/OptiScaler/ext/fg/outswitch/SwitchOpsContract.h");
    const std::string scope = Normalize(Read(repo + "/OptiScaler/ext/fg/outswitch/HookScope.h"));

    std::printf("  the hook installs one class and returns at once when it has one\n");
    // hkFGRelease's early return also uses `o_FGSCPresent`, so pin the plain-language guard with the vtable read that
    // follows it.
    Pin(fgHooks, "if (o_FGSCPresent != nullptr || pSwapChain == nullptr) return; void** pFactoryVTable = *reinterpret_cast<void***>(pSwapChain);", __LINE__);
    Pin(fgHooks, "LOG_INFO(\"Hooking FG SwapChain present\");", __LINE__);
    std::printf("    [ok] HookFGSwapchain guards on the one slot it would overwrite\n");

    std::printf("  the ten vtable slots the rebind has to cover\n");
    Pin(fgHooks, "o_FGRelease = (PFN_Release) pFactoryVTable[2];", __LINE__);
    Pin(fgHooks, "o_FGSCPresent = (PFN_Present) pFactoryVTable[8];", __LINE__);
    Pin(fgHooks, "o_FGSCSetFullscreenState = (PFN_SetFullscreenState) pFactoryVTable[10];", __LINE__);
    Pin(fgHooks, "o_FGSCGetFullscreenState = (PFN_GetFullscreenState) pFactoryVTable[11];", __LINE__);
    Pin(fgHooks, "o_FGSCResizeBuffers = (PFN_ResizeBuffers) pFactoryVTable[13];", __LINE__);
    Pin(fgHooks, "o_FGSCResizeTarget = (PFN_ResizeTarget) pFactoryVTable[14];", __LINE__);
    Pin(fgHooks, "o_FGSCGetFullscreenDesc = (PFN_GetFullscreenDesc) pFactoryVTable[19];", __LINE__);
    Pin(fgHooks, "o_FGSCPresent1 = (PFN_Present1) pFactoryVTable[22];", __LINE__);
    Pin(fgHooks, "o_FGSCGetFrameLatencyWaitableObject = (PFN_GetFrameLatencyWaitableObject) pFactoryVTable[33];", __LINE__);
    Pin(fgHooks, "o_FGSCResizeBuffers1 = (PFN_ResizeBuffers1) pFactoryVTable[39];", __LINE__);
    std::printf("    [ok] Release 2, Present 8, SetFullscreen 10, ... ResizeBuffers1 39\n");

    std::printf("  the attach set, including the extras for one provider class\n");
    Pin(fgHooks, "DetourAttach(&(PVOID&) o_FGRelease, hkFGRelease);", __LINE__);
    Pin(fgHooks, "DetourAttach(&(PVOID&) o_FGSCPresent, hkFGPresent);", __LINE__);
    Pin(fgHooks, "DetourAttach(&(PVOID&) o_FGSCResizeTarget, hkResizeTarget);", __LINE__);
    Pin(fgHooks, "DetourAttach(&(PVOID&) o_FGSCResizeBuffers, hkResizeBuffers);", __LINE__);
    Pin(fgHooks, "DetourAttach(&(PVOID&) o_FGSCSetFullscreenState, hkSetFullscreenState);", __LINE__);
    Pin(fgHooks, "if (o_FGSCPresent1 != nullptr) DetourAttach(&(PVOID&) o_FGSCPresent1, hkFGPresent1);", __LINE__);
    Pin(fgHooks, "if (o_FGSCResizeBuffers1 != nullptr) DetourAttach(&(PVOID&) o_FGSCResizeBuffers1, hkResizeBuffers1);", __LINE__);
    Pin(fgHooks, "if (State::Instance().activeFgOutput == FGOutput::XeFG)", __LINE__);
    Pin(fgHooks, "DetourAttach(&(PVOID&) o_FGSCGetFullscreenState, hkGetFullscreenState);", __LINE__);
    Pin(fgHooks, "DetourAttach(&(PVOID&) o_FGSCGetFullscreenDesc, hkGetFullscreenDesc);", __LINE__);
    Pin(fgHooks, "Config::Instance()->SimulateWaitableObject.value_or_default()", __LINE__);
    Pin(fgHooks, "DetourAttach(&(PVOID&) o_FGSCGetFrameLatencyWaitableObject, hkGetFrameLatencyWaitableObject);", __LINE__);
    Pin(fgHooks, "LOG_ERROR(\"Failed to attach detour: {:X}\", detourResult);", __LINE__);
    Pin(fgHooks, "o_FGRelease = nullptr;", __LINE__);
    std::printf("    [ok] five mandatory, two conditional, three extras for one class\n");

    std::printf("  the release hook's pass-through, and the two branches it must never take during a switch\n");
    Pin(fgHooks,
        "if (skipReleaseChecks || State::Instance().currentFGSwapchain != This || State::Instance().isShuttingDown) "
        "return o_FGRelease(This);",
        __LINE__);
    Pin(fgHooks, "This->AddRef();", __LINE__);
    Pin(fgHooks, "if (!Config::Instance()->FGPreserveSwapChain.value_or_default()) { if (o_FGRelease(This) == 1) {", __LINE__);
    Pin(fgHooks, "LOG_INFO(\"Preserving FG Swapchain from release\"); return 0;", __LINE__);
    Pin(fgHooks, "State::Instance().currentFG->ReleaseSwapchain(_hwnd);", __LINE__);
    Pin(fgHooks, "State::Instance().currentFGSwapchain = nullptr;", __LINE__);
    Pin(fgHooks, "State::Instance().currentWrappedSwapchain->Release();", __LINE__);
    Pin(fgHooks, "LOG_DEBUG(\"FG Swapchain released, clearing currentFGSwapchain\");", __LINE__);
    std::printf("    [ok] the pointer comparison, one AddRef, the two branches\n");

    std::printf("  the bridge's own release order, which the chain teardown copies\n");
    Pin(bridge, "if (State::Instance().currentFGSwapchain == _fgSwapChain) State::Instance().currentFGSwapchain = nullptr;", __LINE__);
    Pin(bridge, "auto fg = State::Instance().currentFG; if (fg != nullptr && fg->Mutex.getOwner() != 1 && fg->SwapchainContext() != nullptr) { fg->Deactivate(); fg->ReleaseSwapchain(_handle); }", __LINE__);
    Pin(bridge, "SafeRelease(_fgSwapChain);", __LINE__);
    Pin(bridge, "if (_real == nullptr || _fgSwapChain == nullptr) return ext::fg::DetachedPresent(*this, _real == nullptr);", __LINE__);
    // The present that ran a switch that ended detached finds no chain where the present started with one: it answers
    // through the same seam instead of falling through to DXGI_ERROR_DEVICE_REMOVED.
    {
        const size_t scope = bridge.find("ext::dx11bridge::PresentScope scenePresentScope;");
        const size_t after = bridge.find("if (_fgSwapChain == nullptr) return ext::fg::DetachedPresent(*this, false);");
        const size_t interop = bridge.find("if (!_InitInteropObjects())");
        if (scope == std::string::npos || after == std::string::npos || interop == std::string::npos || after < scope ||
            after > interop)
        {
            std::printf("    FAIL: the switching present does not answer through DetachedPresent\n");
            ++g_failed;
        }
    }
    std::printf("    [ok] clear the pointer, provider deactivate and destroy, then the pin; the present early out\n");
    std::printf("  the bridge present's two seams, and the lines they sit between\n");
    const std::string swapImpl = Normalize(Read(repo + "/OptiScaler/ext/fg/outswitch/BridgeSwap.cpp"));
    Pin(bridge, "auto result = _fgSwapChain->Present(SyncInterval, Flags);", __LINE__);
    Pin(bridge, "return ext::fg::OnFgPresentResult(*this, result);", __LINE__);
    Pin(bridge, "if (dx11HudfixPresent) Hudfix_Dx11::PresentEnd();", __LINE__);
    // The seam answers the line's own error, and the switch only steps in once the composition armed the swap.
    Pin(swapImpl, "outswitch::DetachedAnswer(outswitch::SwapFor(&sc), realChainMissing, outswitch::kDeviceRemoved)", __LINE__);
    Pin(swapImpl, "outswitch::FgResultAnswer(outswitch::SwapFor(&sc)", __LINE__);
    Pin(Normalize(Read(repo + "/OptiScaler/ext/fg/outswitch/BridgeSwap.h")), "if (swap == nullptr || swap->detached == nullptr) return fallback;", __LINE__);
    // The record the replay reads is written where the bridge already sees it, not by a new upstream line.
    // The record is taken before the OptiHDR branch (so it exists whether or not the fork swallows the call) and is only
    // replayed while the fork does not own the output.
    Pin(Normalize(Read(repo + "/OptiScaler/ext/hdr/BridgeHdr.cpp")), "bridge.lastColorSpace = static_cast<uint32_t>(colorSpace);", __LINE__);
    {
        // The record sits above the OptiHDR branch inside SetColorSpace: located from the function's own first line, so an
        // earlier occurrence elsewhere in the file cannot make the comparison pass by accident.
        const std::string body = Normalize(Read(repo + "/OptiScaler/ext/hdr/BridgeHdr.cpp"));
        const size_t function = body.find("bool SetColorSpace(");
        const size_t record = body.find("bridge.colorSpaceRecorded = true;", function);
        const size_t branch = body.find("if (Access::Bridge::HdrOutput(sc))", function);
        CHECK(function != std::string::npos && record != std::string::npos && branch != std::string::npos && record < branch);
    }
    Pin(Normalize(Read(repo + "/OptiScaler/ext/hdr/BridgeHdr.cpp")), "++bridge.resizeGeneration;", __LINE__);
    // The switch's own epilogue runs the same HDR and window work but is not a resize of the window: it must not move the
    // record the replay reads, or every switch would replay a resize and refuse the next one as a busy resize.
    Pin(Normalize(Read(repo + "/OptiScaler/ext/hdr/BridgeHdr.cpp")), "if (gameResize)", __LINE__);
    Pin(Normalize(Read(repo + "/OptiScaler/ext/fg/outswitch/BridgeSwap.cpp")),
        "ext::dx11bridge::OnResizeDone(*AsBridge(bridge), false)", __LINE__);
    std::printf("    [ok] the seams answer the line's value, and the record is taken in the existing seams\n");


    std::printf("  the provider destroys the switch has to drive, and the model it was measured for\n");
    Pin(ffx, "State::Instance().currentFGSwapchain = nullptr;", __LINE__);
    Pin(ffx, "auto result = FfxApiProxy::D3D12_DestroyContext(&_swapChainContext, nullptr);", __LINE__);
    Pin(ffx, "if (hwnd != _hwnd || _hwnd == NULL) return false;", __LINE__);
    Pin(ffx, "if (Mutex.getOwner() == 1)", __LINE__);
    Pin(xess, "auto result = XeFGProxy::Destroy()(context);", __LINE__);
    Pin(xess, "State::Instance().currentFGSwapchain = nullptr;", __LINE__);
    Pin(feature, "if (State::Instance().currentFGSwapchain != nullptr && _hwnd == desc->OutputWindow)", __LINE__);
    Pin(feature, "if (Config::Instance()->FGPreserveSwapChain.value_or_default())", __LINE__);
    std::printf("    [ok] both destroy calls, both window guards, the re-entry branch\n");

    std::printf("  the pure switch headers stay pure\n");
    Absent(sequence, "#include \"pch.h\"", __LINE__);
    Absent(sequence, "LoadStreamline", __LINE__);
    Absent(sequence, "CheckForFGStatus", __LINE__);
    Absent(policy, "LoadStreamline", __LINE__);
    Absent(policy, "CheckForFGStatus", __LINE__);
    Absent(lifetime, "LoadStreamline", __LINE__);
    Absent(policy, "#include <windows.h>", __LINE__);
    Absent(sequence, "#include <windows.h>", __LINE__);
    Absent(lifetime, "#include <windows.h>", __LINE__);
    std::printf("    [ok] no pch, no provider load and no status check in the preflight or the sequence\n");

    std::printf("  the probe is read-only and never measures a release in the game's process\n");
    // The destructive half (chain creation, destroy, the pin release that may touch freed memory) is a separate host.
    Absent(probeHeader, "Measured()", __LINE__); // the old in-process table export is gone
    Absent(probe, "CreateTestChain", __LINE__);
    Absent(probe, "DestroyTestChain", __LINE__);
    Absent(probe, "SetPrivateDataInterface", __LINE__);
    Absent(probe, "release_xefg_xell", __LINE__);
    Absent(probe, "version.lib", __LINE__);
    Absent(probe, "winver.h", __LINE__);
    Pin(probe, "const ULONG before = live->AddRef();", __LINE__); // P1: the standstill count, read only
    Pin(probe, "const ULONG after = live->Release();", __LINE__);
    Pin(probe, "const HRESULT presentResult = live->Present(0, DXGI_PRESENT_TEST);", __LINE__);
    Pin(probe, "const bool hotSwapKey = ext::Cfg().fg.outputHotSwap.value_or_default();", __LINE__);
    Pin(probe, "const HMODULE xell = GetModuleHandleW(L\"libxell.dll\");", __LINE__);
    Pin(probe, "identity.stamp = pe->FileHeader.TimeDateStamp;", __LINE__);
    Pin(bridgeHdr, "fg::outswitch::probe::OnFirstPresent();", __LINE__); // at the first present, not in the constructor
    Pin(bridgeHdr, "fg::outswitch::Bridges().NoteConfigured(&sc);", __LINE__);
    Pin(bridgeHdr, "fg::outswitch::Bridges().NoteDestroyed(&sc);", __LINE__);
    std::printf("    [ok] the game's process only reads the live chain, at its first present\n");

    std::printf("  only compiled-in rows decide a release, and only after three stable runs\n");
    Pin(registry, "void* expected = nullptr;", __LINE__);
    Pin(registry, "second_.store(true, std::memory_order_relaxed);", __LINE__);
    Pin(statusBox, "state_.store(static_cast<uint8_t>(status.state), std::memory_order_release);", __LINE__);
    Pin(statusBox, "status.state = static_cast<State>(state_.load(std::memory_order_acquire));", __LINE__);
    Pin(hookLife, "bool BeginDetach()", __LINE__);
    Pin(hookLife, "return inFlight_.load(std::memory_order_seq_cst) == 0;", __LINE__);
    Pin(hookLife, "bool DetachFor(uint32_t timeoutMs, NowFn now, SleepFn sleep)", __LINE__);
    Pin(hookLife, "passThroughs_.fetch_add(1, std::memory_order_seq_cst);", __LINE__);
    Pin(scope, "inline HookGate& HookGateFor(Class cls)", __LINE__);
    Pin(scope, "class HookScope", __LINE__);
    Pin(scope, "auto Pass(Call&& call, Fallback&& fallback) const -> decltype(call())", __LINE__);
    Pin(contract, "concept SwitchOps =", __LINE__);
    Pin(contract, "ReadLive(live)", __LINE__);
    Pin(contract, "HooksAttached(target)", __LINE__);
    Pin(contract, "ClearOldSwapChain()", __LINE__);
    std::printf("    [ok] one bridge, a state stored last, a detach that waits, the contract frozen\n");

    std::printf("  every hook body asks the gate first, and the two Get bodies write their refused answer\n");
    const std::string hookAccess = Normalize(Read(repo + "/OptiScaler/ext/fg/outswitch/HookAccess.h"));
    // The 20 body lines (two per body), the two header lines that let them compile, and the one line that reopens the
    // gate after a rehook: 23 added upstream lines, 25 with the two present lines of the bridge that were replaced.
    Pin(fgHooks, "const ext::fg::outswitch::HookScope scope { ext::fg::outswitch::HookGateFor() };", __LINE__);
    CHECK(CountOf(fgHooks, "const ext::fg::outswitch::HookScope scope { ext::fg::outswitch::HookGateFor() };") == 10);
    Pin(fgHooks, "if (!scope) return scope.Pass([&] { return o_FGSCPresent(This, SyncInterval, Flags); }, S_OK);", __LINE__);
    Pin(fgHooks, "if (!scope) return scope.Pass([&] { return o_FGSCPresent1(This, SyncInterval, Flags, pPresentParameters); }, S_OK);", __LINE__);
    Pin(fgHooks, "if (!scope) return scope.Pass([&] { return o_FGRelease(This); }, static_cast<ULONG>(0));", __LINE__);
    Pin(fgHooks, "if (!scope) return scope.Pass([&] { return o_FGSCSetFullscreenState(This, Fullscreen, pTarget); }, S_OK);", __LINE__);
    Pin(fgHooks, "if (!scope) return scope.Pass([&] { return o_FGSCGetFullscreenState(This, pFullscreen, ppTarget); }, [&] { if (pFullscreen != nullptr) *pFullscreen = FALSE; if (ppTarget != nullptr) *ppTarget = nullptr; return S_OK; });", __LINE__);
    Pin(fgHooks, "if (!scope) return scope.Pass([&] { return o_FGSCGetFullscreenDesc(This, pDesc); }, [&] { DXGI_SWAP_CHAIN_FULLSCREEN_DESC refused {}; refused.Windowed = TRUE; if (pDesc != nullptr) *pDesc = refused; return S_OK; });", __LINE__);
    Pin(fgHooks, "if (!scope) return scope.Pass([&] { return o_FGSCResizeBuffers(This, BufferCount, Width, Height, NewFormat, SwapChainFlags); }, S_OK);", __LINE__);
    Pin(fgHooks, "if (!scope) return scope.Pass([&] { return o_FGSCResizeBuffers1(This, BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask, ppPresentQueue); }, S_OK);", __LINE__);
    Pin(fgHooks, "if (!scope) return scope.Pass([&] { return o_FGSCResizeTarget(This, pNewTargetParameters); }, S_OK);", __LINE__);
    Pin(fgHooks, "if (!scope) return scope.Pass([&] { return o_FGSCGetFrameLatencyWaitableObject(This); }, static_cast<HANDLE>(nullptr));", __LINE__);
    // The header: the include that makes the two statements compile, and the one friend line that lets the rebind name the
    // private trampolines. The include lives in the header (not in the .cpp) because the header is parsed before
    // ext/Seams.h in its own translation unit, and `friend struct ext::Access;` needs that declaration to exist.
    const std::string hookHeader = Normalize(Read(repo + "/OptiScaler/hooks/FG_Hooks.h"));
    Pin(hookHeader, "#include \"ext/fg/outswitch/HookScope.h\"", __LINE__);
    Pin(hookHeader, "friend struct ext::Access;", __LINE__);
    // The one line that reopens the gate after a rebind: upstream's own hook function ends with it, so the new class (and
    // the failure path where no class was hooked) never leaves every body answering a refusal.
    Pin(fgHooks, "ext::fg::outswitch::HookGateFor().Reattached();", __LINE__);
    CHECK(CountOf(fgHooks, "ext::fg::outswitch::HookGateFor().Reattached();") == 1);
    Pin(Read(repo + "/OptiScaler/ext/fg/outswitch/HookRebind.cpp"),
        "that function reopens the gate with its last statement, whether the attach worked or not", __LINE__);
    Pin(contractRaw, "23 added upstream lines, and 25 with the two present", __LINE__);
    Pin(contractRaw, "lines of the bridge that were replaced", __LINE__);
    // The refused answers themselves, the set the rebind describes, and the rule that a raw address is never detached.
    Pin(hookAccess, "inline size_t ext::Access::FgHookSlots(ext::fg::outswitch::HookSlot* out, size_t maxOut)", __LINE__);
    Pin(Read(repo + "/OptiScaler/ext/fg/outswitch/HookRebind.h"), "A slot is ours when its original is a Detours trampoline", __LINE__);
    std::printf("    [ok] ten bodies, their answers, the access grant and the slot table are pinned\n");

    std::printf("  the contract carries the rebind's rules and the pass-through table\n");
    Pin(contractRaw, "gate.DetachFor(kHookDrainTimeoutMs, NowMs, SleepMs)", __LINE__);
    Pin(contractRaw, "DetourTransactionAbort()", __LINE__);
    Pin(contractRaw, "hkGetFrameLatencyWaitableObject", __LINE__);
    Pin(contractRaw, "refused: 0", __LINE__);
    Pin(contractRaw, "The switch thread's own hooked calls find the gate open", __LINE__);
    std::printf("    [ok] the gate ownership, the Detours order and the ten answers are written once\n");

    std::printf("  probation ignores the results that say nothing about the chain, and the stall clock is late\n");
    Pin(probation, "if (!PresentHealthy(result))", __LINE__);
    const std::string policySource = Normalize(Read(repo + "/OptiScaler/ext/fg/outswitch/SwitchPolicy.h"));
    Pin(policySource, "if (!in.xeFgSwitchAllowed && (in.active == Target::XeFg || in.target == Target::XeFg))", __LINE__);
    Pin(probation, "presents_ >= kStallGracePresents", __LINE__);
    Pin(sequence, "state.policy.Spend();", __LINE__);
    Pin(sequence, "if (!ops.Wanted())", __LINE__);
    Pin(sequence, "if (!state.managed) { LiveChain live; if (!ops.ReadLive(live) || live.Empty()) return detail::Refused(state, ops, inputs.target, Fail::NoCapture, Severity::Hard, true, 0); state.live = live; }", __LINE__);
    // The re-read runs before the first destructive step, on every attempt: a chain the game replaced must never be
    // captured or torn down from a cached name.
    {
        const size_t read = sequence.find("ops.ReadLive(live)");
        const size_t detach = sequence.find("ops.Detach();", read);
        if (read == std::string::npos || detach == std::string::npos || read > detach)
        {
            std::printf("    FAIL: the live chain is not read before the detach\n");
            ++g_failed;
        }
    }
    std::printf("    [ok] WAS_STILL_DRAWING is healthy, the switch hitch is not a stall, the budget is spent at the detach\n");

    std::printf("  the switch's own ordering rules are written where the pins above are read\n");
    // The teardown clear the release pass-through depends on, and the release that follows it.
    Pin(lifetime, "ops.ClearChainRefs(live);", __LINE__);
    Pin(lifetime, "const bool released = plain ? true : ops.ReleaseSwapchain(live.provider, live.hwnd);", __LINE__);
    Pin(lifetime, "if (releasePin) {", __LINE__);
    Pin(sequence, "const bool barrier = target == Target::XeFg || from == Target::XeFg;", __LINE__);
    // D1's pin-first order: the caller's reference goes first, so the witness answer the guard uses must be read AFTER it.
    // A check placed before the release is vacuous (the witness was just placed) and lets the destroy run on freed memory.
    {
        const size_t pin = lifetime.find("out.refsAfterPin = ops.ReleasePin(live.chain);");
        const size_t checked = lifetime.find("out.witnessAlive = witness && ops.WitnessAlive();", pin == std::string::npos ? 0 : pin);
        if (pin == std::string::npos || checked == std::string::npos || checked < pin)
        {
            std::puts("    FAIL: the pin-first witness guard is not after the pin release");
            ++g_failed;
        }
    }
    Pin(sequence, "ops.ReleaseInterop(capture.hwnd);", __LINE__);
    Pin(sequence, "ops.Detach();", __LINE__);

    std::printf("  the present's duration and the one-shot fault are the binding's own code\n");
    const std::string outputSwitch = Normalize(Read(repo + "/OptiScaler/ext/fg/outswitch/OutputSwitch.cpp"));
    Pin(outputSwitch, "const uint32_t presentMs = ElapsedMs(presentStartMs, NowMs());", __LINE__);
    Pin(outputSwitch, "if (FaultAt(Step::Probation, swap.state.probation.InProbation()))", __LINE__);
    // The witness answer belongs to one chain: a switch drops it so the new chain is asked. A due judgement still runs
    // with the key off, the menu's "try again" is applied on the present thread, and a scripted step never writes the
    // persistent configuration value.
    // The declaration and the three resets: a switch's commit/rollback, a bridge change, and the attach point (a new
    // bridge at a freed address). A wire that drops one leaves the answer of the old chain in place, so the count is what
    // pins them (the declaration matches the same text).
    CHECK(CountOf(outputSwitch, "g_witnessProbed = false;") == 4);
    Pin(outputSwitch, "if (!keyOn && !g_state.probation.RollbackDue())", __LINE__);
    Pin(outputSwitch, "g_tryAgainPending.store(static_cast<uint32_t>(target) + 1, std::memory_order_release);", __LINE__);
    Pin(outputSwitch, "const uint32_t posted = g_tryAgainPending.exchange(0, std::memory_order_acq_rel);", __LINE__);
    Pin(outputSwitch, "Config::Instance()->FGOutput.set_volatile_value(OutputOf(due));", __LINE__);
    // Neither seam may hand the judgement a start time where it wants a duration.
    {
        const size_t duration = outputSwitch.find("OnFgPresentResult(swap.state, ops, result, presentMs)");
        const size_t start = outputSwitch.find("static_cast<uint32_t>(presentStartMs)");
        if (duration == std::string::npos || start != std::string::npos)
        {
            std::printf("    FAIL: the present result is not given a duration\n");
            ++g_failed;
        }
    }

    std::printf("  the switch state belongs to the bridge it was read from\n");
    Pin(sequence, "inline bool OwnedBy(SwitchState& state, void*& owner, void* bridge)", __LINE__);
    Pin(sequence, "const Policy policy = state.policy;", __LINE__);
    // One state object: the sequence's runs on the swap's own state, which is what the two present seams are handed.
    // A second SwitchState made probation, its fault and the limp window inert in the product.
    Pin(outputSwitch, "SwitchState& g_state = g_swap.state;", __LINE__);
    CHECK(CountOf(outputSwitch, "SwitchState g_state") == 0);
    // A new bridge can reuse the freed one's address, so the attach point resets the state as well as the address compare.
    Pin(outputSwitch, "g_state = SwitchState {};", __LINE__);
    // The old provider's captured resources are not handed to the new chain, and "captured only" ends with the old pair.
    Pin(outputSwitch, "state.fgResetCapturedResources = true;", __LINE__);
    Pin(outputSwitch, "state.fgOnlyUseCapturedResources = false;", __LINE__);
    // The plain chain comes from the bridge's own factory: the wrapped create hands back a wrapped chain, which the
    // bridge must not present (it drives its own interop and overlay on the real chain).
    // ...and a capture without it refuses: the captured factory may be the wrapped one.
    Pin(outputSwitch, "if (!provider::CaptureRealFactory(bridge, out))", __LINE__);
    // A rollback or a limp creates plain from the descriptor the last forward capture read on the window, never from the
    // global one a library's inner chain rewrites on every present; the record is written only after a whole capture.
    {
        const std::string capturing = Normalize(Read(repo + "/OptiScaler/ext/fg/outswitch/ProviderOps.cpp"));
        Pin(capturing, "if (recordedBridge_ == bridge && recordedDesc_.OutputWindow != nullptr)", __LINE__);
        const size_t choose = outputSwitch.find("if (!provider::ChooseCreationQueue(bridge, out))");
        const size_t record = outputSwitch.find("provider::RecordChainDesc(bridge, out.desc);");
        if (choose == std::string::npos || record == std::string::npos || record < choose)
        {
            std::printf("    FAIL: the chain descriptor is not recorded after the whole capture\n");
            ++g_failed;
        }
    }
    // The removed reason of a lost device is logged once per bridge, and the latch is reset with the bridge.
    Pin(outputSwitch, "if (DeviceLost(result) && !swap.deviceLostLogged)", __LINE__);
    Pin(outputSwitch, "g_swap.deviceLostLogged = false;", __LINE__);
    // A scripted step answered by a refusal does not leave why=script for the next config-driven attempt.
    Pin(outputSwitch, "case Step::Begin: case Step::Refused: g_scriptWhy = false; break;", __LINE__);
    // The pin follows the bridge's queue (QueueRules.h RepinNeeded), the new reference before the old one goes.
    {
        const std::string pinning = Normalize(Read(repo + "/OptiScaler/ext/fg/outswitch/ProviderOps.cpp"));
        Pin(pinning, "if (!queues::RepinNeeded(pinnedQueue_, queue))", __LINE__);
        const size_t addRef = pinning.find("queue->AddRef(); UnpinBridgeQueue();");
        if (addRef == std::string::npos)
        {
            std::printf("    FAIL: the new pin is not taken before the old one is given back\n");
            ++g_failed;
        }
        CHECK(CountOf(pinning, "pinnedBridge_ == bridge") == 0);
    }
    Pin(outputSwitch, "LOG_WARN(\"[fg-switch] the D3D12 device was removed: reason=0x{:08X}\",", __LINE__);
    // The XeFG directions come from the product's table, one by one (SwitchPolicy.h XeFgDirectionAllowed).
    Pin(outputSwitch, "inputs.xeFgSwitchAllowed = XeFgDirectionAllowed(kXeFgDirections, inputs.active, inputs.target);", __LINE__);
    Pin(outputSwitch, "constexpr XeFgDirections kXeFgDirections {};", __LINE__);
    const std::string plainFactorySource = Normalize(Read(repo + "/OptiScaler/ext/fg/outswitch/ProviderOps.cpp"));
    Pin(plainFactorySource, "bool CaptureRealFactory(void* bridge, Captured& out)", __LINE__);
    Pin(plainFactorySource, "capture.realFactory != nullptr ? static_cast<void*>(capture.realFactory)", __LINE__);
    // The present's start is stamped on every present, before the key check: the seam's duration never spans a key-off.
    {
        const size_t stamp = outputSwitch.find("g_swap.presentStartMs = start;");
        const size_t keyOff = outputSwitch.find("if (!keyOn && !g_state.probation.RollbackDue())");
        if (stamp == std::string::npos || keyOff == std::string::npos || stamp > keyOff)
        {
            std::puts("    FAIL: the present start is not stamped before the key check");
            ++g_failed;
        }
    }

    std::printf("  every step is one [fg-switch] line, the recorder sees the eight milestones, and the adopt line carries the replay\n");
    Pin(outputSwitch, "FormatSwitchLine(f, line);", __LINE__);
    Pin(outputSwitch, "AddSwitchObserver(&RecorderSink);", __LINE__);
    Pin(outputSwitch, "Notify(notification);", __LINE__);
    Pin(outputSwitch, "ext::fgrec::OnSwitch(RecorderKind(event.phase), RecorderProvider(event.from), RecorderProvider(event.to),", __LINE__);
    Pin(sequence, "event.chain = created.chain;", __LINE__);
    Pin(sequence, "teardown.chain = oldChain;", __LINE__);
    {
        // The adopt line is reported after the replay: it carries what the replay put back (the colour space, HDR).
        const size_t replay = sequence.find("if (!ops.Replay(created.chain))");
        const size_t adopt = sequence.find("ops.Report(adopt);", replay == std::string::npos ? 0 : replay);
        if (replay == std::string::npos || adopt == std::string::npos || adopt < replay)
        {
            std::printf("    FAIL: the adopt line is reported before the replay\n");
            ++g_failed;
        }
    }
    const std::string providerOps = Normalize(Read(repo + "/OptiScaler/ext/fg/outswitch/ProviderOps.cpp"));
    Pin(providerOps, "snapshot.current = state.currentSwapchain;", __LINE__);
    Pin(providerOps, "state.currentSwapchain = snapshot.current;", __LINE__);

    std::printf("  every chain on the bridge's pinned queue, the interop given back its queue, the provider created as boot does\n");
    // Rule 1 and 2 (QueueRules.h): the capture pins the bridge's queue before anything is torn down and creates on it. A
    // chain created on the global queue lands on the old library's own queue, which that library's destroy frees.
    Pin(providerOps, "void* queue = queues::CreationQueue(PinBridgeQueue(bridge), ::State::Instance().currentCommandQueue);", __LINE__);
    Pin(providerOps, "ID3D12CommandQueue* queue = Access::Bridge::CopyQueue(*static_cast<Dx11wDx12SC*>(bridge));", __LINE__);
    Pin(providerOps, "queue->AddRef();", __LINE__);
    CHECK(CountOf(providerOps, "out.queue = ::State::Instance().currentCommandQueue;") == 0);
    Pin(outputSwitch, "if (!provider::ChooseCreationQueue(bridge, out))", __LINE__);
    Pin(outputSwitch, "provider::UnpinBridgeQueue();", __LINE__);
    {
        // The bridge capture (the rollback and the limp) chooses the same queue.
        const size_t bridgeCapture = providerOps.find("bool CaptureBridge(void* bridge, Captured& out)");
        const size_t choose = providerOps.find("if (!ChooseCreationQueue(bridge, out))", bridgeCapture);
        const size_t release = providerOps.find("void ReleaseCapture(Captured& capture)", bridgeCapture);
        if (bridgeCapture == std::string::npos || choose == std::string::npos || release == std::string::npos ||
            choose > release)
        {
            std::printf("    FAIL: the bridge capture does not choose the pinned creation queue\n");
            ++g_failed;
        }
    }
    // Rule 3: the interop's device, queue and list type are part of the snapshot, and the restore gives the interop back
    // its queue BEFORE the globals, because the interop's setter writes the global device and queue too.
    Pin(providerOps, "snapshot.interopQueue = WithDx12::GetD3D12CommandQueue();", __LINE__);
    Pin(providerOps, "queues::InteropAfterCreate(snapshot.interopQueue, WithDx12::GetD3D12CommandQueue()));", __LINE__);
    Pin(providerOps, "WithDx12::SetD3D12Objects(device, queue, static_cast<D3D12_COMMAND_LIST_TYPE>(snapshot.interopListType));", __LINE__);
    {
        const size_t restore = providerOps.find("void RestoreState(const StateSnapshot& snapshot)");
        const size_t interop = providerOps.find("WithDx12::SetD3D12Objects(", restore);
        const size_t globals = providerOps.find("state.currentCommandQueue = snapshot.commandQueue;", restore);
        if (restore == std::string::npos || interop == std::string::npos || globals == std::string::npos ||
            interop > globals)
        {
            std::printf("    FAIL: the interop is not restored before the globals\n");
            ++g_failed;
        }
    }
    // The provider create is boot's call: upstream's CreateSwapChain with both factories' FG swapchain creation skipped,
    // so the library's own inner create cannot run the provider's create a second time.
    Pin(providerOps, "Access::CreateProviderSwapChain(factory, capture.queue, &desc, reinterpret_cast<void**>(&chain)));", __LINE__);
    CHECK(CountOf(providerOps, "FGHooks::CreateSwapChain(") == 0);
    const std::string factoryGuards = Normalize(Read(repo + "/OptiScaler/ext/fg/outswitch/FactoryGuards.cpp"));
    {
        const size_t entry = factoryGuards.find("long ext::Access::CreateProviderSwapChain(void* factory, void* device, void* desc, void** chain)");
        const size_t hooked = factoryGuards.find("DxgiFactoryHooks::ScopedSkipFGSCCreation skipHookedFactory {};", entry);
        const size_t wrapped = factoryGuards.find("DxgiFactoryWrappedCalls::ScopedSkipFGSCCreation skipWrappedFactory {};", entry);
        const size_t call = factoryGuards.find("return FGHooks::CreateSwapChain(realFactory, static_cast<IUnknown*>(device), swapchainDesc, out);", entry);
        if (entry == std::string::npos || hooked == std::string::npos || wrapped == std::string::npos ||
            call == std::string::npos || hooked > call || wrapped > call)
        {
            std::printf("    FAIL: the provider create is not inside both factory guards\n");
            ++g_failed;
        }
    }
    // The drain reaches the old provider's own game queue as well as the global and the copy queue.
    Pin(outputSwitch, "result.game = ext::gpu::DrainedOk(ext::gpu::DrainQueueEx(game)) && result.game;", __LINE__);
    // ...and the library's own queue of a provider the switch created (its present and the overlay run there).
    Pin(outputSwitch, "result.fg = ext::gpu::DrainedOk(ext::gpu::DrainQueueEx(library)) && result.fg;", __LINE__);
    Pin(providerOps, "libraryQueue_ = trace_.interopClobbered;", __LINE__);
    // The queue line the benchmark reads.
    Pin(outputSwitch, "LOG_INFO(\"[fg-switch] queues pinned={:X} creation={:X} interopBefore={:X} libraryLeft={:X} interopNow={:X} \"", __LINE__);
    const std::string bridgeSwap = Normalize(Read(repo + "/OptiScaler/ext/fg/outswitch/BridgeSwap.cpp"));
    Pin(bridgeSwap, "::State::Instance().currentSwapchain = static_cast<IDXGISwapChain*>(bridge);", __LINE__);
    Pin(bridgeSwap, "::State::Instance().currentWrappedSwapchain = static_cast<IDXGISwapChain*>(bridge);", __LINE__);

    std::printf("  the judgement of the live chain runs before the idle path\n");
    // The rollback has to be handled before Wanted(): after a commit the configured output is the active one, so the idle
    // path would otherwise swallow the rollback of a chain that fails its presents.
    const size_t rollback = sequence.find("if (state.probation.RollbackDue())");
    const size_t wanted = sequence.find("if (!ops.Wanted())");
    if (rollback == std::string::npos || wanted == std::string::npos || rollback > wanted)
    {
        std::printf("    FAIL: the due rollback is not handled before Wanted()\n");
        ++g_failed;
    }
    Pin(sequence, "state.policy.PauseTransient(ops.NowMs(), target, reason);", __LINE__);
    Pin(sequence, "Fail::HookBusy", __LINE__);
    Pin(lifetime, "Fail::WitnessUnavailable", __LINE__);
    Pin(lifetime, "const bool pinFirst = !plain && releasePin && row.pinFirst;", __LINE__);
    Pin(lifetime, "out.refsAfterPin = ops.ReleasePin(live.chain);", __LINE__);
    Pin(lifetime, "Fail::OldChainLeaked", __LINE__);
    Pin(lifetime, "out.touched = true;", __LINE__);
    // The two orders are one statement each, and the reference the row releases first is never released twice.
    CHECK(CountOf(lifetime, "ops.ReleaseSwapchain(live.provider, live.hwnd)") == 2);
    CHECK(CountOf(lifetime, "ops.ReleasePin(live.chain)") == 2);
    // A pin-first teardown releases the reference before the destroy, and destroys exactly once.
    {
        const size_t pin = lifetime.find("out.refsAfterPin = ops.ReleasePin(live.chain);");
        const size_t destroy = lifetime.find("const bool released = ops.ReleaseSwapchain(live.provider, live.hwnd);", pin);
        const size_t witness = lifetime.find("if (witness && out.witnessAlive)", destroy);
        if (pin == std::string::npos || destroy == std::string::npos || witness == std::string::npos || pin > destroy ||
            destroy > witness)
        {
            std::printf("    FAIL: the pin-first teardown does not release, then destroy once, then check the witness\n");
            ++g_failed;
        }
    }
    Pin(sequence, "if (state.managed)", __LINE__);
    Pin(sequence, "if (unhookAgain)", __LINE__);
    Pin(sequence, "Fail::Rehook", __LINE__);
    Pin(policy, "void PauseTransient(uint64_t nowMs, Target target, Fail why)", __LINE__);
    Pin(policy, "inline constexpr uint32_t kTransientRetryMaxMs = 30000;", __LINE__);
    Pin(policy, "inline constexpr uint32_t kTransientGiveUp = 10;", __LINE__);
    Pin(policy, "bool witnessUsable = true;", __LINE__);
    std::printf("    [ok] rollback before the key check, a transient refusal pauses, a busy detach is not a failure\n");

    std::printf("  the hook gate closes before it reads the count\n");
    Pin(hookLife, "GateEntry Enter()", __LINE__);
    Pin(hookLife, "if (draining_.load(std::memory_order_seq_cst))", __LINE__);
    Pin(hookLife, "inFlight_.fetch_add(1, std::memory_order_seq_cst);", __LINE__);
    std::printf("    [ok] an entry gate, a sequentially consistent count, a late entrant that is not counted\n");
    std::printf("    [ok] clear before release, barrier only for the two XeFG moves\n");

    if (g_failed == 0)
        std::printf("ALL SOURCE PINS MATCH\n");
    else
        std::printf("%d PIN(S) FAILED\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
