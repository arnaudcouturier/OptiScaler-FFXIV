// Text pins of the developer draw diagnostic: where it hangs, what the key off costs, and that it stays a developer switch.
//
// The diagnostic is a log-only probe, so what must not drift is its wiring: one relaxed atomic load on the draw path (and nothing else
// before the branch), the reflection recorded where the shaders are created, the mark of the upscale boundary, the frame closed at the
// Present entry whatever else is armed, the key defaults of today's behaviour, the ini-only entries and the session's single capture.
// Comments and whitespace are ignored, so an edit that changes any of those lines removes its pin.
#include <cstdio>
#include <cstdlib>
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

std::string Code(const std::string& repo, const char* relative)
{
    std::ifstream file(repo + "/OptiScaler/ext/" + relative, std::ios::binary);
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

std::string Raw(const std::string& repo, const char* relative)
{
    std::ifstream file(repo + "/OptiScaler/ext/" + relative, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

size_t Find(const std::string& text, const std::string& needle, size_t from = 0) { return text.find(needle, from); }
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

size_t Count(const std::string& text, const std::string& needle)
{
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
        ++n;
    return n;
}

struct Pin
{
    const char* fragment;
    const char* what;
};

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

    // ---- the draw path: one relaxed atomic load, taken before any of the hook's other work, and only once
    {
        const std::string hooks = Code(repo, "ffxiv/lighting/FfxivLightingCapture.h");
        EXPECT(Has(hooks, "#include\"ext/ffxiv/diag/DrawDiag.h\""), "include");
        Pins(hooks, "lighting", {
            { R"x(if(!inside&&ext::ffxiv::diag::Capturing())ext::ffxiv::diag::OnDraw(c,kind);)x", "the draw hook asks the key first" },
            { R"x(ext::ffxiv::diag::OnPixelShaderCreated(*out,code,size,crc);)x", "the reflection is recorded where the shaders are created" },
        });
        EXPECT(Count(hooks, "ext::ffxiv::diag::") == 3, "the diagnostic is called from the two places only");
        const size_t ui = Find(hooks, "ext::ffxiv::ui::OnDraw(c,c==context);");
        const size_t diag = Find(hooks, "ext::ffxiv::diag::Capturing()");
        const size_t earlyOut = Find(hooks, "constboolwanted=");
        EXPECT(ui != std::string::npos && diag != std::string::npos && earlyOut != std::string::npos && ui < diag && diag < earlyOut,
               "the diagnostic sees every draw: after the UI detector and before the hook's first early-out");
        const size_t inside = Find(hooks, "if(SUCCEEDED(hr)&&out&&*out&&code&&size>=32");
        const size_t created = Find(hooks, "ext::ffxiv::diag::OnPixelShaderCreated");
        EXPECT(inside != std::string::npos && created != std::string::npos && inside < created,
               "the reflection is recorded on the same path as the bytecode CRC, not in a second pass");
    }

    // ---- the key test itself: one load, no state, no D3D in the header
    {
        const std::string header = Code(repo, "ffxiv/diag/DrawDiag.h");
        Pins(header, "DrawDiag.h", {
            { R"x(inlineboolCapturing(){returnext::Active(ext::F::DrawDiag);})x", "the key off is one relaxed atomic load" },
        });
        EXPECT(!Has(header, "std::atomic"), "the hot header keeps no state of its own");
        EXPECT(!Has(header, "#include<d3d11"), "the hot header pulls in no D3D");
        const std::string features = Code(repo, "ExtFeatures.h");
        EXPECT(Has(features, "DrawDiag=1ull<<12,"), "the feature bit");
        EXPECT(Has(features, "inlineboolActive(Ff){return(detail::g_activeFeatures.load(std::memory_order_relaxed)"), "Active() is a relaxed load");
    }

    // ---- the frame and the capture: closed at the Present entry, one capture per session, disarmed when it is written
    {
        const std::string ui = Code(repo, "ffxiv/ui/FfxivUiBoundary.cpp");
        Pins(ui, "present", {
            { R"x(ext::ffxiv::diag::OnPresent();)x", "the frame of the diagnostic is closed at the Present entry" },
        });
        const size_t catalog = Find(ui, "ext::ffxiv::catalog::OnPresent();");
        const size_t diag = Find(ui, "ext::ffxiv::diag::OnPresent();");
        const size_t armed = Find(ui, "RefreshArmed();");
        EXPECT(catalog != std::string::npos && diag != std::string::npos && armed != std::string::npos && catalog < diag && diag < armed,
               "the diagnostic is closed before the Armed() test: it must not need the UI detector to be armed");

        const std::string seams = Code(repo, "Seams.cpp");
        Pins(seams, "evaluate", {
            { R"x(ext::ffxiv::diag::OnUpscaleBoundary();)x", "the upscale boundary is marked at the evaluate" },
        });
        const size_t jitter = Find(seams, "ext::ffxiv::jitter::OnUpscaleBoundary(parameters);");
        const size_t boundary = Find(seams, "ext::ffxiv::diag::OnUpscaleBoundary();");
        EXPECT(jitter != std::string::npos && boundary != std::string::npos && jitter < boundary, "the marker sits in ext::dx11bridge::OnUpscaleBoundary");

        const std::string cpp = Code(repo, "ffxiv/diag/FfxivDrawDiag.cpp");
        Pins(cpp, "runtime", {
            { R"x(if(S().phase.load(std::memory_order_relaxed)!=kCapturing||context==nullptr)return;)x", "the draw path leaves at once when no capture is running" },
            { R"x(voidOnPresent(){if(S().phase.load(std::memory_order_relaxed)!=kCapturing)return;)x", "the present path leaves after one load" },
            { R"x(if(s.phase.load(std::memory_order_relaxed)!=kIdle)return;)x", "a capture happens once per session, whatever the config reload does" },
            { R"x(s.phase.store(kCapturing,std::memory_order_relaxed);ext::SetActive(ext::F::DrawDiag,true);)x", "the bit is set last, so a draw never sees it without a capture behind it" },
            { R"x(s.phase.store(kDone,std::memory_order_relaxed);ext::SetActive(ext::F::DrawDiag,false);)x", "a written capture puts the draw path back to one load" },
            { R"x(if(s.drawsInFrame>=kMinDrawsPerFrame))x", "only a rendered frame (not a loading screen) counts" },
            { R"x(constexprconstchar*kLightParamName="g_LightParam";)x", "the constant buffer of the lighting passes is found by its reflected name" },
            { R"x(if(s.lastReadQpc==0||SecondsBetween(s.lastReadQpc,now)>=1.0))x", "the constant buffer is read at most once a second" },
            { R"x(if(s.drawsInFrame>kMaxRowsPerFrame))x", "the per-frame row cap" },
            { R"x(ext::FfxivConfig::drawDiagnosticsOnLoaded=&ext::ffxiv::diag::OnConfigLoaded;)x", "the key reaches the diagnostic through the config's hook pointer" },
        });
        // Markers are noted before the cap: a frame that loses rows still reports its tone map, UI and boundary ordinals.
        const size_t note = Find(cpp, "s.markers.NoteDraw(row.stage,row.ordinal);");
        const size_t cap = Find(cpp, "if(s.drawsInFrame>kMaxRowsPerFrame)");
        EXPECT(note != std::string::npos && cap != std::string::npos && note < cap, "markers are noted before the row cap is applied");
        // The marker state is reset with the frame, so ordinals never leak into the next one.
        const size_t reset = Find(cpp, "s.markers.Reset();");
        const size_t finished = Find(cpp, "if(s.framesCaptured>=s.wantedFrames)Finish();");
        EXPECT(reset != std::string::npos && finished != std::string::npos && reset < finished, "the frame is reset before the capture can finish");
        // Nothing in the output names a path, a user or a machine.
        EXPECT(!Has(cpp, "GetUserName") && !Has(cpp, "GetComputerName") && !Has(cpp, "C:\\\\"), "no user, machine or absolute path in the output");
        EXPECT(!Has(cpp, "CreateFileA(\"C:") && !Has(cpp, "getenv"), "no environment path");
    }

    // ---- the draw arguments are remembered for both probes behind the same one atomic test
    {
        const std::string plate = Code(repo, "ffxiv/ui/PlateDraws.h");
        Pins(plate, "NoteDraw", {
            { R"x(if(ext::ActiveAny(ext::F::PlateProbe,ext::F::DrawDiag))g_args={kind,count,instances,start,baseVertex};)x", "one load serves both probes" },
        });
        EXPECT(Count(plate, "ext::hudless::plate::Enabled()") == 1, "the argument note no longer tests one probe's bit only (the vertex shader stamp still does)");
    }

    // ---- the keys: today's behaviour by default, ini only, and the frame count is clamped
    {
        const std::string ffxivConfig = Code(repo, "ffxiv/FfxivConfig.h");
        Pins(ffxivConfig, "FfxivConfig.h", {
            { R"x(CustomOptional<std::string>drawDiagnostics{"off"};)x", "the default is off: today's behaviour" },
            { R"x(CustomOptional<int32_t>drawDiagnosticsFrames{3};)x", "three frames by default" },
            { R"x(drawDiagnosticsOnLoaded(drawDiagnostics.value_or_default()=="capture");)x", "any other value counts as off, and the key is published at the end of every config load" },
        });
        const std::string configCpp = Code(repo, "core/ExtConfig.cpp");
        Pins(configCpp, "ExtConfig.cpp", {
            { R"x(b.Add("FFXIV","DrawDiagnostics",drawDiagnostics);)x", "the key" },
            { R"x(b.Add("FFXIV","DrawDiagnosticsFrames",drawDiagnosticsFrames);)x", "the frame count key" },
        });
        const std::string readme = Raw(repo, "ini/FFXIV.ini");
        EXPECT(Has(readme, "\nDrawDiagnostics=auto"), "the ini fragment documents the key");
        EXPECT(Has(readme, "\nDrawDiagnosticsFrames=auto"), "the ini fragment documents the frame count");
        // The ini-only rule of the menu manifest test: a key without a control says so in its own comment.
        for (const char* key : { "DrawDiagnostics=", "DrawDiagnosticsFrames=" })
        {
            const size_t at = readme.find(key);
            EXPECT(at != std::string::npos, "fragment line %s", key);
            if (at == std::string::npos)
                continue;
            size_t begin = at;
            while (begin > 0 && readme[begin - 1] != '\n')
                --begin;
            // walk the comment block above the key line
            size_t block = begin;
            while (block > 0)
            {
                size_t previous = block - 1;
                while (previous > 0 && readme[previous - 1] != '\n')
                    --previous;
                if (readme.compare(previous, 1, ";") != 0)
                    break;
                block = previous;
            }
            EXPECT(readme.find("No menu control", block) < at, "the comment of %s says that it has no menu control", key);
        }
        const std::string manifest = Code(repo, "core/MenuManifest.h");
        EXPECT(Has(manifest, "\"[FFXIV]DrawDiagnostics\",") && Has(manifest, "\"[FFXIV]DrawDiagnosticsFrames\","),
               "both keys are listed as ini-only");
        // The frame count is clamped where it is used: a key of 0 or 999 cannot make a capture write no frames or a hundred.
        const std::string cpp = Code(repo, "ffxiv/diag/FfxivDrawDiag.cpp");
        EXPECT(Has(cpp, "static_cast<uint32_t>(v<1?1:(v>kMaxFrames?kMaxFrames:v))"), "the frame count is clamped to 1..16");
    }

    // ---- the two pure headers stay pure: no D3D, no file, no clock beyond the run id
    for (const char* pure : { "ffxiv/diag/DrawDiagFormat.h", "ffxiv/diag/ShaderBindings.h" })
    {
        const std::string text = Code(repo, pure);
        EXPECT(!Has(text, "#include<d3d11") && !Has(text, "ID3D11"), "%s is D3D free (the unit test builds it alone)", pure);
        EXPECT(!Has(text, "std::ofstream") && !Has(text, "std::filesystem"), "%s does no I/O (the writer is in the runtime)", pure);
    }

    if (g_failures)
    {
        std::printf("FAIL: %d expectation(s)\n", g_failures);
        return 1;
    }
    std::puts("PASS: draw diagnostic wiring pins (one load on the draw path, reflection at shader creation, boundary and Present marks, "
              "one capture per session, defaults off, ini only)");
    return 0;
}
