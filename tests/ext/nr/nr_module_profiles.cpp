// Host check of DlssNrModuleProfiles.h: which NR CUDA modules are verified, and for what (reuse / hybrid are separate).
// cl /std:c++20 /EHsc /I OptiScaler tests/ext/nr/nr_module_profiles.cpp
#include "ext/nr/native/DlssNrModuleProfiles.h"

#include <cstdio>
#include <iterator>
#include <string>

using namespace DlssNrModules;

static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

static std::string Hex(const unsigned char* p, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i)
    {
        s += digits[p[i] >> 4];
        s += digits[p[i] & 15];
    }
    return s;
}

int main()
{
    // The digest the code carried before the table existed (DlssNrNative.cpp Supported(), 3,202,680 bytes), copied verbatim.
    // A typo in the table row would switch reuse and hybrid off for the verified module without any other test noticing.
    const unsigned char original[32] = { 0x3f, 0xa6, 0xf0, 0x76, 0xee, 0xcf, 0xbb, 0x6e, 0x19, 0xc3, 0x78,
                                         0xf8, 0x4b, 0xbb, 0x70, 0x25, 0xcd, 0x80, 0x5e, 0xd4, 0xde, 0x9a,
                                         0x2f, 0x9c, 0x9b, 0x99, 0x5e, 0xb3, 0x4a, 0x7f, 0x3d, 0x76 };
    const std::string originalHex = Hex(original, sizeof original);

    // The production table is well formed and still contains exactly the one verified module, with both permissions.
    CHECK(WellFormed(std::span<const Row>(kProfiles)));
    CHECK(std::size(kProfiles) == 1);
    CHECK(kProfiles[0].bytes == 3202680u);
    CHECK(kProfiles[0].sha256 == originalHex);
    {
        const Flags f = Classify(3202680u, originalHex);
        CHECK(f.reuse && f.hybrid);
    }

    // Both halves of the fingerprint matter.
    {
        Flags f = Classify(3202681u, originalHex); // right digest, wrong size
        CHECK(!f.reuse && !f.hybrid);
        std::string other = originalHex;
        other[10] = other[10] == '0' ? '1' : '0'; // right size, one digit off
        f = Classify(3202680u, other);
        CHECK(!f.reuse && !f.hybrid);
        f = Classify(3202680u, "unavailable"); // IdentifyModule could not hash the blob
        CHECK(!f.reuse && !f.hybrid);
        f = Classify(3202680u, "");
        CHECK(!f.reuse && !f.hybrid);
        std::string upper = originalHex;
        for (char& c : upper)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        f = Classify(3202680u, upper); // a digest pasted from another tool's uppercase output still matches
        CHECK(f.reuse && f.hybrid);
    }

    // The hard rule of S9a: the model in the user's current game is NOT verified, in either way.
    {
        const Flags f = Classify(1417240u, "2a05b056783f74c602b7a7a53d48629547054cd9b23a4c75a92bc2ce77d6d9c3");
        CHECK(!f.reuse && !f.hybrid);
    }

    // Every other module the runtime loads (232 of them in the live log) is unknown: neither permission.
    CHECK(!Classify(15672u, "9176c8f2129228a680d4087375ef0d56e470cae07107bb8b27192296c07d6081").reuse);
    CHECK(!Classify(0u, "").reuse);

    // Split semantics, on synthetic tables so nothing fake enters production.
    {
        const std::string reuseSha(64, 'a'), hybridSha(64, 'b'), bothSha(64, 'c'), noneSha(64, 'd');
        const Row rows[] = {
            { 100u, reuseSha, true, false, "reuse only" },
            { 200u, hybridSha, false, true, "hybrid only" },
            { 300u, bothSha, true, true, "both" },
            { 400u, noneSha, false, false, "listed but verified for nothing" },
        };
        const std::span<const Row> table(rows);
        CHECK(WellFormed(table));

        Flags f = Classify(table, 100u, reuseSha);
        CHECK(f.reuse && !f.hybrid); // a reuse-only row (what S9b adds) never enables the hybrid rewrite
        f = Classify(table, 200u, hybridSha);
        CHECK(!f.reuse && f.hybrid);
        f = Classify(table, 300u, bothSha);
        CHECK(f.reuse && f.hybrid);
        f = Classify(table, 400u, noneSha);
        CHECK(!f.reuse && !f.hybrid);
        f = Classify(table, 100u, hybridSha); // sha of one row with the size of another
        CHECK(!f.reuse && !f.hybrid);
        f = Classify(table, 999u, reuseSha);
        CHECK(!f.reuse && !f.hybrid);
        f = Classify(std::span<const Row>(), 100u, reuseSha); // empty table
        CHECK(!f.reuse && !f.hybrid);
    }

    // Table hygiene checks catch what a hand-edited row can get wrong.
    {
        const std::string good(64, 'a');
        const Row dup[] = { { 1u, good, true, false, "" }, { 1u, good, false, true, "" } };
        CHECK(!WellFormed(std::span<const Row>(dup)));
        const std::string upperSha(64, 'A'), shortShaText(63, 'a'); // views below must not outlive their strings
        const Row upper[] = { { 1u, upperSha, true, false, "" } };
        CHECK(!WellFormed(std::span<const Row>(upper)));
        const Row shortSha[] = { { 1u, shortShaText, true, false, "" } };
        CHECK(!WellFormed(std::span<const Row>(shortSha)));
        const Row zeroSize[] = { { 0u, good, true, false, "" } };
        CHECK(!WellFormed(std::span<const Row>(zeroSize)));
        const Row sameShaOtherSize[] = { { 1u, good, true, false, "" }, { 2u, good, true, false, "" } };
        CHECK(WellFormed(std::span<const Row>(sameShaOtherSize)));
    }

    printf(fails ? "nr_module_profiles: %d FAILED\n" : "nr_module_profiles: all passed\n", fails);
    return fails != 0;
}
