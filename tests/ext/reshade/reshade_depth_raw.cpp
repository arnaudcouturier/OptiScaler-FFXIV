// Automatic ReShade scene depth: the raw layer over effect_runtime (ReShadeDepthRaw.cpp), the only file that calls into ReShade.
// No real ReShade is needed for what it promises:
//  - every call is guarded: a handle that is garbage (a dangling view, a runtime that is gone) makes the call return false instead of
//    taking the process down, and so does a C++ exception from inside ReShade;
//  - the arguments reach ReShade in the right order and shape: update_texture_bindings("DEPTH", srv, srgb), the readback order of
//    get_texture_binding, the POINTER overloads of the uniform accessors with one value and array index 0, the size query with a null
//    buffer first and the capacity second, the "source" annotation, the (effect, name) order of the per-effect definition, back buffer 0;
//  - the enumeration callbacks only count and store handles.
// The fake runtime and device derive from ReShade's abstract classes (their pure virtuals are generated: gen_fake_reshade_stubs.py);
// everything the raw layer must not call reports itself through Unexpected().
#include "pch.h"

#include "ext/reshade/ReShadeDepthBackend.h"
#include "ext/reshade/third_party/reshade/reshade.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ext::reshade::depth;
using namespace ::reshade::api;

namespace
{
int g_fail = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            std::printf("CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #c);                                           \
            ++g_fail;                                                                                                  \
        }                                                                                                              \
    } while (0)

std::vector<std::string> g_unexpected;
void Unexpected(const char* name) { g_unexpected.push_back(name); }

void CopyString(const std::string& s, char* buffer, size_t* size)
{
    if (buffer == nullptr)
    {
        *size = s.size() + 1;
        return;
    }
    const size_t capacity = *size;
    if (capacity == 0)
        return;
    const size_t n = std::min(capacity - 1, s.size());
    std::memcpy(buffer, s.data(), n);
    buffer[n] = '\0';
    *size = n + 1;
}

struct FakeDevice final : device
{
    device_api api = device_api::d3d11;
    uint64_t native = 0xD3D0;
    resource_type type = resource_type::texture_2d;
    uint32_t width = 3840, height = 2160;
    mutable uint64_t described = 0; // the resource get_resource_desc was asked about

    device_api get_api() const override { return api; }
    uint64_t get_native() const override { return native; }
    resource_desc get_resource_desc(resource r) const override
    {
        described = r.handle;
        resource_desc d {};
        d.type = type;
        d.texture.width = width;
        d.texture.height = height;
        return d;
    }
#include "fake_device_stubs.inc"
};

enum class Boom
{
    None,
    Throw,
    AccessViolation,
};

struct FakeRuntime final : effect_runtime
{
    FakeDevice* dev = nullptr;
    uint64_t native = 0x1234;
    uint32_t backBuffers = 1;
    uint64_t backBuffer = 0x77;
    uint32_t backBufferIndex = ~0u; // the index asked for
    Boom boom = Boom::None;         // what every recorded call does before anything else

    std::vector<uint64_t> textures { 0x11, 0x12, 0x13 };
    std::vector<uint64_t> uniforms { 0x21, 0x22 };
    std::vector<uint64_t> techniques { 0x31 };
    std::map<uint64_t, std::string> names { { 0x11, "ReShade::DepthBufferTex" }, { 0x31, "Main.fx" } };
    std::map<uint64_t, std::string> sources { { 0x21, "bufready_depth" } }; // 0x22 has no "source" annotation
    std::map<std::string, std::string> globalDefs { { "A", "1.05" } };
    std::map<std::string, std::string> effectDefs { { "Main.fx|A", "0.5" } };

    // what the raw layer asked
    struct StringCall
    {
        std::string what;
        bool nullBuffer;
        size_t sizeIn;
        uint64_t handle;
        std::string annotation, effect, name;
    };
    std::vector<StringCall> stringCalls;
    uint64_t bindingVariable = 0;
    std::string updateSemantic;
    uint64_t updateSrv = ~0ull, updateSrgb = ~0ull;
    uint64_t setVariable = 0, getVariable = 0;
    size_t setCount = 0, setIndex = 99, getCount = 0, getIndex = 99;
    bool setValue = false;
    bool boolToReturn = true;
    uint64_t stateTechnique = 0;
    bool techniqueEnabled = true;
    int enumerations = 0;

    void Disturb() const
    {
        if (boom == Boom::Throw)
            throw std::runtime_error("boom");
        if (boom == Boom::AccessViolation)
        {
            volatile int* null = nullptr;
            *null = 1;
        }
    }

    uint64_t get_native() const override { return native; }
    device* get_device() override
    {
        Disturb();
        return dev;
    }
    uint32_t get_back_buffer_count() const override
    {
        Disturb();
        return backBuffers;
    }
    resource get_back_buffer(uint32_t index) override
    {
        backBufferIndex = index;
        return resource { backBuffer };
    }

    void enumerate_texture_variables(const char* effect, void (*callback)(effect_runtime*, effect_texture_variable, void*), void* user) override
    {
        Disturb();
        ++enumerations;
        stringCalls.push_back({ "enumerate_texture_variables", false, 0, 0, "", effect == nullptr ? "<null>" : effect, "" });
        for (uint64_t h : textures)
            callback(this, effect_texture_variable { h }, user);
    }
    void enumerate_uniform_variables(const char* effect, void (*callback)(effect_runtime*, effect_uniform_variable, void*), void* user) override
    {
        Disturb();
        ++enumerations;
        stringCalls.push_back({ "enumerate_uniform_variables", false, 0, 0, "", effect == nullptr ? "<null>" : effect, "" });
        for (uint64_t h : uniforms)
            callback(this, effect_uniform_variable { h }, user);
    }
    void enumerate_techniques(const char* effect, void (*callback)(effect_runtime*, effect_technique, void*), void* user) override
    {
        Disturb();
        ++enumerations;
        stringCalls.push_back({ "enumerate_techniques", false, 0, 0, "", effect == nullptr ? "<null>" : effect, "" });
        for (uint64_t h : techniques)
            callback(this, effect_technique { h }, user);
    }

    void get_texture_variable_name(effect_texture_variable v, char* name, size_t* size) const override
    {
        Disturb();
        const_cast<FakeRuntime*>(this)->stringCalls.push_back({ "get_texture_variable_name", name == nullptr, *size, v.handle, "", "", "" });
        CopyString(names.count(v.handle) ? names.at(v.handle) : std::string(), name, size);
    }
    bool get_annotation_string_from_uniform_variable(effect_uniform_variable v, const char* annotation, char* value, size_t* size) const override
    {
        Disturb();
        const_cast<FakeRuntime*>(this)->stringCalls.push_back({ "get_annotation_string_from_uniform_variable", value == nullptr, *size, v.handle, annotation, "", "" });
        if (std::string(annotation) != "source" || !sources.count(v.handle))
            return false;
        CopyString(sources.at(v.handle), value, size);
        return true;
    }
    void get_technique_effect_name(effect_technique t, char* name, size_t* size) const override
    {
        Disturb();
        const_cast<FakeRuntime*>(this)->stringCalls.push_back({ "get_technique_effect_name", name == nullptr, *size, t.handle, "", "", "" });
        CopyString(names.count(t.handle) ? names.at(t.handle) : std::string(), name, size);
    }
    bool get_preprocessor_definition(const char* n, char* value, size_t* size) const override
    {
        Disturb();
        const_cast<FakeRuntime*>(this)->stringCalls.push_back({ "get_preprocessor_definition", value == nullptr, *size, 0, "", "", n });
        const auto it = globalDefs.find(n);
        if (it == globalDefs.end())
            return false;
        CopyString(it->second, value, size);
        return true;
    }
    bool get_preprocessor_definition_for_effect(const char* effect, const char* n, char* value, size_t* size) const override
    {
        Disturb();
        const_cast<FakeRuntime*>(this)->stringCalls.push_back({ "get_preprocessor_definition_for_effect", value == nullptr, *size, 0, "", effect, n });
        const auto it = effectDefs.find(std::string(effect) + "|" + n);
        if (it == effectDefs.end())
            return false;
        CopyString(it->second, value, size);
        return true;
    }
    bool get_technique_state(effect_technique t) const override
    {
        Disturb();
        const_cast<FakeRuntime*>(this)->stateTechnique = t.handle;
        return techniqueEnabled;
    }
    void get_texture_binding(effect_texture_variable v, resource_view* srv, resource_view* srgb) const override
    {
        Disturb();
        const_cast<FakeRuntime*>(this)->bindingVariable = v.handle;
        *srv = resource_view { 0xA1 };  // the two outputs are told apart by value: srv first, srgb second
        *srgb = resource_view { 0xA2 };
    }
    void update_texture_bindings(const char* semantic, resource_view srv, resource_view srgb) override
    {
        Disturb();
        updateSemantic = semantic;
        updateSrv = srv.handle;
        updateSrgb = srgb.handle;
    }
    void get_uniform_value_bool(effect_uniform_variable v, bool* values, size_t count, size_t index) const override
    {
        Disturb();
        auto* self = const_cast<FakeRuntime*>(this);
        self->getVariable = v.handle;
        self->getCount = count;
        self->getIndex = index;
        if (count >= 1)
            values[0] = boolToReturn;
    }
    void set_uniform_value_bool(effect_uniform_variable v, const bool* values, size_t count, size_t index) override
    {
        Disturb();
        setVariable = v.handle;
        setCount = count;
        setIndex = index;
        setValue = count >= 1 && values != nullptr ? values[0] : !setValue;
    }
#include "fake_runtime_stubs.inc"
};

uint64_t Id(FakeRuntime& r) { return reinterpret_cast<uint64_t>(static_cast<effect_runtime*>(&r)); }

// ---- garbage handles -------------------------------------------------------------------------------------------------------------
void GarbageHandlesAreSurvived()
{
    IRawApi& raw = LiveRawApi();
    constexpr uint64_t kGarbage = 0x10;
    uint32_t u32 = 0, w = 0, h = 0;
    uint64_t a = 0, b = 0;
    bool flag = false, flag2 = false;
    size_t count = 0, size = 0;
    uint64_t handles[4] = {};
    char buffer[8] = {};
    CHECK(!raw.AddRefView(kGarbage));
    CHECK(!raw.ReleaseView(kGarbage));
    CHECK(!raw.ViewFormat(kGarbage, u32));
    CHECK(!raw.AddRefView(0));
    CHECK(!raw.ViewFormat(0, u32));
    CHECK(!raw.DeviceInfo(kGarbage, flag, flag2, a));
    CHECK(!raw.DeviceInfo(0, flag, flag2, a));
    CHECK(!raw.BackBuffer(kGarbage, w, h));
    CHECK(!raw.BackBufferDevice(kGarbage, a));
    CHECK(!raw.BackBufferDevice(0, a));
    CHECK(!raw.ListTextures(kGarbage, handles, 4, count));
    CHECK(!raw.ListUniforms(kGarbage, handles, 4, count));
    CHECK(!raw.ListTechniques(kGarbage, handles, 4, count));
    size = 8;
    CHECK(raw.TextureName(kGarbage, 1, buffer, &size) == Call::Fault);
    CHECK(raw.UniformSource(kGarbage, 1, buffer, &size) == Call::Fault);
    CHECK(raw.TechniqueEffect(kGarbage, 1, buffer, &size) == Call::Fault);
    CHECK(raw.Definition(kGarbage, nullptr, "A", buffer, &size) == Call::Fault);
    CHECK(raw.Definition(kGarbage, "Main.fx", "A", buffer, &size) == Call::Fault);
    CHECK(!raw.TechniqueState(kGarbage, 1, flag));
    CHECK(!raw.TextureBinding(kGarbage, 1, a, b));
    CHECK(!raw.UpdateTextureBindings(kGarbage, "DEPTH", 1, 1));
    CHECK(!raw.GetUniformBool(kGarbage, 1, flag));
    CHECK(!raw.SetUniformBool(kGarbage, 1, true));
    CHECK(!raw.UpdateTextureBindings(0, "DEPTH", 1, 1));
}

// a runtime whose methods throw or crash: the same answer, from every entry
void FaultsInsideReShade()
{
    for (const Boom boom : { Boom::Throw, Boom::AccessViolation })
    {
        FakeDevice dev;
        FakeRuntime rt;
        rt.dev = &dev;
        rt.boom = boom;
        IRawApi& raw = LiveRawApi();
        const uint64_t id = Id(rt);
        bool f1 = false, f2 = false;
        uint64_t a = 0, b = 0;
        uint32_t w = 0, h = 0;
        size_t count = 0, size = 0;
        uint64_t handles[4] = {};
        char buffer[8] = {};
        CHECK(!raw.DeviceInfo(id, f1, f2, a));
        CHECK(!raw.BackBuffer(id, w, h));
        CHECK(!raw.BackBufferDevice(id, a));
        CHECK(!raw.ListTextures(id, handles, 4, count));
        CHECK(!raw.ListUniforms(id, handles, 4, count));
        CHECK(!raw.ListTechniques(id, handles, 4, count));
        size = 8;
        CHECK(raw.TextureName(id, 0x11, buffer, &size) == Call::Fault);
        CHECK(raw.UniformSource(id, 0x21, buffer, &size) == Call::Fault);
        CHECK(raw.TechniqueEffect(id, 0x31, buffer, &size) == Call::Fault);
        CHECK(raw.Definition(id, nullptr, "A", buffer, &size) == Call::Fault);
        CHECK(!raw.TechniqueState(id, 0x31, f1));
        CHECK(!raw.TextureBinding(id, 0x11, a, b));
        CHECK(!raw.UpdateTextureBindings(id, "DEPTH", 1, 2));
        CHECK(!raw.GetUniformBool(id, 0x21, f1));
        CHECK(!raw.SetUniformBool(id, 0x21, true));
        // and the layer works again once the runtime does
        rt.boom = Boom::None;
        CHECK(raw.DeviceInfo(id, f1, f2, a));
    }
}

// ---- the arguments -----------------------------------------------------------------------------------------------------------------
void RuntimeAndDevice()
{
    IRawApi& raw = LiveRawApi();
    FakeDevice dev;
    FakeRuntime rt;
    rt.dev = &dev;
    bool has = false, d3d11 = false;
    uint64_t native = 0;
    CHECK(raw.DeviceInfo(Id(rt), has, d3d11, native) && has && d3d11 && native == 0xD3D0);
    dev.api = device_api::d3d12;
    CHECK(raw.DeviceInfo(Id(rt), has, d3d11, native) && has && !d3d11);
    dev.api = device_api::vulkan;
    CHECK(raw.DeviceInfo(Id(rt), has, d3d11, native) && has && !d3d11);
    rt.dev = nullptr;
    CHECK(raw.DeviceInfo(Id(rt), has, d3d11, native) && !has && !d3d11 && native == 0);
    rt.dev = &dev;

    uint32_t w = 1, h = 1;
    dev.api = device_api::d3d11;
    CHECK(raw.BackBuffer(Id(rt), w, h) && w == 3840 && h == 2160);
    CHECK(rt.backBufferIndex == 0 && dev.described == 0x77); // back buffer 0, and that very resource is described
    rt.backBuffers = 0;
    rt.backBufferIndex = ~0u;
    CHECK(raw.BackBuffer(Id(rt), w, h) && w == 0 && h == 0 && rt.backBufferIndex == ~0u); // none: no resource asked for
    rt.backBuffers = 1;
    rt.backBuffer = 0;
    CHECK(raw.BackBuffer(Id(rt), w, h) && w == 0 && h == 0);
    rt.backBuffer = 0x77;
    dev.type = resource_type::buffer;
    CHECK(raw.BackBuffer(Id(rt), w, h) && w == 0 && h == 0);
    dev.type = resource_type::texture_2d;
    rt.dev = nullptr;
    CHECK(raw.BackBuffer(Id(rt), w, h) && w == 0 && h == 0);
}

void Lists()
{
    IRawApi& raw = LiveRawApi();
    FakeRuntime rt;
    uint64_t out[2] = { 0, 0 };
    size_t count = 0;
    // more handles than the capacity: the first ones are stored, all are counted
    CHECK(raw.ListTextures(Id(rt), out, 2, count) && count == 3 && out[0] == 0x11 && out[1] == 0x12);
    uint64_t guard[3] = { 7, 7, 7 };
    CHECK(raw.ListTextures(Id(rt), guard, 2, count) && guard[2] == 7); // nothing is written beyond the capacity
    // no room at all: just the count, and a null pointer is never written through
    CHECK(raw.ListTextures(Id(rt), nullptr, 0, count) && count == 3);
    CHECK(raw.ListUniforms(Id(rt), out, 2, count) && count == 2 && out[0] == 0x21 && out[1] == 0x22);
    CHECK(raw.ListTechniques(Id(rt), out, 2, count) && count == 1 && out[0] == 0x31);
    // the same for the other two lists: all counted, only the capacity stored
    uint64_t one[2] = { 0, 7 };
    CHECK(raw.ListUniforms(Id(rt), one, 1, count) && count == 2 && one[0] == 0x21 && one[1] == 7);
    rt.techniques = { 0x31, 0x32, 0x33 };
    uint64_t two[3] = { 0, 0, 7 };
    CHECK(raw.ListTechniques(Id(rt), two, 2, count) && count == 3 && two[0] == 0x31 && two[1] == 0x32 && two[2] == 7);
    rt.techniques = { 0x31 };
    rt.enumerations = 0;
    rt.stringCalls.clear();
    CHECK(raw.ListUniforms(Id(rt), nullptr, 0, count) && count == 2);
    CHECK(raw.ListTechniques(Id(rt), nullptr, 0, count) && count == 1);
    // all runtimes' effects: the effect name is null
    for (const auto& c : rt.stringCalls)
        CHECK(c.effect == "<null>");
    CHECK(rt.enumerations == 2);
    rt.textures.clear();
    CHECK(raw.ListTextures(Id(rt), out, 2, count) && count == 0);
}

void Strings()
{
    IRawApi& raw = LiveRawApi();
    FakeRuntime rt;
    size_t size = 0;
    // the size query first: a null buffer, and the answer includes the NUL
    CHECK(raw.TextureName(Id(rt), 0x11, nullptr, &size) == Call::Yes && size == 24);
    CHECK(rt.stringCalls.back().nullBuffer && rt.stringCalls.back().handle == 0x11);
    std::vector<char> buffer(25, 'Q');
    size = buffer.size();
    CHECK(raw.TextureName(Id(rt), 0x11, buffer.data(), &size) == Call::Yes);
    CHECK(size == 24 && std::string(buffer.data()) == "ReShade::DepthBufferTex");
    CHECK(!rt.stringCalls.back().nullBuffer && rt.stringCalls.back().sizeIn == 25); // the capacity went in
    // too tiny a buffer: what ReShade answers (a cut string) is reported as it is, for the caller to refuse
    std::vector<char> tiny(5, 'Q');
    size = tiny.size();
    CHECK(raw.TextureName(Id(rt), 0x11, tiny.data(), &size) == Call::Yes && size == 5);

    // the uniform's "source" annotation, and no annotation at all
    size = 0;
    CHECK(raw.UniformSource(Id(rt), 0x21, nullptr, &size) == Call::Yes && size == 15);
    CHECK(rt.stringCalls.back().annotation == "source" && rt.stringCalls.back().handle == 0x21);
    size = 0;
    CHECK(raw.UniformSource(Id(rt), 0x22, nullptr, &size) == Call::No);

    // the effect of a technique
    size = 0;
    CHECK(raw.TechniqueEffect(Id(rt), 0x31, nullptr, &size) == Call::Yes && size == 8);
    CHECK(rt.stringCalls.back().handle == 0x31);

    // a definition: global with a null effect, per effect otherwise, in the order (effect, name)
    size = 0;
    CHECK(raw.Definition(Id(rt), nullptr, "A", nullptr, &size) == Call::Yes && size == 5);
    CHECK(rt.stringCalls.back().what == "get_preprocessor_definition" && rt.stringCalls.back().name == "A");
    size = 0;
    CHECK(raw.Definition(Id(rt), "Main.fx", "A", nullptr, &size) == Call::Yes && size == 4);
    CHECK(rt.stringCalls.back().what == "get_preprocessor_definition_for_effect" && rt.stringCalls.back().effect == "Main.fx" &&
          rt.stringCalls.back().name == "A");
    size = 0;
    CHECK(raw.Definition(Id(rt), nullptr, "Z", nullptr, &size) == Call::No);
    CHECK(raw.Definition(Id(rt), "Main.fx", "Z", nullptr, &size) == Call::No);
    CHECK(raw.Definition(Id(rt), "Other.fx", "A", nullptr, &size) == Call::No);
}

void Bindings()
{
    IRawApi& raw = LiveRawApi();
    FakeRuntime rt;
    uint64_t srv = 0, srgb = 0;
    // srv first, srgb second, both as read
    CHECK(raw.TextureBinding(Id(rt), 0x11, srv, srgb) && srv == 0xA1 && srgb == 0xA2 && rt.bindingVariable == 0x11);
    // the semantic, then srv, then srgb
    CHECK(raw.UpdateTextureBindings(Id(rt), "DEPTH", 0xC0, 0xC1));
    CHECK(rt.updateSemantic == "DEPTH" && rt.updateSrv == 0xC0 && rt.updateSrgb == 0xC1);
    CHECK(raw.UpdateTextureBindings(Id(rt), "DEPTH", 0, 0));
    CHECK(rt.updateSrv == 0 && rt.updateSrgb == 0);

    bool value = false;
    rt.boolToReturn = true;
    CHECK(raw.GetUniformBool(Id(rt), 0x21, value) && value && rt.getVariable == 0x21);
    CHECK(rt.getCount == 1 && rt.getIndex == 0); // one value, array index 0
    rt.boolToReturn = false;
    CHECK(raw.GetUniformBool(Id(rt), 0x21, value) && !value);
    CHECK(raw.SetUniformBool(Id(rt), 0x22, true) && rt.setVariable == 0x22 && rt.setValue);
    CHECK(rt.setCount == 1 && rt.setIndex == 0); // the pointer overload with one value and array index 0 (not bool x, y, z, w)
    CHECK(raw.SetUniformBool(Id(rt), 0x22, false) && !rt.setValue);

    bool enabled = false;
    CHECK(raw.TechniqueState(Id(rt), 0x31, enabled) && enabled && rt.stateTechnique == 0x31);
    rt.techniqueEnabled = false;
    CHECK(raw.TechniqueState(Id(rt), 0x31, enabled) && !enabled);
}

// ---- the views (COM objects ReShade holds raw pointers to) -------------------------------------------------------------------------
struct CountedView final : IUnknown
{
    int adds = 0, releases = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++adds; }
    ULONG STDMETHODCALLTYPE Release() override { return ++releases; }
};

void ViewReferences()
{
    IRawApi& raw = LiveRawApi();
    CountedView v;
    const uint64_t id = reinterpret_cast<uint64_t>(static_cast<IUnknown*>(&v));
    CHECK(raw.AddRefView(id) && v.adds == 1 && v.releases == 0);
    CHECK(raw.AddRefView(id) && v.adds == 2 && v.releases == 0);
    CHECK(raw.ReleaseView(id) && v.adds == 2 && v.releases == 1);
}

// the numeric format of the resource behind a view: a typeless depth texture read through a typed view reports the resource's format
void ViewFormats()
{
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0, got {};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, &got, &context)))
    {
        std::puts("SKIP: no WARP device (the view format check did not run)");
        return;
    }
    IRawApi& raw = LiveRawApi();
    D3D11_TEXTURE2D_DESC td {};
    td.Width = td.Height = 16;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11ShaderResourceView> srv;
    CHECK(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &depth)));
    D3D11_SHADER_RESOURCE_VIEW_DESC vd {};
    vd.Format = DXGI_FORMAT_R32_FLOAT;
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipLevels = 1;
    CHECK(SUCCEEDED(device->CreateShaderResourceView(depth.Get(), &vd, &srv)));
    uint32_t format = 7;
    CHECK(raw.ViewFormat(reinterpret_cast<uint64_t>(srv.Get()), format) && format == kFmtR32Typeless && IsDepthFormatValue(format));
    // the reference count is the same afterwards: the query takes and gives back what it takes
    srv->AddRef();
    const ULONG before = srv->Release();
    CHECK(raw.ViewFormat(reinterpret_cast<uint64_t>(srv.Get()), format));
    srv->AddRef();
    CHECK(srv->Release() == before);
    ComPtr<ID3D11Resource> resource;
    srv->GetResource(&resource);
    resource->AddRef();
    const ULONG resourceBefore = resource->Release();
    CHECK(raw.ViewFormat(reinterpret_cast<uint64_t>(srv.Get()), format));
    resource->AddRef();
    CHECK(resource->Release() == resourceBefore);

    // a view of a buffer is no texture: format 0
    D3D11_BUFFER_DESC bd {};
    bd.ByteWidth = 64;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11ShaderResourceView> bufferView;
    D3D11_SHADER_RESOURCE_VIEW_DESC bvd {};
    bvd.Format = DXGI_FORMAT_R32_TYPELESS;
    bvd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    bvd.BufferEx.NumElements = 16;
    bvd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    if (SUCCEEDED(device->CreateBuffer(&bd, nullptr, &buffer)) && SUCCEEDED(device->CreateShaderResourceView(buffer.Get(), &bvd, &bufferView)))
    {
        format = 7;
        CHECK(raw.ViewFormat(reinterpret_cast<uint64_t>(bufferView.Get()), format) && format == 0);
    }
}

// The device of the resource behind back buffer 0 (ID3D11Resource::GetDevice), which is NOT device::get_native() of the runtime's device:
// the comparison with the game's depth texture is made on the devices of real resources
void BackBufferDeviceIsTheResourceDevice()
{
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0, got {};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, &got, &context)))
    {
        std::puts("SKIP: no WARP device (the back buffer device check did not run)");
        return;
    }
    D3D11_TEXTURE2D_DESC td {};
    td.Width = td.Height = 16;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    CHECK(SUCCEEDED(device->CreateTexture2D(&td, nullptr, &texture)));
    IRawApi& raw = LiveRawApi();
    FakeDevice dev; // its get_native() is 0xD3D0: a pointer that is not the resource's device
    FakeRuntime rt;
    rt.dev = &dev;
    rt.backBuffer = reinterpret_cast<uint64_t>(texture.Get());
    uint64_t d = 7;
    CHECK(raw.BackBufferDevice(Id(rt), d));
    CHECK(d == reinterpret_cast<uint64_t>(device.Get()) && d != 0xD3D0);
    CHECK(rt.backBufferIndex == 0); // back buffer 0
    // the reference the query takes is given back
    device->AddRef();
    const ULONG before = device->Release();
    CHECK(raw.BackBufferDevice(Id(rt), d));
    device->AddRef();
    CHECK(device->Release() == before);
    // no back buffer, or an empty handle: device 0, no fault
    rt.backBuffers = 0;
    rt.backBufferIndex = ~0u;
    d = 7;
    CHECK(raw.BackBufferDevice(Id(rt), d) && d == 0 && rt.backBufferIndex == ~0u);
    rt.backBuffers = 1;
    rt.backBuffer = 0;
    d = 7;
    CHECK(raw.BackBufferDevice(Id(rt), d) && d == 0);
    // a back buffer handle that is garbage: caught, not crashed
    rt.backBuffer = 0x77;
    CHECK(!raw.BackBufferDevice(Id(rt), d));
}

// the layer calls nothing but what it needs
void NothingElse() { CHECK(g_unexpected.empty()); }
} // namespace

int main()
{
    GarbageHandlesAreSurvived();
    FaultsInsideReShade();
    RuntimeAndDevice();
    Lists();
    Strings();
    Bindings();
    ViewReferences();
    ViewFormats();
    BackBufferDeviceIsTheResourceDevice();
    NothingElse();
    if (!g_unexpected.empty())
        for (const std::string& u : g_unexpected)
            std::printf("unexpected call: %s\n", u.c_str());
    if (g_fail != 0)
    {
        std::printf("reshade_depth_raw: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::puts("reshade_depth_raw: all checks passed");
    return 0;
}
