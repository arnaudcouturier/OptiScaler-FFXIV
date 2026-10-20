// [MIP] Source-level guard for the places the automatic mipmap bias is wired in. The behaviour behind them is tested
// in mip_bias_math / mip_bias_policy / mip_bias_glue; what those cannot see is a wiring line that moved or vanished
// (for example in a merge with another feature that edits the same files). Reads the production sources as text.
//
//   argv[1] = repository root
//
//  1. ApplyForcedQuality calls mipbias::OnFeatureCreate exactly once, after the native-quality hook install and
//     before any early return, so it runs for every create: no forced preset needed, native DLAA included.
//  2. [Mipmap] MipmapBiasAuto / MipmapBiasAutoOffset are bound, and the config load calls upscaler.PostLoad().
//  3. The ini fragment documents both keys.
//  4. The menu card is registered in the Textures tab right after upstream's "mipmap" card, which still exists.
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

// Only what the compiler would see: `#if 0` blocks, `//` and `/* */` comments, and the inside of string and character
// literals are blanked, so a wiring line that was commented out, disabled or moved into a string does not count.
static std::string StripNonCode(const std::string& text)
{
    std::string kept;
    {
        std::istringstream in(text);
        std::string line;
        int skipDepth = 0; // > 0 inside an `#if 0` block (nested #if counted)
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
        std::puts("usage: mip_bias_seam_test <repo root>");
        return 2;
    }
    const std::string root = argv[1];

    // ---- 0. the code filter itself: commented-out, disabled and quoted copies of a wiring line must not count -------------
    {
        const std::string sample = "void f()\n{\n    // mipbias::OnFeatureCreate(a);\n    /* mipbias::OnFeatureCreate(b); */\n"
                                   "    const char* s = \"mipbias::OnFeatureCreate(c);\";\n#if 0\n    mipbias::OnFeatureCreate(d);\n"
                                   "#if 1\n    mipbias::OnFeatureCreate(e);\n#endif\n#endif\n    mipbias::OnFeatureCreate(f);\n}\n";
        const std::string code = StripNonCode(sample);
        CHECK(Count(code, "mipbias::OnFeatureCreate(") == 1);
        CHECK(Count(code, "\n    mipbias::OnFeatureCreate(f);") == 1);
    }

    // ---- 1. the call in ApplyForcedQuality ------------------------------------------------------------------------
    {
        const std::string seams = StripNonCode(ReadAll(root + "/OptiScaler/ext/upscaler/UpscalerSeams.cpp"));
        // Live code only, and the call starts a statement (not part of a comment, an `#if 0` block or a string).
        CHECK(Count(seams, "\n    mipbias::OnFeatureCreate(width, height, outWidth, outHeight, MvLowResFlag(params));") == 1);
        CHECK(Count(seams, "mipbias::OnFeatureCreate(") == 1);

        // The MVLowRes create flag is read from the NGX parameters the seam is handed (output scaling counts only with
        // low-res motion vectors or render == display).
        CHECK(seams.find("params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags)") != std::string::npos);
        CHECK(seams.find("NVSDK_NGX_DLSS_Feature_Flags_MVLowRes") != std::string::npos);

        // The create path takes no forced-preset input: a login screen in native DLAA and a DLAA the player chose look
        // the same to it (study 3.2). The glue must not know about the forced-quality state.
        const std::string glue = ReadAll(root + "/OptiScaler/ext/upscaler/MipBias.cpp");
        CHECK(glue.find("dlaaRequired") == std::string::npos);
        CHECK(glue.find("DlaaRequired") == std::string::npos);
        CHECK(glue.find("forcePerfQuality") == std::string::npos);
        CHECK(glue.find("ForcedQuality") == std::string::npos);
        CHECK(glue.find("forced") == std::string::npos);
        CHECK(glue.find("ForcePerfQuality") == std::string::npos);
        CHECK(glue.find("PerfQuality") == std::string::npos);
        CHECK(glue.find("(pq") == std::string::npos && glue.find(" pq") == std::string::npos);

        const size_t fn = seams.find("bool ApplyForcedQuality(");
        const size_t call = seams.find("mipbias::OnFeatureCreate(");
        const size_t gate = seams.find("ShouldApplyForcedQuality(", fn);
        CHECK(fn != std::string::npos && call != std::string::npos && gate != std::string::npos);
        CHECK(fn < call && call < gate); // inside ApplyForcedQuality, before the forced-quality gate

        if (fn < call)
        {
            const std::string before = seams.substr(fn, call - fn);
            CHECK(before.find("EnsureHooks()") != std::string::npos); // after the hook install ...
            CHECK(before.find("return") == std::string::npos);        // ... and no early return in between
            CHECK(Count(before, "if (") == 1);                        // the only branch is `if (auto* quality = ...)`
        }
    }

    // ---- 2. the config keys and their load hook ---------------------------------------------------------------------
    {
        const std::string cfg = ReadAll(root + "/OptiScaler/ext/core/ExtConfig.cpp");
        CHECK(cfg.find("b.Add(\"Mipmap\", \"MipmapBiasAuto\", mipBiasAuto);") != std::string::npos);
        CHECK(cfg.find("b.Add(\"Mipmap\", \"MipmapBiasAutoOffset\", mipBiasAutoOffset);") != std::string::npos);

        const size_t post = cfg.find("void ExtConfig::PostLoad()");
        const size_t hook = cfg.find("upscaler.PostLoad();");
        CHECK(post != std::string::npos && hook != std::string::npos && post < hook);
        CHECK(Count(cfg, "upscaler.PostLoad();") == 1);

        const std::string header = ReadAll(root + "/OptiScaler/ext/upscaler/UpscalerConfig.h");
        // No default on purpose since the bind-time work: an explicit false must survive a save (FFXIV defaults to on).
        CHECK(header.find("CustomOptional<bool, NoDefault> mipBiasAuto;") != std::string::npos);
        CHECK(header.find("CustomOptional<float> mipBiasAutoOffset { 0.0f };") != std::string::npos);

        // PostLoad reaches the glue through a pointer the glue registers, never through a symbol of MipBias.cpp: the
        // unit tests that build ExtConfig.cpp (chara_*, hdr_config, hudless_config) link without the glue.
        CHECK(header.find("static inline LoadedHook onLoaded = nullptr;") != std::string::npos);
        CHECK(header.find("void PostLoad()\n    {\n        if (onLoaded != nullptr)\n            onLoaded(") !=
              std::string::npos);
        CHECK(cfg.find("UpscalerConfig::PostLoad") == std::string::npos);

        const std::string glue = ReadAll(root + "/OptiScaler/ext/upscaler/MipBias.cpp");
        CHECK(glue.find("(UpscalerConfig::onLoaded = &OnConfigLoaded, true);") != std::string::npos);
        CHECK(glue.find("UpscalerConfig::PostLoad") == std::string::npos);

        // A create that arrives before the config load's hook was registered still configures the policy first.
        const size_t ensure = glue.find("EnsureConfigured();\n\n    const Outcome o = OnRawCreate(");
        CHECK(ensure != std::string::npos);
    }

    // ---- 3. the ini fragment ----------------------------------------------------------------------------------------
    {
        const std::string ini = ReadAll(root + "/OptiScaler/ext/ini/Mipmap.ini");
        CHECK(ini.find("\nMipmapBiasAuto=auto\n") != std::string::npos);
        CHECK(ini.find("\nMipmapBiasAutoOffset=auto\n") != std::string::npos);
    }

    // ---- 4. the menu card sits after upstream's "mipmap" card, which must still exist under that id -------------------
    {
        const std::string card = ReadAll(root + "/OptiScaler/ext/upscaler/menu/MipBiasMenu.cpp");
        CHECK(card.find("{ \"mipmap_auto\", \"Textures\", \"Automatic Mipmap Bias\", Draw, \"mipmap\",") !=
              std::string::npos);
        CHECK(card.find("ctx.state.swapchainApi != Vulkan") != std::string::npos);
        CHECK(card.find("\"Follow the upscaling ratio\"") != std::string::npos);
        CHECK(card.find("\"Extra offset\", &offset, -3.0f, 3.0f") != std::string::npos);

        // A value typed with Ctrl+click is limited to the slider range too (otherwise the ini holds 10 while the policy
        // uses 3), and what is stored goes through ClampOffset.
        CHECK(card.find("\"%.2f\", ImGuiSliderFlags_AlwaysClamp)") != std::string::npos);
        CHECK(card.find("settings.mipBiasAutoOffset = static_cast<float>(mipbias::ClampOffset(offset));") !=
              std::string::npos);

        // The Scale override blocks auto: the card shows the red status line instead of the numbers, the note is shown
        // always, and the card notices a tick of "MB Scale Override" made between two creates.
        CHECK(card.find("mipbias::OnScaleOverrideMaybeChanged();") != std::string::npos);
        // ... and ONLY there: nothing else of the module runs every frame or every draw (the sampler hooks are upstream's), so a tick
        // made while this card is not drawn (only upstream's Mipmap Bias card pinned to the Custom Tab, another tab open) waits for
        // the next feature create. The texts say so (third review, M2).
        CHECK(card.find("is noticed while this card is on screen,") != std::string::npos);
        CHECK(card.find("otherwise at the next quality change.") != std::string::npos);
        CHECK(Count(ReadAll(root + "/OptiScaler/ext/upscaler/MipBias.cpp"), "OnScaleOverrideMaybeChanged") == 1); // the definition
        CHECK(Count(card, "OnScaleOverrideMaybeChanged") == 1);                                                  // the one caller
        const std::string docs = ReadAll(root + "/docs/FEATURES.md");
        CHECK(docs.find("takes effect at once while the Automatic Mipmap Bias card is on screen, and otherwise at the next quality change") !=
              std::string::npos);
        CHECK(docs.find("Ticking or unticking Scale while the option is on takes effect at once (") == std::string::npos);
        CHECK(card.find("if (status.blocked)\n            Red(mipbias::BlockedLine(status.blocked));") !=
              std::string::npos);
        CHECK(card.find("ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.3f, 1.0f));") != std::string::npos);
        CHECK(card.find("    Dim(mipbias::CardNote());\n}") != std::string::npos);

        // The glue hands the live Scale override to every entry point of the policy.
        const std::string glue = ReadAll(root + "/OptiScaler/ext/upscaler/MipBias.cpp");
        CHECK(glue.find("Config::Instance()->MipmapBiasScaleOverride.value_or_default()") != std::string::npos);
        CHECK(glue.find("create.scaleMode = ScaleOverrideOn();") != std::string::npos);
        // Every entry point of the policy gets the live flag, each pinned in its own function body (a count could be met by
        // another occurrence while one hand-over is missing): the Configure and OnCreate parameters have no default, so a
        // missing one does not compile either. Six places in all: the definition and the five below.
        CHECK(Count(glue, "ScaleOverrideOn()") == 6);
        CHECK(glue.find("bool ScaleOverrideOn() { return Config::Instance()->MipmapBiasScaleOverride.value_or_default(); }") !=
              std::string::npos);
        const auto body = [&glue](const char* head) {
            const size_t at = glue.find(head);
            const size_t end = at == std::string::npos ? at : glue.find("\n}\n", at);
            return end == std::string::npos ? std::string() : glue.substr(at, end - at);
        };
        CHECK(body("void EnsureConfigured()").find("settings.mipBiasAutoOffset.value_or_default(), ScaleOverrideOn());") !=
              std::string::npos);
        CHECK(body("void OnConfigLoaded(float offset)")
                  .find("const Outcome o = Configure(policy, enabled, offset, ScaleOverrideOn());") != std::string::npos);
        CHECK(body("void OnFeatureCreate(").find("create.scaleMode = ScaleOverrideOn();") != std::string::npos);
        CHECK(body("void OnSettingChanged()")
                  .find("const Outcome o = Configure(CurrentPolicy(), enabled, offset, ScaleOverrideOn());") !=
              std::string::npos);
        CHECK(body("void OnScaleOverrideMaybeChanged()").find("const bool scale = ScaleOverrideOn();") != std::string::npos);

        // Once per episode: the config load re-arms the pending warning only when it started the episode, and the
        // catch-up in EnsureConfigured (which runs at a create, with the logger up) logs the warning it owes.
        CHECK(glue.find("    Write(o);\n\n    // No log here") != std::string::npos);
        CHECK(glue.find("    if (o.warn)\n        policy.warned = false;\n}") != std::string::npos);
        CHECK(glue.find("    if (o.warn) // only called from a create, so the logger exists\n        LOG_WARN(") !=
              std::string::npos);

        // If a sync renames or drops upstream's card, ours would silently move to the end of the tab.
        const std::string menu = ReadAll(root + "/OptiScaler/menu/menu_common.cpp");
        CHECK(menu.find("{ \"mipmap\", \"Textures\", \"Mipmap Bias\", nullptr, RenderApiAndTextureSettings }") !=
              std::string::npos);
    }

    // ---- 5. upstream's CustomOptional is what mip_bias_glue's FakeOverride copies ---------------------------------------------
    // If an upstream sync changes one of these bodies, the fake would keep passing while the product loses the user's value
    // (for example a set_volatile_value that overwrites the saved ini value every time): fail here, and update the fake.
    {
        const std::string config = NoSpace(StripNonCode(ReadAll(root + "/OptiScaler/Config.h")));
        const char* const bodies[] = {
            // set_volatile_value: the previously set value is saved only while not volatile
            "constexprvoidset_volatile_value(constT&value){if(!_volatile){if(this->has_value())_configIni=this->value();"
            "else_configIni=std::nullopt;}_volatile=true;std::optional<T>::operator=(value);}",
            // set_from_config
            "constexprvoidset_from_config(conststd::optional<T>&opt){if(!this->has_value()){_configIni=opt;"
            "std::optional<T>::operator=(opt);}}",
            // every operator= overload clears the volatile flag
            "constexprCustomOptional&operator=(constT&value){_volatile=false;std::optional<T>::operator=(value);return*this;}",
            "constexprCustomOptional&operator=(T&&value){_volatile=false;std::optional<T>::operator=(std::move(value));"
            "return*this;}",
            "constexprCustomOptional&operator=(conststd::optional<T>&opt){_volatile=false;std::optional<T>::operator=(opt);"
            "return*this;}",
            "constexprCustomOptional&operator=(std::optional<T>&&opt){_volatile=false;"
            "std::optional<T>::operator=(std::move(opt));return*this;}",
            // value_for_config of a NoDefault option: the saved ini value while volatile
            "requires(defaultState!=WithDefault){if(_volatile)return_configIni;if(this->has_value())returnthis->value();"
            "returnstd::nullopt;}",
        };
        for (const char* body : bodies)
        {
            const bool found = config.find(body) != std::string::npos;
            if (!found)
                std::printf("upstream CustomOptional changed: %s\n", body);
            CHECK(found);
        }
    }

    if (g_failed != 0)
    {
        std::printf("%d check(s) failed\n", g_failed);
        return 1;
    }

    std::puts("PASS: automatic mip bias wiring: one unconditional call in ApplyForcedQuality, config keys bound, "
              "load hook, ini fragment, menu card anchor");
    return 0;
}
