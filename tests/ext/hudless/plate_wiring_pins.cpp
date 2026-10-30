// Text pins of the native nameplates' wiring: the seams that reach the plate engine, the one-line draw intercept, the
// pixel-shader stamp, the load and menu order (snapshot first, feature bit second, never a clear from the menu), and the
// absence of the plugin-era plate machinery. Comments and whitespace are ignored, so a pin names the code, not the prose.
//
// What this protects: with the key off nothing of this feature may run, and no path may reach the engine before the
// snapshot says the key is on (the presenting threads read the snapshot and act on the bit).
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>

namespace
{
int g_failures = 0;
#define EXPECT(cond, ...)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::printf("    FAIL line %d: %s  ", __LINE__, #cond);                                                    \
            std::printf(__VA_ARGS__);                                                                                  \
            std::printf("\n");                                                                                         \
        }                                                                                                              \
    } while (0)

std::string Read(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::string out;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text.compare(i, 2, "//") == 0)
        {
            while (i < text.size() && text[i] != '\n')
                ++i;
            continue;
        }
        if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n')
            out += text[i];
    }
    return out;
}

std::string Ext(const std::string& repo, const char* relative) { return Read(repo + "/OptiScaler/ext/" + relative); }

size_t Find(const std::string& text, const std::string& needle, size_t from = 0) { return text.find(needle, from); }
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

size_t Count(const std::string& text, const std::string& needle)
{
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
        ++n;
    return n;
}

// Occurrences of `needle` in every header and source of ext/, the whole extension surface of the product.
// A seam that is called from two places is found here even when both calls look right on their own.
size_t CountInExt(const std::string& repo, const std::string& needle)
{
    size_t total = 0;
    std::error_code error;
    const std::filesystem::path root = std::filesystem::path(repo) / "OptiScaler" / "ext";
    for (auto file = std::filesystem::recursive_directory_iterator(root, error);
         !error && file != std::filesystem::recursive_directory_iterator(); file.increment(error))
    {
        if (!file->is_regular_file(error))
            continue;
        const std::string extension = file->path().extension().string();
        if (extension != ".h" && extension != ".cpp")
            continue;
        total += Count(Read(file->path().string()), needle);
    }
    return total;
}

struct Pin
{
    const char* fragment;
    const char* what;
};

// Each fragment must appear exactly once: an edit that changes the line removes it, a copy elsewhere does not hide it.
void Pins(const std::string& code, const char* file, std::initializer_list<Pin> pins)
{
    for (const Pin& pin : pins)
    {
        const size_t n = Count(code, pin.fragment);
        EXPECT(n == 1, "%s: %s (%zu occurrences of %s)", file, pin.what, n, pin.fragment);
    }
}
} // namespace

int main(int, char** argv)
{
    const std::string repo = argv[1];

    // ---- the draw intercept and the pixel-shader stamp (the only two lines in the lighting hooks) ----------------------------------
    const std::string hooks = Ext(repo, "ffxiv/lighting/FfxivLightingCapture.h");
    Pins(hooks, "FfxivLightingCapture.h",
         {
             { "#include\"ext/ffxiv/plates/PlateNative.h\"", "the game side is included by the draw hook" },
             { "#include\"ext/ffxiv/plates/PlateShaderKind.h\"", "the shader stamp header is included" },
             // the intercept is the last test before the game's own draw path, only for the game's own context and only
             // outside our own hooks; a true answer means the plate engine drew the draw.
             { "if(c==context&&!inside&&ext::ffxiv::plates::InterceptIndexed(c,n,start,b,reinterpret_cast<void*>(indexed)))return;",
               "the DrawIndexed intercept" },
             { "ext::ffxiv::plates::OnPixelShaderCreated(*out,crc);", "the nameplate pixel-shader stamp (one line, the CRC already computed)" },
         });
    // The intercept sits in the indexed hook only: the other draw kinds are untouched.
    EXPECT(Count(hooks, "InterceptIndexed(") == 1, "exactly one intercept in the lighting hooks");

    // ---- the engine hooks in the plate seam header, each behind the plate bit, and no second wiring ---------------------
    // One wiring only: the seam header calls each plate seam exactly once, and nothing else under ext/ calls one. A second
    // call publishes a Native record over every Hidden one at the same present, which makes the feature do nothing in the
    // mode it is written for and can lose the plates of a frame.
    const std::string engine = Ext(repo, "hudless/HudlessEngine.cpp");
    EXPECT(Count(engine, "plates::On") == 0, "HudlessEngine.cpp reaches no plate seam (%zu)",
           Count(engine, "plates::On"));
    for (const char* seam : { "plates::OnPost(", "plates::OnRelease(", "plates::OnOverlay(", "plates::OnPresentBegin(" })
    {
        const size_t wired = CountInExt(repo, seam);
        EXPECT(wired == 1, "%s is wired exactly once in OptiScaler/ext (%zu)", seam, wired);
    }

    // ---- the seams: three bits on the shared ones, one bit on the multiplier note -----------------------------------------------------
    const std::string seams = Ext(repo, "hudless/HudlessSeams.h");
    Pins(seams, "HudlessSeams.h",
         {
             { "if(ext::Active(ext::F::Plate))ext::hudless::plates::OnPost(sc);", "PostInteropCopy reaches the plate engine" },
             { "if(ext::Active(ext::F::Plate))ext::hudless::plates::OnRelease();", "OnReleaseInterop reaches the plate engine" },
             { "if(ext::Active(ext::F::Plate))ext::hudless::plates::OnOverlay(swapChain,queue);", "BeforeOverlay reaches the plate engine" },
             { "if(ext::Active(ext::F::Plate))ext::hudless::plates::NoteMultiplier(outputsPerFrame);", "the multiplier note reaches the plate engine" },
             { "if(ext::ActiveAny(ext::F::Hudless,ext::F::PlateProbe,ext::F::Plate)){", "the shared seams test all three bits once" },
         });

    // ---- the load order and the menu order: snapshot first, bit second, and the bit is never cleared there ---------------
    const std::string config = Ext(repo, "hudless/HudlessConfig.cpp");
    const size_t publish = Find(config, "hudless::PublishPlateSettings(PlateSettings());");
    const size_t bit = Find(config, "if(GetPlateInterp()!=hudless::PlateInterp::Off)ext::SetActive(ext::F::Plate,true);");
    EXPECT(publish != std::string::npos && bit != std::string::npos && publish < bit,
           "PostLoad publishes the plate settings before it sets the feature bit");
    EXPECT(Count(config, "SetActive(ext::F::Plate,false)") == 0, "the config never clears the plate bit (the engine's drain owns it)");

    const std::string menu = Ext(repo, "hudless/HudlessMenu.cpp");
    const size_t menuPublish = Find(menu, "ext::hudless::PublishPlateSettings(snapshot);");
    const size_t menuBit = Find(menu, "if(snapshot.interp!=ext::hudless::PlateInterp::Off)ext::SetActive(ext::F::Plate,true);");
    EXPECT(menuPublish != std::string::npos && menuBit != std::string::npos && menuPublish < menuBit,
           "the menu publishes the snapshot before it sets the bit");
    EXPECT(Count(menu, "SetActive(ext::F::Plate,false)") == 0, "the menu never clears the plate bit");
    Pins(menu, "HudlessMenu.cpp",
         {
             { "cfg.plateInterp=std::string(kInterpNames[interp]);", "the switch key has a control" },
             { "cfg.platePhaseOffset=std::string(kPhaseNames[phase]);", "the shift constant has a control" },
             { "cfg.plateMaxStep=step;", "the step cap has a control" },
         });

    // ---- the engine owns its bit: the default sink is the ext bit itself ------------------------------------------------
    const std::string plates = Ext(repo, "hudless/UiPlates.cpp");
    Pins(plates, "UiPlates.cpp",
         {
             { "voidDefaultBitSink(boolon){ext::SetActive(ext::F::Plate,on);}", "the engine's default bit sink" },
             { "RegisterBitSink(){SetPlateFeatureBitSink(&DefaultBitSink);}", "the sink is registered at load" },
         });

    // ---- the plugin-era plate machinery is gone --------------------------------------------------------------------------
    const char* files[] = { "ffxiv/plates/PlateNative.cpp", "ffxiv/plates/PlateNative.h", "hudless/UiPlates.cpp",
                            "hudless/UiPlates.h", "hudless/PlateEngine.h", "hudless/PlateSource.h", "hudless/HudlessMenu.cpp",
                            "hudless/HudlessSeams.h", "ffxiv/lighting/FfxivLightingCapture.h" };
    for (const char* file : files)
    {
        const std::string code = Ext(repo, file);
        EXPECT(!Has(code, "NoteCopied"), "%s: no serial hand-off", file);
        EXPECT(!Has(code, "companionHudReplacement"), "%s: no HUD replacement plugin", file);
        EXPECT(!Has(code, "BatchGuard") && !Has(code, "OnEnqueue") && !Has(code, "OnBindState"), "%s: no batch hooks", file);
        EXPECT(!Has(code, "ActiveBuild().companion") && !Has(code, "CompanionVisibility"), "%s: no plugin visibility path", file);
    }

    if (g_failures == 0)
        puts("PASS: plate wiring pins: the two hook lines, one wiring per plate seam, the snapshot-before-bit order at "
             "load and in the menu, the engine's own bit, and no plugin-era plate machinery");
    return g_failures == 0 ? 0 : 1;
}
