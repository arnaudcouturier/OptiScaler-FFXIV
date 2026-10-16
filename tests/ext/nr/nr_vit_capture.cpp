// Host check of DlssNrVitCapture.h: the record formatter, the budgets and the run-folder rules of the NR ViT capture
// (ini [DlssNr] VitCapture). No GPU, no game, no files. The same records are parsed by tools/nr-vit-capture-summary.ps1 in
// nr_vit_capture_summary.
// cl /std:c++20 /EHsc /I OptiScaler tests/ext/nr/nr_vit_capture.cpp
#include "ext/nr/native/DlssNrVitCapture.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace DlssNrVitCapture;

static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

static size_t Count(const std::string& text, const std::string& needle)
{
    size_t n = 0;
    for (size_t p = text.find(needle); p != std::string::npos; p = text.find(needle, p + needle.size()))
        ++n;
    return n;
}

static bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

// A launch struct shaped like NVAPI_CU_KERNEL_LAUNCH_PARAMS(_EX), so Observe() is exercised without NvAPI headers.
struct Dim3 { uint32_t x, y, z; };
struct BasicLaunch
{
    void* hFunction;
    Dim3 gridDim, blockDim;
    uint32_t dynSharedMemBytes;
    const void* pParams;
    uint32_t paramSize;
};
struct ExLaunch
{
    void* hFunction;
    Dim3 gridDim, blockDim;
    uint32_t dynSharedMemBytes;
    const void* pParams;
    uint32_t paramSize;
    void** kernelParams;
};

int main()
{
    // ---- small helpers ----
    CHECK(Hex("\x01\xab\xff", 3) == "01abff");
    CHECK(Hex(nullptr, 0).empty());
    CHECK(Handle(0) == "0x0" && Handle(0x1a2b) == "0x1a2b" && Handle(0xffffffffffffffffull) == "0xffffffffffffffff");
    CHECK(IsVitKernel("cc_vit_1d_ffn_expand_fp8") && IsVitKernel("cc_vit_ffn_expand") && !IsVitKernel("cc_cb_clear") &&
          !IsVitKernel("") && !IsVitKernel("cc_vit"));
    {
        const std::string blob = std::string(100, '\0') + "xx cc_vit_1d_repack_2d_to_1d_fp8" + std::string(10, '\0');
        CHECK(ContainsVitKernel(blob.data(), blob.size()));
        const std::string other = "cc_cb_clear\0cc_vi";
        CHECK(!ContainsVitKernel(other.data(), other.size()));
        CHECK(!ContainsVitKernel(nullptr, 100) && !ContainsVitKernel("cc_vit_", 0) && ContainsVitKernel("cc_vit_", 7));
    }

    // ---- the LAUNCH record, exactly (the summarizer parses this) ----
    {
        unsigned char params[24];
        for (unsigned i = 0; i < 24; ++i)
            params[i] = static_cast<unsigned char>(i);
        LaunchRecord r;
        r.evaluation = 1; r.ordinal = 7; r.function = 0x1a2b; r.kernel = "cc_vit_1d_repack_2d_to_1d_fp8";
        r.grid[0] = 2176; r.grid[1] = 1; r.grid[2] = 1; r.block[0] = 256; r.block[1] = 1; r.block[2] = 1;
        r.shared = 0; r.paramSize = 24; r.params = params;
        CHECK(FormatLaunch(r) ==
              "LAUNCH eval=1 ord=7 fn=0x1a2b kernel=cc_vit_1d_repack_2d_to_1d_fp8 vit=1 role=start abi=basic grid=2176x1x1 "
              "block=256x1x1 shared=0 psize=24 pmode=blob pbytes=24 trunc=0 "
              "params=000102030405060708090a0b0c0d0e0f1011121314151617\n");

        // unknown name, not a ViT kernel, EX pointer array (size unknowable, never read), and no parameters at all
        LaunchRecord e;
        e.evaluation = 2; e.ordinal = 0; e.function = 0x10; e.ex = true; e.kernelParams = 0xdead0000;
        e.grid[0] = e.grid[1] = e.grid[2] = 1; e.block[0] = 32; e.block[1] = 4; e.block[2] = 1;
        CHECK(FormatLaunch(e) ==
              "LAUNCH eval=2 ord=0 fn=0x10 kernel=? vit=0 role=none abi=ex grid=1x1x1 block=32x4x1 shared=0 psize=0 "
              "pmode=ptrs pbytes=0 trunc=0 kp=0xdead0000 params=-\n");
        LaunchRecord n;
        n.kernel = "cc_cb_clear";
        CHECK(Has(FormatLaunch(n), "vit=0 role=none abi=basic") && Has(FormatLaunch(n), "pmode=none pbytes=0 trunc=0 params=-"));

        // big parameter blocks are cut at kMaxParamBytes and say so
        std::vector<unsigned char> big(1000, 0x5a);
        LaunchRecord b;
        b.kernel = "cc_vit_1d_x"; b.params = big.data(); b.paramSize = 1000;
        const std::string line = FormatLaunch(b);
        CHECK(Has(line, "psize=1000 pmode=blob pbytes=512 trunc=1 params="));
        CHECK(line.size() == line.find("params=") + 7 + 1024 + 1);
        CHECK(Has(line, "role=inner"));
    }

    // ---- other records ----
    {
        const std::string h = FormatHeader("20261016-135152");
        CHECK(Has(h, "CAPTURE format=1 evaluations_max=3 launches_max=512 param_bytes_max=512 run=20261016-135152\n"));
        CHECK(h.rfind("# ", 0) == 0);
        ModuleNote m;
        m.handle = 0x7ff0; m.bytes = 1417240; m.sha256 = std::string(64, 'a'); m.magic = "7f454c46";
        m.dump = "modules/" + m.sha256 + ".bin"; m.status = "dumped";
        CHECK(FormatModule(m) == "MODULE handle=0x7ff0 bytes=1417240 sha256=" + std::string(64, 'a') +
                                     " magic=7f454c46 dump=modules/" + std::string(64, 'a') + ".bin status=dumped\n");
        ModuleNote none;
        none.handle = 1; none.bytes = 5; none.sha256 = "unavailable"; none.status = "no_digest";
        CHECK(Has(FormatModule(none), "magic=- dump=- status=no_digest"));
        CHECK(FormatFunc(0x20, 0x7ff0, "cc_vit_1d_repack_1d_to_2d_fp8") ==
              "FUNC fn=0x20 module=0x7ff0 kernel=cc_vit_1d_repack_1d_to_2d_fp8 role=end\n");
        CHECK(FormatEval(1, 0, true, 2, false, false) == "EVAL n=1 feature=0 reset=1 every=2 override=0 observe_only=0\n");
        CHECK(Has(FormatEval(3, 1, false, 1, true, true), "every=1 override=1 observe_only=1"));
        CHECK(FormatEnd(1, 158, 158, 0, true) == "END eval=1 launches=158 recorded=158 skipped=0 success=1\n");
    }

    // ---- module dump rules ----
    CHECK(WhyNotDumped(1417240, 0, false).empty());
    CHECK(WhyNotDumped(kMaxModuleBytes, 0, false).empty() && WhyNotDumped(kMaxModuleBytes + 1, 0, false) == "too_large");
    CHECK(WhyNotDumped(0, 0, false) == "too_large");
    CHECK(WhyNotDumped(100, kMaxModuleDumps, false) == "dump_limit" && WhyNotDumped(100, kMaxModuleDumps - 1, false).empty());
    CHECK(WhyNotDumped(100, 0, true) == "duplicate");

    // ---- run folders ----
    {
        std::tm t {};
        t.tm_year = 126; t.tm_mon = 9; t.tm_mday = 6; t.tm_hour = 3; t.tm_min = 4; t.tm_sec = 5;
        CHECK(FormatRunId(t) == "20261006-030405");
        CHECK(IsRunId("20261006-030405") && !IsRunId("2026100-6030405") && !IsRunId("20261006-03040") &&
              !IsRunId("20261006-03040x") && !IsRunId("modules") && !IsRunId(""));

        std::vector<std::string> names = { "20261001-000001", "notes", "20261003-000001", "20261002-000001", "20261005-000001",
                                           "20261004-000001", "20261006-000001", "20261006-000001", "modules", "20260930-235959" };
        const auto prune = RunsToPrune(names, 5);
        CHECK((prune == std::vector<std::string> { "20260930-235959", "20261001-000001" })); // oldest two of seven distinct runs
        CHECK(RunsToPrune({ "a", "b", "20261001-000001" }, 5).empty()); // foreign names are never listed
        CHECK(RunsToPrune({}, 5).empty());
        CHECK(RunsToPrune({ "20261001-000001", "20261002-000001" }, 1) == std::vector<std::string> { "20261001-000001" });
    }

    // ---- the recorder: lifecycle and budgets ----
    {
        Recorder r;
        CHECK(!r.Wanted() && !r.Open() && !r.Capturing());

        // nothing is recorded unless asked for
        CHECK(!r.BeginEvaluation(&r, false, 2, false));
        r.NoteFunction(1, 2, "cc_vit_1d_repack_2d_to_1d_fp8");
        CHECK(!r.HasPending());

        r.SetWanted(true);
        CHECK(r.Open());
        r.NoteFunction(0x11, 0x99, "cc_vit_1d_repack_2d_to_1d_fp8");
        r.NoteFunction(0x12, 0x99, "cc_vit_1d_ffn_expand_fp8");
        r.NoteFunction(0x13, 0x99, "cc_vit_1d_repack_1d_to_2d_fp8");
        r.NoteFunction(0x14, 0x98, "cc_cb_clear"); // not a ViT kernel: remembered by name, no FUNC record
        CHECK(Count(r.TakePending(), "FUNC ") == 3);

        // a launch struct of each ABI
        unsigned char twenty4[24] = { 1, 2, 3 };
        BasicLaunch basic { reinterpret_cast<void*>(0x11), { 2176, 1, 1 }, { 256, 1, 1 }, 0, twenty4, 24 };
        ExLaunch ex { reinterpret_cast<void*>(0x12), { 544, 1, 1 }, { 32, 4, 1 }, 0, nullptr, 0,
                      reinterpret_cast<void**>(static_cast<uintptr_t>(0x10)) }; // bogus address: must never be dereferenced
        ExLaunch exBlob { reinterpret_cast<void*>(0x13), { 2160, 1, 1 }, { 256, 1, 1 }, 0, twenty4, 24, nullptr };
        BasicLaunch foreign { reinterpret_cast<void*>(0x14), { 1, 1, 1 }, { 1, 1, 1 }, 0, nullptr, 0 };

        r.Observe(basic); // outside an evaluation: ignored
        CHECK(!r.HasPending());

        for (unsigned e = 1; e <= kEvaluations; ++e)
        {
            CHECK(r.BeginEvaluation(&r, e == 1, 2, false));
            CHECK(r.Capturing());
            r.Observe(foreign);
            r.Observe(basic);
            r.Observe(ex);
            r.Observe(exBlob);
            CHECK(r.EndEvaluation(true));
            CHECK(!r.Capturing());
            const std::string text = r.TakePending();
            CHECK(Count(text, "EVAL n=" + std::to_string(e) + " ") == 1);
            CHECK(Count(text, "LAUNCH eval=" + std::to_string(e) + " ") == 4);
            CHECK(Has(text, "ord=0 fn=0x14 kernel=cc_cb_clear vit=0 role=none abi=basic"));
            CHECK(Has(text, "ord=1 fn=0x11 kernel=cc_vit_1d_repack_2d_to_1d_fp8 vit=1 role=start abi=basic"));
            CHECK(Has(text, "ord=2 fn=0x12 kernel=cc_vit_1d_ffn_expand_fp8 vit=1 role=inner abi=ex"));
            CHECK(Has(text, "pmode=ptrs pbytes=0 trunc=0 kp=0x10 params=-"));
            CHECK(Has(text, "ord=3 fn=0x13 kernel=cc_vit_1d_repack_1d_to_2d_fp8 vit=1 role=end abi=ex"));
            CHECK(Has(text, "END eval=" + std::to_string(e) + " launches=4 recorded=4 skipped=0 success=1\n"));
            CHECK(Has(text, "EVAL n=" + std::to_string(e) + " feature=0 reset=" + (e == 1 ? "1" : "0")));
            CHECK(Has(text, "params=010203") && !Has(text, "params=010203040506"));
            CHECK(Count(text, "DONE") == (e == kEvaluations ? 1u : 0u));
        }

        // the budget is spent: nothing more is recorded, the recorder stops asking for work
        CHECK(r.Done() && !r.Open());
        CHECK(!r.BeginEvaluation(&r, false, 2, false));
        r.Observe(basic);
        r.NoteFunction(0x15, 0x99, "cc_vit_1d_late");
        CHECK(!r.HasPending() && !r.EndEvaluation(true));
        CHECK(r.Evaluations() == kEvaluations);
    }

    // second feature (a later NR pass) gets its own index; reset and every are recorded as applied
    {
        Recorder r;
        r.SetWanted(true);
        int a = 0, b = 0;
        r.BeginEvaluation(&a, false, 1, true);
        r.EndEvaluation(true);
        r.BeginEvaluation(&b, true, 2, false);
        r.EndEvaluation(true);
        r.BeginEvaluation(&a, false, 2, false);
        r.EndEvaluation(false);
        const std::string t = r.TakePending();
        CHECK(Has(t, "EVAL n=1 feature=0 reset=0 every=1 override=1 observe_only=1\n"));
        CHECK(Has(t, "EVAL n=2 feature=1 reset=1 every=2 override=0 observe_only=0\n"));
        CHECK(Has(t, "EVAL n=3 feature=0 reset=0 every=2"));
        CHECK(Has(t, "END eval=3 launches=0 recorded=0 skipped=0 success=0\n"));
    }

    // a new evaluation beginning inside an unfinished one closes it as failed
    {
        Recorder r;
        r.SetWanted(true);
        r.BeginEvaluation(&r, false, 2, false);
        CHECK(r.BeginEvaluation(&r, false, 2, false));
        CHECK(r.Evaluations() == 2);
        const std::string t = r.TakePending();
        CHECK(Has(t, "END eval=1 launches=0 recorded=0 skipped=0 success=0\n") && Count(t, "EVAL ") == 2);
    }

    // per-evaluation launch cap
    {
        Recorder r;
        r.SetWanted(true);
        BasicLaunch k { reinterpret_cast<void*>(0x77), { 1, 1, 1 }, { 1, 1, 1 }, 0, nullptr, 0 };
        r.BeginEvaluation(&r, false, 2, false);
        for (unsigned i = 0; i < kMaxLaunchesPerEvaluation + 40; ++i)
            r.Observe(k);
        r.EndEvaluation(true);
        const std::string t = r.TakePending();
        CHECK(Count(t, "LAUNCH ") == kMaxLaunchesPerEvaluation);
        CHECK(Has(t, "END eval=1 launches=552 recorded=512 skipped=40 success=1\n"));
    }

    // failure stops the capture and says so; the recorder never throws
    {
        Recorder r;
        r.SetWanted(true);
        r.BeginEvaluation(&r, false, 2, false);
        r.Fail("write_failed");
        CHECK(!r.Open() && !r.Capturing() && r.Failed());
        BasicLaunch k { reinterpret_cast<void*>(0x77), { 1, 1, 1 }, { 1, 1, 1 }, 0, nullptr, 0 };
        r.Observe(k);
        CHECK(!r.BeginEvaluation(&r, false, 2, false));
        const std::string t = r.TakePending();
        CHECK(Has(t, "NOTE text=capture_stopped_write_failed\n") && Count(t, "LAUNCH ") == 0);
        CHECK(!r.EndEvaluation(true));
    }

    // module bookkeeping: dump counting, duplicates, and the single kept blob
    {
        Recorder r;
        r.SetWanted(true);
        ModuleNote m;
        m.handle = 5; m.bytes = 10; m.sha256 = std::string(64, 'b'); m.status = "dumped"; m.dump = "modules/x.bin";
        CHECK(!r.ModuleNoted(5) && !r.ShaDumped(m.sha256) && r.ModuleDumps() == 0);
        r.NoteModule(m);
        CHECK(r.ModuleNoted(5) && r.ShaDumped(m.sha256) && r.ModuleDumps() == 1);
        ModuleNote d = m;
        d.handle = 6; d.status = "duplicate";
        r.NoteModule(d);
        CHECK(r.ModuleNoted(6) && r.ModuleDumps() == 1);
        CHECK(Count(r.TakePending(), "MODULE ") == 2);

        const unsigned char blob[4] = { 1, 2, 3, 4 };
        r.RememberModule(7, blob, 4);
        CHECK(r.RememberedModule(7) != nullptr && r.RememberedModule(7)->size() == 4 && r.RememberedModule(8) == nullptr);
        r.RememberModule(9, blob, 2); // a newer module replaces the single slot
        CHECK(r.RememberedModule(7) == nullptr && r.RememberedModule(9)->size() == 2);
        r.RememberModule(10, blob, kMaxModuleBytes + 1); // too large: not kept, and the old one is gone
        CHECK(r.RememberedModule(9) == nullptr && r.RememberedModule(10) == nullptr);
        r.SetWanted(false);
        r.RememberModule(11, blob, 4);
        CHECK(r.RememberedModule(11) == nullptr);
    }

    // function names: bounded map, forgotten on destroy
    {
        Recorder r;
        r.SetWanted(true);
        r.NoteFunction(0x21, 1, "cc_vit_1d_a");
        r.ForgetFunction(0x21);
        r.NoteFunction(0x21, 1, "cc_other");
        BasicLaunch k { reinterpret_cast<void*>(0x21), { 1, 1, 1 }, { 1, 1, 1 }, 0, nullptr, 0 };
        r.TakePending();
        r.BeginEvaluation(&r, false, 2, false);
        r.Observe(k);
        r.EndEvaluation(true);
        const std::string t = r.TakePending();
        CHECK(Has(t, "kernel=cc_other vit=0") && !Has(t, "kernel=cc_vit_1d_a"));
        for (uint64_t i = 0; i < kMaxFunctionNames + 50; ++i)
            r.NoteFunction(0x1000 + i, 1, "cc_k");
        r.Observe(k); // outside an evaluation, harmless
    }

    printf(fails ? "nr_vit_capture: %d FAILED\n" : "nr_vit_capture: all passed\n", fails);
    return fails != 0;
}
