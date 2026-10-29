// Replays a captured NR frame (see docs/notes/nr-frame-replay.md) on the Direct3D 12 software adapter with the shaders of
// this build, and compares every output with the blessed images of the set, bit for bit.
//
// Arguments: the replay folder, then the repository the shaders come from. The replay folder is either one set (it holds
// manifest.txt) or a folder of sets (each subfolder with a manifest.txt is replayed on its own and carries its own blessed
// images). The repository is only asked what commit it is on, when blessing.
//
// Environment:
//   OPTISCALER_NR_REPLAY_BLESS     empty: compare. "all": write the blessed images of every dispatch. A comma separated list of
//                                  dispatch kinds (encode, resolve, ...): rewrite only those kinds and compare the rest. A kind that
//                                  no dispatch of any replayed set has is an error and nothing is written.
//   OPTISCALER_NR_REPLAY_BLESS_OK  must be 1 for the request above to take effect. Without it nothing is replayed or written and the
//                                  run ends with exit code 3, so a request left in the environment never re-blesses by itself.
//   OPTISCALER_NR_REPLAY_COMMIT    recorded in the provenance of a blessing (default: the commit the repository is on)
//
// Exit code 0: every dispatch of every set matched (compare), or was blessed and every write succeeded (bless). 1: a mismatch,
// an incomplete capture, a set with nothing blessed, an error. 2: wrong arguments. 3: a bless request without confirmation.
// A set with nothing blessed fails; it is never skipped.
#define NOMINMAX
#include <windows.h>

#include "DlssNr_ClassicSharpen_Shader.h"
#include "DlssNr_GuideMatch_Shader.h"
#include "DlssNr_Shader.h"
#include "DlssNr_Spatial_Guides_Shader.h"
#include "DlssNr_Spatial_Shader.h"
#include "ext/nr/diag/NrReplayEngine.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>

using namespace ext::nr::replay;
namespace fs = std::filesystem;

namespace
{
std::string Env(const char* name)
{
    char* value = nullptr;
    size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr)
        return {};
    std::string text(value);
    std::free(value);
    return text;
}

// The first line the command prints, without its line ending. Empty when it prints nothing or cannot run.
std::string FirstLine(const std::string& command)
{
    std::string out;
    if (FILE* p = _popen(command.c_str(), "r"))
    {
        char buf[256] = {};
        if (std::fgets(buf, sizeof(buf), p))
            out = buf;
        _pclose(p);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out;
}

// The commit of the repository that was named on the command line (not of the folder the process happens to run in), and
// whether it has uncommitted changes to tracked files. When git cannot say, the commit is "unknown" and the tree counts as dirty.
std::string GitHead(const std::string& repo, bool& dirty)
{
    const std::string head = FirstLine("git -C \"" + repo + "\" rev-parse HEAD 2>nul");
    if (head.empty())
    {
        dirty = true;
        return "unknown";
    }
    dirty = !FirstLine("git -C \"" + repo + "\" status --porcelain --untracked-files=no 2>nul").empty();
    return head;
}

bool Lookup(const std::string& id, Blob& blob)
{
    if (id == "dlssnr")
        blob = { DlssNr_cso, sizeof(DlssNr_cso) };
    else if (id == "guide_match")
        blob = { DlssNr_GuideMatch_cso, sizeof(DlssNr_GuideMatch_cso) };
    else if (id == "spatial")
        blob = { DlssNr_Spatial_cso, sizeof(DlssNr_Spatial_cso) };
    else if (id == "spatial_guides")
        blob = { DlssNr_Spatial_Guides_cso, sizeof(DlssNr_Spatial_Guides_cso) };
    else if (id == "classic_sharpen")
        blob = { DlssNr_ClassicSharpen_cso, sizeof(DlssNr_ClassicSharpen_cso) };
    else
        return false;
    return true;
}

// A dispatch whose two replays differ cannot be compared bit for bit: say so, rather than blessing noise.
bool Reproducible(Replayer& replayer, const fs::path& dir)
{
    SetFiles set;
    std::string error;
    if (!LoadSet(dir, set, error) || !CheckComplete(set.manifest).empty())
        return true; // the run reports it
    bool ok = true;
    for (const Dispatch& d : set.manifest.dispatches)
    {
        Blob blob;
        std::map<uint32_t, Output> a, b;
        if (!Lookup(d.shader, blob) || !replayer.Run(set, d, blob.data, blob.size, a, error) || !replayer.Run(set, d, blob.data, blob.size, b, error))
        {
            if (replayer.Abandoned())
                return true; // the run reports the timeout; nothing more is replayed on this adapter
            continue;        // the run reports it
        }
        for (const auto& [slot, out] : a)
            if (b.count(slot) == 0 || b[slot].bytes != out.bytes)
            {
                std::printf("FAIL: dispatch %u:%u kind=%s is not reproducible on this adapter (two replays differ); give the kind a tolerance\n",
                            d.frame, d.index, d.kind.c_str());
                ok = false;
                break;
            }
    }
    return ok;
}
} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // First of all: a bless request in the environment is never acted on without its confirmation.
    const std::string blessRequest = Env("OPTISCALER_NR_REPLAY_BLESS");
    const RunMode mode = DecideRunMode(blessRequest, Env("OPTISCALER_NR_REPLAY_BLESS_OK"));
    if (mode == RunMode::Refused)
    {
        std::printf("BLESS mode requested (OPTISCALER_NR_REPLAY_BLESS=%s) but OPTISCALER_NR_REPLAY_BLESS_OK=1 is not set.\n"
                    "Nothing was replayed and nothing was written. This is not a pass and not a comparison. Unset the request to compare, "
                    "or set the confirmation to bless.\n",
                    blessRequest.c_str());
        return kExitBlessRefused;
    }

    if (argc != 3 || argv[1][0] == '\0' || argv[2][0] == '\0')
    {
        std::printf("pass the replay folder and the repository\n");
        return 2;
    }
    const fs::path root = argv[1];
    const std::string repo = argv[2];

    std::vector<fs::path> sets;
    std::vector<fs::path> withoutManifest;
    if (fs::exists(root / "manifest.txt"))
        sets.push_back(root);
    else
    {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(root, ec))
        {
            if (!e.is_directory())
                continue;
            if (fs::exists(e.path() / "manifest.txt"))
                sets.push_back(e.path());
            else if (fs::exists(e.path() / "images"))
                withoutManifest.push_back(e.path()); // a capture that was cut short: never skipped silently
        }
    }
    if (sets.empty() && withoutManifest.empty())
    {
        std::printf("FAIL: no replay set (manifest.txt) in %s\n", root.string().c_str());
        return 1;
    }
    for (const fs::path& dir : withoutManifest)
        std::printf("FAIL: %s has images but no manifest.txt (a capture that was cut short, or a failed write)\n", dir.string().c_str());

    Replayer replayer;
    std::string error;
    if (!replayer.Init(error))
    {
        std::printf("FAIL: %s\n", error.c_str());
        return 1;
    }

    const WarpInfo warp = GetWarpInfo();
    std::printf("software adapter: version %s, file hash %s\n", warp.version.c_str(), Hex64(warp.hash).c_str());
    if (!warp.Valid())
    {
        std::printf("FAIL: the software adapter could not be identified (%s); a replay on it could not be told from another\n", warp.error.c_str());
        return 1;
    }

    BlessSpec bless;
    std::string commit;
    bool dirty = false;
    if (mode == RunMode::Bless)
    {
        bless = ParseBless(blessRequest);
        commit = GitHead(repo, dirty);
        const std::string override_ = Env("OPTISCALER_NR_REPLAY_COMMIT");
        if (!override_.empty())
            commit = override_;
        const std::string problem = BlessKindProblems(bless, sets);
        std::printf("BLESS MODE: this run rewrites the blessed images of %s at commit %s%s. It is not a comparison.\n", blessRequest.c_str(),
                    commit.c_str(), dirty ? " (the working tree has uncommitted changes)" : "");
        if (!bless.Any() || !problem.empty() || !withoutManifest.empty())
        {
            const char* why = !problem.empty() ? problem.c_str()
                              : !bless.Any()   ? "the bless request names no kind. Nothing was written."
                                               : "a capture folder is incomplete. Nothing was written.";
            std::printf("FAIL: %s\n", why);
            return 1;
        }
    }

    char date[32] = {};
    const std::time_t now = std::time(nullptr);
    std::tm local {};
    localtime_s(&local, &now);
    std::strftime(date, sizeof(date), "%Y-%m-%d", &local);

    int failed = static_cast<int>(withoutManifest.size());
    for (const fs::path& dir : sets)
    {
        std::printf("set %s\n", dir.string().c_str());
        bool ok = Reproducible(replayer, dir);

        RunOptions o;
        o.setDir = dir;
        o.outDir = dir / "replay-out";
        o.bless = bless;
        o.commit = commit;
        o.dirty = dirty;
        o.date = date;
        o.warp = warp;
        o.shaders = Lookup;
        const RunReport r = RunReplaySet(o, replayer);
        for (const auto& line : r.lines)
            std::printf("  %s\n", line.c_str());
        if (r.comparedToCaptured > 0)
            std::printf("  information only: %d outputs compared with what the game's GPU wrote, largest component error %.6g\n",
                        r.comparedToCaptured, r.maxErrorVsCaptured);
        ok = ok && r.ok;
        if (!ok)
            ++failed;
        if (replayer.Abandoned())
        {
            std::printf("FAIL: the software adapter timed out or was removed; the remaining sets were not replayed\n");
            ++failed;
            break;
        }
    }
    if (mode == RunMode::Bless)
    {
        if (failed == 0)
            std::printf("%s: every write succeeded\n", BlessBanner(bless, commit, dirty).c_str());
        else
            std::printf("BLESS did not complete: %d set(s) failed; the blessed images may be partly rewritten\n", failed);
    }
    std::printf(failed == 0 ? "nr_replay: all sets passed\n" : "nr_replay: %d set(s) failed\n", failed);
    return failed == 0 ? 0 : 1;
}
