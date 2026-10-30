// The status file renderer: the real ToJson output always parses, stays within
// the size cap, cleans its strings and gates writes by state only. This test
// builds the real StatusJson.cpp and checks its output with a strict JSON
// parser that lives in this file, so no renderer bug can hide behind a
// lenient reader: unknown words, missing separators or chopped text fail here.
#include "ext/status/StatusCollect.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace status = ext::status;

// The two smaller renders are defined in StatusJson.cpp without a header
// declaration; they are part of the cap guarantee and tested directly.
namespace ext::status
{
std::string RenderSnapshot(const Snapshot& snap, bool slim);
std::string RenderMinimal(const Snapshot& snap);
} // namespace ext::status

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

// ---- strict UTF-8 check (the whole document must be clean on its own) ----

static bool IsValidUtf8(const std::string& s)
{
    for (size_t i = 0; i < s.size();)
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 0;
        if (c < 0x80)
            len = 1;
        else if (c >= 0xC2 && c <= 0xDF)
            len = 2;
        else if (c >= 0xE0 && c <= 0xEF)
            len = 3;
        else if (c >= 0xF0 && c <= 0xF4)
            len = 4;
        else
            return false;
        if (i + len > s.size())
            return false;
        for (size_t k = 1; k < len; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80)
                return false;
        if (len == 3)
        {
            const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 > 0x9F))
                return false;
        }
        if (len == 4)
        {
            const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            if ((c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 > 0x8F))
                return false;
        }
        i += len;
    }
    return true;
}

// ---- strict JSON parser (objects, arrays, strings, strict numbers) ----
// Rejects anything outside the grammar: unknown words, single quotes,
// trailing text, raw control characters. Numbers follow the strict shape,
// so a non-finite value written as a word fails the parse.

struct JsonValue
{
    enum class Type
    {
        Null,
        Bool,
        Number,
        Text,
        Array,
        Object
    };
    Type type = Type::Null;
    bool boolean = false;
    std::string text; // decoded text, or the number literal
    std::vector<JsonValue> items;
    std::vector<std::pair<std::string, JsonValue>> fields;
};

struct JsonParser
{
    const char* p;
    const char* end;
    bool ok = true;

    explicit JsonParser(const std::string& s) : p(s.data()), end(s.data() + s.size()) {}

    void SkipSpace()
    {
        while (p != end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            ++p;
    }

    bool Take(char c)
    {
        if (p != end && *p == c)
        {
            ++p;
            return true;
        }
        ok = false;
        return false;
    }

    void TakeLiteral(const char* word)
    {
        for (const char* w = word; *w != '\0'; ++w)
            if (!Take(*w))
                return;
    }

    static void AppendUtf8(std::string& out, unsigned long cp)
    {
        if (cp < 0x80)
            out += static_cast<char>(cp);
        else if (cp < 0x800)
        {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    unsigned ParseHex4()
    {
        unsigned v = 0;
        for (int i = 0; i < 4; ++i)
        {
            if (p == end)
            {
                ok = false;
                return 0;
            }
            const char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= static_cast<unsigned>(c - 'A' + 10);
            else
                ok = false;
        }
        return v;
    }

    std::string ParseString()
    {
        std::string out;
        if (!Take('"'))
            return out;
        while (ok && p != end && *p != '"')
        {
            const char c = *p++;
            if (c == '\\')
            {
                if (p == end)
                {
                    ok = false;
                    break;
                }
                const char e = *p++;
                switch (e)
                {
                case '"':
                    out += '"';
                    break;
                case '\\':
                    out += '\\';
                    break;
                case '/':
                    out += '/';
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u':
                {
                    const unsigned hi = ParseHex4();
                    if (!ok)
                        break;
                    if (hi >= 0xD800 && hi <= 0xDBFF)
                    {
                        if (p != end && *p == '\\' && p + 1 != end && p[1] == 'u')
                        {
                            p += 2;
                            const unsigned lo = ParseHex4();
                            if (!ok || lo < 0xDC00 || lo > 0xDFFF)
                            {
                                ok = false;
                                break;
                            }
                            AppendUtf8(out, 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00));
                        }
                        else
                            ok = false; // a lone leading surrogate is not a character
                    }
                    else if (hi >= 0xDC00 && hi <= 0xDFFF)
                        ok = false; // a lone trailing surrogate is not a character
                    else
                        AppendUtf8(out, hi);
                    break;
                }
                default:
                    ok = false;
                    break;
                }
            }
            else if (static_cast<unsigned char>(c) < 0x20)
                ok = false; // raw control characters never appear in strings
            else
                out += c;
        }
        if (!Take('"'))
            return out;
        return out;
    }

    std::string ParseNumber()
    {
        const char* start = p;
        if (p != end && *p == '-')
            ++p;
        if (p == end)
        {
            ok = false;
            return {};
        }
        if (*p == '0')
            ++p;
        else if (*p >= '1' && *p <= '9')
            while (p != end && *p >= '0' && *p <= '9')
                ++p;
        else
            ok = false;
        if (ok && p != end && *p == '.')
        {
            ++p;
            if (p == end || *p < '0' || *p > '9')
                ok = false;
            while (p != end && *p >= '0' && *p <= '9')
                ++p;
        }
        if (ok && p != end && (*p == 'e' || *p == 'E'))
        {
            ++p;
            if (p != end && (*p == '+' || *p == '-'))
                ++p;
            if (p == end || *p < '0' || *p > '9')
                ok = false;
            while (p != end && *p >= '0' && *p <= '9')
                ++p;
        }
        if (!ok)
            return {};
        return std::string(start, p);
    }

    JsonValue ParseValue()
    {
        JsonValue v;
        SkipSpace();
        if (!ok || p == end)
        {
            ok = false;
            return v;
        }
        if (*p == '{')
        {
            ++p;
            v.type = JsonValue::Type::Object;
            SkipSpace();
            if (p != end && *p == '}') // peek: a missing brace must not poison the state
            {
                ++p;
                return v;
            }
            while (ok)
            {
                SkipSpace();
                std::string key = ParseString();
                SkipSpace();
                Take(':');
                JsonValue item = ParseValue();
                v.fields.emplace_back(key, std::move(item));
                SkipSpace();
                if (p != end && *p == ',')
                {
                    ++p;
                    continue;
                }
                Take('}');
                break;
            }
            return v;
        }
        if (*p == '[')
        {
            ++p;
            v.type = JsonValue::Type::Array;
            SkipSpace();
            if (p != end && *p == ']') // peek: a missing bracket must not poison the state
            {
                ++p;
                return v;
            }
            while (ok)
            {
                v.items.push_back(ParseValue());
                SkipSpace();
                if (p != end && *p == ',')
                {
                    ++p;
                    continue;
                }
                Take(']');
                break;
            }
            return v;
        }
        if (*p == '"')
        {
            v.type = JsonValue::Type::Text;
            v.text = ParseString();
            return v;
        }
        if (*p == 't')
        {
            TakeLiteral("true");
            v.type = JsonValue::Type::Bool;
            v.boolean = true;
            return v;
        }
        if (*p == 'f')
        {
            TakeLiteral("false");
            v.type = JsonValue::Type::Bool;
            return v;
        }
        if (*p == 'n')
        {
            TakeLiteral("null");
            v.type = JsonValue::Type::Null;
            return v;
        }
        v.type = JsonValue::Type::Number;
        v.text = ParseNumber();
        return v; // anything else (a bare word, a leading plus) fails the number shape
    }
};

static JsonValue ParseJson(const std::string& s, bool& ok)
{
    JsonParser parser(s);
    JsonValue v = parser.ParseValue();
    parser.SkipSpace();
    ok = parser.ok && parser.p == parser.end; // no trailing text after the document
    return v;
}

static const JsonValue* Find(const JsonValue& obj, const std::string& key)
{
    if (obj.type != JsonValue::Type::Object)
        return nullptr;
    for (const auto& field : obj.fields)
        if (field.first == key)
            return &field.second;
    return nullptr;
}

static bool IsText(const JsonValue* v, const std::string& want)
{
    return v != nullptr && v->type == JsonValue::Type::Text && v->text == want;
}

static bool IsBool(const JsonValue* v, bool want)
{
    return v != nullptr && v->type == JsonValue::Type::Bool && v->boolean == want;
}

static bool IsNull(const JsonValue* v) { return v != nullptr && v->type == JsonValue::Type::Null; }

// The parser itself must have teeth: it rejects what the renderer must
// never emit, so a passing parse means something.
static void CheckParserTeeth()
{
    const char* const bad[] = {
        "{\"a\":NaN}",         // non-finite words are not numbers
        "{\"a\":Infinity}",    //
        "{\"a\":undefined}",   //
        "{\"a\":}",            // a missing value
        "{\"a\":1,}",          // a trailing comma
        "{\"a\" 1}",           // a missing separator
        "{'a':1}",             // single quotes are not strings
        "{\"a\":1} trailing",  // text after the document
        "{\"a\":\"raw\x01\"}", // raw control characters in strings
        "{\"a\":01}",          // leading zeros are not numbers
        "{\"a\":+1}",          //
        "{\"a\":\"\\uD800\"}", // lone surrogates are not characters
        "{\"a\":1.}",          //
        "{\"a\":tru}",         //
    };
    for (const char* text : bad)
    {
        bool ok = true;
        ParseJson(text, ok);
        if (ok)
        {
            std::printf("FAIL teeth: parser accepted %s\n", text);
            ++g_failed;
        }
    }
    bool ok = false;
    JsonValue good = ParseJson("{\"a\":[1,-2.5,0.000,true,false,null,\"x\\u00e9\\n\"]}", ok);
    CHECK(ok);
    if (ok)
    {
        const JsonValue* a = Find(good, "a");
        CHECK(a != nullptr && a->type == JsonValue::Type::Array && a->items.size() == 7);
    }
}

// ---- snapshots ----

static status::PerfStats Stats(uint64_t frames, double p50, double p95, double p99, double mean, double max)
{
    status::PerfStats s;
    s.frames = frames;
    s.p50Ms = p50;
    s.p95Ms = p95;
    s.p99Ms = p99;
    s.meanMs = mean;
    s.maxMs = max;
    return s;
}

static status::Feature MakeFeature(const char* name, const char* state, const char* reason, const std::string& detail)
{
    status::Feature f;
    f.name = name;
    f.state = state;
    f.reason = reason;
    f.detail = detail;
    return f;
}

static status::Runtime MakeRuntime(const char* name, bool found, bool loaded, const std::string& version,
                                   const std::string& folder)
{
    status::Runtime r;
    r.name = name;
    r.found = found;
    r.loaded = loaded;
    r.version = version;
    r.folder = folder;
    return r;
}

// A small fixed snapshot whose rendering is pinned byte for byte: any change
// to the shape, the key order or the number format fails this test.
static status::Snapshot SmallSnapshot()
{
    status::Snapshot snap;
    snap.seq = 7;
    snap.writtenUtc = "2026-10-29T21:00:00Z";
    snap.sessionId = "0123456789abcdef";
    snap.pid = 1234;
    snap.startUtc = "2026-10-29T20:00:00Z";
    snap.uptimeSec = 3600;
    snap.forkDescribe = "d";
    snap.forkUpstream = "u";
    snap.exeName = "ffxiv_dx11.exe";
    snap.gameMatched = true;
    snap.peStamp = 1;
    snap.peImageSize = 2;
    snap.buildVer = "b";
    snap.gpuName = "g";
    snap.gpuVramMB = 3;
    snap.runtimes.push_back(MakeRuntime("dlss", true, false, "v", "game folder"));
    snap.features.push_back(MakeFeature("nr", "off", "disabled", ""));
    snap.fgInput = "NoFG";
    snap.fgOutput = "NoFG";
    snap.base.recent = Stats(1, 1.0, 2.0, 3.0, 4.0, 5.0);
    return snap;
}

static void CheckGolden()
{
    const std::string want =
        "{\"format\":\"optiscaler-ffxiv-status\",\"version\":1,\"seq\":7,"
        "\"writtenUtc\":\"2026-10-29T21:00:00Z\","
        "\"session\":{\"id\":\"0123456789abcdef\",\"pid\":1234,\"uptimeSec\":3600,"
        "\"startUtc\":\"2026-10-29T20:00:00Z\"},"
        "\"fork\":{\"describe\":\"d\",\"upstream\":\"u\"},"
        "\"game\":{\"exe\":\"ffxiv_dx11.exe\",\"matched\":true,\"peStamp\":1,\"peImageSize\":2,\"buildVer\":\"b\"},"
        "\"gpu\":{\"name\":\"g\",\"vramMB\":3},"
        "\"runtimes\":{\"dlss\":{\"found\":true,\"loaded\":false,\"version\":\"v\",\"folder\":\"game folder\"}},"
        "\"features\":{\"nr\":{\"state\":\"off\",\"reason\":\"disabled\",\"detail\":\"\"}},"
        "\"frameGeneration\":{\"input\":\"NoFG\",\"output\":\"NoFG\"},\"nr\":{\"enabled\":false,\"active\":false},"
        "\"performance\":{\"base\":{\"recent\":{\"frames\":1,\"p50Ms\":1.000,\"p95Ms\":2.000,\"p99Ms\":3.000,"
        "\"meanMs\":4.000,\"maxMs\":5.000},"
        "\"session\":{\"frames\":0,\"p50Ms\":0.000,\"p95Ms\":0.000,\"p99Ms\":0.000,\"meanMs\":0.000,"
        "\"maxMs\":0.000}},\"output\":null},"
        "\"errors\":{\"count\":0,\"code\":\"\",\"detail\":\"\"}}";
    const std::string got = status::ToJson(SmallSnapshot());
    if (got != want)
    {
        std::printf("FAIL golden: got %zu bytes, want %zu bytes\n", got.size(), want.size());
        std::printf("got:  %s\n", got.c_str());
        ++g_failed;
    }
    bool ok = false;
    ParseJson(got, ok);
    CHECK(ok); // the golden text parses too
    CHECK(IsValidUtf8(got));
}

// A hostile but realistic snapshot: every feature and runtime, hostile text in
// the free fields, unknown versions, non-finite measurements. Realistic means
// no path ever appears: folders are labels, errors are numbers.
static status::Snapshot HostileSnapshot()
{
    status::Snapshot snap;
    snap.seq = 41;
    snap.writtenUtc = "2026-10-29T21:30:00Z";
    snap.sessionId = "abcdef0123456789";
    snap.pid = 9876;
    snap.startUtc = "2026-10-29T20:00:00Z";
    snap.uptimeSec = 5400;
    snap.forkDescribe = "v040-12-gabcdef1";
    snap.forkUpstream = "0123456789abcdef0123456789abcdef01234567";
    snap.exeName = "ffxiv_dx11.exe";
    snap.gameMatched = true;
    snap.peStamp = 0xDEADBEEFu;
    snap.peImageSize = 0x100000u;
    snap.buildVer = "2026.09.1234.5678";
    snap.gpuName = "NVIDIA GeForce RTX 4090";
    snap.gpuVramMB = 24564;
    snap.runtimes.push_back(MakeRuntime("dlss", true, true, "310.2.1", "NGX driver folder"));
    snap.runtimes.push_back(MakeRuntime("dlssg", true, false, "", "OptiScaler folder"));
    snap.runtimes.push_back(MakeRuntime("nr", false, false, "", ""));
    snap.runtimes.push_back(MakeRuntime("streamline", true, true, "2.7.1", "streamline folder"));
    const std::string tricky = "quote\" back\\ new\nline tab\t ctrl\x01 "
                               "CJK\xE6\xBC\xA2\xE5\xAD\x97 emoji\xF0\x9F\x98\x80 end";
    snap.features.push_back(MakeFeature("gameModule", status::kEngaged, "matched", "geometry+quality+hudless"));
    snap.features.push_back(MakeFeature("nativeQuality", status::kEngaged, "live", "preset"));
    snap.features.push_back(MakeFeature("companion", status::kPending, "configured-not-installed", "replacement"));
    snap.features.push_back(MakeFeature("nr", status::kPending, "armed-no-evaluate", ""));
    snap.features.push_back(MakeFeature("frameGeneration", status::kEngaged, "active", "Upscaler-in/DLSSG-out"));
    snap.features.push_back(MakeFeature("hudless", status::kPending, "fallback", tricky));
    snap.features.push_back(MakeFeature("hdr", status::kUnknown, "no-phase", std::string("bad\xFF") + "byte"));
    snap.fgInput = "Upscaler";
    snap.fgOutput = "DLSSG";
    snap.nrEnabled = true;
    snap.nrActive = false;
    snap.base.recent = Stats(180, 8.333, 9.101, 12.5, 8.412, 33.333);
    snap.base.session = Stats(100000, 8.3, 9.0, 12.0, 8.4, 120.0);
    snap.output.recent = Stats(90, 16.666, 16.9, 17.2, 16.7, 20.0);
    snap.output.session = Stats(9000, 16.6, 16.9, 17.1, 16.65, 25.0);
    snap.haveOutput = true;
    return snap;
}

static void CheckClosedLabels(const JsonValue& doc)
{
    static const char* const kLabels[] = { "game folder", "OptiScaler folder", "streamline folder",
                                           "NGX driver folder", "other folder", "" };
    const JsonValue* runtimes = Find(doc, "runtimes");
    CHECK(runtimes != nullptr);
    if (runtimes == nullptr)
        return;
    for (const auto& entry : runtimes->fields)
    {
        const JsonValue* folder = Find(entry.second, "folder");
        bool known = false;
        if (folder != nullptr && folder->type == JsonValue::Type::Text)
            for (const char* label : kLabels)
                if (folder->text == label)
                    known = true;
        if (!known)
        {
            std::printf("FAIL label: runtime %s has folder \"%s\"\n", entry.first.c_str(),
                        folder != nullptr ? folder->text.c_str() : "<missing>");
            ++g_failed;
        }
    }
}

// The produced file must never hold a path, a drive, a user name or a
// character name: the scan fails on any drive-letter or home-folder pattern.
static void CheckPrivacy(const std::string& raw)
{
    CHECK(raw.find(":\\") == std::string::npos);
    CHECK(raw.find("\\Users\\") == std::string::npos);
    CHECK(raw.find("/Users/") == std::string::npos);
}

static void CheckFull()
{
    status::Snapshot snap = HostileSnapshot();
    snap.base.recent.meanMs = std::numeric_limits<double>::quiet_NaN(); // renders as null, never a bare word
    snap.base.recent.maxMs = std::numeric_limits<double>::infinity();
    const std::string raw = status::ToJson(snap);
    CHECK(raw.size() <= status::kFileMax);
    CHECK(raw.find("NaN") == std::string::npos);
    CHECK(raw.find("Infinity") == std::string::npos);
    CHECK(raw.find("Inf") == std::string::npos);
    CHECK(IsValidUtf8(raw));
    CheckPrivacy(raw);
    bool ok = false;
    JsonValue doc = ParseJson(raw, ok);
    CHECK(ok);
    if (!ok)
        return;
    CHECK(IsText(Find(doc, "format"), "optiscaler-ffxiv-status"));
    CHECK(Find(doc, "truncated") == nullptr); // a fitting document carries no marker
    const JsonValue* runtimes = Find(doc, "runtimes");
    CHECK(runtimes != nullptr && runtimes->fields.size() == 4);
    const JsonValue* features = Find(doc, "features");
    CHECK(features != nullptr && features->fields.size() == 7);
    if (features != nullptr)
        for (const auto& entry : features->fields)
        {
            CHECK(Find(entry.second, "state") != nullptr);
            CHECK(Find(entry.second, "reason") != nullptr);
            CHECK(Find(entry.second, "detail") != nullptr);
        }
    CheckClosedLabels(doc);
    const JsonValue* perf = Find(doc, "performance");
    CHECK(perf != nullptr);
    if (perf != nullptr)
    {
        const JsonValue* base = Find(*perf, "base");
        CHECK(base != nullptr);
        if (base != nullptr)
        {
            const JsonValue* recent = Find(*base, "recent");
            CHECK(recent != nullptr);
            if (recent != nullptr)
            {
                CHECK(IsNull(Find(*recent, "meanMs"))); // the NaN above
                CHECK(IsNull(Find(*recent, "maxMs")));  // the infinity above
            }
        }
        CHECK(Find(*perf, "output") != nullptr); // an observed output present renders
    }
    if (features != nullptr)
    {
        const JsonValue* hudless = Find(*features, "hudless");
        CHECK(hudless != nullptr);
        if (hudless != nullptr)
        {
            // Escapes round-trip: quotes, backslashes, newlines, tabs, CJK and emoji survive.
            const std::string want = "quote\" back\\ new\nline tab\t ctrl\x01 "
                                     "CJK\xE6\xBC\xA2\xE5\xAD\x97 emoji\xF0\x9F\x98\x80 end";
            CHECK(IsText(Find(*hudless, "detail"), want));
        }
        const JsonValue* hdr = Find(*features, "hdr");
        CHECK(hdr != nullptr);
        if (hdr != nullptr)
            CHECK(IsText(Find(*hdr, "detail"), "bad\xEF\xBF\xBD" "byte")); // the raw byte became U+FFFD
    }
}

// Over the cap the free-text details are dropped and the document is marked;
// the slim render still parses and still fits.
static void CheckSlimRender()
{
    status::Snapshot snap = HostileSnapshot();
    snap.features[0].detail = std::string(20000, 'x'); // capped per string, details dropped in slim
    const std::string raw = status::RenderSnapshot(snap, true);
    CHECK(raw.size() <= status::kFileMax);
    CHECK(IsValidUtf8(raw));
    CheckPrivacy(raw);
    bool ok = false;
    JsonValue doc = ParseJson(raw, ok);
    CHECK(ok);
    if (!ok)
        return;
    CHECK(IsBool(Find(doc, "truncated"), true));
    const JsonValue* features = Find(doc, "features");
    CHECK(features != nullptr && features->fields.size() == 7);
    if (features != nullptr)
        for (const auto& entry : features->fields)
            CHECK(IsText(Find(entry.second, "detail"), "")); // every free-text detail dropped
    CheckClosedLabels(doc);
}

// Fills every non-detail string field with filler of the given length; the
// details stay at a fixed moderate size so the slim choice is reachable.
static status::Snapshot WideSnapshot(size_t fill)
{
    // The name/state/reason fields are bare pointers: the filler lives in
    // function-static storage so it outlives the returned snapshot.
    static std::string s_fill;
    static std::string s_detail;
    s_fill.assign(fill, 'q');
    s_detail.assign(400, 'd');
    status::Snapshot snap = HostileSnapshot();
    snap.forkDescribe = s_fill;
    snap.forkUpstream = s_fill;
    snap.exeName = s_fill;
    snap.buildVer = s_fill;
    snap.gpuName = s_fill;
    for (auto& r : snap.runtimes)
    {
        r.name = s_fill.c_str();
        r.version = s_fill;
        r.folder = s_fill;
    }
    for (auto& f : snap.features)
    {
        f.name = s_fill.c_str();
        f.state = s_fill.c_str();
        f.reason = s_fill.c_str();
        f.detail = s_detail;
    }
    snap.fgInput = s_fill;
    snap.fgOutput = s_fill;
    snap.errors.code = s_fill;
    snap.errors.detail = s_fill;
    return snap;
}

// ToJson picks the slim render once the full one no longer fits: the picked
// text parses, is marked, drops the details, and fits.
static void CheckSlimSelected()
{
    status::Snapshot snap = HostileSnapshot();
    for (size_t fill = 20; fill <= 400; fill += 20)
    {
        snap = WideSnapshot(fill);
        if (status::RenderSnapshot(snap, false).size() > status::kFileMax)
            break;
    }
    CHECK(status::RenderSnapshot(snap, false).size() > status::kFileMax);
    const std::string slim = status::RenderSnapshot(snap, true);
    CHECK(slim.size() <= status::kFileMax);
    CHECK(status::ToJson(snap) == slim); // the writer picks slim, byte for byte
    bool ok = false;
    JsonValue doc = ParseJson(slim, ok);
    CHECK(ok);
    if (!ok)
        return;
    CHECK(IsBool(Find(doc, "truncated"), true));
    const JsonValue* features = Find(doc, "features");
    CHECK(features != nullptr && features->fields.size() == 7);
    if (features != nullptr)
        for (const auto& entry : features->fields)
            CHECK(IsText(Find(entry.second, "detail"), ""));
}

// ToJson falls back to the minimal document when even the slim render does
// not fit: fixed fields only, marked, parses.
static void CheckMinimalSelected()
{
    const status::Snapshot snap = WideSnapshot(20000);
    CHECK(status::RenderSnapshot(snap, false).size() > status::kFileMax);
    CHECK(status::RenderSnapshot(snap, true).size() > status::kFileMax);
    const std::string raw = status::ToJson(snap);
    CHECK(raw == status::RenderMinimal(snap));
    bool ok = false;
    JsonValue doc = ParseJson(raw, ok);
    CHECK(ok);
    if (ok)
        CHECK(IsBool(Find(doc, "truncated"), true));
}

// The minimal fallback carries fixed fields only, always parses, always fits.
static void CheckMinimal()
{
    status::Snapshot snap = HostileSnapshot();
    const std::string raw = status::RenderMinimal(snap);
    CHECK(raw.size() < 1024);
    CHECK(IsValidUtf8(raw));
    CheckPrivacy(raw);
    bool ok = false;
    JsonValue doc = ParseJson(raw, ok);
    CHECK(ok);
    if (!ok)
        return;
    CHECK(IsText(Find(doc, "format"), "optiscaler-ffxiv-status"));
    CHECK(IsBool(Find(doc, "truncated"), true));
    const JsonValue* seq = Find(doc, "seq");
    CHECK(seq != nullptr && seq->type == JsonValue::Type::Number);
    const JsonValue* session = Find(doc, "session");
    const JsonValue* id = session != nullptr ? Find(*session, "id") : nullptr;
    const JsonValue* pid = session != nullptr ? Find(*session, "pid") : nullptr;
    CHECK(session != nullptr && IsText(id, snap.sessionId));
    CHECK(pid != nullptr);
    CHECK(Find(doc, "errors") != nullptr);
    CHECK(Find(doc, "features") == nullptr); // no free text at all
    CHECK(Find(doc, "runtimes") == nullptr);
}

// Every rendered string is capped: even an unbounded source cannot push a
// field past the per-string backstop, and multibyte text stays valid.
static void CheckStringCap()
{
    status::Snapshot snap = SmallSnapshot();
    snap.features[0].detail = std::string(20000, 'x');
    snap.buildVer = std::string(20000, 'y');
    bool ok = false;
    JsonValue doc = ParseJson(status::RenderSnapshot(snap, false), ok);
    CHECK(ok);
    if (!ok)
        return;
    const JsonValue* features = Find(doc, "features");
    const JsonValue* detail = features != nullptr && !features->fields.empty()
                                  ? Find(features->fields[0].second, "detail")
                                  : nullptr;
    CHECK(detail != nullptr && detail->text.size() == 512);
    const JsonValue* game = Find(doc, "game");
    const JsonValue* buildVer = game != nullptr ? Find(*game, "buildVer") : nullptr;
    CHECK(buildVer != nullptr && buildVer->text.size() == 512);

    status::Snapshot wide = SmallSnapshot();
    std::string eAccent;
    for (int i = 0; i < 10000; ++i)
        eAccent += "\xC3\xA9"; // two-byte characters: the cap cuts at a boundary
    wide.features[0].detail = eAccent;
    const std::string raw = status::RenderSnapshot(wide, false);
    CHECK(IsValidUtf8(raw));
    JsonValue wide2 = ParseJson(raw, ok);
    CHECK(ok);
    if (ok)
    {
        std::string want;
        for (int i = 0; i < 256; ++i) // 256 two-byte characters = 512 bytes, still valid
            want += "\xC3\xA9";
        const JsonValue* f = Find(wide2, "features");
        const JsonValue* wideDetail =
            f != nullptr && !f->fields.empty() ? Find(f->fields[0].second, "detail") : nullptr;
        CHECK(wideDetail != nullptr && wideDetail->text == want);
    }
}

// The change key covers the state fields only: percentiles, counters and
// stamps alone never cause a write, while any state flip does.
static void CheckStateKey()
{
    const status::Snapshot base = HostileSnapshot();
    const std::string key = status::StateKey(base);

    status::Snapshot statsOnly = base; // percentiles and stamps move, state does not
    statsOnly.seq = 999;
    statsOnly.writtenUtc = "2030-01-01T00:00:00Z";
    statsOnly.uptimeSec = 99999;
    statsOnly.sessionId = "ffffffffffffffff";
    statsOnly.base.recent = Stats(180, 1.0, 1.0, 1.0, 1.0, 1.0);
    statsOnly.base.session = Stats(5, 1.0, 1.0, 1.0, 1.0, 1.0);
    statsOnly.output.recent = Stats(0, 0.0, 0.0, 0.0, 0.0, 0.0);
    CHECK(status::StateKey(statsOnly) == key);

    const auto differs = [&](status::Snapshot modified)
    {
        if (status::StateKey(modified) == key)
        {
            std::printf("FAIL statekey: a state change kept the key\n");
            ++g_failed;
        }
    };
    status::Snapshot m = base;
    m.features[0].state = status::kOff;
    differs(m);
    m = base;
    m.features[1].reason = "not-live";
    differs(m);
    m = base;
    m.features[2].detail = "interpolation";
    differs(m);
    m = base;
    m.runtimes[0].version = "1.0.0";
    differs(m);
    m = base;
    m.runtimes[0].found = false;
    differs(m);
    m = base;
    m.runtimes[0].loaded = false;
    differs(m);
    m = base;
    m.runtimes[0].folder = "other folder";
    differs(m);
    m = base;
    m.fgInput = "NoFG";
    differs(m);
    m = base;
    m.fgOutput = "NoFG";
    differs(m);
    m = base;
    m.nrEnabled = false;
    differs(m);
    m = base;
    m.nrActive = true;
    differs(m);
    m = base;
    m.errors.count = 1;
    differs(m);
    m = base;
    m.errors.code = "write";
    differs(m);
    m = base;
    m.errors.detail = "move 5";
    differs(m);
    m = base;
    m.forkDescribe = "other";
    differs(m);
    m = base;
    m.buildVer = "other";
    differs(m);
    m = base;
    m.gpuName = "other";
    differs(m);
    m = base;
    m.gpuVramMB = 1;
    differs(m);
    m = base;
    m.exeName = "other.exe";
    differs(m);
    m = base;
    m.gameMatched = false;
    differs(m);
    m = base;
    m.peStamp = 2;
    differs(m);
}

// Only version-like text may enter a version field: dotted numbers pass, the
// unlock's provisional sentence and anything with spaces or length never do.
static void CheckIsVersionText()
{
    CHECK(status::IsVersionText("310.2.1"));
    CHECK(status::IsVersionText("10.0.1"));
    CHECK(status::IsVersionText("1.2.3.4"));
    CHECK(status::IsVersionText("2.7.1"));
    CHECK(status::IsVersionText("310.9_0.3.1"));
    CHECK(status::IsVersionText("1.0-beta+2"));
    CHECK(!status::IsVersionText("metadata pending")); // the unlock's provisional readout
    CHECK(!status::IsVersionText(""));
    CHECK(!status::IsVersionText("unknown")); // letters alone carry no version
    CHECK(!status::IsVersionText(std::string(65, '1'))); // longer than the field allows
    CHECK(status::IsVersionText(std::string(64, '1'))); // exactly at the limit
    CHECK(!status::IsVersionText("1.2 3"));
    CHECK(!status::IsVersionText("1.2/3"));
    CHECK(!status::IsVersionText("ver:1"));
    CHECK(!status::IsVersionText("1\xC3\xA9" "2"));
}

// Raw file bytes become U+FFFD; valid text, including multibyte characters,
// passes through; shrinking never splits a character.
static void CheckUtf8Helpers()
{
    CHECK(status::CleanUtf8("plain") == "plain");
    CHECK(status::CleanUtf8("CJK\xE6\xBC\xA2\xE5\xAD\x97") == "CJK\xE6\xBC\xA2\xE5\xAD\x97");
    CHECK(status::CleanUtf8("emoji\xF0\x9F\x98\x80") == "emoji\xF0\x9F\x98\x80");
    CHECK(status::CleanUtf8(std::string("bad\xFF") + "byte") == "bad\xEF\xBF\xBD" "byte");
    CHECK(status::CleanUtf8(std::string("cut\xE2\x82")) == "cut\xEF\xBF\xBD");
    CHECK(status::CleanUtf8(std::string("\x80lone")) == "\xEF\xBF\xBD" "lone");
    CHECK(status::CleanUtf8(std::string("\xED\xA0\x80") + "surrogate") ==
          "\xEF\xBF\xBD" "\xEF\xBF\xBD" "\xEF\xBF\xBD" "surrogate");

    std::string ascii = "abcdef";
    status::TruncateUtf8(ascii, 3);
    CHECK(ascii == "abc");
    std::string keep = "ab";
    status::TruncateUtf8(keep, 10);
    CHECK(keep == "ab"); // short text is untouched
    std::string lead = "ab\xE2\x82"; // cut lands right after a lead byte: the tail goes
    status::TruncateUtf8(lead, 3);
    CHECK(lead == "ab");
    std::string mid = "a\xC3\xA9" "c"; // cut inside a two-byte character drops it whole
    status::TruncateUtf8(mid, 2);
    CHECK(mid == "a");
    std::string emoji = "x\xF0\x9F\x98\x80" "y";
    status::TruncateUtf8(emoji, 4);
    CHECK(emoji == "x");
}

// The closed state vocabulary keeps its documented words.
static void CheckVocabulary()
{
    CHECK(std::string(status::kEngaged) == "engaged");
    CHECK(std::string(status::kPending) == "pending");
    CHECK(std::string(status::kOff) == "off");
    CHECK(std::string(status::kFailed) == "failed");
    CHECK(std::string(status::kUnknown) == "unknown");
}

int main()
{
    CheckParserTeeth();
    CheckGolden();
    CheckFull();
    CheckSlimRender();
    CheckSlimSelected();
    CheckMinimal();
    CheckMinimalSelected();
    CheckStringCap();
    CheckStateKey();
    CheckIsVersionText();
    CheckUtf8Helpers();
    CheckVocabulary();
    if (g_failed == 0)
        std::puts("PASS: status.json renders full/slim/minimal JSON that parses, stays capped and clean, "
                  "gates on state only, and guards version text");
    return g_failed == 0 ? 0 : 1;
}

