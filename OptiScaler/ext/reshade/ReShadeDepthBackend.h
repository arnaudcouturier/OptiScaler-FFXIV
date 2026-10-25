#pragma once
// Automatic ReShade scene depth, the live backend of the depth binder (ReShadeDepthCore.h): it implements depth::IBackend over a
// small raw layer, IRawApi, one function per ReShade API call. No ReShade, D3D or ImGui types here, so the marshalling is tested
// with a fake raw layer (tests/ext/reshade/reshade_depth_backend.cpp); the real one (ReShadeDepthRaw.cpp) is the only file that
// touches effect_runtime, and every call it makes runs in a POD thunk under SEH (ReShade's handles are untrusted: a view read
// from its binding map may be dangling).
//
// What the backend does on top of the raw calls:
//  - Strings (variable, annotation, effect and technique names, preprocessor definitions) are read with ReShade's size query first.
//    The contract, OBSERVED in game 2026-10-25 (ReShade 6.8.0.2155; the vendored header's "including the null-terminator" is only
//    right for the query): a query with a null buffer returns the length + 1 (the NUL counted); a copy with capacity N writes
//    min(N - 1, length) characters, then the NUL, and sets *size to the NUMBER OF CHARACTERS COPIED, WITHOUT the NUL (it is
//    std::string::copy). So the backend asks for need, copies into need + 1, and accepts the string only if a NUL ends it exactly at
//    need - 1: a string that grew in between would come back one character longer (not cut), one that shrank or holds an interior NUL
//    shorter. The second *size is only required to be <= the capacity, which accepts the observed behaviour and the header's wording.
//    There is never a fixed-size overload, so a value is never silently cut ("1.05" must not read as "1.0" and be taken for the neutral
//    macro value). A missing string, an absurd size (0 or more than 64 KiB) or a second answer that disagrees is a failure.
//  - A definition is tri-state: undefined, defined, error.
//  - A call that faults makes the method answer false / Error / throw (see IBackend): the core never lets either reach ReShade.
//  - SameDevice: ReShade hands the game PROXY objects. Measured against ReShade 6.8.0.2155 on 2026-10-25: D3D11CreateDeviceAndSwapChain
//    returns a proxy device, context and swap chain; context->GetDevice() and GetDevice() of any resource created on them answer the
//    proxy; the runtime's device::get_native() is the REAL d3d11.dll device, and never equals a pointer the game sees (the game log:
//    0x228aa25f390 = get_native() against the game's 0x228b20940d0). The capture's device is GetDevice() of the game's depth texture,
//    i.e. the proxy. So a match is any of: get_native() == the target, or GetDevice() of the runtime's back buffer == the target (that
//    answer was the native device early in a process and the proxy later in the offline harness: mechanism unknown, hence not the only
//    rule); and otherwise the ONLY D3D11 runtime of the process counts as the game's. FFXIV has exactly one D3D11 device, one
//    proxy/native pair, so one runtime is the game's runtime; with two or more D3D11 runtimes the strict comparison stands and a
//    runtime that matches neither pointer is refused (both pointers go to the log, once).
//  - The first real bind: the readback of ReShade's binding after the first update_texture_bindings with a view is logged once.
//  - What the headers cannot settle is logged once each, so the first run in the game answers it: the first string that breaks the size
//    contract (query kind, both sizes, the length found), the first refusal of a runtime by a device that differs once a capture
//    exists (both devices and the primary one), and the texture names that contain "depth" of a runtime with no depth input.
//
// Threading: the binder calls the backend under its own runtime mutex from whatever thread the ReShade event arrives on; the
// registry and the capture device are the only state shared with other threads, and are guarded.

#include "ext/reshade/ReShadeDepthCore.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ext::reshade::depth
{
// ---- depth formats --------------------------------------------------------------------------------------------------------------
// Formats ReShade (and DXGI) use for depth buffers and the single-channel float copies of them, by their numeric value: ReShade's
// format enum has the DXGI numbering (the live raw layer static_asserts every value). One definition, shared with the probe
// (ReShadeEvents.cpp IsDepthFormat).
inline constexpr uint32_t kFmtR32G8X24Typeless = 19, kFmtD32FloatS8X24 = 20, kFmtR32FloatX8X24 = 21, kFmtR32Typeless = 39,
                          kFmtD32Float = 40, kFmtR32Float = 41, kFmtR24G8Typeless = 44, kFmtD24UnormS8 = 45,
                          kFmtR24UnormX8 = 46, kFmtX24TypelessG8 = 47, kFmtR16Typeless = 53, kFmtD16Unorm = 55,
                          kFmtR16Unorm = 56, kFmtD16UnormS8 = 0x38363144; // 'D168', ReShade only
constexpr bool IsDepthFormatValue(uint32_t format)
{
    switch (format)
    {
    case kFmtD16Unorm:
    case kFmtD16UnormS8:
    case kFmtD24UnormS8:
    case kFmtD32Float:
    case kFmtD32FloatS8X24:
    case kFmtR16Typeless:
    case kFmtR16Unorm:
    case kFmtR24G8Typeless:
    case kFmtR24UnormX8:
    case kFmtX24TypelessG8:
    case kFmtR32Typeless:
    case kFmtR32Float:
    case kFmtR32G8X24Typeless:
    case kFmtR32FloatX8X24:
        return true;
    default:
        return false;
    }
}

// ---- the raw layer --------------------------------------------------------------------------------------------------------------
// Answer of a call that can also say "does not exist".
enum class Call : uint8_t
{
    Fault, // the call faulted (an access violation or a C++ exception): outputs are unspecified
    No,    // it ran and answered "not there" (an annotation or a definition that does not exist)
    Yes,
};

// One function per ReShade API call (a few small groups). Runtimes, variables, techniques and views are opaque 64-bit handles: a
// runtime is the effect_runtime pointer, a view an ID3D11View pointer. A bool result is false when the call faulted.
// Strings follow ReShade's convention (observed 2026-10-25, see the top of this file): buffer == nullptr asks for the length + 1; with a
// buffer `*size` is the capacity on entry (it includes the NUL) and the number of characters copied, without the NUL, on return. A
// fake that wants the header's wording instead returns the count including the NUL; the backend accepts both.
struct IRawApi
{
    virtual ~IRawApi() = default;

    // the runtime's device: hasDevice false when it has none; d3d11 when its API is D3D11; native = device::get_native()
    virtual bool DeviceInfo(uint64_t runtime, bool& hasDevice, bool& d3d11, uint64_t& native) = 0;
    // the size of back buffer 0; 0x0 when there is none or it is not a 2D texture
    virtual bool BackBuffer(uint64_t runtime, uint32_t& width, uint32_t& height) = 0;
    // The device of the resource behind back buffer 0 (ID3D11Resource::GetDevice, the pointer value; the reference taken is given back);
    // 0 when there is no back buffer. true means the call ran without a fault.
    virtual bool BackBufferDevice(uint64_t runtime, uint64_t& device) = 0;

    // Enumerations. The handles go to out[0..min(count, cap)); `count` is how many ReShade reported, also beyond the capacity.
    virtual bool ListTextures(uint64_t runtime, uint64_t* out, size_t cap, size_t& count) = 0;
    virtual bool ListUniforms(uint64_t runtime, uint64_t* out, size_t cap, size_t& count) = 0;
    virtual bool ListTechniques(uint64_t runtime, uint64_t* out, size_t cap, size_t& count) = 0;

    virtual Call TextureName(uint64_t runtime, uint64_t variable, char* buffer, size_t* size) = 0;
    virtual Call UniformSource(uint64_t runtime, uint64_t variable, char* buffer, size_t* size) = 0; // annotation "source"
    virtual Call TechniqueEffect(uint64_t runtime, uint64_t technique, char* buffer, size_t* size) = 0;
    // effect == nullptr: the global definition
    virtual Call Definition(uint64_t runtime, const char* effect, const char* name, char* buffer, size_t* size) = 0;
    virtual bool TechniqueState(uint64_t runtime, uint64_t technique, bool& enabled) = 0;

    virtual bool TextureBinding(uint64_t runtime, uint64_t variable, uint64_t& srv, uint64_t& srvSrgb) = 0;
    // The live ReShade call returns void: true means it ran without a fault.
    virtual bool UpdateTextureBindings(uint64_t runtime, const char* semantic, uint64_t srv, uint64_t srvSrgb) = 0;
    virtual bool GetUniformBool(uint64_t runtime, uint64_t variable, bool& value) = 0;
    virtual bool SetUniformBool(uint64_t runtime, uint64_t variable, bool value) = 0;

    // COM views (untrusted handles from ReShade's binding map)
    virtual bool AddRefView(uint64_t view) = 0;
    virtual bool ReleaseView(uint64_t view) = 0;
    // The numeric format of the view's resource when that is a 2D texture, 0 otherwise.
    virtual bool ViewFormat(uint64_t view, uint32_t& format) = 0;
};

// The one raw layer of the process (heap-allocated, never destroyed). Defined in ReShadeDepthRaw.cpp.
IRawApi& LiveRawApi();

// ---- what the events need from the backend ----------------------------------------------------------------------------------------
struct IEventHost
{
    virtual ~IEventHost() = default;
    virtual void NoteRuntime(uint64_t runtime) = 0;   // a begin or finish of this runtime was seen
    virtual void ForgetRuntime(uint64_t runtime) = 0; // it was destroyed
};

// ---- the backend ----------------------------------------------------------------------------------------------------------------
// What the backend takes from the outside (so tests supply their own). All may be null: the clock then reads 0, the frame 0 and the
// primary device is unknown.
struct BackendEnv
{
    int64_t (*nowMs)() = nullptr;           // lock-free and cheap: the binder calls it under its runtime mutex
    uint64_t (*frame)() = nullptr;          // the depth-owned frame counter
    uint64_t (*primaryDevice)() = nullptr;  // the game's primary D3D11 device, 0 = unknown
};

// Largest string the backend accepts from ReShade, and largest list it reads: bigger is a failure, never a cut.
inline constexpr size_t kMaxStringBytes = 64 * 1024;
inline constexpr size_t kMaxHandles = 64 * 1024;

class LiveDepthBackend final : public IBackend, public IEventHost
{
  public:
    LiveDepthBackend(IRawApi& raw, const BackendEnv& env);

    // IBackend
    int64_t NowMs() override;
    uint64_t Frame() override;
    void AddRef(uint64_t view) override;  // throws when the call faults: a COM AddRef either happened or not
    void Release(uint64_t view) override; // ... and so does this
    bool IsDepthView(uint64_t view) override;
    bool IsD3D11(uint64_t runtime) override;
    // The runtime's device is the capture's device: get_native() of the runtime's device, or GetDevice() of its back buffer resource,
    // equals it (captureDevice 0: no capture yet, so "the game's primary D3D11 device", BackendEnv::primaryDevice); or, when neither
    // does, the runtime is the only D3D11 runtime of the process (the way the boundary picks its runtime; see the top of this file for
    // why pointers cannot be trusted under ReShade's proxy). With two or more D3D11 runtimes only the exact matches count.
    bool SameDevice(uint64_t runtime, uint64_t captureDevice) override;
    // Whether `runtime` is the only D3D11 runtime seen in begin/finish events (the registry fed by NoteRuntime/ForgetRuntime).
    bool OnlyD3D11Runtime(uint64_t runtime);
    bool BackBufferSize(uint64_t runtime, uint32_t& width, uint32_t& height) override;
    bool TextureVariables(uint64_t runtime, std::vector<TextureVar>& out) override;
    void NoDepthCandidate(uint64_t runtime, const std::vector<TextureVar>& textures) override;
    void DepthInputsFound(uint64_t runtime, const std::vector<std::string>& names) override;
    bool TextureBinding(uint64_t runtime, uint64_t variable, uint64_t& srv, uint64_t& srvSrgb) override;
    bool UpdateTextureBindings(uint64_t runtime, const char* semantic, uint64_t srv, uint64_t srvSrgb) override;
    bool UniformVariables(uint64_t runtime, std::vector<UniformVar>& out) override;
    bool GetUniformBool(uint64_t runtime, uint64_t variable, bool& value) override;
    bool SetUniformBool(uint64_t runtime, uint64_t variable, bool value) override;
    bool Techniques(uint64_t runtime, std::vector<EffectTechnique>& out) override;
    DefinitionState Definition(uint64_t runtime, const char* effect, const char* name, std::string& value) override;

    // IEventHost
    void NoteRuntime(uint64_t runtime) override;
    void ForgetRuntime(uint64_t runtime) override;

  private:
    // Reads one string with the size query first. Yes = *out is the value; No = the thing does not exist; Fault = a call faulted, the
    // sizes disagree or the string is not terminated (nothing is returned). `kind` names the query in the log of the first Fault.
    template <class Query> Call ReadString(Query&& query, std::string& out, const char* kind);
    void LogStringFault(const char* kind, const char* why, size_t firstSize, size_t secondSize, size_t length);
    // Handles of an enumeration, grown once if the first capacity was too small.
    template <class List> bool ReadHandles(List&& list, std::vector<uint64_t>& handles);
    void LogFirstBind(uint64_t runtime, uint64_t variable, uint64_t srv, uint64_t srvSrgb);

    IRawApi& _raw;
    BackendEnv _env;

    std::mutex _registryMutex;
    std::vector<uint64_t> _d3d11Runtimes; // runtimes seen in begin/finish events that are D3D11 runtimes

    // The readback log of the first real bind. The first update_texture_bindings of the DEPTH semantic with a view starts an
    // observation of that runtime: the readbacks that follow are logged, the one that shows exactly what was bound (the reader) always,
    // the others (decoys) at most kDecoyLogs. The next update ends it; if it logged anything that was the first bind, once. An update
    // that ends an observation never starts the next one (it is the restore). Plain atomics: the binder serialises the calls, and a
    // lost update only costs or repeats a log line.
    std::atomic<uint64_t> _obsRuntime { 0 }; // 0 = no observation
    std::atomic<uint64_t> _obsSrv { 0 }, _obsSrgb { 0 };
    std::atomic<int> _decoysLeft { 0 };
    std::atomic<bool> _obsLogged { false };
    std::atomic<bool> _obsReaderLogged { false };
    std::atomic<bool> _firstBindDone { false };
    std::atomic<bool> _fallbackLogged { false };
    std::atomic<bool> _stringFaultLogged { false };
    std::atomic<bool> _deviceRefusalLogged { false };
    std::atomic<int> _noCandidateLogs { 0 };
    std::atomic<int> _foundLogs { 0 };
};
} // namespace ext::reshade::depth
