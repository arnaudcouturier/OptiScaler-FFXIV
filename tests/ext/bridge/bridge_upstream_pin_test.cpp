// [BRIDGE] Source pin on the D3D11 -> D3D12 hand-over. The flush experiment is one line in an upstream file, so nothing
// in a pure test can see it move: this test reads the site (comments stripped, whitespace collapsed) and pins the exact
// statement sequence around it. The seam must sit right after the accepted fence Signal and its error check, keep its
// `if`, and stay ahead of the D3D12 wait -- a call moved before the Signal would flush work the D3D12 side is not
// waiting for, and a call without its `if` would flush every frame whether the key is on or not.
// A pin that stops matching means the site moved: re-derive it, do not loosen it. Source text only, no git inside a test.
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
        if (s[i] == '"')
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

// text between the start of `from` and the next occurrence of `to` after it ("" when missing)
static std::string Between(const std::string& text, const std::string& from, const std::string& to)
{
    const auto a = text.find(from);
    if (a == std::string::npos)
        return {};
    const auto b = text.find(to, a + from.size());
    return b == std::string::npos ? std::string {} : text.substr(a, b - a);
}

// The whole hand-over site in one statement sequence: the Signal, its error check with its early return, then the seam.
// This is what must not move or lose its guard.
static const char* kHandOver =
    "auto result = Dx11DeviceContext->Signal(Dx11FenceTextureCopy, fenceValue); "
    "if (result != S_OK) { "
    "LOG_ERROR(\"Dx11WithDx12 Dx11 signal failed for fence {}: {:X}\", fenceValue, (UINT) result); return false; } "
    "if (ext::bridge::FlushAfterSignal()) Dx11DeviceContext->Flush();";

static const char* kSeam = "if (ext::bridge::FlushAfterSignal()) Dx11DeviceContext->Flush();";

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    const auto raw = Read(repo + "/OptiScaler/with_dx12/dx11_with_dx12.cpp");
    const auto text = Normalize(raw);
    CHECK(raw.size() > 10000 && text.size() > 10000); // the file was read

    // ---- the site: one seam, right after the Signal and its error check, ahead of the D3D12 wait -------------------------
    CHECK(Count(text, kSeam) == 1);
    CHECK(Count(text, "FlushAfterSignal") == 1);
    CHECK(Has(text, kHandOver));
    {
        const auto fn = Between(text, "bool Dx11WithDx12::SyncDx11ToDx12()", "result = Dx12CommandQueue->Wait(");
        CHECK(!fn.empty());
        CHECK(Has(fn, kSeam));                                   // inside the D3D11 -> D3D12 hand-over
        CHECK(Has(fn, "Dx11DeviceContext->Signal("));            // and after the Signal, not before it
        CHECK(fn.find("Dx11DeviceContext->Signal(") < fn.find(kSeam));
        CHECK(!Has(fn, "result = Dx12CommandQueue->Wait("));     // the seam is the last statement before the D3D12 wait
        CHECK(Count(fn, "Dx11DeviceContext->Flush()") == 1);     // the seam is the hand-over's only flush
    }

    // ---- the other Flush in the file is upstream's own (the CPU-wait path), not a second unconditional one -----------------
    CHECK(Count(text, "Dx11DeviceContext->Flush()") == 2);
    CHECK(Count(text, "if (UseCpuWaitForDx12()) Dx11DeviceContext->Flush();") == 1);

    // ---- the seam is reachable: the file includes the fork's seam header --------------------------------------------------
    CHECK(Has(raw, "#include \"ext/Seams.h\""));

    // ---- negative controls: the pin notices each breakage the brief names -------------------------------------------------
    {
        // the call moved before the Signal: the sequence no longer matches and the order check fails
        std::string moved = text;
        const auto at = moved.find(kSeam);
        moved.erase(at, std::string(kSeam).size());
        const auto signal = moved.find("auto result = Dx11DeviceContext->Signal(");
        moved.insert(signal, std::string(kSeam) + " ");
        CHECK(!Has(moved, kHandOver));
        const auto fn = Between(moved, "bool Dx11WithDx12::SyncDx11ToDx12()", "result = Dx12CommandQueue->Wait(");
        CHECK(!fn.empty() && !(fn.find("Dx11DeviceContext->Signal(") < fn.find(kSeam)));
    }
    {
        // the guard lost: an unconditional flush every frame
        std::string unguarded = text;
        const auto at = unguarded.find(std::string(kSeam));
        unguarded.replace(at, std::string(kSeam).size(), "Dx11DeviceContext->Flush();");
        CHECK(!Has(unguarded, kHandOver));
        CHECK(Count(unguarded, kSeam) == 0);
    }
    {
        // a second, unconditional flush added next to the seam: the file-wide count notices
        std::string twice = text;
        twice += " Dx11DeviceContext->Flush();";
        CHECK(Count(twice, "Dx11DeviceContext->Flush()") != 2);
    }

    if (g_failed != 0)
    {
        std::printf("bridge_upstream_pin: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("bridge_upstream_pin: all passed\n");
    return 0;
}
