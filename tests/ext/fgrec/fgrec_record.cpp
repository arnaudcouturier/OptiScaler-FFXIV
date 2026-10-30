// The recorder's record encoding: every line of a session file is strict JSON
// (parsed here with the strict grammar from tests/ext/status/status_json.cpp,
// so an unescaped quote, a raw control character or a cut number fails the
// parse instead of passing through a lenient reader), fields carry the call
// values, sequences are contiguous, and hostile bytes are escaped or replaced
// (invalid UTF-8 becomes U+FFFD, a long message is cut at 159 characters). A
// broken escaper, a truncated write or a dropped field fails this test.
#include "fgrec_json.h"

#include "ext/fgrec/FgRecorder.cpp"

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace fg = ext::fgrec;
namespace ft = fgrec_test;

static int g_failed = 0;
#define CHECK(cond)                                                                                              \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                                           \
        {                                                                                                        \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                  \
            ++g_failed;                                                                                          \
        }                                                                                                        \
    } while (0)


// True worker exit. Running() gates on the key first, so it reports false as
// soon as the key is off while the worker is still starting: a test that just
// stopped the recorder must watch the flag itself, or it looks before the
// file exists.
static bool WorkerGone()
{
    std::lock_guard<std::mutex> lock(fg::Global().mutex);
    return !fg::Global().running;
}

static std::wstring SessionFile(const std::wstring& dir)
{
    std::wstring found;
    int count = 0;
    WIN32_FIND_DATAW f {};
    HANDLE h = FindFirstFileW((dir + L"\\fg-recordings\\fg-rec-*.jsonl").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE)
        return found;
    do
    {
        ++count;
        found = dir + L"\\fg-recordings\\" + f.cFileName;
    } while (FindNextFileW(h, &f) != 0);
    FindClose(h);
    CHECK(count == 1);
    return found;
}

static const ft::JsonValue* WithKind(const std::vector<ft::JsonValue>& rows, const char* kind)
{
    for (const auto& r : rows)
    {
        const ft::JsonValue* k = ft::Find(r, "kind");
        if (k != nullptr && k->type == ft::JsonValue::Type::Text && k->text == kind)
            return &r;
    }
    return nullptr;
}

static void RecordEncoding()
{
    const std::wstring dir = ft::FreshDir("record");
    fg::SessionInfo info;
    const std::string version = "7.0-test", exe = "ffxiv_dx11.exe", build = "matched",
                      summary = "fg=dlssg automfg=on";
    info.version = version.c_str();
    info.exe = exe.c_str();
    info.build = build.c_str();
    info.summary = summary.c_str();
    info.moduleDir = dir.c_str();
    fg::EnsureStarted(true, info);
    CHECK(fg::Running());

    fg::Emit(fg::kFgPresentEnd, 41, -1, 1234, 0, nullptr);
    fg::Emit(fg::kSleep, 42, 567, 0, 0, "reflex");
    fg::Emit("we\"ird", 43, 0, 0, 0, "say \"hi\" \\ bye");
    fg::Emit(fg::kAutoMfg, 44, 1, 0, 0, "a\x01" "b\nc");
    fg::Emit(fg::kNrRebuild, 45, 0, 0, 0, "caf\xC3\xA9 \xF0\x9F\x8E\xAE");
    fg::Emit(fg::kModule, 46, 0, 0, 0, "bad\xFF" "byte");
    fg::Emit(fg::kXeFactor, 47, 2, 3, 0, "cut\xE2\x82");
    fg::Emit(fg::kHiddenPresent, 48, 0, 9, 0, std::string(200, 'm').c_str());

    fg::EnsureStarted(false, fg::SessionInfo {});
    CHECK(ft::WaitUntil(WorkerGone, 30000));
    CHECK(fg::Error() == 0);

    const std::wstring file = SessionFile(dir);
    CHECK(!file.empty());
    if (file.empty())
        return;
    // The worker's flag goes false before its last write: wait for the terminal
    // record, which it emits after every queued record.
    CHECK(ft::WaitForSessionEnd(file, 30000));
    const std::string bytes = ft::ReadBytes(file);
    CHECK(!bytes.empty());
    CHECK(bytes.find('\r') == std::string::npos); // strict \n endings, no \r anywhere
    const std::vector<std::string> lines = ft::SplitLines(bytes);
    CHECK(lines.size() == 10); // begin + 8 events + end

    std::vector<ft::JsonValue> rows;
    for (const auto& line : lines)
    {
        bool ok = false;
        ft::JsonValue v = ft::ParseLine(line, ok);
        CHECK(ok);
        rows.push_back(std::move(v));
    }
    if (g_failed != 0)
        return;

    const ft::JsonValue* begin = WithKind(rows, "session.begin");
    CHECK(begin != nullptr);
    if (begin != nullptr)
    {
        const ft::JsonValue* schema = ft::Find(*begin, "schema");
        CHECK(schema != nullptr && schema->type == ft::JsonValue::Type::Number && schema->text == "1");
        const ft::JsonValue* freq = ft::Find(*begin, "qpc_frequency");
        CHECK(freq != nullptr && freq->type == ft::JsonValue::Type::Number && freq->text != "0");
        CHECK(ft::IsText(ft::Find(*begin, "version"), "7.0-test"));
        CHECK(ft::IsText(ft::Find(*begin, "exe"), "ffxiv_dx11.exe"));
        CHECK(ft::IsText(ft::Find(*begin, "build"), "matched"));
        CHECK(ft::IsText(ft::Find(*begin, "summary"), "fg=dlssg automfg=on"));
    }
    CHECK(ft::IsText(ft::Find(rows.front(), "kind"), "session.begin"));
    CHECK(ft::IsText(ft::Find(rows.back(), "kind"), "session.end"));

    // every data line carries sequence, clock, thread, frame, generation and provider numbers
    long long expectSeq = 0;
    for (const auto& r : rows)
    {
        const ft::JsonValue* seq = ft::Find(r, "seq");
        if (ft::IsText(ft::Find(r, "kind"), "session.begin") || ft::IsText(ft::Find(r, "kind"), "session.end"))
        {
            CHECK(seq == nullptr);
            continue;
        }
        ++expectSeq;
        CHECK(seq != nullptr && seq->type == ft::JsonValue::Type::Number &&
              seq->text == std::to_string(expectSeq));
        for (const char* key : { "qpc", "tid", "frame", "gen", "prov", "a", "b", "c" })
            CHECK(ft::Find(r, key) != nullptr && ft::Find(r, key)->type == ft::JsonValue::Type::Number);
        CHECK(ft::Find(r, "msg") != nullptr && ft::Find(r, "msg")->type == ft::JsonValue::Type::Text);
    }

    const ft::JsonValue* present = WithKind(rows, "fg.present.end");
    CHECK(present != nullptr);
    if (present != nullptr)
    {
        CHECK(ft::Find(*present, "frame")->text == "41");
        CHECK(ft::Find(*present, "a")->text == "-1");
        CHECK(ft::Find(*present, "b")->text == "1234");
    }
    // hostile bytes round-trip through the escaper
    CHECK(ft::IsText(ft::Find(*WithKind(rows, "we\"ird"), "msg"), "say \"hi\" \\ bye"));
    CHECK(ft::IsText(ft::Find(*WithKind(rows, "automfg"), "msg"), "a\x01" "b\nc"));
    CHECK(ft::IsText(ft::Find(*WithKind(rows, "nr.rebuild"), "msg"), "caf\xC3\xA9 \xF0\x9F\x8E\xAE"));
    CHECK(ft::IsText(ft::Find(*WithKind(rows, "provider.module"), "msg"), "bad\xEF\xBF\xBD" "byte"));
    CHECK(ft::IsText(ft::Find(*WithKind(rows, "xefg.factor"), "msg"), "cut\xEF\xBF\xBD" "\xEF\xBF\xBD"));
    const ft::JsonValue* hidden = WithKind(rows, "hidden.present");
    CHECK(hidden != nullptr && ft::Find(*hidden, "msg")->text.size() == 159); // cut at 159 characters
}

int main()
{
    RecordEncoding();
    if (g_failed != 0)
    {
        std::printf("fgrec_record: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("fgrec_record: all passed\n");
    return 0;
}
