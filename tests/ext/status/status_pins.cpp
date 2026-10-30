// Source pins for the status file writer. The behaviour tests cannot see a
// moved throttle constant, a dropped separator or a skipped ownership check,
// so this test reads the sites (comments stripped, whitespace collapsed) and
// pins the exact statements. A pin that stops matching means a site moved:
// re-derive it, do not loosen it. Source text only, no git inside a test.
#include <cctype>
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

static std::string Read(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Comments out, string literals kept, every run of whitespace one space.
static std::string Normalize(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '\'')
        {
            out += s[i++]; // a character literal never opens a string: copy it whole
            while (i < s.size() && s[i] != '\'')
            {
                if (s[i] == '\\' && i + 1 < s.size())
                    out += s[i++];
                out += s[i++];
            }
            if (i < s.size())
                out += s[i++];
        }
        else if (s[i] == '"')
        {
            out += s[i++];
            while (i < s.size() && s[i] != '"')
            {
                if (s[i] == '\\' && i + 1 < s.size())
                    out += s[i++];
                out += s[i++];
            }
            out += '"';
        }
        else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/')
        {
            while (i < s.size() && s[i] != '\n')
                ++i;
            if (out.empty() || out.back() != ' ')
                out += ' ';
        }
        else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*')
        {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/'))
                ++i;
            ++i;
            if (out.empty() || out.back() != ' ')
                out += ' ';
        }
        else if (std::isspace(static_cast<unsigned char>(s[i])))
        {
            if (out.empty() || out.back() != ' ')
                out += ' ';
        }
        else
            out += s[i];
    }
    return out;
}

static bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

static size_t Count(const std::string& text, const std::string& what)
{
    size_t n = 0;
    for (auto p = text.find(what); p != std::string::npos; p = text.find(what, p + 1))
        ++n;
    return n;
}

// Text from the first occurrence of `from` to the end ("" when missing).
static std::string Tail(const std::string& text, const std::string& from)
{
    const auto a = text.find(from);
    return a == std::string::npos ? std::string {} : text.substr(a);
}

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    const std::string rawFile = Read(repo + "/OptiScaler/ext/status/StatusFile.cpp");
    const std::string rawCollect = Read(repo + "/OptiScaler/ext/status/StatusCollect.cpp");
    const std::string rawJson = Read(repo + "/OptiScaler/ext/status/StatusJson.cpp");
    const std::string rawHeader = Read(repo + "/OptiScaler/ext/status/StatusCollect.h");
    const std::string rawDecl = Read(repo + "/OptiScaler/ext/status/StatusFile.h");
    const std::string rawDetach = Read(repo + "/OptiScaler/dllmain.cpp");
    const std::string rawDoc = Read(repo + "/docs/notes/status-json.md");
    const auto file = Normalize(rawFile);
    const auto collect = Normalize(rawCollect);
    const auto json = Normalize(rawJson);
    const auto header = Normalize(rawHeader);
    const auto decl = Normalize(rawDecl);
    const auto detach = Normalize(rawDetach);
    CHECK(rawFile.size() > 20000 && rawCollect.size() > 20000 && rawJson.size() > 8000 &&
          rawHeader.size() > 3000 && rawDecl.size() > 500 && rawDetach.size() > 10000 && rawDoc.size() > 3000);

    // ---- capacities and throttle constants: every timing number lives here --
    CHECK(Has(file, "constexpr size_t kRecentFrames = 180;"));
    CHECK(Has(file, "constexpr size_t kReservoirCap = 1024;"));
    CHECK(Has(file, "constexpr double kStaleMs = 60.0 * 1000.0;"));
    CHECK(Has(file, "kFirstWriteDelay = std::chrono::seconds(2);"));
    CHECK(Has(file, "kHeartbeat = std::chrono::seconds(10);"));
    CHECK(Has(file, "kMinWriteGap = std::chrono::seconds(1);"));
    CHECK(Has(file, "kTick = std::chrono::seconds(1);"));
    CHECK(Has(file, "kLockBackoff = std::chrono::seconds(30);"));
    CHECK(Has(file, "kFirstWriteDelay.count()"));
    CHECK(Has(file, "kTick.count()"));
    CHECK(Has(file, "now - state.lastWrite >= kHeartbeat"));
    CHECK(Has(file, "now - state.lastWrite >= kMinWriteGap"));
    CHECK(Has(collect, "constexpr size_t kDetailMax = 160;"));
    CHECK(Has(collect, "kIoTtl = std::chrono::seconds(30);"));
    CHECK(Has(json, "inline constexpr size_t kRenderMax = 512;"));
    CHECK(Has(header, "inline constexpr size_t kFileMax = 16 * 1024;"));
    CHECK(Has(collect, "static constexpr int kMaxAttempts = 60;"));
    CHECK(Has(collect, "attempts < kMaxAttempts"));

    // ---- ownership: self-lock, takeover age, access-denied, own-file guards -
    CHECK(Has(file, "else if (ownerPid == pid)"));
    CHECK(Has(file, "(now - ownerTime) > 30"));
    CHECK(Has(file, "ownerErr == ERROR_ACCESS_DENIED"));
    CHECK(Has(file, "err = { \"open\", GetLastError() };"));
    CHECK(Has(file, "if (state.seq == 0) return;"));
    CHECK(Has(file, "ownerPid != pid) return;"));
    CHECK(Has(collect, "GetLastError() == ERROR_ACCESS_DENIED")); // the temp sweep agrees
    CHECK(Has(collect, "status.tmp-"));

    // ---- atomic replace: named steps, numeric codes, flush before the move --
    CHECK(Has(file, "return { false, \"create\", GetLastError() };"));
    CHECK(Has(file, "wrote && written == json.size()")); // a short write is a failure, not success
    CHECK(Has(file, "step = \"write\";"));
    CHECK(Has(file, "ERROR_WRITE_FAULT")); // a short write carries a real code
    CHECK(Has(file, "code = wrote ? ERROR_WRITE_FAULT : GetLastError();"));
    CHECK(Has(file, "step = \"flush\";"));
    CHECK(Has(file, "return { false, \"move\", code };"));
    CHECK(Has(file, "status.tmp-"));
    CHECK(file.find("FlushFileBuffers(h)") < file.find("MoveFileExW")); // flushed, then renamed
    CHECK(Has(file, "\"%s %lu\"")); // detail is operation plus number, never message text

    // ---- error episodes: counted once, backed off, cleared by success -------
    CHECK(Has(file, "if (!state.fileErrorActive)"));
    CHECK(Count(file, "state.fileErrorActive = true;") == 1);
    CHECK(Has(file, "state.fileErrorActive = false;"));
    CHECK(Has(file, "state.fileRetryAfter = Clock::now() + kLockBackoff;"));

    // ---- worker start: pin first, no retry after any failure -----------------
    CHECK(Has(file, "if (state.threadRunning || state.threadFailed) return;"));
    CHECK(Has(file, "if (!PinModule())"));
    CHECK(Has(file, "\"status.json: module pin failed, no file will be written\""));
    CHECK(Count(file, "state.threadFailed = true;") == 2); // pin failure and thread failure
    CHECK(Has(file, "THREAD_PRIORITY_BELOW_NORMAL"));
    CHECK(Has(file, "CreateEventW(nullptr, TRUE, FALSE, nullptr)"));

    // ---- shutdown: signal, never wait; the late worker cannot recreate ------
    CHECK(Count(file, "if (state.stopRequested.load(std::memory_order_relaxed)) return;") == 3);
    const std::string shutdown = Tail(file, "void OnShutdown(bool freeLibrary) noexcept");
    CHECK(shutdown.size() > 300); // the region is comment-heavy; the needles below prove its shape
    CHECK(Has(shutdown, "state.stopRequested.store(true, std::memory_order_relaxed);"));
    CHECK(Has(shutdown, "SetEvent(state.stopEvent);"));
    CHECK(Has(shutdown, "CloseHandle(state.thread);"));
    CHECK(Has(shutdown, "DeleteOwnFile(state);"));
    CHECK(!Has(shutdown, "WaitForSingleObject")); // never waits under the loader lock
    CHECK(!Has(shutdown, "WaitStop"));
    CHECK(!Has(shutdown, "sleep_for"));
    CHECK(!Has(shutdown, "_beginthreadex"));
    CHECK(Has(rawFile, "unreachable once the worker runs")); // the pin makes this branch dead
    CHECK(Has(decl, "void OnShutdown(bool freeLibrary) noexcept;"));
    CHECK(Has(decl, "void OnScopeBegin(ext::present::Phase phase) noexcept;"));
    CHECK(Has(detach, "ext::status::OnShutdown(lpReserved == nullptr);")); // the one upstream line

    // ---- present path: ignored phases first, gaps are not frame times -------
    CHECK(Has(file, "if (phase != ext::present::Phase::NativePresent && "
                    "phase != ext::present::Phase::OutputPresent) return;"));
    CHECK(Has(file, "if (!Enabled()) return;"));
    CHECK(Has(file, "if (ms >= 0.0 && ms < kStaleMs)"));
    CHECK(Has(file, "series->Push(ms);"));

    // ---- session id: 16 hex characters ---------------------------------------
    CHECK(Has(file, "\"0123456789abcdef\""));
    CHECK(Has(file, "for (int i = 0; i < 16; ++i)"));

    // ---- loader list: change ticks reuse fallbacks, one settled copy --------
    CHECK(Has(collect, "if (!refreshIo)"));
    CHECK(Has(collect, "if (GetModuleHandleW(L\"nvngx_dlss.dll\") == nullptr)"));
    CHECK(Has(collect, "if (++readyHits < 2)"));
    CHECK(Has(collect, "frozen = true;"));
    CHECK(Count(collect, "readyHits = 0;") == 4); // declaration plus the three resets
    CHECK(Count(collect, "NVNGX_FeatureInfo_Paths") == 1); // the single settled copy
    CHECK(Has(collect, "const ext::core::BypassScope bypass;")); // never a wrapped factory
    CHECK(Count(collect, "ext::Active(ext::F::Nr)") == 2);
    CHECK(Has(collect, "ext::Active(ext::F::Hdr)"));
    CHECK(Has(collect, "if (IsVersionText(unlockVer))"));

    // ---- renderer: every key keeps its separator, truncation is marked ------
    CHECK(Has(json, "\":{\\\"found\\\":\"")); // dropping the colon breaks the parse test
    CHECK(Has(json, "\":{\\\"state\\\":\""));
    CHECK(Count(json, "\\\"truncated\\\":true") == 2); // slim and minimal both mark
    CHECK(Count(json, "out += \"\\\"\\\"\";") == 1);   // slim drops the free-text details
    CHECK(Has(json, "s.size() > 64"));
    CHECK(Has(json, "return digit;"));
    CHECK(Has(header, "bool IsVersionText(const std::string& s);"));
    CHECK(Has(header, "std::string StateKey(const Snapshot& snap);"));
    CHECK(Has(header, "std::string ToJson(const Snapshot& snap);"));
    CHECK(Has(rawHeader, "non-empty, at most 64 characters")); // the exact version gate

    // ---- doc accuracy: game-exit deletion, signal-only detach, privacy scan --
    CHECK(Has(rawDoc, "game exit deletes the file once the worker ran"));
    CHECK(Has(rawDoc, "unreachable once the worker runs"));
    CHECK(Has(rawDoc, "only signals the worker, never waits"));
    CHECK(Has(rawDoc, "C:\\"));

    if (g_failed == 0)
        std::puts("PASS: status file source pins hold (constants, branches, call sites, wording)");
    return g_failed == 0 ? 0 : 1;
}
