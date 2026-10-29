// How the replay capture is wired into the pass: the key and its default, what a capture trigger does with the key on, the
// order of the recording around a dispatch, and which image is the "before" of the on-demand capture. The source is read as
// text because the pass itself needs a GPU, a game and the NVIDIA runtime to run.
#include "ext/nr/diag/NrReplayCapture.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

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

void TestKindNames()
{
    using ext::nr::replay::KindName;
    const char* main[] = { "encode", "resolve", "downsample", "meter", "calibrate", "encode_residual", "apply_residual", "unit_exposure",
                           "normalize_motion", "compose_motion", "apply_interpolated_residual", "zero_motion", "bound_residual", "feedback",
                           "resize_guides" };
    for (uint32_t m = 0; m < 15; ++m)
        CHECK(KindName("dlssnr", m) == main[m]);
    CHECK(KindName("dlssnr", 99) == "mode_99");
    CHECK(KindName("guide_match", 14) == "guide_match");
    CHECK(KindName("guide_match", 0) == "guide_match");
    CHECK(KindName("spatial", 100) == "spatial_pack");
    CHECK(KindName("spatial", 101) == "spatial_guides");
    CHECK(KindName("spatial_guides", 101) == "spatial_guides");
    CHECK(KindName("spatial", 102) == "spatial_unpack");
    CHECK(KindName("spatial", 7) == "spatial_7");
    // no kind has a space or an equals sign: the manifest keeps one record to a line
    for (uint32_t m = 0; m < 120; ++m)
    {
        const std::string k = KindName("dlssnr", m);
        CHECK(k.find(' ') == std::string::npos && k.find('=') == std::string::npos && !k.empty());
    }
}

void TestKeyAndFragments(const std::string& repo)
{
    const std::string cfgH = Read(repo + "/OptiScaler/ext/nr/NrConfig.h");
    const std::string cfgCpp = Read(repo + "/OptiScaler/ext/nr/NrConfig.cpp");
    const std::string ini = Read(repo + "/OptiScaler/ext/ini/DlssNr.ini");
    const std::string manifest = Read(repo + "/OptiScaler/ext/core/MenuManifest.h");

    // default false: a build that never sets the key never records
    CHECK(Count(cfgH, "CustomOptional<bool> replayCapture { false };") == 1);
    CHECK(Count(cfgCpp, "b.Add(\"DlssNr\", \"ReplayCapture\", replayCapture);") == 1);

    // an ini-only developer key: documented as such, listed once
    const size_t at = ini.find("\nReplayCapture=auto");
    CHECK(at != std::string::npos);
    if (at != std::string::npos)
    {
        const size_t start = ini.rfind("\n\n", at);
        const std::string block = ini.substr(start, at - start);
        CHECK(Has(block, "No menu control") && Has(block, "true or false - Default (auto) is false") && Has(block, "never share or commit"));
    }
    CHECK(Count(ini, "ReplayCapture=") == 1);
    CHECK(Count(manifest, "\"[DlssNr] ReplayCapture\"") == 1);
}

void TestPassWiring(const std::string& repo)
{
    const std::string pass = Read(repo + "/OptiScaler/ext/nr/pass/DlssNr_Dx12.cpp");

    // the on-demand capture's "before" is the untouched game frame, never the proxy the encode wrote
    CHECK(Count(pass, "g_capture.recordBefore(cmdList, device, g_nr.hdrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);") == 1);
    CHECK(!Has(pass, "recordBefore(cmdList, device, g_nr.colorCopy"));
    CHECK(Count(pass, "g_capture.recordAfter(cmdList, device, target, targetState);") == 1);
    // and it is taken right after the encode's barriers, before anything can edit the frame
    const size_t before = pass.find("g_capture.recordBefore(");
    const size_t afterEncode = pass.find("// Below full resolution the model is shown");
    const size_t resolve = pass.find("DlssNrConstants resolveParams {};");
    CHECK(before != std::string::npos && afterEncode != std::string::npos && resolve != std::string::npos);
    CHECK(before < afterEncode && afterEncode < resolve);

    // the scene pass opens a frame with the key, and closes it whichever way it returns
    CHECK(Count(pass, "g_replay.BeginFrame(ext::Cfg().nr.replayCapture.value_or_default())") == 1);
    CHECK(Count(pass, "g_replay.EndFrame(g_frames);") == 1);
    CHECK(Count(pass, "g_replay.ReadyToWrite(g_frames)") == 1);

    // a capture trigger with the key on records a replay set and nothing else; with it off, the old capture
    const size_t request = pass.find("void RequestCapture(unsigned int frames)");
    CHECK(request != std::string::npos);
    const std::string body = pass.substr(request, 1000);
    const size_t keyed = body.find("ext::Cfg().nr.replayCapture.value_or_default()");
    const size_t replay = body.find("g_replay.Request()");
    const size_t old = body.find("g_capture.request(frames);");
    CHECK(keyed != std::string::npos && replay != std::string::npos && old != std::string::npos && keyed < replay && replay < old);
    CHECK(Has(body, "return;"));
    CHECK(Has(pass, "bool CaptureInProgress() { return g_capture.isActive() || g_replay.Requested() || g_replay.WritePending(); }"));

    // clearing the capture folder at the first request keeps the replay sets
    CHECK(Has(pass, "name.rfind(\"replay-\", 0) == 0"));
    CHECK(!Has(pass, "std::filesystem::remove_all(dir, ec);"));

    // a dispatch: inputs recorded before it, outputs after it, nothing unless a frame is being recorded
    const size_t beforeCall = pass.find("g_replay.BeforeDispatch(InCmdList, _device, replay)");
    const size_t dispatch = pass.find("InCmdList->Dispatch(dispatchWidth, dispatchHeight, 1);");
    const size_t afterCall = pass.find("g_replay.AfterDispatch(InCmdList, _device);");
    CHECK(beforeCall != std::string::npos && dispatch != std::string::npos && afterCall != std::string::npos);
    CHECK(beforeCall < dispatch && dispatch < afterCall);
    CHECK(Has(pass.substr(0, beforeCall), "if (g_replay.Recording())"));
    // every way the pass gives up on a dispatch before it reaches the recorder still takes its index as left out
    const size_t passStart = pass.find("bool DlssNr_Dx12::DispatchPass(");
    CHECK(passStart != std::string::npos && passStart < beforeCall);
    const std::string early = pass.substr(passStart, beforeCall - passStart);
    CHECK(Count(early, "return false;") == 3 && Count(early, "g_replay.DispatchDropped();\r\n        return false;") + Count(early, "g_replay.DispatchDropped();\n        return false;") == 3);
    // the views recorded are the ones created above them: typed member of a typeless family, depth plane format as it is
    CHECK(Has(pass, "format != DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS ? TranslateTypelessFormats(format) : format;"));
    CHECK(Has(pass, "b.viewFormat = TranslateTypelessFormats(uavs[i]->GetDesc().Format);"));
    // the game's exposure texture is never copied: its state is not known
    CHECK(Has(pass, "b.readable = srvs[i] != g_replayForeign;"));
    CHECK(Has(pass, "g_replayForeign = (ID3D12Resource*) frame.ExposureTexture;"));

    // writes are noted at the barriers
    CHECK(Has(pass, "if (g_replay.Recording() && ((from & writeStates) != 0 || (to & writeStates) != 0))"));
    CHECK(Has(pass, "constexpr UINT writeStates = D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_COPY_DEST;"));

    // the boundaries of the pass
    for (const char* name : { "\"game_frame\"", "\"output_scaling_up\"", "\"output_scaling_down\"", "\"final_frame\"", "\"model_answer_\"" })
        CHECK(Count(pass, std::string("g_replay.AddBoundary(cmdList, device, ") + name) + Count(pass, std::string("(") + name) >= 1);
}

// What the pass says when the set cannot be written, and how the local replay test is started.
void TestWriteReportAndLocalTest(const std::string& repo)
{
    const std::string pass = Read(repo + "/OptiScaler/ext/nr/pass/DlssNr_Dx12.cpp");
    const size_t failedWrite = pass.find("if (!written.ok)");
    const size_t errorLine = pass.find("LOG_ERROR(\"DLSS-NR could not write the replay set {}: {}\"");
    const size_t wroteLine = pass.find("LOG_INFO(\"DLSS-NR wrote a replay set to {}");
    CHECK(failedWrite != std::string::npos && errorLine != std::string::npos && wroteLine != std::string::npos);
    CHECK(failedWrite < errorLine && errorLine < wroteLine); // a failed write never reaches the "wrote" line
    CHECK(Has(pass.substr(failedWrite, 300), "return;"));
    CHECK(Has(pass, "LOG_WARN(\"DLSS-NR replay set {} is incomplete"));
    CHECK(Has(pass, "written.skipped, written.boundariesSkipped"));

    const std::string test = Read(repo + "/tests/ext/nr/nr_replay.cpp");
    const std::string recipe = Read(repo + "/tests/ext/nr/nr_replay.test.psd1");
    // a bless request is gated before anything else, with its own exit code
    const size_t gate = test.find("DecideRunMode(blessRequest, Env(\"OPTISCALER_NR_REPLAY_BLESS_OK\"))");
    const size_t refused = test.find("return kExitBlessRefused;");
    const size_t usage = test.find("argc != 3");
    const size_t discovery = test.find("fs::directory_iterator(root, ec)");
    CHECK(gate != std::string::npos && refused != std::string::npos && usage != std::string::npos && discovery != std::string::npos);
    CHECK(gate < refused && refused < usage && usage < discovery);
    // the commit is read from the repository named on the command line, with its dirty flag
    CHECK(Count(test, "rev-parse HEAD") == 1 && Has(test, "git -C \\\"\" + repo + \"\\\" rev-parse HEAD"));
    CHECK(Has(test, "status --porcelain --untracked-files=no") && Has(test, "commit = GitHead(repo, dirty);"));
    // the names that match nothing are refused before any write, and a banner ends a bless run
    CHECK(Has(test, "BlessKindProblems(bless, sets)") && Has(test, "BlessBanner(bless, commit, dirty)"));
    CHECK(Has(recipe, "Args        = @('{env:OPTISCALER_NR_REPLAY_DIR}', '{repo}')") && Has(recipe, "OPTISCALER_NR_REPLAY_BLESS_OK=1"));
    // the replayer is only ever the software adapter, and the wait is long
    const std::string engine = Read(repo + "/OptiScaler/ext/nr/diag/NrReplayEngine.h");
    CHECK(Has(engine, "AdapterIsWarp(adapter.VendorId, adapter.DeviceId)") && Has(engine, "kWaitMilliseconds = 10 * 60 * 1000"));
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    TestKindNames();
    TestKeyAndFragments(repo);
    TestPassWiring(repo);
    TestWriteReportAndLocalTest(repo);
    std::printf(failures == 0 ? "nr_replay_wiring: all checks passed\n" : "nr_replay_wiring: %d check(s) failed\n", failures);
    return failures;
}
