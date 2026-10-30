#pragma once
// Shared helpers of the FG recorder tests: a strict JSON parser in the style
// of tests/ext/status/status_json.cpp (objects, arrays, strings, strict
// numbers; unknown words, single quotes, raw control characters, lone
// surrogates and trailing text all fail), plus the small file and session
// helpers every recorder test needs. A fixture the recorder writes wrongly
// (an unescaped quote, a cut number) fails the parse here instead of passing
// through a lenient reader.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace fgrec_test
{
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
                            ok = false;
                    }
                    else if (hi >= 0xDC00 && hi <= 0xDFFF)
                        ok = false;
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
                ok = false;
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
            if (p != end && *p == '}')
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
            if (p != end && *p == ']')
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
            v.boolean = false;
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
        return v;
    }
};

inline JsonValue ParseLine(const std::string& s, bool& ok)
{
    JsonParser parser(s);
    JsonValue v = parser.ParseValue();
    parser.SkipSpace();
    ok = parser.ok && parser.p == parser.end;
    return v;
}

inline const JsonValue* Find(const JsonValue& obj, const std::string& key)
{
    if (obj.type != JsonValue::Type::Object)
        return nullptr;
    for (const auto& field : obj.fields)
        if (field.first == key)
            return &field.second;
    return nullptr;
}

inline bool IsText(const JsonValue* v, const std::string& want)
{
    return v != nullptr && v->type == JsonValue::Type::Text && v->text == want;
}

// Whole file bytes (binary: the writer uses \n only, so a \r anywhere is a failure).
inline std::string ReadBytes(const std::wstring& path)
{
    std::string out;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr)
        return out;
    char buf[65536] = {};
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) != 0)
        out.append(buf, n);
    fclose(f);
    return out;
}

// Raw lines without the \n (no line may carry a \r: the file is strict \n).
inline std::vector<std::string> SplitLines(const std::string& bytes)
{
    std::vector<std::string> lines;
    size_t at = 0;
    while (at < bytes.size())
    {
        const size_t nl = bytes.find('\n', at);
        const size_t end = nl == std::string::npos ? bytes.size() : nl;
        lines.push_back(bytes.substr(at, end - at));
        at = nl == std::string::npos ? bytes.size() : nl + 1;
    }
    return lines;
}

// A fresh empty directory under the temp folder, unique per call.
inline std::wstring FreshDir(const char* tag)
{
    static long counter = 0;
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
    wchar_t dir[MAX_PATH] = {};
    _snwprintf_s(dir, _TRUNCATE, L"%sfgrec-test-%lu-%d-%hs", temp, GetCurrentProcessId(), ++counter, tag);
    CreateDirectoryW(dir, nullptr);
    return dir;
}

inline bool WaitUntil(bool (*ready)(), int timeoutMs)
{
    const DWORD start = GetTickCount();
    while (!ready())
    {
        if (GetTickCount() - start > static_cast<DWORD>(timeoutMs))
            return false;
        Sleep(20);
    }
    return true;
}

// True once the session file ends with its terminal record: the writer emits session.end last, after every
// queued record, so a parsed end line means the whole session is in the file. The worker's running flag goes
// false before that write, so a test that reads a file right after a stop must wait for this instead.
inline bool SessionEnded(const std::wstring& path)
{
    const std::vector<std::string> lines = SplitLines(ReadBytes(path));
    if (lines.empty())
        return false;
    bool ok = false;
    const JsonValue last = ParseLine(lines.back(), ok);
    return ok && IsText(Find(last, "kind"), "session.end");
}

// Waits for the terminal record of one file; false on timeout or when the file never appears.
inline bool WaitForSessionEnd(const std::wstring& path, int timeoutMs)
{
    const DWORD start = GetTickCount();
    while (!SessionEnded(path))
    {
        if (GetTickCount() - start > static_cast<DWORD>(timeoutMs))
            return false;
        Sleep(20);
    }
    return true;
}
} // namespace fgrec_test
