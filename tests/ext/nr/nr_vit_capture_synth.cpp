// Writes synthetic NR ViT captures with the real DlssNrVitCapture.h formatter, so that tools/nr-vit-capture-summary.ps1 is
// checked against the exact bytes the product writes (run_nr_vit_capture_summary.ps1 builds and runs this, then the script).
// Usage: nr_vit_capture_synth <output folder>; creates <folder>\<scenario>\capture.txt for each scenario.
//
// The fake model: encoder kernels, then a ViT run (Start, three FFN/attention blocks, End), then a decoder. Pointer values
// are made up; their sharing pattern is what the summarizer reads.
#include "ext/nr/native/DlssNrVitCapture.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

using namespace DlssNrVitCapture;

namespace
{
constexpr uint64_t kIn = 0xC400000000ull, k1dA = 0xC400100000ull, k1dB = 0xC400200000ull, kSync = 0xC400300000ull,
                   kOut = 0xC400400000ull, kGlobal = 0xC400500000ull, kY = 0xC400600000ull, kWeights = 0xC410000000ull;

struct Scenario
{
    const char* name;
    bool leak = false;       // a kernel after the run references the run's sync counter
    bool foreign = false;    // a foreign kernel inside the run
    bool noOutside = false;  // only cc_vit_* launches captured
    bool rotateOut = false;  // the End kernel's output pointer changes every evaluation
    bool badOrder = false;   // the order differs in the second evaluation
};

std::vector<unsigned char> Params(std::initializer_list<uint64_t> qwords, bool dims = false)
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

struct Fake
{
    Recorder rec;
    uint64_t nextFn = 0x1000;

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
};

void Write(const std::filesystem::path& dir, const Scenario& s)
{
    Fake f;
    f.rec.SetWanted(true);

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
        f.rec.BeginEvaluation(&f, e == 1, 2, false);
        const uint64_t out = s.rotateOut ? kOut + 0x10000ull * e : kOut;

        if (!s.noOutside)
        {
            f.Launch(e, "cc_clear", Params({ kGlobal }), 1, 1, 1, 256, 1);
            f.Launch(e, "cc_enc_in", Params({ kGlobal, kIn }, true), 64, 1, 1, 256, 1);
        }

        f.Launch(e, "cc_vit_1d_repack_2d_to_1d_fp8", Params({ kIn, k1dA }, false), 2176, 1, 1, 256, 1);

        for (unsigned b = 0; b < 3; ++b)
        {
            const uint64_t done = kSync + 0x100ull * b, doneC = kSync + 0x1000ull + 0x100ull * b, w = kWeights + 0x10000ull * b;
            f.Launch(e, b == 0 ? "cc_vit_1d_ffn_expand_publish_fp8" : "cc_vit_1d_ffn_expand_chained_fp8",
                     Params({ k1dA, w, kY, 0, 0, 0, 0, done }, true), 544, 1, 1, 32, 4);
            if (s.foreign && b == 1)
                f.Launch(e, "cc_foreign", Params({ kGlobal }), 1, 1, 1, 32, 1);
            f.Launch(e, "cc_vit_1d_ffn_contract_chained_fp8", Params({ kY, k1dA, k1dA, 0, kSync, 0, done, doneC }, true), 136, 1, 4, 32, 4);
            f.Launch(e, "cc_vit_1d_qkv_chained_fp8", Params({ k1dA, w + 0x1000, k1dB, 0, kSync, 0, 0, 0, 0, 0 }), 272, 1, 2, 32, 4);
            if (s.badOrder && e == 2 && b == 1)
                continue;
            f.Launch(e, "cc_vit_1d_attention_chained_fp8", Params({ k1dB, k1dA, kSync, 0, 0, 0, 0, 0 }), 32, 9, 1, 32, 4);
            f.Launch(e, b == 2 ? "cc_vit_1d_projection_wait_fp8" : "cc_vit_1d_projection_chained_fp8",
                     Params({ k1dB, w + 0x2000, k1dA, 0, kSync, 0, 0, 0 }, true), 136, 1, 4, 32, 4);
        }

        f.Launch(e, "cc_vit_1d_repack_1d_to_2d_fp8", Params({ k1dA, out }, false), 2160, 1, 1, 256, 1);

        if (!s.noOutside)
        {
            f.Launch(e, "cc_dec_out", Params({ out, kGlobal }, true), 64, 1, 1, 256, 1);
            if (s.leak)
                f.Launch(e, "cc_after_leak", Params({ kSync }), 1, 1, 1, 32, 1);
        }

        f.rec.EndEvaluation(true);
    }

    std::filesystem::create_directories(dir);
    std::ofstream file(dir / "capture.txt", std::ios::binary);
    file << FormatHeader("20261016-135152") << f.rec.TakePending();
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
    Write(root / "good", { "good" });
    Write(root / "leak", { "leak", true });
    Write(root / "foreign", { "foreign", false, true });
    Write(root / "bare", { "bare", false, false, true });
    Write(root / "rotate", { "rotate", false, false, false, true });
    Write(root / "badorder", { "badorder", false, false, false, false, true });
    puts("nr_vit_capture_synth: wrote 6 captures");
    return 0;
}
