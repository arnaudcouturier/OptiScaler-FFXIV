#include "pch.h"

#include "ext/reshade/ReShadeDepthBackend.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace ext::reshade::depth
{
namespace
{
constexpr size_t kFirstHandles = 512; // the first capacity of a handle list; a longer list is read again with the right one
constexpr int kDecoyLogs = 3;         // readbacks of the first bind that are not the reader and are logged all the same
constexpr int kNoCandidateLogs = 3;   // lines about runtimes without a depth input candidate, per process
constexpr int kFoundLogs = 3;         // ... and about runtimes with one
constexpr size_t kDepthNamesShown = 8;
const char* const kDepthSemantic = "DEPTH";
} // namespace

LiveDepthBackend::LiveDepthBackend(IRawApi& raw, const BackendEnv& env) : _raw(raw), _env(env) {}

int64_t LiveDepthBackend::NowMs() { return _env.nowMs != nullptr ? _env.nowMs() : 0; }
uint64_t LiveDepthBackend::Frame() { return _env.frame != nullptr ? _env.frame() : 0; }

// ---- strings and lists ---------------------------------------------------------------------------------------------------------
// The size query first (a null buffer: the length + 1), then a buffer of need + 1 bytes (see the contract at the top of
// ReShadeDepthBackend.h). The second call gets ONE byte more capacity than asked for: a string that grew in between would then come
// back one character longer instead of cut to fit, and the difference shows. The string is accepted only if the buffer holds a NUL
// exactly at need - 1 (strnlen == need - 1), which rejects a string that grew, one that shrank, an interior NUL and a copy that never
// wrote; the second *size is not compared with the first (real ReShade says the characters copied, the header the count with the NUL)
// except that it may not exceed the capacity. A length of 0 or more than 64 KiB, or a missing string, is a failure, never a cut value.
template <class Query> Call LiveDepthBackend::ReadString(Query&& query, std::string& out, const char* kind)
{
    size_t need = 0;
    const Call first = query(nullptr, &need);
    if (first == Call::Fault)
    {
        LogStringFault(kind, "call faulted", 0, 0, 0);
        return Call::Fault;
    }
    if (first != Call::Yes)
        return first; // not there
    if (need == 0 || need > kMaxStringBytes)
    {
        LogStringFault(kind, "unusable length", need, 0, 0);
        return Call::Fault;
    }
    const size_t capacity = need + 1;
    std::vector<char> buffer(capacity, '\0');
    size_t size = capacity;
    if (query(buffer.data(), &size) != Call::Yes) // gone a moment after it was there: not "not there"
    {
        LogStringFault(kind, "second call faulted or answered no", need, 0, 0);
        return Call::Fault;
    }
    const size_t length = strnlen(buffer.data(), capacity);
    if (size > capacity || length != need - 1)
    {
        LogStringFault(kind, "answers disagree", need, size, length);
        return Call::Fault;
    }
    out.assign(buffer.data(), length);
    return Call::Yes;
}

// Said once: the first string that breaks the size contract, whatever its kind: the kind, the first answer (length + 1), the second
// *size and the length found in the buffer, so a ReShade that differs from the observed behaviour shows how.
void LiveDepthBackend::LogStringFault([[maybe_unused]] const char* kind, [[maybe_unused]] const char* why, [[maybe_unused]] size_t firstSize,
                                      [[maybe_unused]] size_t secondSize, [[maybe_unused]] size_t length)
{
    if (_stringFaultLogged.exchange(true))
        return;
    LOG_WARN("Automatic ReShade depth: string contract broken for {} ({}): first size {}, second size {}, string length {}", kind, why,
             firstSize, secondSize, length);
}

template <class List> bool LiveDepthBackend::ReadHandles(List&& list, std::vector<uint64_t>& handles)
{
    handles.assign(kFirstHandles, 0);
    size_t count = 0;
    if (!list(handles.data(), handles.size(), count) || count > kMaxHandles)
        return false;
    if (count > handles.size())
    {
        handles.assign(count, 0);
        size_t again = 0;
        if (!list(handles.data(), handles.size(), again) || again > handles.size()) // grew in between: a failure, not a cut
            return false;
        count = again;
    }
    handles.resize(count);
    return true;
}

// ---- views ---------------------------------------------------------------------------------------------------------------------
void LiveDepthBackend::AddRef(uint64_t view)
{
    if (!_raw.AddRefView(view))
        throw std::runtime_error("AddRef of a view faulted");
}

void LiveDepthBackend::Release(uint64_t view)
{
    if (!_raw.ReleaseView(view))
        throw std::runtime_error("Release of a view faulted");
}

bool LiveDepthBackend::IsDepthView(uint64_t view)
{
    uint32_t format = 0;
    return _raw.ViewFormat(view, format) && IsDepthFormatValue(format);
}

// ---- runtimes ------------------------------------------------------------------------------------------------------------------
bool LiveDepthBackend::IsD3D11(uint64_t runtime)
{
    bool hasDevice = false, d3d11 = false;
    uint64_t native = 0;
    return _raw.DeviceInfo(runtime, hasDevice, d3d11, native) && hasDevice && d3d11;
}

bool LiveDepthBackend::OnlyD3D11Runtime(uint64_t runtime)
{
    std::lock_guard lock(_registryMutex);
    for (const uint64_t other : _d3d11Runtimes)
        if (other != runtime)
            return false;
    return true;
}

bool LiveDepthBackend::SameDevice(uint64_t runtime, uint64_t captureDevice)
{
    bool hasDevice = false, d3d11 = false;
    uint64_t native = 0;
    if (!_raw.DeviceInfo(runtime, hasDevice, d3d11, native) || !hasDevice || !d3d11)
        return false;
    const uint64_t primary = _env.primaryDevice != nullptr ? _env.primaryDevice() : 0;
    const uint64_t target = captureDevice != 0 ? captureDevice : primary;
    if (target != 0 && native == target)
        return true;
    // get_native() is not the device of the game's resources in every setup (ReShade's own wrapper object came back in game): ask the
    // resource that really is on the runtime's side, its back buffer. A fault or no back buffer leaves it 0 = unknown.
    uint64_t resourceDevice = 0;
    if (!_raw.BackBufferDevice(runtime, resourceDevice))
        resourceDevice = 0;
    if (target != 0 && resourceDevice == target)
        return true;
    // A real capture was made on the device of the game's depth texture: another device never gets a view of it.
    if (captureDevice != 0)
    {
        if (!_deviceRefusalLogged.exchange(true))
            LOG_WARN("Automatic ReShade depth: device mismatch: the runtime's device {:#x} (its back buffer's device {:#x}) is not the capture's "
                     "device {:#x} (primary {:#x}); runtimes of this device get no depth copy",
                     native, resourceDevice, captureDevice, primary);
        return false;
    }
    // No capture yet. The primary device may be known under another pointer (a proxy) or not at all: the only D3D11 runtime is ours.
    if (!OnlyD3D11Runtime(runtime))
        return false;
    if (!_fallbackLogged.exchange(true))
        LOG_INFO("Automatic ReShade depth: the runtime's device {:#x} (its back buffer's device {:#x}) is not the game's device {:#x}; taking "
                 "the only D3D11 runtime (no capture yet)",
                 native, resourceDevice, target);
    return true;
}

bool LiveDepthBackend::BackBufferSize(uint64_t runtime, uint32_t& width, uint32_t& height)
{
    uint32_t w = 0, h = 0;
    if (!_raw.BackBuffer(runtime, w, h) || w == 0 || h == 0)
        return false;
    width = w;
    height = h;
    return true;
}

void LiveDepthBackend::NoteRuntime(uint64_t runtime)
{
    {
        std::lock_guard lock(_registryMutex);
        if (std::find(_d3d11Runtimes.begin(), _d3d11Runtimes.end(), runtime) != _d3d11Runtimes.end())
            return;
    }
    bool hasDevice = false, d3d11 = false;
    uint64_t native = 0;
    if (!_raw.DeviceInfo(runtime, hasDevice, d3d11, native) || !hasDevice || !d3d11)
        return;
    std::lock_guard lock(_registryMutex);
    if (std::find(_d3d11Runtimes.begin(), _d3d11Runtimes.end(), runtime) == _d3d11Runtimes.end())
        _d3d11Runtimes.push_back(runtime);
}

void LiveDepthBackend::ForgetRuntime(uint64_t runtime)
{
    std::lock_guard lock(_registryMutex);
    _d3d11Runtimes.erase(std::remove(_d3d11Runtimes.begin(), _d3d11Runtimes.end(), runtime), _d3d11Runtimes.end());
}

bool LiveDepthBackend::IsOurRuntime(uint64_t runtime) { return SameDevice(runtime, _captureDevice.load(std::memory_order_relaxed)); }

// ---- variables, bindings, uniforms, techniques ----------------------------------------------------------------------------------
bool LiveDepthBackend::TextureVariables(uint64_t runtime, std::vector<TextureVar>& out)
{
    out.clear();
    std::vector<uint64_t> handles;
    if (!ReadHandles([&](uint64_t* h, size_t cap, size_t& n) { return _raw.ListTextures(runtime, h, cap, n); }, handles))
        return false;
    std::vector<TextureVar> result;
    result.reserve(handles.size());
    for (const uint64_t handle : handles)
    {
        std::string name;
        if (ReadString([&](char* b, size_t* s) { return _raw.TextureName(runtime, handle, b, s); }, name, "texture name") != Call::Yes)
            return false;
        result.push_back({ handle, std::move(name) });
    }
    out = std::move(result);
    return true;
}

// The first run answers which names real presets use for their depth input: this lists the texture names that contain "depth" (any case)
// of a runtime in which none of the three known names exists (an MXAO-only preset declares DepthInputTex only; anything else shows here).
void LiveDepthBackend::NoDepthCandidate([[maybe_unused]] uint64_t runtime, const std::vector<TextureVar>& textures)
{
    if (_noCandidateLogs.fetch_add(1, std::memory_order_relaxed) >= kNoCandidateLogs)
        return;
    std::string names;
    size_t matching = 0, shown = 0;
    for (const TextureVar& t : textures)
    {
        std::string lower = t.name;
        for (char& c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower.find("depth") == std::string::npos)
            continue;
        ++matching;
        if (shown < kDepthNamesShown)
        {
            names += shown == 0 ? "" : ", ";
            names += t.name;
            ++shown;
        }
    }
    LOG_INFO("Automatic ReShade depth: runtime {:#x} has {} texture variables and no known depth input; {} contain 'depth' in the name: {}",
             runtime, textures.size(), matching, names.empty() ? std::string("(none)") : names);
}

// The first runtimes with a depth input say which variables matched, so the first run shows the unique-name form ReShade really reports.
void LiveDepthBackend::DepthInputsFound([[maybe_unused]] uint64_t runtime, const std::vector<std::string>& names)
{
    if (_foundLogs.fetch_add(1, std::memory_order_relaxed) >= kFoundLogs)
        return;
    std::string list;
    for (const std::string& name : names)
        list += (list.empty() ? "" : ", ") + name;
    LOG_INFO("Automatic ReShade depth: runtime {:#x} has depth input variables: {}", runtime, list);
}

bool LiveDepthBackend::TextureBinding(uint64_t runtime, uint64_t variable, uint64_t& srv, uint64_t& srvSrgb)
{
    uint64_t a = 0, b = 0;
    if (!_raw.TextureBinding(runtime, variable, a, b))
        return false;
    srv = a;
    srvSrgb = b;
    LogFirstBind(runtime, variable, a, b);
    return true;
}

bool LiveDepthBackend::UpdateTextureBindings(uint64_t runtime, const char* semantic, uint64_t srv, uint64_t srvSrgb)
{
    if (!_raw.UpdateTextureBindings(runtime, semantic, srv, srvSrgb))
        return false;
    if (_firstBindDone.load(std::memory_order_relaxed))
        return true;
    // An observation in progress ends with the next update (the restore, or an erase): the first bind is done if it logged anything.
    // This update is then the end of the observation and never the start of another.
    if (_obsRuntime.load(std::memory_order_relaxed) != 0)
    {
        _obsRuntime.store(0, std::memory_order_relaxed);
        if (_obsLogged.load(std::memory_order_relaxed))
            _firstBindDone.store(true, std::memory_order_relaxed);
        return true;
    }
    if (semantic != nullptr && std::strcmp(semantic, kDepthSemantic) == 0 && (srv != 0 || srvSrgb != 0))
    {
        _obsSrv.store(srv, std::memory_order_relaxed);
        _obsSrgb.store(srvSrgb, std::memory_order_relaxed);
        _obsLogged.store(false, std::memory_order_relaxed);
        _obsReaderLogged.store(false, std::memory_order_relaxed);
        _decoysLeft.store(kDecoyLogs, std::memory_order_relaxed);
        _obsRuntime.store(runtime, std::memory_order_relaxed);
    }
    return true;
}

// What real ReShade answers for the variable that reads the DEPTH semantic right after the first bind is not settled by the headers.
void LiveDepthBackend::LogFirstBind(uint64_t runtime, uint64_t variable, uint64_t srv, uint64_t srvSrgb)
{
    if (_obsRuntime.load(std::memory_order_relaxed) != runtime)
        return;
    const uint64_t boundSrv = _obsSrv.load(std::memory_order_relaxed), boundSrgb = _obsSrgb.load(std::memory_order_relaxed);
    const bool reader = srv == boundSrv && srvSrgb == boundSrgb;
    if (reader ? _obsReaderLogged.exchange(true, std::memory_order_relaxed) : _decoysLeft.fetch_sub(1, std::memory_order_relaxed) <= 0)
        return; // the reader is said once; the decoys (variables with the right name that read something else) a few times
    _obsLogged.store(true, std::memory_order_relaxed);
    LOG_INFO("Automatic ReShade depth: first bind, ReShade reads variable {:#x} back as srv {:#x} / srgb {:#x} (bound {:#x} / {:#x}): {}",
             variable, srv, srvSrgb, boundSrv, boundSrgb, reader ? "the bound copy, a reader" : "not the copy, a decoy");
}

bool LiveDepthBackend::UniformVariables(uint64_t runtime, std::vector<UniformVar>& out)
{
    out.clear();
    std::vector<uint64_t> handles;
    if (!ReadHandles([&](uint64_t* h, size_t cap, size_t& n) { return _raw.ListUniforms(runtime, h, cap, n); }, handles))
        return false;
    std::vector<UniformVar> result;
    result.reserve(handles.size());
    for (const uint64_t handle : handles)
    {
        std::string source;
        const Call c = ReadString([&](char* b, size_t* s) { return _raw.UniformSource(runtime, handle, b, s); }, source, "uniform source");
        if (c == Call::Fault)
            return false;
        result.push_back({ handle, c == Call::Yes ? std::move(source) : std::string() }); // no annotation: empty
    }
    out = std::move(result);
    return true;
}

bool LiveDepthBackend::GetUniformBool(uint64_t runtime, uint64_t variable, bool& value)
{
    bool v = false;
    if (!_raw.GetUniformBool(runtime, variable, v))
        return false;
    value = v;
    return true;
}

bool LiveDepthBackend::SetUniformBool(uint64_t runtime, uint64_t variable, bool value) { return _raw.SetUniformBool(runtime, variable, value); }

bool LiveDepthBackend::Techniques(uint64_t runtime, std::vector<EffectTechnique>& out)
{
    out.clear();
    std::vector<uint64_t> handles;
    if (!ReadHandles([&](uint64_t* h, size_t cap, size_t& n) { return _raw.ListTechniques(runtime, h, cap, n); }, handles))
        return false;
    std::vector<EffectTechnique> result;
    result.reserve(handles.size());
    for (const uint64_t handle : handles)
    {
        EffectTechnique t;
        bool enabled = false;
        if (ReadString([&](char* b, size_t* s) { return _raw.TechniqueEffect(runtime, handle, b, s); }, t.effect, "technique effect") != Call::Yes ||
            !_raw.TechniqueState(runtime, handle, enabled))
            return false;
        t.enabled = enabled;
        result.push_back(std::move(t));
    }
    out = std::move(result);
    return true;
}

DefinitionState LiveDepthBackend::Definition(uint64_t runtime, const char* effect, const char* name, std::string& value)
{
    std::string text;
    const Call c = ReadString([&](char* b, size_t* s) { return _raw.Definition(runtime, effect, name, b, s); }, text, "definition");
    if (c == Call::Yes)
    {
        value = std::move(text);
        return DefinitionState::Defined;
    }
    return c == Call::No ? DefinitionState::Undefined : DefinitionState::Error;
}
} // namespace ext::reshade::depth
