// Writes synthetic NR ViT captures with the real DlssNrVitCapture.h formatter, so that tools/nr-vit-capture-summary.ps1 is
// checked against the exact bytes the product writes (run_nr_vit_capture_summary.ps1 builds and runs this, then the script).
// Usage: nr_vit_capture_synth <output folder>; creates <folder>\<scenario>\capture.txt for each scenario.
//
// The fake model: encoder kernels, then a ViT run (Start, three FFN/attention blocks, End), then a decoder. Pointer values
// are made up; their sharing pattern is what the summarizer reads.
//
// Scenarios fall in two groups. The first six are the original single-feature captures (clean, leaked counter, foreign kernel,
// no outside launches, rotating End output, changed order). The rest follow the fixed go/no-go rule of the NR reuse study
// (STUDY-nr-flicker.md, Step A) one condition at a time: a "Passes = 2" capture is evaluation 1 = pass 0 (feature 0),
// evaluation 2 = pass 1 (feature 1, its own buffers, VitEvery as applied 1), evaluation 3 = pass 0 again; the "go" scenario
// meets every condition, and each "go_*" variant breaks exactly one of them.
#include "ext/nr/native/DlssNrVitCapture.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

using namespace DlssNrVitCapture;

namespace
{
constexpr uint64_t kIn = 0xC400000000ull, k1dA = 0xC400100000ull, k1dB = 0xC400200000ull, kSync = 0xC400300000ull,
                   kOut = 0xC400400000ull, kGlobal = 0xC400500000ull, kY = 0xC400600000ull, kCtl = 0xC400700000ull,
                   kOut2 = 0xC400800000ull, kWeights = 0xC410000000ull;
constexpr uint64_t kFeatureStride = 0x1000000ull; // pass 1 has its own buffers

// Where the clear kernel (cc_cb_clear, which resets the run's flag block to "not published") is launched.
enum class Clear
{
    None,
    Before,                // before Start in every evaluation, pointer = the control block the run shares
    After,                 // after End only
    NotLast,               // before Start, but not in the last evaluation
    OtherPointerAndAfter,  // before Start with a pointer the run does not use; the shared block is cleared after End
    Region,                // before Start with a base pointer; the run's flags sit inside the region it covers
};

struct Scenario
{
    // original scenarios
    bool leak = false;       // a kernel after the run references the run's sync counter
    bool foreign = false;    // a foreign kernel inside the run
    bool noOutside = false;  // only cc_vit_* launches captured
    bool rotateOut = false;  // the End kernel's output pointer changes every evaluation
    bool badOrder = false;   // the order differs in the second evaluation
    // per-feature scenarios
    bool twoFeatures = false;       // evaluations: pass 0, pass 1, pass 0; pass 1 has other buffers and every=1
    bool ctlShared = false;         // one Inner kernel also takes the control block pointer (a LEAK candidate)
    const char* sharer = nullptr;   // a launch after End with this kernel name ("" = name never told) takes the control block
    const char* sharer2 = nullptr;  // a second such launch
    Clear clear = Clear::None;
    bool startMoves = false;        // the 1D buffer (Start slot 1, End slot 0) changes every evaluation
    bool movedScratch = false;      // a run-private scratch buffer moves every evaluation (not an epoch)
    bool epoch = false;             // an Inner kernel gets the evaluation number as a parameter
    bool endBefore = false;         // a launch before Start carries End slot 1
    bool noConsumer = false;        // nothing after End carries End slot 1
    bool xfeature = false;          // pass 1's evaluation carries pass 0's End slot 1
    bool endPtrs = false;           // the End launch is recorded in pointer-array form (no readable parameters)
};

std::vector<unsigned char> Params(const std::vector<uint64_t>& qwords, bool dims = false)
{
    std::vector<unsigned char> b;
    for (uint64_t q : qwords)
    {
        const auto* p = reinterpret_cast<const unsigned char*>(&q);
        b.insert(b.end(), p, p + 8);
    }
    if (dims)
    {
        const uint32_t d[2] = { 68, 16 };
        const auto* p = reinterpret_cast<const unsigned char*>(d);
        b.insert(b.end(), p, p + 8);
    }
    return b;
}

// cc_cb_clear(ptr, count, other): 16 bytes, like the real one
std::vector<unsigned char> ClearParams(uint64_t ptr, uint32_t count)
{
    return Params({ ptr, static_cast<uint64_t>(count) | (static_cast<uint64_t>(411) << 32) });
}

struct Fake
{
    Recorder rec;

    void Launch(unsigned eval, const std::string& name, const std::vector<unsigned char>& params, uint32_t gx, uint32_t gy,
                uint32_t gz, uint32_t bx, uint32_t by)
    {
        LaunchRecord r;
        r.evaluation = eval;
        r.kernel = name;
        r.function = 0x7000 + std::hash<std::string>()(name) % 0x1000;
        r.grid[0] = gx; r.grid[1] = gy; r.grid[2] = gz; r.block[0] = bx; r.block[1] = by; r.block[2] = 1;
        r.paramSize = static_cast<uint32_t>(params.size());
        r.params = params.empty() ? nullptr : params.data();
        rec.Record(r);
    }

    // a launch made through the extended ABI: only the kernelParams pointer array is visible
    void LaunchPtrs(unsigned eval, const std::string& name, uint32_t gx, uint32_t bx)
    {
        LaunchRecord r;
        r.evaluation = eval;
        r.kernel = name;
        r.function = 0x7000 + std::hash<std::string>()(name) % 0x1000;
        r.grid[0] = gx; r.grid[1] = 1; r.grid[2] = 1; r.block[0] = bx; r.block[1] = 1; r.block[2] = 1;
        r.paramSize = 24;
        r.params = nullptr;
        r.kernelParams = 0x123450;
        r.ex = true;
        rec.Record(r);
    }
};

void Write(const std::filesystem::path& dir, const Scenario& s)
{
    Fake f;
    f.rec.SetWanted(true);
    int passOneKey = 0; // the recorder numbers features by the address it is given

    ModuleNote m;
    m.handle = 0x7000; m.bytes = 1417240; m.sha256 = std::string(64, '2'); m.magic = "7f454c46";
    m.dump = "modules/" + m.sha256 + ".bin"; m.status = "dumped";
    f.rec.NoteModule(m);
    for (const char* n : { "cc_vit_1d_repack_2d_to_1d_fp8", "cc_vit_1d_ffn_expand_publish_fp8", "cc_vit_1d_ffn_contract_chained_fp8",
                           "cc_vit_1d_qkv_chained_fp8", "cc_vit_1d_attention_chained_fp8", "cc_vit_1d_projection_chained_fp8",
                           "cc_vit_1d_ffn_expand_chained_fp8", "cc_vit_1d_projection_wait_fp8", "cc_vit_1d_repack_1d_to_2d_fp8" })
        f.rec.NoteFunction(0x7000 + std::hash<std::string>()(n) % 0x1000, 0x7000, n);

    for (unsigned e = 1; e <= kEvaluations; ++e)
    {
        const unsigned feature = (s.twoFeatures && e == 2) ? 1u : 0u;
        const void* featureKey = feature == 0 ? static_cast<const void*>(&f) : static_cast<const void*>(&passOneKey);
        f.rec.BeginEvaluation(featureKey, e == 1, feature == 1 ? 1u : 2u, false);

        const uint64_t off = feature * kFeatureStride;
        const uint64_t in = kIn + off, global = kGlobal + off, sync = kSync + off, b1d = k1dB + off, weights = kWeights + off,
                       ctl = kCtl + off, out2 = kOut2 + off;
        const uint64_t a1d = k1dA + off + (s.startMoves ? 0x100000ull * e : 0ull);
        const uint64_t y = kY + off + (s.movedScratch ? 0x10000ull * e : 0ull);
        const uint64_t out = (s.rotateOut ? kOut + 0x10000ull * e : kOut) + off;
        const uint64_t region = sync - 0x1000ull;

        if (!s.noOutside)
        {
            if (s.clear == Clear::Before || (s.clear == Clear::NotLast && e < kEvaluations))
                f.Launch(e, "cc_cb_clear", ClearParams(ctl, 0x4000), 64, 1, 1, 256, 1);
            if (s.clear == Clear::OtherPointerAndAfter || s.clear == Clear::Region)
                f.Launch(e, "cc_cb_clear", ClearParams(region, 0x4000), 64, 1, 1, 256, 1);
            f.Launch(e, "cc_clear", Params({ global }), 1, 1, 1, 256, 1);
            f.Launch(e, "cc_enc_in", Params({ global, in }, true), 64, 1, 1, 256, 1);
            if (s.endBefore)
                f.Launch(e, "cc_before_user", Params({ out }), 1, 1, 1, 32, 1);
        }

        f.Launch(e, "cc_vit_1d_repack_2d_to_1d_fp8", Params({ in, a1d }, false), 2176, 1, 1, 256, 1);

        for (unsigned b = 0; b < 3; ++b)
        {
            const uint64_t done = sync + 0x100ull * b, doneC = sync + 0x1000ull + 0x100ull * b, w = weights + 0x10000ull * b;
            std::vector<uint64_t> expand = { a1d, w, y, 0, 0, 0, 0, done };
            if (s.ctlShared && b == 0)
                expand.push_back(ctl);
            f.Launch(e, b == 0 ? "cc_vit_1d_ffn_expand_publish_fp8" : "cc_vit_1d_ffn_expand_chained_fp8", Params(expand, true), 544, 1, 1,
                     32, 4);
            if (s.foreign && b == 1)
                f.Launch(e, "cc_foreign", Params({ global }), 1, 1, 1, 32, 1);
            f.Launch(e, "cc_vit_1d_ffn_contract_chained_fp8", Params({ y, a1d, a1d, 0, sync, 0, done, doneC }, true), 136, 1, 4, 32, 4);
            f.Launch(e, "cc_vit_1d_qkv_chained_fp8",
                     Params({ a1d, w + 0x1000, b1d, (s.epoch && b == 0) ? static_cast<uint64_t>(e) : 0ull, sync, 0, 0, 0, 0, 0 }), 272, 1, 2, 32, 4);
            if (s.badOrder && e == 2 && b == 1)
                continue;
            f.Launch(e, "cc_vit_1d_attention_chained_fp8", Params({ b1d, a1d, sync, 0, 0, 0, 0, 0 }), 32, 9, 1, 32, 4);
            f.Launch(e, b == 2 ? "cc_vit_1d_projection_wait_fp8" : "cc_vit_1d_projection_chained_fp8",
                     Params({ b1d, w + 0x2000, a1d, 0, sync, 0, 0, 0 }, true), 136, 1, 4, 32, 4);
        }

        if (s.endPtrs)
            f.LaunchPtrs(e, "cc_vit_1d_repack_1d_to_2d_fp8", 2160, 256);
        else
            f.Launch(e, "cc_vit_1d_repack_1d_to_2d_fp8", Params({ a1d, out }, false), 2160, 1, 1, 256, 1);

        if (!s.noOutside)
        {
            f.Launch(e, "cc_dec_out", Params({ s.noConsumer ? out2 : out, global }, true), 64, 1, 1, 256, 1);
            if (s.leak)
                f.Launch(e, "cc_after_leak", Params({ sync }), 1, 1, 1, 32, 1);
            if (s.sharer)
                f.Launch(e, s.sharer, Params({ ctl }), 1, 1, 1, 32, 1);
            if (s.sharer2)
                f.Launch(e, s.sharer2, Params({ ctl }), 1, 1, 1, 32, 1);
            if (s.clear == Clear::After || s.clear == Clear::OtherPointerAndAfter)
                f.Launch(e, "cc_cb_clear", ClearParams(ctl, 0x4000), 64, 1, 1, 256, 1);
            if (s.xfeature && feature == 1)
                f.Launch(e, "cc_other_pass", Params({ kOut }), 1, 1, 1, 32, 1);
        }

        f.rec.EndEvaluation(true);
    }

    std::filesystem::create_directories(dir);
    std::ofstream file(dir / "capture.txt", std::ios::binary);
    file << FormatHeader("20261016-135152") << f.rec.TakePending();
}

// meets every condition of the fixed rule: Passes = 2, one LEAK pointer shared only with cc_cb_clear, cleared before Start in
// every evaluation, nothing after End uses End's output buffer
void Go(Scenario& s)
{
    s.twoFeatures = true;
    s.ctlShared = true;
    s.clear = Clear::Before;
    s.noConsumer = true;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        puts("usage: nr_vit_capture_synth <output folder>");
        return 2;
    }

    const std::filesystem::path root = argv[1];
    int count = 0;
    const auto emit = [&](const char* name, const std::function<void(Scenario&)>& configure) {
        Scenario s;
        configure(s);
        Write(root / name, s);
        ++count;
    };

    // original single-feature captures
    emit("good", [](Scenario&) {});
    emit("leak", [](Scenario& s) { s.leak = true; });
    emit("foreign", [](Scenario& s) { s.foreign = true; });
    emit("bare", [](Scenario& s) { s.noOutside = true; });
    emit("rotate", [](Scenario& s) { s.rotateOut = true; });
    emit("badorder", [](Scenario& s) { s.badOrder = true; });

    // Passes = 2 (pass 0 / pass 1 / pass 0) with nothing else special
    emit("passes2", [](Scenario& s) { s.twoFeatures = true; });
    // the fixed rule, one condition at a time
    emit("go", Go);
    emit("go_endmoves", [](Scenario& s) { Go(s); s.rotateOut = true; });
    emit("go_startmoves", [](Scenario& s) { Go(s); s.startMoves = true; });
    emit("go_leakdata", [](Scenario& s) { Go(s); s.sharer = "cc_after_data"; });
    emit("go_leakwaiter", [](Scenario& s) { Go(s); s.sharer = "cc_after_waiter_fp8"; }); // ends in _waiter: an ordinary kernel
    emit("go_wait", [](Scenario& s) { Go(s); s.sharer = "cc_after_wait_fp8"; });
    emit("go_chained", [](Scenario& s) { Go(s); s.sharer = "cc_after_chained"; });
    emit("go_tilesync", [](Scenario& s) { Go(s); s.sharer = "cc_after_tilesync_fp8"; });
    emit("go_publish", [](Scenario& s) { Go(s); s.sharer = "cc_after_publish_fp8"; });
    emit("go_unknown", [](Scenario& s) { Go(s); s.sharer = ""; });
    emit("go_clearafter", [](Scenario& s) { Go(s); s.clear = Clear::After; });
    emit("go_clearlast", [](Scenario& s) { Go(s); s.clear = Clear::NotLast; });
    emit("go_clearother", [](Scenario& s) { Go(s); s.clear = Clear::OtherPointerAndAfter; });
    emit("go_epoch", [](Scenario& s) { Go(s); s.epoch = true; });
    emit("go_movedscratch", [](Scenario& s) { Go(s); s.movedScratch = true; });
    emit("go_endbefore", [](Scenario& s) { Go(s); s.endBefore = true; });
    emit("go_endafter", [](Scenario& s) { Go(s); s.noConsumer = false; });
    emit("go_xfeature", [](Scenario& s) { Go(s); s.xfeature = true; });
    emit("go_endptrs", [](Scenario& s) { Go(s); s.endPtrs = true; });
    // what the real captures look like: the shared block is used by encoder/decoder waiters and an ordinary kernel, and the
    // clear kernel's pointer is a region base that the run's flags sit inside
    emit("realistic", [](Scenario& s) {
        s.twoFeatures = true;
        s.ctlShared = true;
        s.sharer = "cc_after_chained_fp8";
        s.sharer2 = "cc_after_data";
        s.clear = Clear::Region;
    });

    printf("nr_vit_capture_synth: wrote %d captures\n", count);
    return 0;
}
