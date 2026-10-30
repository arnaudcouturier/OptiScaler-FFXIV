// M5 + L6: provider teardown fires once per real teardown, and one rebuild is
// one record. The deactivating tap used to run before the provider's active
// check with its label from global state, so every no-op Deactivate (about 30
// call sites) and the preset-switch path wrote a deactivating line, possibly
// naming the wrong provider. The fix emits only when the feature IsActive()
// and labels it by dynamic_cast, the same cast the activation tap uses. The NR
// rebuild tap used to write one record for the park and one for the build of
// the same rebuild; the fix keeps one per frame. A label from global state, a
// missing active check or a doubled rebuild fails here.
#include <cstdio>
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

static std::string ReadSource(const std::string& repo, const char* rel)
{
    const std::string path = repo + rel;
    std::string text;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || f == nullptr)
    {
        std::printf("FAIL: cannot read %s\n", path.c_str());
        ++g_failed;
        return text;
    }
    char buf[131072] = {};
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) != 0)
        text.append(buf, n);
    fclose(f);
    return text;
}

static std::string BodyOf(const std::string& text, const char* func)
{
    const size_t at = text.find(func);
    if (at == std::string::npos)
        return {};
    // The OnDeactivating body is short: the next function starts the next
    // namespace-level block. 1500 characters cover it without reaching past.
    return text.substr(at, 1500);
}

static void DeactivatingTapPins(const std::string& repo)
{
    const std::string guides = ReadSource(repo, "/OptiScaler/ext/fg/GuideSeams.cpp");
    const std::string body = BodyOf(guides, "void OnDeactivating(");
    CHECK(!body.empty());
    // Once per real teardown: only when the feature being torn down is active.
    CHECK(body.find("IsActive()") != std::string::npos);
    // Labeled by the provider being torn down (the activation cast), never by
    // global state that may already name the other provider.
    CHECK(body.find("dynamic_cast") != std::string::npos);
    CHECK(body.find("activeFgOutput") == std::string::npos);
}

static void ModuleConversionPin(const std::string& repo)
{
    // Non-ASCII module names convert with WideCharToMultiByte (CP_UTF8), never
    // with %S (which fails in the C locale and yields an empty label).
    const std::string seams = ReadSource(repo, "/OptiScaler/ext/fgrec/FgRecSeams.cpp");
    const std::string body = BodyOf(seams, "void OnModule(");
    CHECK(!body.empty());
    CHECK(body.find("WideCharToMultiByte") != std::string::npos);
    CHECK(body.find("%S") == std::string::npos);
}

static void RebuildDedupePin(const std::string& repo)
{
    // Park and build of one rebuild share the frame: the second same-frame
    // rebuild is dropped.
    const std::string seams = ReadSource(repo, "/OptiScaler/ext/fgrec/FgRecSeams.cpp");
    const std::string body = BodyOf(seams, "void OnNrRebuild()");
    CHECK(!body.empty());
    CHECK(body.find("lastRebuildFrame") != std::string::npos);
}

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    DeactivatingTapPins(repo);
    ModuleConversionPin(repo);
    RebuildDedupePin(repo);
    if (g_failed != 0)
    {
        std::printf("fgrec_teardown: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("fgrec_teardown: all passed\n");
    return 0;
}
