#include "check.h"
#include "ext/ffxiv/chara/CharaCameraRules.h"

using namespace ext::ffxiv::chara::camera;

namespace
{
void TestAccept()
{
    CHECK(kMinBytes == 944 && kVectors == 59 && kSlot == 2 && kBoundSlot == 1);

    CHECK(Accept(0x3b0, 0, 59));      // the plain 59-vector buffer
    CHECK(Accept(0x3b0, 0, 100));     // a larger range is fine
    CHECK(!Accept(0x3af, 0, 59));     // one byte too small
    CHECK(!Accept(0x3b0, 0, 58));     // fewer than 59 vectors
    CHECK(!Accept(0x3b0, 1, 59));     // the range would run past the end (1 + 59 vectors > 944 bytes)
    CHECK(Accept(0x3b0 + 16, 1, 59)); // ... but fits in a buffer one vector larger
    CHECK(Accept(kMaxBytes, 0, 59));  // the upper bound is inclusive
    CHECK(!Accept(kMaxBytes + 1, 0, 59));
    CHECK(!Accept(0x3b0, 0xFFFFFFFFu, 59)); // no overflow on a hostile first constant
    CHECK(!Accept(0, 0, 0));
}

void TestReuse()
{
    CHECK(CanReuseCopy(0x3b0, 0x3b0));
    CHECK(!CanReuseCopy(0x3b0, 0x400));
    CHECK(!CanReuseCopy(0, 0));
}

// The stage the camera block is asked from, chosen by the draw that starts the pass. One stage only: a stale binding at the other stage
// would name the pass after another window.
void TestStage()
{
    CHECK(StageFor(true) == Stage::Vertex); // a depth prepass carries the block in vertex-shader slot 0
    CHECK(StageFor(false) == Stage::Pixel); // a G-buffer draw in pixel-shader slot 2
    CHECK(SlotOf(Stage::Vertex) == kVertexSlot && kVertexSlot == 0);
    CHECK(SlotOf(Stage::Pixel) == kSlot && kSlot == 2);
}

// Which window a pass draws, as far as the binding says: the constant buffer object, its first constant and the pass size. The buffer's byte
// width is deliberately not part of it, so the same window keeps one identity whether its block is bound as the exact 944-byte buffer or as a
// view into a larger one.
void TestBlockKey()
{
    const BlockKey plain { 0x1000, 0, 576, 960 };
    CHECK(plain.Valid());
    CHECK(plain == (BlockKey { 0x1000, 0, 576, 960 }));
    CHECK(!(plain == BlockKey { 0x2000, 0, 576, 960 })); // another buffer object: another window
    CHECK(!(plain == BlockKey { 0x1000, 4, 576, 960 })); // another first constant: a view into a shared per-frame buffer
    CHECK(!(plain == BlockKey { 0x1000, 0, 640, 960 })); // another pass width
    CHECK(!(plain == BlockKey { 0x1000, 0, 576, 640 })); // another pass height
    CHECK(!BlockKey {}.Valid());                         // nothing that can hold the camera block is bound
    CHECK(!(BlockKey {} == plain));
}
} // namespace

int main()
{
    TestAccept();
    TestReuse();
    TestStage();
    TestBlockKey();
    return TEST_RESULT("chara_camera_rules");
}
