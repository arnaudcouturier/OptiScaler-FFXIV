#include "pch.h"

#include "ext/status/StatusCollect.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <system_error>
#include <type_traits>

// JSON rendering (docs/notes/status-json.md): the snapshot to UTF-8 text, at most kFileMax bytes. Numbers use
// std::to_chars (never printf), so no locale can turn a decimal point into a comma; doubles that cannot be
// represented (NaN/Inf) render as null, so the output always parses.
namespace ext::status
{
// Only version-like text may enter a version field (see StatusCollect.h): the dlssg block gates the unlock's
// readout through this, so "metadata pending" stays unknown and never lands in the version field.
bool IsVersionText(const std::string& s)
{
    if (s.empty() || s.size() > 64)
        return false;
    bool digit = false;
    for (const unsigned char c : s)
    {
        if (c >= '0' && c <= '9')
            digit = true;
        else if (c != '.' && c != '_' && c != '-' && c != '+' && !(c >= 'a' && c <= 'z') &&
                 !(c >= 'A' && c <= 'Z'))
            return false;
    }
    return digit;
}

// Shrink to at most maxBytes without splitting a UTF-8 sequence. The input is valid UTF-8 (collection cleans it),
// so the tail is either complete or one incomplete trailing sequence, which is dropped.
void TruncateUtf8(std::string& s, size_t maxBytes)
{
    if (s.size() <= maxBytes)
        return;
    s.resize(maxBytes);
    // Start of the last character: back over trailing continuation bytes. A cut can also land right after a lead
    // byte (no trailing continuations, but the sequence is still incomplete), so the tail is validated either way.
    size_t j = s.size();
    while (j > 0 && (static_cast<unsigned char>(s[j - 1]) & 0xC0) == 0x80)
        --j;
    if (j == 0)
    {
        s.clear(); // only continuations: nothing complete (cannot happen on valid input)
        return;
    }
    const unsigned char lead = static_cast<unsigned char>(s[j - 1]);
    if (lead < 0x80)
        return; // ends in ASCII: complete
    size_t need = 0;
    if ((lead & 0xE0) == 0xC0)
        need = 2;
    else if ((lead & 0xF0) == 0xE0)
        need = 3;
    else if ((lead & 0xF8) == 0xF0)
        need = 4;
    if (need == 0 || s.size() - (j - 1) < need)
        s.resize(j - 1); // incomplete (or invalid) tail: drop it
}

// Replace invalid UTF-8 sequences (raw file bytes, e.g. ffxivgame.ver) with U+FFFD.
std::string CleanUtf8(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    static constexpr char kReplacement[] = "\xEF\xBF\xBD";
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
        {
            out += kReplacement;
            ++i;
            continue;
        }
        if (i + len > s.size())
        {
            out += kReplacement;
            break;
        }
        bool ok = true;
        for (size_t k = 1; k < len; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80)
            {
                ok = false;
                break;
            }
        if (ok && len == 3)
        {
            const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 > 0x9F))
                ok = false; // overlong or surrogate
        }
        if (ok && len == 4)
        {
            const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            if ((c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 > 0x8F))
                ok = false; // overlong or out of range
        }
        if (ok)
        {
            out.append(s, i, len);
            i += len;
        }
        else
        {
            out += kReplacement;
            ++i;
        }
    }
    return out;
}

namespace
{
template <typename T>
void AppendUint(std::string& out, T value)
{
    static_assert(std::is_integral_v<T>, "integers only");
    char buf[32] = {};
    const auto r = std::to_chars(buf, buf + sizeof buf, value);
    if (r.ec == std::errc())
        out.append(buf, r.ptr);
    else
        out += '0';
}

void AppendFixed3(std::string& out, double value)
{
    if (!std::isfinite(value))
    {
        out += "null"; // NaN/Inf can never appear as a JSON number
        return;
    }
    char buf[32] = {};
    const auto r = std::to_chars(buf, buf + sizeof buf, value, std::chars_format::fixed, 3);
    if (r.ec == std::errc())
        out.append(buf, r.ptr);
    else
        out += "null";
}

// JSON string escaping (UTF-8 in, JSON out). Control characters become \u00XX; the rest passes through, so no
// recoding can smuggle a path separator in a different form.
inline constexpr size_t kRenderMax = 512; // per-string backstop (collection already caps details shorter)

// Every string that reaches the file passes through here: raw file bytes are cleaned to valid UTF-8 (U+FFFD) and
// the text is capped, so no field can carry unbounded or invalid content even when a source missed cleaning.
void AppendEscaped(std::string& out, const std::string& s)
{
    out += '"';
    std::string clean = CleanUtf8(s);
    TruncateUtf8(clean, kRenderMax);
    for (const unsigned char c : clean)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20)
            {
                static constexpr char kHex[] = "0123456789abcdef";
                out += "\\u00";
                out += kHex[(c >> 4) & 0xF];
                out += kHex[c & 0xF];
            }
            else
                out += static_cast<char>(c);
        }
    }
    out += '"';
}

void AppendStats(std::string& out, const PerfStats& s)
{
    out += "{\"frames\":";
    AppendUint(out, s.frames);
    out += ",\"p50Ms\":";
    AppendFixed3(out, s.p50Ms);
    out += ",\"p95Ms\":";
    AppendFixed3(out, s.p95Ms);
    out += ",\"p99Ms\":";
    AppendFixed3(out, s.p99Ms);
    out += ",\"meanMs\":";
    AppendFixed3(out, s.meanMs);
    out += ",\"maxMs\":";
    AppendFixed3(out, s.maxMs);
    out += '}';
}
} // namespace

// The change-detection key: only the state fields (features, runtimes, frame generation, errors, versions).
// Percentiles and timestamps (seq, writtenUtc, uptimeSec, performance) alone never cause a write.
std::string StateKey(const Snapshot& snap)
{
    std::string key;
    key.reserve(1024);
    const auto field = [&](const std::string& s)
    {
        key += s;
        key += '\x1F';
    };
    const auto num = [&](uint64_t v)
    {
        char buf[24] = {};
        const auto r = std::to_chars(buf, buf + sizeof buf, v);
        key.append(buf, r.ptr);
        key += '\x1F';
    };
    field(snap.forkDescribe);
    field(snap.forkUpstream);
    field(snap.exeName);
    key += snap.gameMatched ? '1' : '0';
    key += '\x1F';
    num(snap.peStamp);
    num(snap.peImageSize);
    field(snap.buildVer);
    field(snap.gpuName);
    num(snap.gpuVramMB);
    for (const Runtime& r : snap.runtimes)
    {
        field(r.name);
        key += r.found ? '1' : '0';
        key += r.loaded ? '1' : '0';
        key += '\x1F';
        field(r.version);
        field(r.folder);
    }
    for (const Feature& f : snap.features)
    {
        field(f.name);
        field(f.state);
        field(f.reason);
        field(f.detail);
    }
    field(snap.fgInput);
    field(snap.fgOutput);
    key += snap.nrEnabled ? '1' : '0';
    key += snap.nrActive ? '1' : '0';
    key += '\x1F';
    field(snap.errors.code);
    num(snap.errors.count);
    field(snap.errors.detail);
    return key;
}

std::string RenderSnapshot(const Snapshot& snap, bool slim)
{
    std::string out;
    out.reserve(4096);
    out += "{\"format\":\"optiscaler-ffxiv-status\",\"version\":1";
    if (slim)
        out += ",\"truncated\":true"; // free-text details below are dropped to fit the cap
    out += ",\"seq\":";
    AppendUint(out, snap.seq);
    out += ",\"writtenUtc\":";
    AppendEscaped(out, snap.writtenUtc);
    out += ",\"session\":{\"id\":";
    AppendEscaped(out, snap.sessionId);
    out += ",\"pid\":";
    AppendUint(out, snap.pid);
    out += ",\"uptimeSec\":";
    AppendUint(out, snap.uptimeSec);
    out += ",\"startUtc\":";
    AppendEscaped(out, snap.startUtc);
    out += "},\"fork\":{\"describe\":";
    AppendEscaped(out, snap.forkDescribe);
    out += ",\"upstream\":";
    AppendEscaped(out, snap.forkUpstream);
    out += "},\"game\":{\"exe\":";
    AppendEscaped(out, snap.exeName);
    out += ",\"matched\":";
    out += snap.gameMatched ? "true" : "false";
    out += ",\"peStamp\":";
    AppendUint(out, snap.peStamp);
    out += ",\"peImageSize\":";
    AppendUint(out, snap.peImageSize);
    out += ",\"buildVer\":";
    AppendEscaped(out, snap.buildVer);
    out += "},\"gpu\":{\"name\":";
    AppendEscaped(out, snap.gpuName);
    out += ",\"vramMB\":";
    AppendUint(out, snap.gpuVramMB);
    out += "},\"runtimes\":{";
    for (size_t i = 0; i < snap.runtimes.size(); ++i)
    {
        const Runtime& r = snap.runtimes[i];
        if (i != 0)
            out += ',';
        AppendEscaped(out, r.name);
        out += ":{\"found\":"; // the colon: every key is followed by one (audit: all object/array writers below)
        out += r.found ? "true" : "false";
        out += ",\"loaded\":";
        out += r.loaded ? "true" : "false";
        out += ",\"version\":";
        AppendEscaped(out, r.version);
        out += ",\"folder\":";
        AppendEscaped(out, r.folder);
        out += '}';
    }
    out += "},\"features\":{";
    for (size_t i = 0; i < snap.features.size(); ++i)
    {
        const Feature& f = snap.features[i];
        if (i != 0)
            out += ',';
        AppendEscaped(out, f.name);
        out += ":{\"state\":";
        AppendEscaped(out, f.state);
        out += ",\"reason\":";
        AppendEscaped(out, f.reason);
        out += ",\"detail\":";
        if (slim)
            out += "\"\""; // free-text details dropped to fit the cap
        else
            AppendEscaped(out, f.detail);
        out += '}';
    }
    out += "},\"frameGeneration\":{\"input\":";
    AppendEscaped(out, snap.fgInput);
    out += ",\"output\":";
    AppendEscaped(out, snap.fgOutput);
    out += "},\"nr\":{\"enabled\":";
    out += snap.nrEnabled ? "true" : "false";
    out += ",\"active\":";
    out += snap.nrActive ? "true" : "false";
    out += "},\"performance\":{\"base\":{\"recent\":";
    AppendStats(out, snap.base.recent);
    out += ",\"session\":";
    AppendStats(out, snap.base.session);
    out += "},\"output\":";
    if (!snap.haveOutput)
        out += "null";
    else
    {
        out += "{\"recent\":";
        AppendStats(out, snap.output.recent);
        out += ",\"session\":";
        AppendStats(out, snap.output.session);
        out += '}';
    }
    out += "},\"errors\":{\"count\":";
    AppendUint(out, snap.errors.count);
    out += ",\"code\":";
    AppendEscaped(out, snap.errors.code);
    out += ",\"detail\":";
    AppendEscaped(out, snap.errors.detail);
    out += "}}";
    return out; // the caller enforces the cap (see ToJson): the text is never chopped
}

// Minimal valid document: fixed fields only, always far below the cap. Reached only when even the slim render
// does not fit (unreachable with capped details, but the cap guarantee must not depend on that).
std::string RenderMinimal(const Snapshot& snap)
{
    std::string out;
    out.reserve(512);
    out += "{\"format\":\"optiscaler-ffxiv-status\",\"version\":1,\"truncated\":true,\"seq\":";
    AppendUint(out, snap.seq);
    out += ",\"session\":{\"id\":";
    AppendEscaped(out, snap.sessionId);
    out += ",\"pid\":";
    AppendUint(out, snap.pid);
    out += "},\"errors\":{\"count\":";
    AppendUint(out, snap.errors.count);
    out += ",\"code\":";
    AppendEscaped(out, snap.errors.code);
    out += ",\"detail\":";
    AppendEscaped(out, snap.errors.detail);
    out += "}}";
    return out;
}

std::string ToJson(const Snapshot& snap)
{
    std::string full = RenderSnapshot(snap, false);
    if (full.size() <= kFileMax)
        return full;
    std::string slim = RenderSnapshot(snap, true); // free-text details dropped, marked truncated
    if (slim.size() <= kFileMax)
        return slim;
    return RenderMinimal(snap);
}

} // namespace ext::status
