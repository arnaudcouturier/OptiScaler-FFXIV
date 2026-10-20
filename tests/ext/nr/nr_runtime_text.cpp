// The words of the NR model version picker (ext/nr/runtime/NrRuntimeText.h), pinned.
#include "ext/nr/runtime/NrRuntimeText.h"

#include <cstdio>

using namespace ext::nr::runtime;
namespace txt = ext::nr::runtime::text;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)
#define CHECK_EQ(a, b) do { const std::string _a = (a); const std::string _b = (b); if (_a != _b) { std::printf("FAIL line %d: %s\n  got:      %s\n  expected: %s\n", __LINE__, #a, _a.c_str(), _b.c_str()); ++failures; } } while (0)

static Entry Folder(const wchar_t* name, VersionParts fileVersion, uint64_t size, const char* sha)
{
    Entry e;
    e.id = name;
    e.folderVersion = *ParseFolderVersion(name);
    e.fileVersion = std::move(fileVersion);
    e.sizeBytes = size;
    e.sha256 = sha;
    return e;
}
static Entry Legacy(VersionParts fileVersion, uint64_t size, const char* sha)
{
    Entry e;
    e.id = L"legacy";
    e.legacy = true;
    e.fileVersion = std::move(fileVersion);
    e.sizeBytes = size;
    e.sha256 = sha;
    return e;
}

static const char* kSha = "1a2b3c4d5e6f00112233445566778899aabbccddeeff00112233445566778899";

static void Labels()
{
    CHECK_EQ(txt::EntryLabel(Folder(L"NR.310.9", { 310, 9, 16, 0 }, 276162560, kSha)), "NR.310.9  (310.9.16.0, 263.4 MB, SHA-256 1a2b3c4d5e6f)");
    CHECK_EQ(txt::EntryLabel(Legacy({ 310, 8, 0, 0 }, 276162560, kSha)), "Legacy location  (310.8.0.0, 263.4 MB, SHA-256 1a2b3c4d5e6f)");
    CHECK_EQ(txt::EntryLabel(Folder(L"NR.310.7", {}, 1000, "")), "NR.310.7  (310.7, 1000 B, SHA-256 hashing...)"); // folder version while the file has none; hash not ready
    CHECK_EQ(txt::EntryLabel(Legacy({}, 0, "")), "Legacy location  (0 B, SHA-256 hashing...)");
    CHECK_EQ(txt::AutomaticLabel({}), "Automatic (nothing installed)");
    CHECK_EQ(txt::AutomaticLabel({ Legacy({ 310, 8, 0, 0 }, 1, kSha) }), "Automatic (newest: legacy location)");
    CHECK_EQ(txt::AutomaticLabel({ Legacy({ 310, 8, 0, 0 }, 1, kSha), Folder(L"NR.310.9", { 310, 9, 0, 0 }, 1, kSha) }), "Automatic (newest: NR.310.9)");
}

static void ComboChoices()
{
    const std::vector<Entry> entries { Folder(L"NR.310.9", { 310, 9, 16, 0 }, 100, kSha), Legacy({ 310, 8, 0, 0 }, 100, kSha), Folder(L"NR.310.10", {}, 100, kSha) };
    {
        const auto c = txt::BuildChoices(entries, "");
        CHECK(c.labels.size() == 4u && c.values.size() == 4u && c.selected == 0);
        CHECK_EQ(c.values[0], "");
        CHECK_EQ(c.values[1], "legacy"); // legacy first, then the folders, newest first
        CHECK_EQ(c.values[2], "NR.310.10");
        CHECK_EQ(c.values[3], "NR.310.9");
        CHECK_EQ(c.labels[0], "Automatic (newest: NR.310.10)");
    }
    CHECK(txt::BuildChoices(entries, "auto").selected == 0);
    CHECK(txt::BuildChoices(entries, "legacy").selected == 1);
    CHECK(txt::BuildChoices(entries, "NR.310.9").selected == 3);
    CHECK(txt::BuildChoices(entries, "nr.310.10").selected == 2); // not case sensitive
    {
        // a runtime named in the config but not installed stays visible, selected, and says so (never silently another model)
        const auto c = txt::BuildChoices(entries, "NR.310.8");
        CHECK(c.labels.size() == 5u && c.selected == 4);
        CHECK_EQ(c.labels[4], "NR.310.8 (not installed)");
        CHECK_EQ(c.values[4], "NR.310.8");
    }
    {
        const auto c = txt::BuildChoices(entries, "legacy");
        CHECK(c.selected == 1);
        const auto none = txt::BuildChoices({ Folder(L"NR.1", {}, 1, kSha) }, "legacy");
        CHECK(none.selected == 2 && none.labels[2] == "legacy (not installed)");
    }
}

static void CardStatus()
{
    txt::CardFacts f;
    f.available = true;
    f.loadedId = "NR.310.9";
    f.loadedVersion = "310.9.16.0";
    {
        const auto lines = txt::StatusLines(f);
        CHECK(lines.size() == 1u);
        CHECK_EQ(lines[0], "Selected model: NR.310.9, file version 310.9.16.0.");
        f.scanNote = "the last rescan failed (the scan failed (disk exploded)); the previous list is kept";
        const auto withNote = txt::StatusLines(f);
        CHECK(withNote.size() == 2u);
        CHECK_EQ(withNote[0], "The last rescan failed (the scan failed (disk exploded)); the previous list is kept.");
        f.scanNote.clear();
    }
    f.pending = true;
    CHECK_EQ(txt::StatusLines(f)[0], "Switching as soon as the clicks settle...");
    f.pending = false;
    f.lockReason = "the hybrid FP4 model precision keeps its weights per device";
    f.nativeVulkanRunning = true;
    f.ngxPathFollows = false;
    f.ngxFolder = "C:\\game\\OptiScaler";
    f.reuseWanted = true;
    f.reuseStatus = "computed 3, reused 1 | verified kernel module active";
    {
        const auto lines = txt::StatusLines(f);
        CHECK(lines.size() == 5u);
        CHECK_EQ(lines[1], "Live switching is off for this session: the hybrid FP4 model precision keeps its weights per device. Restart the game to change the model.");
        CHECK_EQ(lines[2], "Native Vulkan Neural Rendering keeps the model it started with; restart the game to change it.");
        CHECK_EQ(lines[3], "The NGX core was told at startup to search C:\\game\\OptiScaler first. This model is loaded by its full path, so it works now; restart the game to move that search path too.");
        CHECK_EQ(lines[4], "Bottleneck reuse (describes the loaded model only): computed 3, reused 1 | verified kernel module active");
    }
    {
        txt::CardFacts bad;
        bad.available = false;
        bad.reason = "the selected NR runtime NR.310.8 is not installed; nothing else is loaded in its place";
        bad.ngxPathFollows = false; // no model: the search-path note is not shown
        const auto lines = txt::StatusLines(bad);
        CHECK(lines.size() == 1u);
        CHECK_EQ(lines[0], "Neural Rendering is unavailable: the selected NR runtime NR.310.8 is not installed; nothing else is loaded in its place.");
    }
    {
        txt::CardFacts noVersion;
        noVersion.available = true;
        noVersion.loadedId = "legacy";
        CHECK_EQ(txt::StatusLines(noVersion)[0], "Selected model: legacy.");
    }
}

int main()
{
    Labels();
    ComboChoices();
    CardStatus();
    if (failures == 0)
        std::puts("PASS: NR runtime text");
    return failures == 0 ? 0 : 1;
}
