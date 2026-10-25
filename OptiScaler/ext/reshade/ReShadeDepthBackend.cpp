#include "pch.h"

#include "ext/reshade/ReShadeDepthBackend.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace ext::reshade::depth
{
namespace
{
constexpr size_t kFirstHandles = 512; // the first capacity of a handle list; a longer list is read again with the right one
constexpr int kReadbackLogs = 4;      // readbacks of the first bind that are logged
const char* const kDepthSemantic = "DEPTH";
} // namespace

LiveDepthBackend::LiveDepthBackend(IRawApi& raw, const BackendEnv& env) : _raw(raw), _env(env) {}

int64_t LiveDepthBackend::NowMs() { return _env.nowMs != nullptr ? _env.nowMs() : 0; }
uint64_t LiveDepthBackend::Frame() { return _env.frame != nullptr ? _env.frame() : 0; }

// ---- strings and lists ---------------------------------------------------------------------------------------------------------
// The size query first (a null buffer: the length including the NUL), then a buffer of that size. The second call gets ONE byte more
// capacity than asked for: a string that grew in between would then come back longer than the first answer instead of cut to fit, and
// the difference shows. The NUL is checked and stripped; a length of 0 with "exists", an absurd length, a missing terminator or one
// inside the value are failures, never a shortened value.
template <class Query> Call LiveDepthBackend::ReadString(Query&& query, std::string& out)
{
    size_t size = 0;
    const Call first = query(nullptr, &size);
    if (first != Call::Yes)
        return first; // Fault, or No: not there
    if (size == 0 || size > kMaxStringBytes)
        return Call::Fault;
    std::vector<char> buffer(size + 1, '\0');
    size_t capacity = size + 1;
    if (query(buffer.data(), &capacity) != Call::Yes) // gone a moment after it was there: not "not there"
        return Call::Fault;
    if (capacity != size || buffer[size - 1] != '\0' || std::memchr(buffer.data(), 0, size - 1) != nullptr)
        return Call::Fault;
    out.assign(buffer.data(), size - 1);
    return Call::Yes;
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
    const uint64_t target = captureDevice != 0 ? captureDevice : (_env.primaryDevice != nullptr ? _env.primaryDevice() : 0);
    if (target != 0 && native == target)
        return true;
    // A real capture was made on the native device of the game's depth texture: another device never gets a view of it.
    if (captureDevice != 0)
        return false;
    // No capture yet. The primary device may be known under another pointer (a proxy) or not at all: the only D3D11 runtime is ours.
    if (!OnlyD3D11Runtime(runtime))
        return false;
    if (!_fallbackLogged.exchange(true))
        LOG_INFO("Automatic ReShade depth: the runtime's device {:#x} is not the game's device {:#x}; taking the only D3D11 runtime (no capture yet)",
                 native, target);
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
        if (ReadString([&](char* b, size_t* s) { return _raw.TextureName(runtime, handle, b, s); }, name) != Call::Yes)
            return false;
        result.push_back({ handle, std::move(name) });
    }
    out = std::move(result);
    return true;
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
    // An observation in progress ends with the next update (the restore): the first bind is done if it logged anything.
    if (_readbackRuntime.load(std::memory_order_relaxed) != 0)
    {
        _readbackRuntime.store(0, std::memory_order_relaxed);
        _readbacksLeft.store(0, std::memory_order_relaxed);
        if (_readbackLogged.load(std::memory_order_relaxed))
        {
            _firstBindDone.store(true, std::memory_order_relaxed);
            return true;
        }
    }
    if (semantic != nullptr && std::strcmp(semantic, kDepthSemantic) == 0 && (srv != 0 || srvSrgb != 0))
    {
        _readbackView.store(srv != 0 ? srv : srvSrgb, std::memory_order_relaxed);
        _readbackLogged.store(false, std::memory_order_relaxed);
        _readbacksLeft.store(kReadbackLogs, std::memory_order_relaxed);
        _readbackRuntime.store(runtime, std::memory_order_relaxed);
    }
    return true;
}

// What real ReShade answers for the variable that reads the DEPTH semantic right after the first bind is not settled by the headers.
void LiveDepthBackend::LogFirstBind(uint64_t runtime, uint64_t variable, uint64_t srv, uint64_t srvSrgb)
{
    if (_readbacksLeft.load(std::memory_order_relaxed) <= 0 || _readbackRuntime.load(std::memory_order_relaxed) != runtime)
        return;
    const int left = _readbacksLeft.fetch_sub(1, std::memory_order_relaxed);
    if (left <= 0)
        return;
    _readbackLogged.store(true, std::memory_order_relaxed);
    LOG_INFO("Automatic ReShade depth: first bind, ReShade reads variable {:#x} back as srv {:#x} / srgb {:#x} (bound {:#x})", variable, srv,
             srvSrgb, _readbackView.load(std::memory_order_relaxed));
    if (left == 1)
        _firstBindDone.store(true, std::memory_order_relaxed);
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
        const Call c = ReadString([&](char* b, size_t* s) { return _raw.UniformSource(runtime, handle, b, s); }, source);
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
        if (ReadString([&](char* b, size_t* s) { return _raw.TechniqueEffect(runtime, handle, b, s); }, t.effect) != Call::Yes ||
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
    const Call c = ReadString([&](char* b, size_t* s) { return _raw.Definition(runtime, effect, name, b, s); }, text);
    if (c == Call::Yes)
    {
        value = std::move(text);
        return DefinitionState::Defined;
    }
    return c == Call::No ? DefinitionState::Undefined : DefinitionState::Error;
}
} // namespace ext::reshade::depth
