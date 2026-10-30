// FSR FG frame IDs ([FrameGen] FsrMonotonicFrameId, OptiScaler/ext/fg/FsrFrameId.h and FsrIdSeams.cpp).
//
// Part one, the rule itself: FSR frame generation resets its interpolation history when the frame ID it is given does not
// rise by exactly 1, and the shared dispatch selector can hand out an ID that jumps (a frame went out without a dispatch)
// or falls back (a quality change). FsrFrameIds keeps FSR's ID in its own space, one step per dispatch, and remembers which
// buffer slot each dispatch used so the provider's callbacks can still index the right frame.
//
// Part two, the wiring: the provider's two frame ID writes and its two callbacks go through the seam, the key exists with
// the default that keeps today's behaviour, and the other providers do not touch it.
#include "ext/fg/FsrFrameId.h"

#include <cstdio>
#include <fstream>
#include <iterator>
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

using ext::fg::FsrFrameIds;

namespace
{
void IdsRiseByOnePerDispatch()
{
    // The game's counter jumps forward past the allowed frame-ahead (8 to 10), falls back after a quality change (10 to 3)
    // and falls back again later (4 to 1): the ID FSR gets must rise by exactly 1 every time, because any other difference
    // resets its history.
    FsrFrameIds ids;
    const unsigned long long frames[] = { 7, 8, 10, 3, 4, 1 };
    const int slots[] = { 3, 0, 2, 3, 2, 0 };
    unsigned long long expected = 0;
    for (int i = 0; i < 6; ++i)
    {
        const unsigned long long id = ids.Next(frames[i], slots[i]);
        ++expected;
        CHECK(id == expected);
    }
    // a backwards game counter is the case that used to send the ID backwards, the worst one for FSR
    CHECK(ids.Next(2, 2) == 7ull);
}

void OneDispatchAskedTwice()
{
    // The provider writes the ID once into the configure descriptor and once into the prepare descriptor of the same
    // frame; both must carry the same ID, or FSR sees a repeat and drops history. A later dispatch can never repeat the
    // previous game frame and slot pair (the selector answers "same frame" with no dispatch at all), so the pair is enough
    // to tell the two calls of one dispatch apart from two dispatches.
    FsrFrameIds ids;
    const auto first = ids.Next(42, 2);
    const auto again = ids.Next(42, 2);
    CHECK(first == again);
    CHECK(first == 1ull);
    CHECK(ids.Next(43, 2) == 2ull);
    CHECK(ids.Next(9, 1) == 3ull);
    CHECK(ids.Next(9, 1) == 3ull); // the same dispatch again
    CHECK(ids.Next(10, 0) == 4ull);
}

void SlotsFollowTheDispatch()
{
    FsrFrameIds ids;
    // a resync means the slot no longer follows from the ID, so the callbacks have to ask
    const auto a = ids.Next(10, 2);
    const auto b = ids.Next(13, 1);
    const auto c = ids.Next(14, 2);
    CHECK(ids.Slot(a) == 2);
    CHECK(ids.Slot(b) == 1);
    CHECK(ids.Slot(c) == 2);
    // ids nobody recorded (0 because IDs start at 1, or one from another object) answer -1
    CHECK(ids.Slot(0) == -1);
    CHECK(ids.Slot(999) == -1);
    // the four slots of the provider are all recordable
    for (int slot = 0; slot < 4; ++slot)
    {
        const auto id = ids.Next(static_cast<unsigned long long>(100 + slot), slot);
        CHECK(ids.Slot(id) == slot);
    }
}

void OldIdsAreForgotten()
{
    FsrFrameIds ids;
    const auto first = ids.Next(1, 0);
    for (unsigned i = 0; i < FsrFrameIds::kIds; ++i)
        ids.Next(static_cast<unsigned long long>(2 + i), static_cast<int>(i % 4));
    // an ID that has been overwritten is not one of ours any more: the provider falls back to its own slot instead of
    // reading a stale one
    CHECK(ids.Slot(first) == -1);
}

void ResetKeepsTheCounterRising()
{
    FsrFrameIds ids;
    for (int i = 0; i < 4; ++i)
        ids.Next(static_cast<unsigned long long>(i + 1), i % 4);
    ids.Reset();
    // the provider was torn down: the counter continues (a fresh FSR context has no history to protect, and an ID must
    // never repeat one it has seen), and no slot is remembered any more
    CHECK(ids.Next(1, 0) == 5ull);
    CHECK(ids.Slot(4) == -1);
    CHECK(ids.Slot(5) == 0);
}

// ---- wiring pins --------------------------------------------------------------------------------------------

std::string Read(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

size_t Count(const std::string& text, const std::string& what)
{
    size_t n = 0;
    for (auto p = text.find(what); p != std::string::npos; p = text.find(what, p + 1))
        ++n;
    return n;
}

bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

void Wiring(const std::string& repo)
{
    const auto ffx = Read(repo + "/OptiScaler/framegen/ffx/FSRFG_Dx12.cpp");
    const auto dlssg = Read(repo + "/OptiScaler/framegen/dlssg/DLSSG_Dx12.cpp");
    const auto xefg = Read(repo + "/OptiScaler/framegen/xefg/XeFG_Dx12.cpp");
    const auto seams = Read(repo + "/OptiScaler/ext/fg/FsrIdSeams.cpp");
    const auto header = Read(repo + "/OptiScaler/ext/fg/FgSeams.h");
    const auto config = Read(repo + "/OptiScaler/ext/fg/FgConfig.h");
    const auto binder = Read(repo + "/OptiScaler/ext/fg/FgConfig.cpp");
    const auto manifest = Read(repo + "/OptiScaler/ext/core/MenuManifest.h");
    const auto ini = Read(repo + "/OptiScaler/ext/ini/FrameGen.ini");
    CHECK(ffx.size() > 20000 && seams.size() > 1000 && ini.size() > 200); // the files were read

    // the two ID writes and the two callbacks go through the seam; no ID-derived slot is left in the provider
    CHECK(Count(ffx, "ext::fg::FsrFrameId(this, willDispatchFrame, fIndex)") == 2);
    CHECK(Count(ffx, "ext::fg::FsrFrameIndex(this, params->frameID)") == 2);
    CHECK(Count(ffx, "params->frameID % BUFFER_COUNT") == 0);
    CHECK(Count(ffx, "frameID = willDispatchFrame") == 0);
    // the other providers are untouched: their own ID handling must not go through the FSR seam
    CHECK(Count(dlssg, "FsrFrame") == 0);
    CHECK(Count(xefg, "FsrFrame") == 0);

    // the seam asks the key once per call and fails open to the provider's own slot
    CHECK(Has(seams, "ext::Cfg().fg.fsrMonotonicFrameId.value_or_default()"));
    CHECK(Has(seams, "return gameFrame;"));
    CHECK(Has(seams, "return static_cast<int>(id % BUFFER_COUNT);"));
    CHECK(Has(header, "uint64_t FsrFrameId(IFGFeature_Dx12* fg, uint64_t gameFrame, int slot);"));
    CHECK(Has(header, "int FsrFrameIndex(IFGFeature_Dx12* fg, uint64_t id);"));

    // the key: declared false, bound to [FrameGen], ini-only, and its line is in the fragment
    CHECK(Has(config, "CustomOptional<bool> fsrMonotonicFrameId { false };"));
    CHECK(Has(binder, "b.Add(\"FrameGen\", \"FsrMonotonicFrameId\", fsrMonotonicFrameId);"));
    CHECK(Has(manifest, "\"[FrameGen] FsrMonotonicFrameId\""));
    CHECK(Has(ini, "\nFsrMonotonicFrameId=auto"));
}
} // namespace

int main(int argc, char** argv)
{
    IdsRiseByOnePerDispatch();
    OneDispatchAskedTwice();
    SlotsFollowTheDispatch();
    OldIdsAreForgotten();
    ResetKeepsTheCounterRising();
    Wiring(argc > 1 ? argv[1] : ".");
    if (g_failed != 0)
    {
        std::printf("fsr_frame_id: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("fsr_frame_id: all passed\n");
    return 0;
}
