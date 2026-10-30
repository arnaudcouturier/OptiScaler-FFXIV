// [VFXMASK] Source-level guard for the places the VFX mask gate probe is wired in. The behaviour behind them is covered by
// vfxmask_core, vfxmask_texture_warp and vfxmask_apply_warp; what those cannot see is a wiring line that moved or vanished (for
// example in a merge with another feature that edits the same files). Reads the production sources as text.
//
//   argv[1] = repository root
//
//  1. ext/Seams.cpp calls the probe from ext::dx11bridge::OnUpscaleBoundary, behind one Armed() load, and the upstream seam call
//     that reaches it (// ext: B6 in upscalers/IFeature_Dx11wDx12.cpp) is still there exactly once.
//  2. The write path writes the mask key once and never a null pointer (the game's own value has to survive).
//  3. The key is bound, defaults to off, is ini only, has its ini fragment line and its schema entry.
//  4. The texture is the R8_UNORM mask the guide describes, filled by the core's rule.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
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

static std::string Trim(const std::string& s)
{
    const size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos)
        return {};
    return s.substr(b, s.find_last_not_of(" \t") - b + 1);
}

// Only what the compiler would see: `#if 0` blocks, `//` and `/* */` comments, and the inside of string and character literals
// are blanked, so a wiring line that was commented out, disabled or moved into a string does not count.
static std::string StripNonCode(const std::string& text)
{
    std::string kept;
    {
        std::istringstream in(text);
        std::string line;
        int skipDepth = 0;
        while (std::getline(in, line))
        {
            const std::string t = Trim(line);
            if (skipDepth > 0)
            {
                if (t.rfind("#if", 0) == 0)
                    ++skipDepth;
                else if (t.rfind("#endif", 0) == 0)
                    --skipDepth;
                kept += '\n';
            }
            else if (t == "#if 0")
            {
                skipDepth = 1;
                kept += '\n';
            }
            else
            {
                kept += line + '\n';
            }
        }
    }

    std::string out;
    for (size_t i = 0; i < kept.size();)
    {
        if (kept.compare(i, 2, "//") == 0)
        {
            while (i < kept.size() && kept[i] != '\n')
                ++i;
        }
        else if (kept.compare(i, 2, "/*") == 0)
        {
            const size_t end = kept.find("*/", i + 2);
            i = end == std::string::npos ? kept.size() : end + 2;
            out += ' ';
        }
        else if (kept[i] == '"' || kept[i] == '\'')
        {
            const char quote = kept[i];
            out += quote;
            ++i;
            while (i < kept.size() && kept[i] != quote && kept[i] != '\n')
            {
                if (kept[i] == '\\')
                    ++i;
                ++i;
            }
            if (i < kept.size() && kept[i] == quote)
            {
                out += quote;
                ++i;
            }
        }
        else
        {
            out += kept[i++];
        }
    }
    return out;
}

static std::string NoSpace(const std::string& s)
{
    std::string out;
    for (const char c : s)
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            out += c;
    return out;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::puts("usage: vfxmask_seam_test <repo root>");
        return 2;
    }
    const std::string root = argv[1];

    // ---- 0. the code filter itself ----------------------------------------------------------------------------------
    {
        const std::string sample = "void f()\n{\n    // ext::ffxiv::vfxmask::Armed()\n    /* ext::ffxiv::vfxmask::Armed() */\n"
                                   "    const char* s = \"ext::ffxiv::vfxmask::Armed()\";\n#if 0\n    ext::ffxiv::vfxmask::Armed()\n#endif\n"
                                   "    if (ext::ffxiv::vfxmask::Armed()) g();\n}\n";
        const std::string code = StripNonCode(sample);
        CHECK(Count(code, "ext::ffxiv::vfxmask::Armed()") == 1);
    }

    // ---- 1. the call in the bridge's evaluate boundary --------------------------------------------------------------
    {
        const std::string seams = StripNonCode(ReadAll(root + "/OptiScaler/ext/Seams.cpp"));
        CHECK(Count(seams, "if (ext::ffxiv::vfxmask::Armed())") == 1);
        CHECK(Count(seams, "ext::ffxiv::vfxmask::OnUpscaleBoundary(context, color, parameters);") == 1);

        const size_t fn = seams.find("void OnUpscaleBoundary(ID3D11DeviceContext* context, ID3D11Resource* color,");
        const size_t after = seams.find("void AfterSharedCopy(");
        const size_t guard = seams.find("if (ext::ffxiv::vfxmask::Armed())");
        const size_t call = seams.find("ext::ffxiv::vfxmask::OnUpscaleBoundary(context, color, parameters);");
        CHECK(fn != std::string::npos && after != std::string::npos && guard != std::string::npos && call != std::string::npos);
        // Inside ext::dx11bridge::OnUpscaleBoundary, and behind the load (the call is never reached with the key off, so the
        // probe's own work costs nothing while it is off).
        CHECK(fn < guard && guard < call && call < after);
        CHECK(call - guard < 200); // the call is the next statement, not somewhere later in the file

        // The probe is reached only through the existing bridge seam: the call in the upstream bridge file is still there, once.
        const std::string bridgeRaw = ReadAll(root + "/OptiScaler/upscalers/IFeature_Dx11wDx12.cpp");
        const std::string bridge = StripNonCode(bridgeRaw);
        CHECK(Count(bridge, "ext::dx11bridge::OnUpscaleBoundary(InDeviceContext, restoreParamColor, InParameters);") == 1);
        CHECK(bridgeRaw.find("// ext: B6") != std::string::npos);
        // ... and before the bridge copies and binds the inputs, which is what makes the mask of THIS evaluate reach the upscaler.
        const size_t boundary = bridge.find("ext::dx11bridge::OnUpscaleBoundary(");
        const size_t process = bridge.find("ProcessDx11Textures(InParameters)");
        CHECK(boundary != std::string::npos && process != std::string::npos && boundary < process);
    }

    // ---- 2. the write path: one key, never a null pointer, never a clear --------------------------------------------
    {
        const std::string apply = StripNonCode(ReadAll(root + "/OptiScaler/ext/ffxiv/vfxmask/VfxMaskApply.h"));
        CHECK(Count(apply, "parameters->Set(") == 1);
        CHECK(apply.find("parameters->Set(kMaskParameterKey, mask);") != std::string::npos);
        CHECK(apply.find("NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask") != std::string::npos);
        CHECK(apply.find("nullptr);") == std::string::npos); // no Set(key, nullptr), no Reset of the key
        CHECK(apply.find("->Reset(") == std::string::npos);

        // The armed check the seam asks is one relaxed load; the mode itself is an atomic, so the config load can switch the
        // probe without locking the evaluate path.
        const std::string glueRaw = ReadAll(root + "/OptiScaler/ext/ffxiv/vfxmask/VfxMaskProbe.cpp");
        const std::string glue = StripNonCode(glueRaw);
        CHECK(Count(glue, "g_mode.load(std::memory_order_relaxed)") == 2); // Armed() and CurrentMode()
        CHECK(glue.find("return g_mode.load(std::memory_order_relaxed) != static_cast<std::uint8_t>(ProbeMode::Off);") !=
              std::string::npos);
        // The config load arms it and counts the arming (so switching the key off and on says what happened again).
        CHECK(Count(glue, "g_mode.store(static_cast<std::uint8_t>(mode), std::memory_order_relaxed);") == 1);
        CHECK(Count(glue, "g_arms.fetch_add(1, std::memory_order_relaxed);") == 1);
        CHECK(glue.find("FfxivConfig::vfxMaskProbeOnLoaded = &OnConfigLoaded;") != std::string::npos);
        // The log line names the backend and the readback outcome through the core's formatter (one line per change).
        CHECK(glue.find("FormatLine(key, BackendName(), reason)") != std::string::npos);
        CHECK(glue.find("Dx11Upscaler.value_or_default()") != std::string::npos);

        // The refusals that keep a null out of the D3D11 calls report and return (a refusal that keeps going would dereference
        // the null it just described).
        CHECK(glueRaw.find("Report(mode, 0, 0, Outcome::Refused, \"the evaluate has no colour input\");\n            return;") !=
              std::string::npos);
        CHECK(glueRaw.find("Report(mode, 0, 0, Outcome::Refused, \"the colour input is not a texture\");\n            return;") !=
              std::string::npos);
        CHECK(glueRaw.find("Report(mode, 0, 0, Outcome::Refused, \"the evaluate has no parameter block\");\n            return;") !=
              std::string::npos);

        // The entry point is noexcept (nothing propagates into the evaluate path) and both of its bodies have the net the seams
        // expect: the config load and the evaluate. A body that loses its net is one error away from terminating the game.
        const std::string header = NoSpace(StripNonCode(ReadAll(root + "/OptiScaler/ext/ffxiv/vfxmask/VfxMaskProbe.h")));
        CHECK(header.find("boolArmed()noexcept;") != std::string::npos);
        CHECK(header.find("voidOnConfigLoaded(constchar*text)noexcept;") != std::string::npos);
        CHECK(header.find("voidOnUpscaleBoundary(ID3D11DeviceContext*context,ID3D11Resource*color,"
                          "NVSDK_NGX_Parameter*parameters)noexcept;") != std::string::npos);
        CHECK(Count(NoSpace(glue), "catch(...)") == 2);
    }

    // ---- 3. the key: bound, off by default, ini only, documented ----------------------------------------------------
    {
        const std::string config = ReadAll(root + "/OptiScaler/ext/core/ExtConfig.cpp");
        CHECK(Count(config, "b.Add(\"FFXIV\", \"VfxMaskProbe\", vfxMaskProbe);") == 1);

        const std::string ffxiv = ReadAll(root + "/OptiScaler/ext/ffxiv/FfxivConfig.h");
        CHECK(Count(ffxiv, "CustomOptional<std::string> vfxMaskProbe { \"off\" };") == 1);
        CHECK(Count(ffxiv, "vfxMaskProbeOnLoaded(vfxMaskProbe.value_or_default().c_str());") == 1);
        const size_t post = ffxiv.find("void PostLoad()");
        const size_t call = ffxiv.find("vfxMaskProbeOnLoaded(vfxMaskProbe.value_or_default().c_str());");
        CHECK(post != std::string::npos && call != std::string::npos && post < call);
        // The hook is declared as a pointer, so the config stays loadable without the probe's translation unit.
        CHECK(ffxiv.find("static inline TextHook vfxMaskProbeOnLoaded = nullptr;") != std::string::npos);

        const std::string manifest = ReadAll(root + "/OptiScaler/ext/core/MenuManifest.h");
        CHECK(manifest.find("\"[FFXIV] VfxMaskProbe\"") != std::string::npos);

        // The ini line, with the comment above it saying there is no menu control (ini_fragments.cpp asks for exactly that).
        const std::string ini = ReadAll(root + "/OptiScaler/ext/ini/FFXIV.ini");
        const size_t line = ini.find("\nVfxMaskProbe=auto\n");
        CHECK(line != std::string::npos);
        if (line != std::string::npos)
        {
            const size_t block = ini.rfind("\n\n", line);
            const std::string comment = ini.substr(block == std::string::npos ? 0 : block, line - (block == std::string::npos ? 0 : block));
            CHECK(comment.find("No menu control") != std::string::npos);
            CHECK(comment.find("Default (auto) is off") != std::string::npos);
        }

        // The schema entry the settings reference is generated from.
        const std::string schema = ReadAll(root + "/tools/settings-schema.json");
        const size_t key = schema.find("\"[FFXIV] VfxMaskProbe\"");
        CHECK(key != std::string::npos);
        if (key != std::string::npos)
        {
            const std::string entry = NoSpace(schema.substr(key, 1400));
            CHECK(entry.find("\"type\":\"enum\"") != std::string::npos);
            CHECK(entry.find("\"values\":[\"off\",\"left\",\"right\"]") != std::string::npos);
            CHECK(entry.find("\"defaultMeans\":\"off(today'sbehaviour)\"") != std::string::npos);
            CHECK(entry.find("\"iniOnly\":") != std::string::npos);
            CHECK(entry.find("\"ffxivOnly\"") != std::string::npos);
        }
    }

    // ---- 4. the texture: the mask the guide describes ---------------------------------------------------------------
    {
        const std::string texture = StripNonCode(ReadAll(root + "/OptiScaler/ext/ffxiv/vfxmask/VfxMaskTexture.cpp"));
        CHECK(texture.find("desc.Format = DXGI_FORMAT_R8_UNORM;") != std::string::npos);
        CHECK(texture.find("desc.SampleDesc.Count = 1;") != std::string::npos);
        CHECK(texture.find("desc.MipLevels = 1;") != std::string::npos);
        CHECK(texture.find("desc.ArraySize = 1;") != std::string::npos);
        CHECK(Count(texture, "FillRow(") == 1); // the halves come from the core's rule, there is no second fill rule
        CHECK(Count(texture, "UpdateSubresource(") == 1);
        CHECK(Count(texture, "CreateTexture2D(") == 1);
        // Every path that hands a texture over has validated its description first, the re-used one included (one GetDesc per
        // evaluate is what keeps a texture that changed under the cache from reaching the upscaler).
        CHECK(Count(texture, "ValidMaskDesc(") >= 3);
        CHECK(texture.find("cache.texture->GetDesc(&current);") != std::string::npos);
        CHECK(texture.find("if (ValidMaskDesc(current))") != std::string::npos);
        CHECK(texture.find("desc.Width = plan.width;") != std::string::npos);
        CHECK(texture.find("desc.Height = plan.height;") != std::string::npos);

        // The size comes from the colour input of this evaluate, never from a parameter the probe would have to trust.
        const std::string glue = StripNonCode(ReadAll(root + "/OptiScaler/ext/ffxiv/vfxmask/VfxMaskProbe.cpp"));
        CHECK(glue.find("ID3D11Resource* colour = ColourInput(color, parameters);") != std::string::npos);
        CHECK(glue.find("texture->GetDesc(&colourDesc);") != std::string::npos);
        CHECK(glue.find("PlanMask(colourDesc.Width, colourDesc.Height, mode)") != std::string::npos);
        CHECK(glue.find("EnsureMaskTexture(context, plan.width, plan.height, mode, S().mask)") != std::string::npos);
        // The mask key is written only with the texture the texture module just validated.
        CHECK(glue.find("ApplyBiasMask(parameters, built.texture, mode, plan.width, plan.height)") != std::string::npos);
    }

    if (g_failed != 0)
    {
        std::printf("%d check(s) failed\n", g_failed);
        return 1;
    }
    std::puts("PASS: VFX mask probe wiring: one guarded call at the bridge boundary, one key write, the key bound and ini only, "
              "an R8_UNORM mask of the colour input's size");
    return 0;
}
