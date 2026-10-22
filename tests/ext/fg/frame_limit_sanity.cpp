// FramerateLimit sanity (ext/fg/FrameLimitSanity.h): a cap that is NaN, infinite or not above zero means "no cap" (0);
// every positive finite cap is kept exactly. FrameInterval.cpp and FgConfig::PostLoad (the config test pins the
// ini-load behaviour with the real FgConfig.cpp) share this one predicate.
#include "ext/fg/FrameLimitSanity.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
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

std::string ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
} // namespace

int main(int argc, char** argv)
{
    using ext::present::IsUsableFrameLimit;
    using ext::present::SaneFrameLimit;

    constexpr float kInf = std::numeric_limits<float>::infinity();
    constexpr float kNan = std::numeric_limits<float>::quiet_NaN();

    // Bad values: no cap.
    for (float bad : { kNan, -kNan, kInf, -kInf, 0.0f, -0.0f, -0.001f, -1.0f, -5.0f, -144.0f,
                       -std::numeric_limits<float>::max(), -std::numeric_limits<float>::denorm_min() })
    {
        CHECK(!IsUsableFrameLimit(bad));
        CHECK(SaneFrameLimit(bad) == 0.0f);
        CHECK(!std::signbit(SaneFrameLimit(bad)));
    }

    // Good values pass unchanged.
    for (float good : { 1.0f, 30.0f, 59.94f, 60.5f, 144.0f, 240.0f, 1000.0f, 0.5f, 100000.0f })
    {
        CHECK(IsUsableFrameLimit(good));
        CHECK(SaneFrameLimit(good) == good);
    }

    // FrameInterval.cpp (the limiter's interval) uses the same predicate, so the two cannot drift apart.
    if (argc > 1)
    {
        const std::string repo = argv[1];
        const std::string interval = ReadFile(repo + "/OptiScaler/ext/fg/FrameInterval.cpp");
        CHECK(!interval.empty());
        CHECK(interval.find("#include \"ext/fg/FrameLimitSanity.h\"") != std::string::npos);
        CHECK(interval.find("if (!IsUsableFrameLimit(fpsCap))") != std::string::npos);

        const std::string cfg = ReadFile(repo + "/OptiScaler/ext/fg/FgConfig.cpp");
        CHECK(cfg.find("#include \"ext/fg/FrameLimitSanity.h\"") != std::string::npos);
        CHECK(cfg.find("limit.has_value() && !present::IsUsableFrameLimit(limit.value())") != std::string::npos);
    }
    else
    {
        CHECK(!"repo path argument missing");
    }

    if (failures == 0)
        std::printf("frame_limit_sanity: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
