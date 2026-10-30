// How the Classic-enlargement sharpening is wired into the pass: the ini lines and their defaults, the
// load-time clamp, every conjunct of the runtime skip rule, the strength slot the shader reads, the
// one-shot pipeline creation, the copy-back order and the state restore on both outcomes, and the menu
// controls that grey out exactly while the runtime would skip. The source is read as text because the
// pass itself needs a GPU, a game and the NVIDIA runtime to run.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
int failures = 0;

void Check(bool ok, const char* what, int line)
{
    if (!ok)
    {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

std::string Read(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    std::string out;
    for (char c : text)
        if (c != '\r')
            out.push_back(c);
    return out;
}

size_t Count(const std::string& text, const std::string& part)
{
    size_t n = 0;
    for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size()))
        ++n;
    return n;
}

bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// every part appears in order; a reordered gate or copy-back fails
bool InOrder(const std::string& text, const std::vector<std::string>& parts)
{
    size_t at = 0;
    for (const auto& part : parts)
    {
        at = text.find(part, at);
        if (at == std::string::npos)
            return false;
        at += part.size();
    }
    return true;
}

void TestIniAndBinding(const std::string& repo)
{
    const std::string ini = Read(repo + "/OptiScaler/ext/ini/DlssNr.ini");
    const std::string config = Read(repo + "/OptiScaler/ext/nr/NrConfig.cpp");
    // both keys are bound under [DlssNr] and documented in the ini fragment
    CHECK(Count(config, "b.Add(\"DlssNr\", \"ClassicSharpen\", classicSharpen);") == 1);
    CHECK(Count(config, "b.Add(\"DlssNr\", \"ClassicSharpenStrength\", classicSharpenStrength);") == 1);
    CHECK(Has(ini, "ClassicSharpen=auto") && Has(ini, "ClassicSharpenStrength=auto"));
    // the fragment states the defaults the code declares: off, and the filter's own upstream default
    CHECK(Has(ini, "Default (auto) is false") && Has(ini, "Default (auto) is 0.4 (upstream RCAS"));
    // the load-time clamp: an uninterpretable value fails open to the default, the rest is clamped to 0..1
    CHECK(Has(config, "if (!std::isfinite(classicSharpenStrength.value_or_default()))"));
    CHECK(Count(config, "classicSharpenStrength = 0.4f;") == 1);
    CHECK(Has(config, "std::clamp(classicSharpenStrength.value_or_default(), 0.0f, 1.0f)"));
    // the declared defaults the fragment documents
    const std::string header = Read(repo + "/OptiScaler/ext/nr/NrConfig.h");
    CHECK(Has(header, "CustomOptional<bool> classicSharpen { false };"));
    CHECK(Has(header, "CustomOptional<float> classicSharpenStrength { 0.4f };"));
}

void TestRuntimeGate(const std::string& repo)
{
    const std::string pass = Read(repo + "/OptiScaler/ext/nr/pass/DlssNr_Dx12.cpp");
    // a live menu write lands past the load-time clamp, so the dispatch clamps again; uninterpretable fails open
    CHECK(Has(pass, "const float sharpenAmount = nr.classicSharpenStrength.value_or_default();"));
    CHECK(Count(pass, "const float sharpenStrength = std::isfinite(sharpenAmount) ? std::clamp(sharpenAmount, 0.0f, 1.0f) : 0.4f;") == 1);
    // the skip rule: the key, a non-zero strength, the shared sharpen rule on the geometry the resolve actually ran (the
    // applied working size, not the requested one the settle may still be holding off), the resolve's own Transfer and the
    // spatial state, a model that applies, no diagnostic view, and a plain single-mip frame target
    CHECK(Has(pass, "const bool classicSharpen = nr.classicSharpen.value_or_default() && sharpenStrength > 0.0f &&"));
    CHECK(Has(pass, "DlssNr::Hotkeys::ClassicSharpens(geometry.workScaleX, geometry.workScaleY, resolveParams.Transfer, spatial) &&"));
    // ... never on the requested scales: those already carry the next tier while the old size still renders
    CHECK(Count(pass, "ClassicSharpens(scaleX, scaleY,") == 0);
    CHECK(Has(pass, "resolveParams.ApplyModel != 0 && resolveParams.CompareMode == 0 &&"));
    CHECK(Has(pass, "resolveParams.DebugView == 0 && resolveParams.ShowSkinMask == 0 && g_nr.hdrCopy != nullptr &&"));
    CHECK(Has(pass, "sharpenTargetDesc.MipLevels == 1 && sharpenTargetDesc.DepthOrArraySize == 1;"));
    // strength zero skips the dispatch and the copy-back entirely: the gate is the only guard, before any barrier
    CHECK(Has(pass, "if (classicSharpen)"));
    CHECK(InOrder(pass, { "const bool classicSharpen =", "if (classicSharpen)",
                          "TransitionTarget(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);",
                          "Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE," }));
    // the spatial path forces Matched residual before the gate, so it never sharpens; the shared rule also takes the
    // spatial state itself, so the gate says so even without that write
    CHECK(Count(pass, "resolveParams.Transfer = 1; resolveParams.SpatialResidual = resolveWarp.residual;") == 1);
}

void TestDispatchAndCopyBack(const std::string& repo)
{
    const std::string pass = Read(repo + "/OptiScaler/ext/nr/pass/DlssNr_Dx12.cpp");
    // the dispatch carries the mode the replay names the kind from and the strength in the documented slot
    CHECK(Has(pass, "sharpen.Mode = DlssNrMode_ClassicSharpen;"));
    CHECK(Count(pass, "sharpen.TransferStrength = sharpenStrength;") == 1);
    CHECK(Count(pass, "sharpen.TransferStrength =") == 1); // the amount reaches no other slot
    // the copy-back moves the staged result over the frame, in that direction: the resolve's own copy-back and the sharpen one
    CHECK(Count(pass, "cmdList->CopyResource(target, g_nr.hdrCopy)") == 2);
    // the sharpen restore leaves the temp readable again (the 28-space continuation below is the sharpen restore, not the resolve one)
    CHECK(Count(pass, "D3D12_RESOURCE_STATE_COPY_SOURCE,\n                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);") == 1);
    // the result is staged into the dead-after-resolve temp and copied back over the frame, in that order
    CHECK(InOrder(pass, { "g_nr.hdrCopy, nullptr, _classicSharpenPipeline))",
                          "Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,",
                          "D3D12_RESOURCE_STATE_COPY_SOURCE);",
                          "TransitionTarget(D3D12_RESOURCE_STATE_COPY_DEST);" }));
    // both textures return to the states the resolve left them in, on the dispatch and on the dropped-dispatch path
    CHECK(Count(pass, "TransitionTarget(sharpenTargetState);") == 2);
    CHECK(InOrder(pass, { "The dispatch was dropped (no descriptor slot): leave the frame as the resolve",
                          "TransitionTarget(sharpenTargetState);",
                          "Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS," }));
    // the kind comes from the pipeline the dispatch ran on, with this build's blob
    CHECK(Has(pass, "else if (pipeline != nullptr && pipeline == _classicSharpenPipeline)"));
    CHECK(Has(pass, "shaderId = \"classic_sharpen\";"));
    CHECK(Has(pass, "blob = DlssNr_ClassicSharpen_cso;"));
    // the one-shot pipeline creation: tried once per device lifetime, with one log line on failure
    CHECK(Has(pass, "if (!_classicSharpenPipeline && !_classicSharpenPipelineFailed)"));
    CHECK(Count(pass, "_classicSharpenPipelineFailed = true;") == 1);
    CHECK(Count(pass, "DLSS-NR Classic sharpening unavailable") == 1);
    const std::string dx12h = Read(repo + "/OptiScaler/ext/nr/pass/DlssNr_Dx12.h");
    CHECK(Has(dx12h, "ID3D12PipelineState* _classicSharpenPipeline = nullptr;"));
    CHECK(Has(dx12h, "bool _classicSharpenPipelineFailed = false;"));
}

void TestReplayAndShaderWiring(const std::string& repo)
{
    // the recorder names the kind from the shader id, before the mode switch
    const std::string capture = Read(repo + "/OptiScaler/ext/nr/diag/NrReplayCapture.h");
    CHECK(InOrder(capture, { "if (shader == \"classic_sharpen\")", "return \"classic_sharpen\";", "switch (mode)" }));
    // the replay test resolves the id to this build's sharpen blob
    const std::string replay = Read(repo + "/tests/ext/nr/nr_replay.cpp");
    CHECK(Has(replay, "#include \"DlssNr_ClassicSharpen_Shader.h\""));
    CHECK(Has(replay, "else if (id == \"classic_sharpen\")"));
    CHECK(Has(replay, "blob = { DlssNr_ClassicSharpen_cso, sizeof(DlssNr_ClassicSharpen_cso) };"));
    // the replay run needs the generated sharpen header beside the other shader headers
    const std::string recipe = Read(repo + "/tests/ext/nr/nr_replay.test.psd1");
    CHECK(Has(recipe, "{repo}/OptiScaler/x64/Release/ext_gen/DlssNr_ClassicSharpen_Shader.h"));
    // the shader is its own compiled blob with the documented constant layout
    const std::string props = Read(repo + "/OptiScaler/ext/nr/nr.shaders.props");
    CHECK(Has(props, "<Variable>DlssNr_ClassicSharpen_cso</Variable>"));
    CHECK(Has(props, "<Header>DlssNr_ClassicSharpen_Shader.h</Header>"));
    CHECK(Has(props, "dlssnr_guide_match.hlsl") && Has(props, "dlssnr_classic_sharpen.hlsl"));
    // the layout contract names both prefix-sharing shaders: the sharpen constants must track the main struct's prefix
    CHECK(Has(props, "the first fields of dlssnr_guide_match.hlsl's and dlssnr_classic_sharpen.hlsl's Params are a prefix of it."));
    const std::string shader = Read(repo + "/OptiScaler/ext/nr/shaders/dlssnr_classic_sharpen.hlsl");
    CHECK(Has(shader, "the TransferStrength slot: the sharpen amount"));
    CHECK(Has(shader, "const float strength = saturate(gStrength);"));
    CHECK(Has(shader, "if (strength == 0.0)")); // strength zero is a copy
    CHECK(Has(shader, "gTarget[id.xy] = float4(output, e.a);")); // the frame's own alpha is kept
}

void TestMenu(const std::string& repo)
{
    const std::string menu = Read(repo + "/OptiScaler/ext/nr/menu/DlssNr_Menu.cpp");
    // the controls grey by the same shared rule the runtime gate reads, on the menu's shown scales and on the spatial
    // state of the frame being processed (one readout for the sharpening and the depth-aware control)
    CHECK(Count(menu, "const bool depthSpatial = depthReadout.valid ? depthReadout.active : nr.spatialCompression.value_or_default();") == 1);
    CHECK(Has(menu, "const bool classicApplies = DlssNr::Hotkeys::ClassicSharpens(effectiveScaleX, effectiveScaleY, nr.transfer.value_or_default(), depthSpatial);"));
    CHECK(InOrder(menu, { "const bool depthSpatial =", "const bool classicApplies = DlssNr::Hotkeys::ClassicSharpens(" }));
    CHECK(Count(menu, "ImGui::BeginDisabled(!classicApplies);") == 1);
    CHECK(Has(menu, "ImGui::BeginDisabled(!classicApplies || !nr.classicSharpen.value_or_default());"));
    // ... and the menu says why the controls are grey while spatial compression composes as matched residual
    CHECK(Has(menu, "if (depthSpatial)\n                ImGui::TextDisabled(\"Spatial compression composes as matched residual: Classic sharpening does not run.\");"));
    CHECK(Has(menu, "ImGui::Checkbox(\"Classic sharpening\""));
    CHECK(Has(menu, "if (ImGui::Checkbox(\"Classic sharpening\", &sharpen))"));
    CHECK(Has(menu, "nr.classicSharpen = sharpen;"));
    CHECK(Has(menu, "ImGui::SliderFloat(\"Classic sharpening strength\", &sharpenStrength, 0.0f, 1.0f, \"%.2f\")"));
    CHECK(Has(menu, "nr.classicSharpenStrength = sharpenStrength;"));
}

int Run(const std::string& repo)
{
    TestIniAndBinding(repo);
    TestRuntimeGate(repo);
    TestDispatchAndCopyBack(repo);
    TestReplayAndShaderWiring(repo);
    TestMenu(repo);
    return failures;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::printf("pass the repo root\n");
        return 2;
    }
    const int failed = Run(argv[1]);
    if (failed != 0)
    {
        std::printf("%d check(s) failed\n", failed);
        return 1;
    }
    std::puts("PASS: Classic sharpening wiring (ini lines and defaults, load-time and dispatch clamps, skip rule, strength slot, one-shot pipeline, copy-back order and state restore, replay and shader wiring, menu grey rule)");
    return 0;
}
