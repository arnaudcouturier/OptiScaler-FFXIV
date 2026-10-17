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
} // namespace

int main()
{
    TestAccept();
    TestReuse();
    return TEST_RESULT("chara_camera_rules");
}
