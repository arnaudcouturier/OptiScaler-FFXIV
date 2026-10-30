// S10 package B: the upstream attachment points (ext/hudless/HudlessSeams.h) are inline wrappers on the feature bit
// ext::F::Hudless. With the bit clear (mode off after the first present) every seam returns at once and never reaches the
// engine; BeforeOverlay also ignores D3D11 chains and a null queue. The detail:: bodies are counted by this test instead
// of the engine.
//
// The developer probe (ext::F::PlateProbe, [HudLess] PlateDiagnostics) and the native nameplate engine (ext::F::Plate,
// [HudLess] PlateInterp) are further consumers of two of the seams (the real-frame point and the output hook) plus the
// multiplier note and the release hook. Each runs only with its own bit: with one bit alone the other bodies are never
// reached; with all bits clear nothing is reached at all (the keys-off rule).
#include "ext/hudless/HudlessSeams.h"

#include <cassert>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

class Dx11wDx12SC
{
};

namespace
{
// 0 PreInteropCopy, 1 UiFreeFrame, 2 PostInteropCopy (engine), 3 OnReleaseInterop, 4 BeforeOverlay (engine),
// 5 PlateProbePost, 6 PlateProbeExpected, 7 PlateProbeOutput, 8 plates::OnPost, 9 plates::OnRelease, 10 plates::OnOverlay,
// 11 plates::OnPresentBegin
constexpr int kCalls = 12;
int calls[kCalls] = {};
uint32_t lastMultiplier = 99;
} // namespace

namespace ext::hudless::detail
{
void PreInteropCopy(Dx11wDx12SC&) { ++calls[0]; }
void UiFreeFrame(Dx11wDx12SC&, unsigned int, ID3D12Resource*&, D3D12_RESOURCE_STATES&) { ++calls[1]; }
void PostInteropCopy(Dx11wDx12SC&) { ++calls[2]; }
void OnReleaseInterop(Dx11wDx12SC&) { ++calls[3]; }
void BeforeOverlay(IDXGISwapChain*, ID3D12CommandQueue*) { ++calls[4]; }
void PlateProbePost(Dx11wDx12SC&) { ++calls[5]; }
void PlateProbeExpected(uint32_t m)
{
    ++calls[6];
    lastMultiplier = m;
}
void PlateProbeOutput(IDXGISwapChain*, ID3D12CommandQueue*) { ++calls[7]; }
} // namespace ext::hudless::detail

// The plate engine's seam bodies are not linked here (the engine has its own tests): the seams are counted instead.
namespace ext::hudless::plates
{
void OnPost(Dx11wDx12SC&) { ++calls[8]; }
void OnRelease() { ++calls[9]; }
void OnOverlay(IDXGISwapChain*, ID3D12CommandQueue*) { ++calls[10]; }
void OnPresentBegin() { ++calls[11]; }
} // namespace ext::hudless::plates

namespace
{
std::string Strip(const std::string& text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text.compare(i, 2, "//") == 0)
        {
            while (i < text.size() && text[i] != '\n')
                ++i;
            continue;
        }
        if (!isspace(static_cast<unsigned char>(text[i])))
            out += text[i];
    }
    return out;
}

// the text of a function between its signature and the closing of the namespace-level block after it
std::string Body(const std::string& code, const std::string& signature, const std::string& next)
{
    const size_t a = code.find(signature);
    assert(a != std::string::npos);
    const size_t b = code.find(next, a + signature.size());
    assert(b != std::string::npos);
    return code.substr(a, b - a);
}
} // namespace

int main(int, char** argv)
{
    Dx11wDx12SC bridge;
    ID3D12Resource* source = reinterpret_cast<ID3D12Resource*>(0x10);
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    auto* swapChain = reinterpret_cast<IDXGISwapChain*>(0x20);
    auto* queue = reinterpret_cast<ID3D12CommandQueue*>(0x30);

    auto total = [] { int n = 0; for (int c : calls) n += c; return n; };
    auto fireAll = [&]
    {
        ext::hudless::PreInteropCopy(bridge);
        ext::hudless::UiFreeFrame(bridge, 1, source, state);
        ext::hudless::PostInteropCopy(bridge);
        ext::hudless::OnReleaseInterop(bridge);
        ext::present::BeforeOverlay(swapChain, queue, false);
        ext::hudless::NoteExpectedMultiplier(3);
        ext::hudless::OnPresentBegin();
    };
    auto reset = []
    {
        for (int& c : calls) c = 0;
        lastMultiplier = 99;
    };

    // the probe bit and the plate bit are their own bits, and the defaults are all clear
    static_assert(ext::F::PlateProbe != ext::F::Hudless);
    static_assert(ext::F::Plate != ext::F::Hudless && ext::F::Plate != ext::F::PlateProbe);
    {
        // one bit of its own: not the bit of any other feature (a shared bit would switch it on with that feature)
        constexpr uint64_t plate = static_cast<uint64_t>(ext::F::Plate);
        constexpr uint64_t probe = static_cast<uint64_t>(ext::F::PlateProbe);
        constexpr uint64_t others = static_cast<uint64_t>(ext::F::Nr) | static_cast<uint64_t>(ext::F::Fg) | static_cast<uint64_t>(ext::F::Hdr) |
                                    static_cast<uint64_t>(ext::F::Hudless) | static_cast<uint64_t>(ext::F::Gpu) | static_cast<uint64_t>(ext::F::Restrack) |
                                    static_cast<uint64_t>(ext::F::Present) | static_cast<uint64_t>(ext::F::Streamline) |
                                    static_cast<uint64_t>(ext::F::Dx11Bridge) | static_cast<uint64_t>(ext::F::Upscaler) |
                                    static_cast<uint64_t>(ext::F::Game);
        for (const uint64_t bit : { plate, probe })
        {
            assert(bit != 0 && (bit & (bit - 1)) == 0);
            assert((bit & others) == 0);
        }
        static_assert(ext::F::PlateProbe != ext::F::Game);
    }
    assert(!ext::Active(ext::F::Hudless) && !ext::Active(ext::F::PlateProbe) && !ext::Active(ext::F::Plate));
    assert(!ext::ActiveAny(ext::F::Hudless, ext::F::PlateProbe));
    assert(!ext::ActiveAny(ext::F::Hudless, ext::F::PlateProbe, ext::F::Plate));

    // all bits clear: nothing reaches the engine, the plate engine or the probe, the in/out arguments are left alone
    fireAll();
    assert(total() == 0 && source == reinterpret_cast<ID3D12Resource*>(0x10) && state == D3D12_RESOURCE_STATE_COMMON);
    assert(lastMultiplier == 99 && ext::hudless::plates::ExpectedMultiplier() == 0);

    // engine bit set: every engine seam reaches its body exactly once, the probe and the plate engine are not touched
    ext::SetActive(ext::F::Hudless, true);
    assert(ext::ActiveAny(ext::F::Hudless, ext::F::PlateProbe) && !ext::Active(ext::F::PlateProbe));
    fireAll();
    for (int i = 0; i < 5; ++i)
        assert(calls[i] == 1);
    for (int i = 5; i < kCalls; ++i)
        assert(calls[i] == 0);
    assert(lastMultiplier == 99 && ext::hudless::plates::ExpectedMultiplier() == 0);

    // the overlay seam ignores a D3D11 chain and a null queue
    ext::present::BeforeOverlay(swapChain, queue, true);
    ext::present::BeforeOverlay(swapChain, nullptr, false);
    assert(calls[4] == 1 && calls[7] == 0 && calls[10] == 0);

    // the engine clears the bit once it saw mode off: quiet again
    ext::SetActive(ext::F::Hudless, false);
    fireAll();
    for (int i = 0; i < 5; ++i)
        assert(calls[i] == 1);

    // the probe alone: the real-frame point, the output hook and the multiplier note reach the probe; the engine and the
    // plate engine are never reached, not even at the seams the three share, and the engine-only seams stay quiet
    reset();
    ext::SetActive(ext::F::PlateProbe, true);
    fireAll();
    for (int i = 0; i < 5; ++i)
        assert(calls[i] == 0);
    assert(calls[5] == 1 && calls[6] == 1 && calls[7] == 1 && lastMultiplier == 3);
    assert(calls[8] == 0 && calls[9] == 0 && calls[10] == 0 && calls[11] == 0 &&
           ext::hudless::plates::ExpectedMultiplier() == 0);
    ext::present::BeforeOverlay(swapChain, queue, true);
    ext::present::BeforeOverlay(swapChain, nullptr, false);
    assert(calls[7] == 1 && calls[4] == 0 && calls[10] == 0);

    // the plate engine alone: the frame seam, the release seam, the output hook and the multiplier note reach it; neither
    // the HUD-less engine nor the probe does
    reset();
    ext::SetActive(ext::F::PlateProbe, false);
    ext::SetActive(ext::F::Plate, true);
    fireAll();
    for (int i = 0; i < 8; ++i)
        assert(calls[i] == 0);
    assert(calls[8] == 1 && calls[9] == 1 && calls[10] == 1 && calls[11] == 1);
    assert(ext::hudless::plates::ExpectedMultiplier() == 3 && lastMultiplier == 99);
    ext::present::BeforeOverlay(swapChain, queue, true);
    ext::present::BeforeOverlay(swapChain, nullptr, false);
    assert(calls[10] == 1 && calls[4] == 0 && calls[7] == 0);

    // all bits: each consumer once
    reset();
    ext::hudless::plates::NoteMultiplier(0);
    ext::SetActive(ext::F::Hudless, true);
    ext::SetActive(ext::F::PlateProbe, true);
    fireAll();
    for (int c : calls)
        assert(c == 1);
    assert(lastMultiplier == 3 && ext::hudless::plates::ExpectedMultiplier() == 3);

    // the probe switched off again (a failed write does this): the probe is quiet, the engines are not
    reset();
    ext::SetActive(ext::F::PlateProbe, false);
    fireAll();
    for (int i = 0; i < 5; ++i)
        assert(calls[i] == 1);
    for (int i = 5; i < 8; ++i)
        assert(calls[i] == 0);
    assert(calls[8] == 1 && calls[9] == 1 && calls[10] == 1);
    ext::SetActive(ext::F::Hudless, false);
    ext::SetActive(ext::F::Plate, false);
    reset();
    fireAll();
    assert(total() == 0);

    // text pin of the guards: only the shared seams test the bits they serve, the probe is called before the plate engine
    // and the plate engine before the HUD-less engine, and the two seams that serve neither stay exactly what they were
    std::ifstream file(std::string(argv[1]) + "/OptiScaler/ext/hudless/HudlessSeams.h", std::ios::binary);
    assert(file);
    const std::string code = Strip(std::string(std::istreambuf_iterator<char>(file), {}));
    const std::string post = Body(code, "inlinevoidPostInteropCopy(Dx11wDx12SC&sc)", "inlinevoidOnReleaseInterop");
    assert(post.find("if(ext::ActiveAny(ext::F::Hudless,ext::F::PlateProbe,ext::F::Plate)){") != std::string::npos);
    assert(post.find("if(ext::Active(ext::F::PlateProbe))detail::PlateProbePost(sc);") != std::string::npos);
    assert(post.find("if(ext::Active(ext::F::Plate))ext::hudless::plates::OnPost(sc);") != std::string::npos);
    assert(post.find("if(ext::Active(ext::F::Hudless))detail::PostInteropCopy(sc);") != std::string::npos);
    assert(post.find("detail::PlateProbePost(sc)") < post.find("plates::OnPost(sc)"));
    assert(post.find("plates::OnPost(sc)") < post.find("detail::PostInteropCopy(sc)"));
    const std::string overlay = code.substr(code.find("inlinevoidBeforeOverlay(IDXGISwapChain*swapChain"));
    assert(overlay.find("if(!isD3D11&&queue!=nullptr&&ext::ActiveAny(ext::F::Hudless,ext::F::PlateProbe,ext::F::Plate)){") != std::string::npos);
    assert(overlay.find("if(ext::Active(ext::F::PlateProbe))hudless::detail::PlateProbeOutput(swapChain,queue);") != std::string::npos);
    assert(overlay.find("if(ext::Active(ext::F::Plate))ext::hudless::plates::OnOverlay(swapChain,queue);") != std::string::npos);
    assert(overlay.find("if(ext::Active(ext::F::Hudless))hudless::detail::BeforeOverlay(swapChain,queue);") != std::string::npos);
    assert(overlay.find("PlateProbeOutput") < overlay.find("plates::OnOverlay(swapChain,queue)"));
    assert(overlay.find("plates::OnOverlay(swapChain,queue)") < overlay.find("hudless::detail::BeforeOverlay(swapChain,queue)"));
    const std::string pre = Body(code, "inlinevoidPreInteropCopy(Dx11wDx12SC&sc)", "inlinevoidUiFreeFrame");
    const std::string swap = Body(code, "inlinevoidUiFreeFrame(", "inlinevoidPostInteropCopy");
    for (const std::string* seam : { &pre, &swap })
    {
        assert(seam->find("Plate") == std::string::npos);
        assert(seam->find("if(ext::Active(ext::F::Hudless))") != std::string::npos);
    }
    const std::string release = Body(code, "inlinevoidOnReleaseInterop(Dx11wDx12SC&sc)", "inlinevoidNoteExpectedMultiplier");
    assert(release.find("if(ext::Active(ext::F::Plate))ext::hudless::plates::OnRelease();") != std::string::npos);
    assert(release.find("if(ext::Active(ext::F::Hudless))detail::OnReleaseInterop(sc);") != std::string::npos);
    // the present entry: the plate engine's frame boundary, and nothing else hangs on it
    assert(code.find("inlinevoidOnPresentBegin(){if(ext::Active(ext::F::Plate))ext::hudless::plates::OnPresentBegin();}") !=
           std::string::npos);
    const std::string note = code.substr(code.find("inlinevoidNoteExpectedMultiplier"));
    assert(note.find("inlinevoidOnPresentBegin") != std::string::npos);
    assert(note.find("if(ext::ActiveAny(ext::F::PlateProbe,ext::F::Plate)){") != std::string::npos);
    assert(note.find("if(ext::Active(ext::F::Plate))ext::hudless::plates::NoteMultiplier(outputsPerFrame);") != std::string::npos);
    assert(note.find("if(ext::Active(ext::F::PlateProbe))detail::PlateProbeExpected(outputsPerFrame);") != std::string::npos);
    assert(note.find("F::Hudless") == std::string::npos || note.find("F::Hudless") > note.find("}")); // the note never reaches the HUD-less engine

    std::puts("PASS: seam wrappers are inert with every feature bit clear, each engine runs only with its own bit");
    return 0;
}
