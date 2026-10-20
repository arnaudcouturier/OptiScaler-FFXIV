// [MIP-BIND] Source-level guard for the places the bind-time mipmap bias is wired in. The behaviour is tested in
// mip_bind_* (math, mode, table, bind, cost, vtable, settings); what those cannot see is a wiring line that moved or vanished
// (for example in a merge with another feature that edits the same files), and the properties that make the bind path safe.
//
//   argv[1] = repository root
//
//  1. Seams.cpp reaches the module from the three existing seams: device hooked, unhook, evaluate boundary.
//  2. The engine (the bind path) has no lock and no upstream types; the hooks file never takes the mutex in a thunk.
//  3. The keys are bound, the ini fragment documents them.
//  4. The older create-time module stands down while the bind-time hooks are attached, at every entry point.
//  5. No upstream file names the module: zero added upstream lines for the hooks themselves.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

static int g_failed = 0;

#define CHECK(cond)                                                                                              \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                             \
        {                                                                                                        \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                  \
            ++g_failed;                                                                                          \
        }                                                                                                        \
    } while (0)

static std::string ReadAll(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        std::printf("FAIL: cannot read %s\n", path.c_str());
        ++g_failed;
        return {};
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string out;
    for (char c : text)
        if (c != '\r')
            out += c;
    return out;
}

static size_t Count(const std::string& text, const std::string& needle)
{
    size_t n = 0;
    for (size_t pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + needle.size()))
        ++n;
    return n;
}

// The text of the function that starts with `head` up to its closing brace at column 0 or 4 (namespace member).
static std::string Body(const std::string& text, const std::string& head)
{
    const size_t at = text.find(head);
    if (at == std::string::npos)
        return {};
    const size_t end = text.find("\n}\n", at);
    return end == std::string::npos ? std::string() : text.substr(at, end - at);
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("FAIL: repository root missing\n");
        return 1;
    }
    const std::string root = argv[1];
    const std::string ext = root + "/OptiScaler/ext/";

    // ---- 1. the three seams -------------------------------------------------------------------------------------
    {
        const std::string seams = ReadAll(ext + "Seams.cpp");
        CHECK(Count(seams, "ext::upscaler::mipbind::OnDeviceHooked(device);") == 1);
        CHECK(Count(seams, "ext::upscaler::mipbind::OnUnhook();") == 1);
        CHECK(Count(seams, "ext::upscaler::mipbind::OnEvaluate(context, parameters);") == 1);
        CHECK(seams.find("#include \"ext/upscaler/MipBindHooks.h\"") != std::string::npos);

        // each one sits in its own seam, and only for a matched FFXIV build where the seam is not an evaluate
        CHECK(Body(seams, "void OnD3D11DeviceHooked(").find("mipbind::OnDeviceHooked(device);") != std::string::npos);
        CHECK(Body(seams, "void OnD3D11Unhook()").find("mipbind::OnUnhook();") != std::string::npos);
        CHECK(Body(seams, "void OnUpscaleBoundary(").find("mipbind::OnEvaluate(context, parameters);") != std::string::npos);
        CHECK(Body(seams, "void OnD3D11DeviceHooked(").find("if (Current().geometry != nullptr)\n        ext::upscaler::mipbind::OnDeviceHooked(device);") !=
              std::string::npos);
        CHECK(Body(seams, "void OnD3D11Unhook()").find("if (Current().geometry != nullptr)\n        ext::upscaler::mipbind::OnUnhook();") !=
              std::string::npos);
    }

    // ---- 2. the engine's bind path ------------------------------------------------------------------------------
    {
        const std::string engine = ReadAll(ext + "upscaler/MipBindEngine.h");
        CHECK(!engine.empty());
        // no lock of any kind in the file that holds the bind path
        CHECK(engine.find("mutex") == std::string::npos);
        CHECK(engine.find("lock_guard") == std::string::npos);
        CHECK(engine.find("scoped_lock") == std::string::npos);
        CHECK(engine.find("unique_lock") == std::string::npos);
        CHECK(engine.find("SRWLOCK") == std::string::npos);
        CHECK(engine.find("CRITICAL_SECTION") == std::string::npos);
        // pure D3D11 + standard library: no config, no state, no logger
        CHECK(engine.find("#include <Config.h>") == std::string::npos);
        CHECK(engine.find("#include <State.h>") == std::string::npos);
        CHECK(engine.find("LOG_") == std::string::npos);
        CHECK(engine.find("#include \"pch.h\"") == std::string::npos);

        const std::string hooks = ReadAll(ext + "upscaler/MipBindHooks.cpp");
        // the thunks go straight to the engine; the mutex is for the create path and the evaluate only
        const std::string setThunk = Body(hooks, "void STDMETHODCALLTYPE HkSet(");
        const std::string getThunk = Body(hooks, "void STDMETHODCALLTYPE HkGet(");
        CHECK(!setThunk.empty() && !getThunk.empty());
        CHECK(setThunk.find("g_mutex") == std::string::npos && getThunk.find("g_mutex") == std::string::npos);
        CHECK(setThunk.find("g_engine.Set(S, c, start, count, input);") != std::string::npos);
        CHECK(getThunk.find("g_engine.Get(S, c, start, count, output);") != std::string::npos);
        CHECK(setThunk.find("g_ready") != std::string::npos && getThunk.find("g_ready") != std::string::npos);

        // variants are made inside a bypass scope, through the trampoline
        CHECK(Body(hooks, "HRESULT STDMETHODCALLTYPE CreateVariant(").find("ext::core::BypassScope scope;") != std::string::npos);
        // the fork's own samplers (made in a bypass scope) are not classified
        CHECK(Body(hooks, "HRESULT STDMETHODCALLTYPE HkCreateSamplerState(").find("ext::core::HooksBypassed()") !=
              std::string::npos);
        CHECK(Body(hooks, "void OnSamplerCreated(").find("!bypassed &&") != std::string::npos);
        // a manual value makes the sampler not eligible
        CHECK(Body(hooks, "void OnSamplerCreated(").find("!ManualOverrideSet() &&") != std::string::npos);
        // thirteen detours: CreateSamplerState plus the six pairs; the CreateSamplerState one is attached LAST (at the first
        // upscaler create, after upstream's hook on the same function) and detached FIRST, each in its own transaction
        CHECK(Count(hooks, "DetourAttach(") == 3 && Count(hooks, "DetourDetach(") == 3);
        CHECK(Body(hooks, "bool InstallCreateHook()").find("DetourAttach(reinterpret_cast<PVOID*>(&g_createTramp), HkCreateSamplerState)") !=
              std::string::npos);
        const std::string uninstall = Body(hooks, "void Uninstall()");
        const size_t detachCreate = uninstall.find("DetourDetach(reinterpret_cast<PVOID*>(&g_createTramp), HkCreateSamplerState)");
        const size_t detachBind = uninstall.find("DetourDetach(reinterpret_cast<PVOID*>(&g_set[s])");
        CHECK(detachCreate != std::string::npos && detachBind != std::string::npos && detachCreate < detachBind);
        CHECK(Count(uninstall, "DetourTransactionBegin()") == 2);
        CHECK(Body(hooks, "void OnFeatureCreate()").find("InstallCreateHook();") != std::string::npos);
        // variants are made from the description the device really created (it keeps upstream's changes, e.g. anisotropy)
        CHECK(Body(hooks, "void OnSamplerCreated(").find("sampler->GetDesc(&created);") != std::string::npos);
        CHECK(hooks.find("kContextSetSamplersSlots[s]") != std::string::npos && hooks.find("kContextGetSamplersSlots[s]") != std::string::npos);
        CHECK(hooks.find("kDeviceCreateSamplerStateSlot") != std::string::npos);
    }

    // ---- 3. keys and ini fragment -------------------------------------------------------------------------------
    {
        const std::string cfg = ReadAll(ext + "core/ExtConfig.cpp");
        CHECK(cfg.find("b.Add(\"Mipmap\", \"MipmapBiasAuto\", mipBiasAuto);") != std::string::npos);
        CHECK(cfg.find("b.Add(\"Mipmap\", \"AutomaticFFXIV\", mipBiasAutoAlias);") != std::string::npos);
        CHECK(cfg.find("b.Add(\"Mipmap\", \"MipmapBiasObserve\", mipBiasObserve);") != std::string::npos);
        const std::string header = ReadAll(ext + "upscaler/UpscalerConfig.h");
        CHECK(header.find("CustomOptional<bool, NoDefault> mipBiasAuto;") != std::string::npos);
        CHECK(header.find("CustomOptional<bool, NoDefault> mipBiasAutoAlias;") != std::string::npos);
        CHECK(header.find("CustomOptional<bool> mipBiasObserve { false };") != std::string::npos);

        const std::string ini = ReadAll(ext + "ini/Mipmap.ini");
        CHECK(ini.find("\nMipmapBiasAuto=auto\n") != std::string::npos);
        CHECK(ini.find("\nAutomaticFFXIV=auto\n") != std::string::npos);
        CHECK(ini.find("\nMipmapBiasAutoOffset=auto\n") != std::string::npos);
        CHECK(ini.find("\nMipmapBiasObserve=auto\n") != std::string::npos);
        CHECK(ini.find("true for FFXIV and false for other games") != std::string::npos);
    }

    // ---- 4. the older module stands down ------------------------------------------------------------------------
    {
        const std::string glue = ReadAll(ext + "upscaler/MipBias.cpp");
        CHECK(glue.find("#include \"ext/upscaler/MipBindHooks.h\"") != std::string::npos);
        // the three places that decide whether the policy is enabled ask the bind-time module
        CHECK(Count(glue, "mipbind::AutoEnabled() && !mipbind::Owns()") == 3);
        CHECK(Body(glue, "void EnsureConfigured()").find("mipbind::AutoEnabled() && !mipbind::Owns()") != std::string::npos);
        CHECK(Body(glue, "void OnConfigLoaded(float offset)").find("mipbind::AutoEnabled() && !mipbind::Owns()") != std::string::npos);
        CHECK(Body(glue, "void OnSettingChanged()").find("mipbind::AutoEnabled() && !mipbind::Owns()") != std::string::npos);
        // the first feature create attaches the CreateSamplerState hook of the bind-time module
        CHECK(Body(glue, "void OnFeatureCreate(").find("mipbind::OnFeatureCreate();") != std::string::npos);
        // and the "manual setting is back" line is not logged then
        CHECK(Body(glue, "void LogSetting(").find("if (mipbind::Owns())\n        return;") != std::string::npos);
    }

    // ---- 5. upstream files do not name the module ---------------------------------------------------------------
    {
        const char* const files[] = { "hooks/D3D11_Hooks.cpp", "hooks/D3D11_Hooks.h", "Config.cpp", "Config.h",
                                      "upscalers/IFeature_Dx11wDx12.cpp", "menu/menu_common.cpp" };
        for (const char* f : files)
        {
            const std::string text = ReadAll(root + "/OptiScaler/" + f);
            CHECK(text.find("mipbind") == std::string::npos);
            CHECK(text.find("MipBind") == std::string::npos);
        }
    }

    if (g_failed == 0)
        std::printf("mip_bind_seam: all passed\n");
    return g_failed == 0 ? 0 : 1;
}
