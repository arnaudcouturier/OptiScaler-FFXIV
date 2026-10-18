// The live command channel of the character preview probe: the command executor (CharaProbeCommands.h) on a fake configuration and the
// file side (CharaProbeChannel.h) on real files in a temp folder: the 100 ms age gate, the size limit, deleting / renaming / remembering
// the command file, the result file with its 256 KiB cap.
#include "check.h"
#include "ext/ffxiv/chara/CharaProbeChannel.h"
#include "ext/ffxiv/chara/CharaProbeCommands.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace ext::ffxiv::chara::probe;
namespace fs = std::filesystem;

namespace
{
// Same member names and member behaviour as ext::ffxiv::chara::CharaConfig (a CustomOptional<bool> per key).
struct Flag
{
    bool value = false;
    Flag& operator=(bool v)
    {
        value = v;
        return *this;
    }
    bool value_or_default() const { return value; }
};

struct FakeConfig
{
    Flag previewDlss, previewAA, previewUpscale, previewDebugTint, previewDebugFlood;

    std::string Values() const
    {
        std::string s;
        for (const Flag* f : { &previewDlss, &previewAA, &previewUpscale, &previewDebugTint, &previewDebugFlood })
            s += f->value ? '1' : '0';
        return s; // order: dlss aa upscale tint flood
    }
};

struct FakeActions
{
    std::vector<std::string> calls;
    std::string SetCopies(bool on)
    {
        calls.push_back(on ? "copies on" : "copies off");
        return "ok, copies";
    }
    std::string RequestDump()
    {
        calls.push_back("dump");
        return "ok, dump";
    }
    std::string RequestState()
    {
        calls.push_back("state");
        return "ok, state";
    }
    std::string ResetAll()
    {
        calls.push_back("reset");
        return "ok, reset";
    }
};

std::string Run(const std::string& line, FakeConfig& cfg, FakeActions& act)
{
    const Command c = ParseLine(line);
    return Execute(c, cfg, act);
}

void TestExecutor()
{
    // Each toggle changes exactly its own key (order of Values(): dlss aa upscale tint flood).
    struct Case
    {
        const char* on;
        const char* off;
        size_t index;
        const char* key;
    };
    const Case cases[] = {
        { "tint on", "tint off", 3, "PreviewDebugTint" },       { "flood on", "flood off", 4, "PreviewDebugFlood" },
        { "aa on", "aa off", 1, "PreviewAA" },                   { "dlss on", "dlss off", 0, "PreviewDLSS" },
        { "upscale on", "upscale off", 2, "PreviewUpscale" },
    };
    for (const Case& c : cases)
    {
        FakeConfig cfg;
        FakeActions act;
        std::string expectOn(5, '0');
        expectOn[c.index] = '1';
        const std::string onText = Run(c.on, cfg, act);
        CHECK(cfg.Values() == expectOn);
        CHECK(onText.find(std::string("ok, ") + c.key + " is now on (was off)") == 0);
        CHECK(onText.find("in memory only") != std::string::npos);
        const std::string offText = Run(c.off, cfg, act);
        CHECK(cfg.Values() == "00000");
        CHECK(offText.find(std::string("ok, ") + c.key + " is now off (was on)") == 0);
        CHECK(act.calls.empty()); // a toggle never reaches the actions
    }

    // upper case, 1/0/true/false, extra spaces
    {
        FakeConfig cfg;
        FakeActions act;
        Run("  TINT   True ", cfg, act);
        CHECK(cfg.previewDebugTint.value);
        Run("tint 0", cfg, act);
        CHECK(!cfg.previewDebugTint.value);
    }

    // the plain commands call the actions and touch no key
    {
        FakeConfig cfg;
        FakeActions act;
        CHECK(Run("dump", cfg, act) == "ok, dump");
        CHECK(Run("state", cfg, act) == "ok, state");
        CHECK(Run("reset", cfg, act) == "ok, reset");
        CHECK(Run("copies off", cfg, act) == "ok, copies");
        CHECK(Run("copies on", cfg, act) == "ok, copies");
        CHECK((act.calls == std::vector<std::string> { "dump", "state", "reset", "copies off", "copies on" }));
        CHECK(cfg.Values() == "00000");
        CHECK(Run("help", cfg, act) == kHelpText);
        CHECK(act.calls.size() == 5);
    }

    // PreviewAA is shadowed by PreviewDLSS: the answer says so
    {
        FakeConfig cfg;
        FakeActions act;
        cfg.previewDlss = true;
        CHECK(Run("aa on", cfg, act).find("bypassed while the PreviewDLSS pass runs") != std::string::npos);
        cfg.previewDlss = false;
        CHECK(Run("aa on", cfg, act).find("bypassed") == std::string::npos);
    }

    // unknown lines are ignored with the parser's reason and change nothing
    {
        FakeConfig cfg;
        FakeActions act;
        CHECK(Run("frobnicate", cfg, act) == "ignored, unknown command");
        CHECK(Run("tint", cfg, act) == "ignored, expected on or off");
        CHECK(Run("tint maybe", cfg, act) == "ignored, expected on or off");
        CHECK(Run("dump now", cfg, act) == "ignored, this command takes no argument");
        CHECK(cfg.Values() == "00000" && act.calls.empty());
    }

    // a whole file
    {
        FakeConfig cfg;
        FakeActions act;
        const ParsedFile file = ParseFile("# a comment\ntint on\n\nflood on\nbogus\naa on\ndump\n");
        std::vector<std::string> results;
        for (const Command& c : file.commands)
            results.push_back(Execute(c, cfg, act));
        CHECK(results.size() == 5 && results[2] == "ignored, unknown command");
        CHECK(cfg.Values() == "01011" && act.calls.size() == 1);
    }
}

void TestDispose()
{
    int removed = 0, renamed = 0;
    fs::path renamedTo;
    const fs::path p = "x\\chara-probe.cmd";
    CHECK(Dispose(p, [&](const fs::path&) { ++removed; return true; }, [&](const fs::path&, const fs::path&) { ++renamed; return true; }) ==
              Disposition::Removed &&
          removed == 1 && renamed == 0);
    removed = renamed = 0;
    CHECK(Dispose(p, [&](const fs::path&) { ++removed; return false; },
                  [&](const fs::path&, const fs::path& to) { ++renamed; renamedTo = to; return true; }) == Disposition::Renamed &&
          removed == 1 && renamed == 1 && renamedTo.filename() == "chara-probe.cmd.done");
    removed = renamed = 0;
    CHECK(Dispose(p, [&](const fs::path&) { ++removed; return false; }, [&](const fs::path&, const fs::path&) { ++renamed; return false; }) ==
              Disposition::Kept &&
          removed == 1 && renamed == 1);
}

void Write(const fs::path& p, const std::string& text)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string ReadText(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

uint64_t WriteTime(const fs::path& p)
{
    WIN32_FILE_ATTRIBUTE_DATA d {};
    GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d);
    return (static_cast<uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
}

void TestPoll(const fs::path& root)
{
    const fs::path dir = root / "poll";
    fs::create_directories(dir);
    CommandChannel ch(dir);
    const fs::path cmd = ch.CommandPath();
    CHECK(cmd.filename() == "chara-probe.cmd" && ch.OutputPath().filename() == "chara-probe.out");
    constexpr uint64_t kMs = 10'000; // FILETIME units per millisecond

    // no file
    CHECK(ch.Poll(WriteTime(dir) + 10'000'000).state == CommandChannel::Polled::State::None);

    // a file written less than 100 ms ago is left alone, one that is 100 ms old runs and is deleted
    Write(cmd, "tint on\r\nflood on\r\n");
    const uint64_t t = WriteTime(cmd);
    CHECK(ch.Poll(t + 50 * kMs).state == CommandChannel::Polled::State::NotReady && fs::exists(cmd));
    CHECK(ch.Poll(t + 99 * kMs).state == CommandChannel::Polled::State::NotReady && fs::exists(cmd));
    auto p = ch.Poll(t + 100 * kMs);
    CHECK(p.state == CommandChannel::Polled::State::Commands && p.disposition == Disposition::Removed && !fs::exists(cmd));
    CHECK(p.parsed.commands.size() == 2 && p.parsed.commands[0].kind == CommandKind::Tint && p.parsed.commands[1].kind == CommandKind::Flood);
    CHECK(p.fileBytes == 19);
    CHECK(ch.Poll(t + 5000 * kMs).state == CommandChannel::Polled::State::None); // gone

    // a last-write time in the future (clock skew) does not block for ever, but a write that is in progress right now is not read
    Write(cmd, "dump\n");
    const uint64_t t2 = WriteTime(cmd);
    CHECK(ch.Poll(t2 - 1 * kMs).state == CommandChannel::Polled::State::NotReady);           // written "after" now: in progress
    CHECK(ch.Poll(t2 - 5000 * kMs).state == CommandChannel::Polled::State::Commands);         // 5 s ahead of the clock: read
    CHECK(!fs::exists(cmd));

    // an empty file runs no commands and is removed
    Write(cmd, "");
    p = ch.Poll(WriteTime(cmd) + 200 * kMs);
    CHECK(p.state == CommandChannel::Polled::State::Commands && p.parsed.commands.empty() && !fs::exists(cmd));

    // oversize: only the first 4096 bytes are read; a partial last line never runs
    {
        std::string text;
        while (text.size() < 5000)
            text += "tint on\n";
        text += "dump\n";
        Write(cmd, text);
        p = ch.Poll(WriteTime(cmd) + 200 * kMs);
        CHECK(p.state == CommandChannel::Polled::State::Commands && p.parsed.truncatedBytes && p.fileBytes == text.size());
        CHECK(p.parsed.commands.size() == 4096 / 8 || p.parsed.truncatedLines); // 32 lines at most
        CHECK(p.parsed.commands.size() <= kMaxCommandLines);
        for (const Command& c : p.parsed.commands)
            CHECK(c.kind == CommandKind::Tint); // the dump line at the end was never reached
        CHECK(!fs::exists(cmd));
    }

    // binary garbage: no crash, every non-empty line is Unknown
    {
        std::string junk;
        for (int i = 0; i < 300; ++i)
            junk.push_back(static_cast<char>((i * 37 + 11) & 0xFF));
        junk[10] = '\0';
        Write(cmd, junk);
        p = ch.Poll(WriteTime(cmd) + 200 * kMs);
        CHECK(p.state == CommandChannel::Polled::State::Commands);
        for (const Command& c : p.parsed.commands)
            CHECK(c.kind == CommandKind::Unknown || c.kind == CommandKind::Empty);
        CHECK(!fs::exists(cmd));
    }

    // a directory with the name of the command file is not a command file
    fs::create_directory(cmd);
    CHECK(ch.Poll(WriteTime(cmd) + 200 * kMs).state == CommandChannel::Polled::State::None);
    fs::remove(cmd);

    // a file an editor holds open without sharing cannot be read now: looked at again later, not disposed of
    {
        Write(cmd, "tint on\n");
        const uint64_t w = WriteTime(cmd);
        HANDLE h = CreateFileW(cmd.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        CHECK(ch.Poll(w + 200 * kMs).state == CommandChannel::Polled::State::Unreadable && fs::exists(cmd));
        CloseHandle(h);
        CHECK(ch.Poll(w + 200 * kMs).state == CommandChannel::Polled::State::Commands && !fs::exists(cmd));
    }

    // a file that can be read but neither deleted nor renamed runs once; it is remembered by (last write, size) until it changes
    {
        Write(cmd, "tint on\n");
        const uint64_t w = WriteTime(cmd);
        HANDLE h = CreateFileW(cmd.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        p = ch.Poll(w + 200 * kMs);
        CHECK(p.state == CommandChannel::Polled::State::Commands && p.disposition == Disposition::Kept && fs::exists(cmd));
        CHECK(p.parsed.commands.size() == 1);
        CHECK(ch.Poll(w + 400 * kMs).state == CommandChannel::Polled::State::None); // not run again
        CHECK(ch.Poll(w + 4000 * kMs).state == CommandChannel::Polled::State::None);
        CloseHandle(h);
        CHECK(ch.Poll(w + 6000 * kMs).state == CommandChannel::Polled::State::None); // still the same file
        Write(cmd, "tint off\nflood on\n");                                         // changed: size and time differ
        const uint64_t w2 = WriteTime(cmd);
        p = ch.Poll(w2 + 200 * kMs);
        CHECK(p.state == CommandChannel::Polled::State::Commands && p.parsed.commands.size() == 2 && p.disposition == Disposition::Removed);
        CHECK(!fs::exists(cmd));
    }
}

void TestOutput(const fs::path& root)
{
    const fs::path dir = root / "out";
    {
        CommandChannel ch(dir); // the folder does not exist yet
        CHECK(ch.AppendOutput({ "2026-10-17 12:00:00.000 tint on: ok", "second\tline\x01 with control" }));
        CHECK(ch.AppendOutput({ "third" }));
        CHECK(ReadText(ch.OutputPath()) == "2026-10-17 12:00:00.000 tint on: ok\nsecond line  with control\nthird\n");
    }
    {
        // a new session appends to a small file
        CommandChannel ch(dir);
        const std::string before = ReadText(ch.OutputPath());
        CHECK(ch.AppendOutput({ "fourth" }));
        CHECK(ReadText(ch.OutputPath()) == before + "fourth\n");
    }

    // the cap: lines are accepted until the file would go past 256 KiB minus the room for the last line; then one line says so and the rest is dropped
    {
        const fs::path dir2 = root / "cap";
        CommandChannel ch(dir2);
        const std::string line(99, 'x');
        size_t accepted = 0;
        for (int i = 0; i < 5000; ++i)
        {
            if (!ch.AppendOutput({ line }))
                break;
            ++accepted;
        }
        const std::string text = ReadText(ch.OutputPath());
        CHECK(accepted >= 2500 && accepted <= 2700); // about 256 KiB of 100 byte lines
        CHECK(text.size() <= kOutputCap);
        CHECK(text.find("output limit of 256 KiB reached") != std::string::npos);
        CHECK(text.rfind("(output limit") == text.size() - std::string("(output limit of 256 KiB reached; further lines are dropped until the game restarts)\n").size());
        const size_t before = text.size();
        CHECK(!ch.AppendOutput({ "more" }) && !ch.AppendOutput({ "more" }));
        CHECK(ReadText(ch.OutputPath()).size() == before);

        // the next session finds the file full: it starts over instead of being blocked for ever
        CommandChannel next(dir2);
        CHECK(next.AppendOutput({ "fresh start" }));
        const std::string fresh = ReadText(next.OutputPath());
        CHECK(fresh.size() < 300 && fresh.find("were discarded") != std::string::npos && fresh.find("fresh start\n") != std::string::npos);
        CHECK(fresh.find("xxxx") == std::string::npos);
        CHECK(next.AppendOutput({ "second line" }));
        CHECK(ReadText(next.OutputPath()).find("fresh start\nsecond line\n") != std::string::npos);
    }

    // a file over the cap from an older version/session is also restarted
    {
        const fs::path dir3 = root / "big";
        fs::create_directories(dir3);
        Write(dir3 / "chara-probe.out", std::string(kOutputCap + 1000, 'y'));
        CommandChannel ch(dir3);
        CHECK(ch.AppendOutput({ "hello" }));
        CHECK(ReadText(ch.OutputPath()).size() < 300);
    }
}
} // namespace

int main()
{
    const fs::path root = fs::temp_directory_path() / "chara_probe_channel_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);

    TestExecutor();
    TestDispose();
    TestPoll(root);
    TestOutput(root);

    fs::remove_all(root, ec);
    return TEST_RESULT("chara_probe_channel");
}
