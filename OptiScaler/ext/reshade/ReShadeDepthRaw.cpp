#include "pch.h"

#include "ext/reshade/ReShadeD3D11.h"
#include "ext/reshade/ReShadeDepthBackend.h"

// ReShade's headers and ImGui never share a translation unit (see ReShadeRegister.cpp).
#ifdef IMGUI_VERSION_NUM
#error "ReShadeDepthRaw.cpp must not see imgui.h"
#endif

#include "ext/reshade/third_party/reshade/reshade.hpp"

#include <d3d11.h>

// The live raw layer of the depth backend: one function per ReShade API call, every one of them in a POD thunk under SEH. ReShade's
// handles are untrusted (a view read from its binding map may be dangling, a runtime may be half torn down), and an access violation
// must not reach the game: the thunks hold no C++ objects (an access violation unwinds without destructors), the jobs live in the
// frame of the caller, and a C++ exception that crosses a thunk is caught by the same guard.
namespace ext::reshade::depth
{
namespace
{
using namespace ::reshade::api;

// The raw layer relies on ReShade's format enum having the DXGI numbering for every depth format (IsDepthFormatValue).
static_assert(static_cast<uint32_t>(format::r32_g8_typeless) == kFmtR32G8X24Typeless);
static_assert(static_cast<uint32_t>(format::d32_float_s8_uint) == kFmtD32FloatS8X24);
static_assert(static_cast<uint32_t>(format::r32_float_x8_uint) == kFmtR32FloatX8X24);
static_assert(static_cast<uint32_t>(format::r32_typeless) == kFmtR32Typeless);
static_assert(static_cast<uint32_t>(format::d32_float) == kFmtD32Float);
static_assert(static_cast<uint32_t>(format::r32_float) == kFmtR32Float);
static_assert(static_cast<uint32_t>(format::r24_g8_typeless) == kFmtR24G8Typeless);
static_assert(static_cast<uint32_t>(format::d24_unorm_s8_uint) == kFmtD24UnormS8);
static_assert(static_cast<uint32_t>(format::r24_unorm_x8_uint) == kFmtR24UnormX8);
static_assert(static_cast<uint32_t>(format::x24_unorm_g8_uint) == kFmtX24TypelessG8);
static_assert(static_cast<uint32_t>(format::r16_typeless) == kFmtR16Typeless);
static_assert(static_cast<uint32_t>(format::d16_unorm) == kFmtD16Unorm);
static_assert(static_cast<uint32_t>(format::r16_unorm) == kFmtR16Unorm);
static_assert(static_cast<uint32_t>(format::d16_unorm_s8_uint) == kFmtD16UnormS8);

effect_runtime* Rt(uint64_t id) { return reinterpret_cast<effect_runtime*>(id); }

// Runs a thunk guarded; true = it returned normally.
bool Run(void (*thunk)(void*), void* job)
{
    try
    {
        return d3d11::GuardedCall(thunk, job) == 0;
    }
    catch (...)
    {
        return false;
    }
}

// ---- thunks (no C++ objects, no destructors) -------------------------------------------------------------------------------------
struct DeviceJob
{
    effect_runtime* runtime;
    bool hasDevice, d3d11;
    uint64_t native;
};
void DeviceThunk(void* p)
{
    auto& j = *static_cast<DeviceJob*>(p);
    j.hasDevice = j.d3d11 = false;
    j.native = 0;
    device* d = j.runtime->get_device();
    if (d == nullptr)
        return;
    j.hasDevice = true;
    j.d3d11 = d->get_api() == device_api::d3d11;
    j.native = d->get_native();
}

struct BackBufferJob
{
    effect_runtime* runtime;
    uint32_t width, height;
};
void BackBufferThunk(void* p)
{
    auto& j = *static_cast<BackBufferJob*>(p);
    j.width = j.height = 0;
    if (j.runtime->get_back_buffer_count() == 0)
        return;
    const resource back = j.runtime->get_back_buffer(0);
    device* d = j.runtime->get_device();
    if (back.handle == 0 || d == nullptr)
        return;
    const resource_desc desc = d->get_resource_desc(back);
    if (desc.type != resource_type::texture_2d)
        return;
    j.width = desc.texture.width;
    j.height = desc.texture.height;
}

struct BackBufferDeviceJob
{
    effect_runtime* runtime;
    uint64_t device;
};
void BackBufferDeviceThunk(void* p)
{
    auto& j = *static_cast<BackBufferDeviceJob*>(p);
    j.device = 0;
    if (j.runtime->get_back_buffer_count() == 0)
        return;
    const resource back = j.runtime->get_back_buffer(0);
    if (back.handle == 0)
        return;
    // For D3D11 a ReShade resource handle is the ID3D11Resource pointer itself.
    ID3D11Device* device = nullptr;
    reinterpret_cast<ID3D11Resource*>(back.handle)->GetDevice(&device);
    if (device == nullptr)
        return;
    j.device = reinterpret_cast<uint64_t>(device);
    device->Release();
}

// The enumeration callbacks only count and store handles: ReShade calls them, so they must not throw or allocate.
struct ListJob
{
    effect_runtime* runtime;
    uint64_t* out;
    size_t cap, count;
};
void TextureCallback(effect_runtime*, effect_texture_variable v, void* user)
{
    auto& j = *static_cast<ListJob*>(user);
    if (j.count < j.cap)
        j.out[j.count] = v.handle;
    ++j.count;
}
void UniformCallback(effect_runtime*, effect_uniform_variable v, void* user)
{
    auto& j = *static_cast<ListJob*>(user);
    if (j.count < j.cap)
        j.out[j.count] = v.handle;
    ++j.count;
}
void TechniqueCallback(effect_runtime*, effect_technique v, void* user)
{
    auto& j = *static_cast<ListJob*>(user);
    if (j.count < j.cap)
        j.out[j.count] = v.handle;
    ++j.count;
}
void ListTexturesThunk(void* p)
{
    auto& j = *static_cast<ListJob*>(p);
    j.runtime->enumerate_texture_variables(nullptr, &TextureCallback, &j);
}
void ListUniformsThunk(void* p)
{
    auto& j = *static_cast<ListJob*>(p);
    j.runtime->enumerate_uniform_variables(nullptr, &UniformCallback, &j);
}
void ListTechniquesThunk(void* p)
{
    auto& j = *static_cast<ListJob*>(p);
    j.runtime->enumerate_techniques(nullptr, &TechniqueCallback, &j);
}

struct StringJob
{
    effect_runtime* runtime;
    uint64_t handle;
    const char* effect; // Definition only: null = global
    const char* name;   // Definition only
    char* buffer;
    size_t size;
    bool exists;
};
void TextureNameThunk(void* p)
{
    auto& j = *static_cast<StringJob*>(p);
    j.runtime->get_texture_variable_name(effect_texture_variable { j.handle }, j.buffer, &j.size);
    j.exists = true;
}
void UniformSourceThunk(void* p)
{
    auto& j = *static_cast<StringJob*>(p);
    j.exists = j.runtime->get_annotation_string_from_uniform_variable(effect_uniform_variable { j.handle }, "source", j.buffer, &j.size);
}
void TechniqueEffectThunk(void* p)
{
    auto& j = *static_cast<StringJob*>(p);
    j.runtime->get_technique_effect_name(effect_technique { j.handle }, j.buffer, &j.size);
    j.exists = true;
}
void DefinitionThunk(void* p)
{
    auto& j = *static_cast<StringJob*>(p);
    j.exists = j.effect == nullptr ? j.runtime->get_preprocessor_definition(j.name, j.buffer, &j.size)
                                   : j.runtime->get_preprocessor_definition_for_effect(j.effect, j.name, j.buffer, &j.size);
}

struct TechniqueStateJob
{
    effect_runtime* runtime;
    uint64_t handle;
    bool enabled;
};
void TechniqueStateThunk(void* p)
{
    auto& j = *static_cast<TechniqueStateJob*>(p);
    j.enabled = j.runtime->get_technique_state(effect_technique { j.handle });
}

struct BindingJob
{
    effect_runtime* runtime;
    uint64_t variable, srv, srvSrgb;
};
void GetBindingThunk(void* p)
{
    auto& j = *static_cast<BindingJob*>(p);
    resource_view a {}, b {};
    j.runtime->get_texture_binding(effect_texture_variable { j.variable }, &a, &b);
    j.srv = a.handle;
    j.srvSrgb = b.handle;
}
struct UpdateJob
{
    effect_runtime* runtime;
    const char* semantic;
    uint64_t srv, srvSrgb;
};
void UpdateBindingsThunk(void* p)
{
    auto& j = *static_cast<UpdateJob*>(p);
    j.runtime->update_texture_bindings(j.semantic, resource_view { j.srv }, resource_view { j.srvSrgb });
}

struct BoolJob
{
    effect_runtime* runtime;
    uint64_t variable;
    bool value;
};
void GetBoolThunk(void* p)
{
    auto& j = *static_cast<BoolJob*>(p);
    bool v = false;
    j.runtime->get_uniform_value_bool(effect_uniform_variable { j.variable }, &v, 1, 0);
    j.value = v;
}
void SetBoolThunk(void* p)
{
    auto& j = *static_cast<BoolJob*>(p);
    const bool v = j.value;
    j.runtime->set_uniform_value_bool(effect_uniform_variable { j.variable }, &v, 1, 0);
}

struct ViewJob
{
    uint64_t view;
    uint32_t format;
};
void AddRefThunk(void* p) { reinterpret_cast<IUnknown*>(static_cast<ViewJob*>(p)->view)->AddRef(); }
void ReleaseThunk(void* p) { reinterpret_cast<IUnknown*>(static_cast<ViewJob*>(p)->view)->Release(); }
void ViewFormatThunk(void* p)
{
    auto& j = *static_cast<ViewJob*>(p);
    j.format = 0;
    auto* view = reinterpret_cast<ID3D11View*>(j.view);
    ID3D11Resource* resource = nullptr;
    view->GetResource(&resource);
    if (resource == nullptr)
        return;
    ID3D11Texture2D* texture = nullptr;
    if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture))) && texture != nullptr)
    {
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        j.format = static_cast<uint32_t>(desc.Format);
        texture->Release();
    }
    resource->Release();
}

// ---- the raw layer ---------------------------------------------------------------------------------------------------------------
class LiveRaw final : public IRawApi
{
  public:
    bool DeviceInfo(uint64_t runtime, bool& hasDevice, bool& d3d11, uint64_t& native) override
    {
        DeviceJob j { Rt(runtime), false, false, 0 };
        if (!Run(&DeviceThunk, &j))
            return false;
        hasDevice = j.hasDevice;
        d3d11 = j.d3d11;
        native = j.native;
        return true;
    }
    bool BackBuffer(uint64_t runtime, uint32_t& width, uint32_t& height) override
    {
        BackBufferJob j { Rt(runtime), 0, 0 };
        if (!Run(&BackBufferThunk, &j))
            return false;
        width = j.width;
        height = j.height;
        return true;
    }
    bool BackBufferDevice(uint64_t runtime, uint64_t& device) override
    {
        BackBufferDeviceJob j { Rt(runtime), 0 };
        if (!Run(&BackBufferDeviceThunk, &j))
            return false;
        device = j.device;
        return true;
    }
    bool ListTextures(uint64_t runtime, uint64_t* out, size_t cap, size_t& count) override
    {
        ListJob j { Rt(runtime), out, cap, 0 };
        if (!Run(&ListTexturesThunk, &j))
            return false;
        count = j.count;
        return true;
    }
    bool ListUniforms(uint64_t runtime, uint64_t* out, size_t cap, size_t& count) override
    {
        ListJob j { Rt(runtime), out, cap, 0 };
        if (!Run(&ListUniformsThunk, &j))
            return false;
        count = j.count;
        return true;
    }
    bool ListTechniques(uint64_t runtime, uint64_t* out, size_t cap, size_t& count) override
    {
        ListJob j { Rt(runtime), out, cap, 0 };
        if (!Run(&ListTechniquesThunk, &j))
            return false;
        count = j.count;
        return true;
    }

    Call TextureName(uint64_t runtime, uint64_t variable, char* buffer, size_t* size) override
    {
        return String(&TextureNameThunk, runtime, variable, nullptr, nullptr, buffer, size);
    }
    Call UniformSource(uint64_t runtime, uint64_t variable, char* buffer, size_t* size) override
    {
        return String(&UniformSourceThunk, runtime, variable, nullptr, nullptr, buffer, size);
    }
    Call TechniqueEffect(uint64_t runtime, uint64_t technique, char* buffer, size_t* size) override
    {
        return String(&TechniqueEffectThunk, runtime, technique, nullptr, nullptr, buffer, size);
    }
    Call Definition(uint64_t runtime, const char* effect, const char* name, char* buffer, size_t* size) override
    {
        return String(&DefinitionThunk, runtime, 0, effect, name, buffer, size);
    }
    bool TechniqueState(uint64_t runtime, uint64_t technique, bool& enabled) override
    {
        TechniqueStateJob j { Rt(runtime), technique, false };
        if (!Run(&TechniqueStateThunk, &j))
            return false;
        enabled = j.enabled;
        return true;
    }

    bool TextureBinding(uint64_t runtime, uint64_t variable, uint64_t& srv, uint64_t& srvSrgb) override
    {
        BindingJob j { Rt(runtime), variable, 0, 0 };
        if (!Run(&GetBindingThunk, &j))
            return false;
        srv = j.srv;
        srvSrgb = j.srvSrgb;
        return true;
    }
    bool UpdateTextureBindings(uint64_t runtime, const char* semantic, uint64_t srv, uint64_t srvSrgb) override
    {
        UpdateJob j { Rt(runtime), semantic, srv, srvSrgb };
        return Run(&UpdateBindingsThunk, &j);
    }
    bool GetUniformBool(uint64_t runtime, uint64_t variable, bool& value) override
    {
        BoolJob j { Rt(runtime), variable, false };
        if (!Run(&GetBoolThunk, &j))
            return false;
        value = j.value;
        return true;
    }
    bool SetUniformBool(uint64_t runtime, uint64_t variable, bool value) override
    {
        BoolJob j { Rt(runtime), variable, value };
        return Run(&SetBoolThunk, &j);
    }

    bool AddRefView(uint64_t view) override
    {
        ViewJob j { view, 0 };
        return Run(&AddRefThunk, &j);
    }
    bool ReleaseView(uint64_t view) override
    {
        ViewJob j { view, 0 };
        return Run(&ReleaseThunk, &j);
    }
    bool ViewFormat(uint64_t view, uint32_t& format) override
    {
        ViewJob j { view, 0 };
        if (!Run(&ViewFormatThunk, &j))
            return false;
        format = j.format;
        return true;
    }

  private:
    static Call String(void (*thunk)(void*), uint64_t runtime, uint64_t handle, const char* effect, const char* name, char* buffer, size_t* size)
    {
        StringJob j { Rt(runtime), handle, effect, name, buffer, *size, false };
        if (!Run(thunk, &j))
            return Call::Fault;
        *size = j.size;
        return j.exists ? Call::Yes : Call::No;
    }
};
} // namespace

IRawApi& LiveRawApi()
{
    static LiveRaw* raw = new LiveRaw; // never destroyed: ReShade may still deliver events while the process shuts down
    return *raw;
}
} // namespace ext::reshade::depth
