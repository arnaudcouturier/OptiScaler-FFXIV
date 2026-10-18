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

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::puts("usage: mip_bias_seam_test <repo root>");
        return 2;
    }
    const std::string root = argv[1];

    // ---- 1. the call in ApplyForcedQuality ------------------------------------------------------------------------
    {
        const std::string seams = ReadAll(root + "/OptiScaler/ext/upscaler/UpscalerSeams.cpp");
        CHECK(Count(seams, "mipbias::OnFeatureCreate(width, height, outWidth, outHeight);") == 1);

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
        CHECK(header.find("CustomOptional<bool> mipBiasAuto { false };") != std::string::npos);
        CHECK(header.find("CustomOptional<float> mipBiasAutoOffset { 0.0f };") != std::string::npos);
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

        // If a sync renames or drops upstream's card, ours would silently move to the end of the tab.
        const std::string menu = ReadAll(root + "/OptiScaler/menu/menu_common.cpp");
        CHECK(menu.find("{ \"mipmap\", \"Textures\", \"Mipmap Bias\", nullptr, RenderApiAndTextureSettings }") !=
              std::string::npos);
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
