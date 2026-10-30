// [VFXMASK] The write the VFX mask gate probe makes (ext/ffxiv/vfxmask/VfxMaskApply.h), against a fake NVSDK_NGX_Parameter and a
// real WARP mask texture: the mask key is written exactly while the probe is armed, it is the key the DirectX 11 bridge copies as
// "ReactiveMask", the pointer written is the mask, and nothing at all is written when the key is off or the mask cannot be used.
//
// The "off" case is the default every player runs: the game's own NULL for that key has to reach the upscaler exactly as before, so
// a single write on an evaluate with the key off would be a behaviour change, not a diagnostic.
#define NOMINMAX
#include <d3d11.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>

#include "ext/ffxiv/vfxmask/VfxMaskApply.h"
#include "ext/ffxiv/vfxmask/VfxMaskTexture.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::vfxmask;

namespace
{
int g_failed = 0;
#define CHECK(cond)                                                                                              \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                             \
        {                                                                                                        \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                  \
            ++g_failed;                                                                                          \
        }                                                                                                        \
    } while (0)

// Stand-in for the NGX parameter block of an evaluate (the same shape tests/ext/reshade uses): it records what was written and
// hands resources back, or reports that it did not keep them.
struct FakeParams final : NVSDK_NGX_Parameter
{
    std::map<std::string, ID3D11Resource*> resources;
    int sets = 0;
    mutable int gets = 0;
    bool keeps = true;  // false: a block that takes the pointer and does not hand it back
    bool onlyVoid = false; // true: the resource is only findable through the void-pointer overload, as NGX blocks can store it
    std::string lastSetName;

    void Set(const char* name, ID3D11Resource* value) override
    {
        ++sets;
        lastSetName = name != nullptr ? name : "";
        if (value == nullptr)
            resources.erase(lastSetName);
        else
            resources[lastSetName] = value;
    }
    void Set(const char* name, void* value) override
    {
        Set(name, static_cast<ID3D11Resource*>(value));
    }
    void Set(const char*, unsigned long long) override {}
    void Set(const char*, float) override {}
    void Set(const char*, double) override {}
    void Set(const char*, unsigned int) override {}
    void Set(const char*, int) override {}
    void Set(const char*, ID3D12Resource*) override {}

    NVSDK_NGX_Result Get(const char* name, ID3D11Resource** out) const override
    {
        ++gets;
        if (!keeps || onlyVoid || name == nullptr || out == nullptr)
            return NVSDK_NGX_Result_Fail;
        const auto it = resources.find(name);
        if (it == resources.end())
            return NVSDK_NGX_Result_Fail;
        *out = it->second;
        return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Get(const char* name, void** out) const override
    {
        ++gets;
        if (!keeps || name == nullptr || out == nullptr)
            return NVSDK_NGX_Result_Fail;
        const auto it = resources.find(name);
        if (it == resources.end())
            return NVSDK_NGX_Result_Fail;
        *out = it->second;
        return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Get(const char*, unsigned long long*) const override { return NVSDK_NGX_Result_Fail; }
    NVSDK_NGX_Result Get(const char*, float*) const override { return NVSDK_NGX_Result_Fail; }
    NVSDK_NGX_Result Get(const char*, double*) const override { return NVSDK_NGX_Result_Fail; }
    NVSDK_NGX_Result Get(const char*, unsigned int*) const override { return NVSDK_NGX_Result_Fail; }
    NVSDK_NGX_Result Get(const char*, int*) const override { return NVSDK_NGX_Result_Fail; }
    NVSDK_NGX_Result Get(const char*, ID3D12Resource**) const override { return NVSDK_NGX_Result_Fail; }
    void Reset() override { resources.clear(); }
};

struct Device
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;

    bool Create()
    {
        const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                             D3D11_SDK_VERSION, &device, nullptr, &context);
        return SUCCEEDED(hr) && device != nullptr && context != nullptr;
    }
};

bool SameText(const char* a, const char* b)
{
    return a != nullptr && b != nullptr && std::strcmp(a, b) == 0;
}
} // namespace

int main()
{
    Device gpu;
    if (!gpu.Create())
    {
        std::puts("SKIP: no WARP device");
        return 0;
    }

    MaskTexture cache;
    const EnsureResult built = EnsureMaskTexture(gpu.context.Get(), 64, 32, ProbeMode::Left, cache);
    if (built.texture == nullptr)
    {
        std::puts("SKIP: the mask texture could not be created");
        return 0;
    }
    ID3D11Resource* const mask = built.texture;

    // ---- the key off: nothing is written, and the block is not even read -------------------------------------------
    {
        FakeParams block;
        for (const ProbeMode mode : { ProbeMode::Off, ProbeMode::Off, ProbeMode::Off })
        {
            const ApplyResult applied = ApplyBiasMask(&block, mask, mode, 64, 32);
            CHECK(applied.outcome == Outcome::Refused);
            CHECK(applied.writes == 0);
        }
        CHECK(block.sets == 0);
        CHECK(block.gets == 0);
        CHECK(block.resources.empty());
    }

    // ---- the key on: the mask key, the mask itself, once per evaluate ----------------------------------------------
    for (const ProbeMode mode : { ProbeMode::Left, ProbeMode::Right })
    {
        FakeParams block;
        const ApplyResult applied = ApplyBiasMask(&block, mask, mode, 64, 32);
        CHECK(applied.writes == 1);
        CHECK(applied.outcome == Outcome::Given);
        CHECK(block.sets == 1);
        CHECK(block.gets == 1); // Set returns void: the acceptance is read back
        CHECK(block.resources.size() == 1);
        CHECK(SameText(block.lastSetName.c_str(), NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask));
        CHECK(SameText(block.lastSetName.c_str(), "DLSS.Input.Bias.Current.Color.Mask"));
        CHECK(SameText(block.lastSetName.c_str(), kMaskParameterKey));
        const auto only = block.resources.begin();
        CHECK(only != block.resources.end() && only->second == mask);

        // Every evaluate of an armed probe writes again: the bridge copies the input it finds for THIS evaluate, and the game
        // sets every key again too.
        const ApplyResult second = ApplyBiasMask(&block, mask, mode, 64, 32);
        CHECK(second.writes == 1 && second.outcome == Outcome::Given);
        CHECK(block.sets == 2);
    }

    // ---- a block that does not keep the pointer: the write happened, the readback says so --------------------------
    {
        FakeParams block;
        block.keeps = false;
        const ApplyResult applied = ApplyBiasMask(&block, mask, ProbeMode::Left, 64, 32);
        CHECK(applied.writes == 1);
        CHECK(applied.outcome == Outcome::Unconfirmed);
        CHECK(block.sets == 1);
    }

    // ---- a block that only finds the resource through the void-pointer overload: the readback still confirms it ---------
    {
        FakeParams block;
        block.onlyVoid = true;
        const ApplyResult applied = ApplyBiasMask(&block, mask, ProbeMode::Left, 64, 32);
        CHECK(applied.writes == 1);
        CHECK(applied.outcome == Outcome::Given);
        CHECK(block.gets == 2); // the typed read fails, the void-pointer one answers
    }

    // ---- a mask that cannot be used: no write at all (fail open) ---------------------------------------------------
    {
        FakeParams block;

        CHECK(ApplyBiasMask(&block, nullptr, ProbeMode::Left, 64, 32).writes == 0);
        CHECK(ApplyBiasMask<NVSDK_NGX_Parameter>(nullptr, mask, ProbeMode::Left, 64, 32).writes == 0);
        CHECK(ApplyBiasMask(&block, mask, ProbeMode::Left, 0, 32).writes == 0);   // no size
        CHECK(ApplyBiasMask(&block, mask, ProbeMode::Left, 1, 32).writes == 0);   // one column cannot be split
        CHECK(ApplyBiasMask(&block, mask, ProbeMode::Left, 64, 0).writes == 0);
        CHECK(ApplyBiasMask(&block, mask, ProbeMode::Left, kMaxMaskEdge + 1, 32).writes == 0);
        CHECK(block.sets == 0);

        // ... and once it can be used again, a write happens.
        CHECK(ApplyBiasMask(&block, mask, ProbeMode::Right, 64, 32).writes == 1);
        CHECK(block.sets == 1);
    }

    // ---- switching the key off after a run leaves the key alone ----------------------------------------------------
    {
        FakeParams block;
        CHECK(ApplyBiasMask(&block, mask, ProbeMode::Left, 64, 32).writes == 1);
        for (int i = 0; i < 5; ++i)
            CHECK(ApplyBiasMask(&block, mask, ProbeMode::Off, 64, 32).writes == 0);
        CHECK(block.sets == 1);
        // The block still holds the last mask it was given: the probe never clears the key, and the game's own evaluate sets it
        // again before the upscaler reads it.
        CHECK(block.resources.size() == 1);
    }

    if (g_failed != 0)
    {
        std::printf("%d check(s) failed\n", g_failed);
        return 1;
    }
    std::puts("PASS: VFX mask probe write: the mask key only while the probe is armed, the mask pointer, the readback and "
              "the fail-open cases");
    return 0;
}
