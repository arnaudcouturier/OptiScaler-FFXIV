// The file side of the character preview probe (CharaProbeFiles.h): pixel conversion, the dump plan, the output budget in front of the
// writes, folder naming, state.txt text. No D3D; real files in a temp folder.
#include "check.h"
#include "ext/ffxiv/chara/CharaProbeFiles.h"

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
std::vector<uint8_t> ReadAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string ReadText(const fs::path& p)
{
    const auto bytes = ReadAll(p);
    return std::string(bytes.begin(), bytes.end());
}

size_t CountEntries(const fs::path& dir)
{
    size_t n = 0;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        ++n;
    return n;
}

// 3x2 image, rows 16 bytes apart (4 bytes of padding after each row of 12). Pixel (x, y) bytes: first, second, third, alpha.
std::vector<uint8_t> PaddedImage(uint8_t c0, uint8_t c1, uint8_t c2)
{
    std::vector<uint8_t> data(16 * 2, 0xEE);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 3; ++x)
        {
            uint8_t* p = data.data() + y * 16 + x * 4;
            p[0] = static_cast<uint8_t>(c0 + x * 10 + y * 40);
            p[1] = static_cast<uint8_t>(c1 + x * 10 + y * 40);
            p[2] = static_cast<uint8_t>(c2 + x * 10 + y * 40);
            p[3] = static_cast<uint8_t>(200 + x + y * 5);
        }
    return data;
}

DumpImage MakeImage(const std::string& name, uint32_t format, const std::string& formatText, const std::vector<uint8_t>& padded)
{
    DumpImage img;
    img.name = name;
    img.present = true;
    img.width = 3;
    img.height = 2;
    img.format = format;
    img.formatText = formatText;
    img.rowPitch = 16;
    img.data = padded;
    return img;
}

void TestText()
{
    CHECK(FormatStamp(2026, 10, 7, 3, 4, 5) == "20261007-030405");
    CHECK(FormatStamp(2026, 12, 31, 23, 59, 59) == "20261231-235959");
    CHECK(FormatLogTime(2026, 10, 7, 3, 4, 5, 9) == "2026-10-07 03:04:05.009");
    CHECK(SizeText(12) == "12 bytes" && SizeText(2048) == "2.0 KiB" && SizeText(3 * 1024 * 1024) == "3.0 MiB");

    StateText t;
    t.Add("a.b", "value");
    t.Add("bad key:!", "two\nlines\tand\rmore");
    t.Add("n", uint64_t { 42 });
    t.AddBool("yes", true);
    t.AddBool("no", false);
    CHECK(t.Str() == "a.b: value\nbad_key__: two lines and more\nn: 42\nyes: yes\nno: no\n");

    // UTF-8 conversion of a path with non-ASCII characters; no throw
    const fs::path japanese = fs::path(L"C:\\game\\\u65e5\u672c");
    CHECK(Utf8(japanese) == "C:\\game\\\xE6\x97\xA5\xE6\x9C\xAC");
    CHECK(Utf8(fs::path()).empty());
}

void TestConvert()
{
    CHECK(LayoutOfFormat(27) == PixelLayout::Rgba && LayoutOfFormat(28) == PixelLayout::Rgba && LayoutOfFormat(29) == PixelLayout::Rgba);
    CHECK(LayoutOfFormat(90) == PixelLayout::Bgra && LayoutOfFormat(87) == PixelLayout::Bgra && LayoutOfFormat(91) == PixelLayout::Bgra);
    for (uint32_t f : { 0u, 2u, 10u, 24u, 26u, 88u, 92u, 93u })
        CHECK(LayoutOfFormat(f) == PixelLayout::Unsupported);

    // B8G8R8A8 bytes are blue, green, red, alpha: the red plane value is byte 2
    const auto bgra = PaddedImage(1, 2, 3); // bytes b=1 g=2 r=3 for pixel (0,0)
    Planes planes;
    CHECK(ConvertPixels(87, 3, 2, bgra.data(), bgra.size(), 16, planes));
    CHECK(planes.rgb.size() == 18 && planes.alpha.size() == 6);
    CHECK(planes.rgb[0] == 3 && planes.rgb[1] == 2 && planes.rgb[2] == 1); // R G B of pixel (0,0)
    CHECK(planes.alpha[0] == 200 && planes.alpha[1] == 201 && planes.alpha[2] == 202 && planes.alpha[3] == 205);
    CHECK(planes.rgb[3] == 13 && planes.rgb[4] == 12 && planes.rgb[5] == 11); // pixel (1,0): +10 on each byte
    CHECK(planes.rgb[9] == 43 && planes.rgb[10] == 42 && planes.rgb[11] == 41); // pixel (0,1): +40
    CHECK(ConvertPixels(91, 3, 2, bgra.data(), bgra.size(), 16, planes) && planes.rgb[0] == 3 && planes.rgb[2] == 1);
    CHECK(ConvertPixels(90, 3, 2, bgra.data(), bgra.size(), 16, planes) && planes.rgb[0] == 3 && planes.rgb[2] == 1);

    // R8G8B8A8 bytes are red, green, blue, alpha
    const auto rgba = PaddedImage(7, 8, 9);
    CHECK(ConvertPixels(28, 3, 2, rgba.data(), rgba.size(), 16, planes));
    CHECK(planes.rgb[0] == 7 && planes.rgb[1] == 8 && planes.rgb[2] == 9);
    CHECK(planes.alpha[0] == 200 && planes.alpha[5] == 207);
    CHECK(ConvertPixels(29, 3, 2, rgba.data(), rgba.size(), 16, planes) && planes.rgb[0] == 7);
    CHECK(ConvertPixels(27, 3, 2, rgba.data(), rgba.size(), 16, planes) && planes.rgb[0] == 7);

    // the padding bytes are never read as pixels; a tightly packed buffer works with pitch == width * 4
    std::vector<uint8_t> tight(24);
    for (size_t i = 0; i < tight.size(); ++i)
        tight[i] = static_cast<uint8_t>(i);
    CHECK(ConvertPixels(28, 3, 2, tight.data(), tight.size(), 12, planes));
    CHECK(planes.rgb[3] == 4 && planes.rgb[4] == 5 && planes.rgb[5] == 6 && planes.alpha[1] == 7 && planes.alpha[3] == 15);

    // failures leave the output alone
    Planes keep;
    keep.rgb = { 1, 2, 3 };
    keep.alpha = { 9 };
    CHECK(!ConvertPixels(10, 3, 2, rgba.data(), rgba.size(), 16, keep));    // unsupported format
    CHECK(!ConvertPixels(28, 0, 2, rgba.data(), rgba.size(), 16, keep));    // empty
    CHECK(!ConvertPixels(28, 3, 0, rgba.data(), rgba.size(), 16, keep));
    CHECK(!ConvertPixels(28, 3, 2, nullptr, rgba.size(), 16, keep));
    CHECK(!ConvertPixels(28, 3, 2, rgba.data(), rgba.size(), 8, keep));     // pitch smaller than a row
    CHECK(!ConvertPixels(28, 3, 2, rgba.data(), 16 + 11, 16, keep));        // buffer one byte short of the last row
    CHECK(ConvertPixels(28, 3, 2, rgba.data(), 16 + 12, 16, keep));         // exactly enough (no padding after the last row)
    keep.rgb = { 1, 2, 3 };
    CHECK(!ConvertPixels(28, kMaxPngDimension + 1, 1, rgba.data(), rgba.size(), 16, keep) && keep.rgb.size() == 3);
}

void TestPlan()
{
    std::vector<DumpImage> images;
    images.push_back(MakeImage("native", 87, "B8G8R8A8_UNORM(87)", PaddedImage(1, 2, 3)));
    DumpImage missing;
    missing.name = "aa";
    missing.note = "the spatial AA has not produced an image";
    images.push_back(missing);
    images.push_back(MakeImage("dlss", 10, "R16G16B16A16_FLOAT(10)", PaddedImage(1, 2, 3)));
    DumpImage bad = MakeImage("odd", 28, "R8G8B8A8_UNORM(28)", PaddedImage(1, 2, 3));
    bad.width = kMaxPngDimension + 1;
    images.push_back(bad);
    DumpImage empty;
    empty.name = "native-2"; // no note
    images.push_back(empty);

    const DumpPlan plan = MakePlan(images, "key: value\n");
    CHECK(plan.files.size() == 3);
    CHECK(plan.files[0].fileName == "native.png" && plan.files[0].size == PngSize(3, 2, 3) && !plan.files[0].alpha && plan.files[0].image == 0);
    CHECK(plan.files[1].fileName == "alpha-native.png" && plan.files[1].size == PngSize(3, 2, 1) && plan.files[1].alpha);
    CHECK(plan.files[2].fileName == "state.txt" && plan.files[2].size == plan.stateText.size());
    CHECK(plan.bytes == PngSize(3, 2, 3) + PngSize(3, 2, 1) + plan.stateText.size());
    CHECK(plan.stateText.rfind("key: value\n", 0) == 0);
    CHECK(plan.stateText.find("image.native: 3x2 B8G8R8A8_UNORM(87), saved as native.png and alpha-native.png\n") != std::string::npos);
    CHECK(plan.stateText.find("image.aa: not saved, the spatial AA has not produced an image\n") != std::string::npos);
    CHECK(plan.stateText.find("image.dlss: not saved, format R16G16B16A16_FLOAT(10) is not supported") != std::string::npos);
    CHECK(plan.stateText.find("image.odd: not saved, the size 16385x2 is not valid\n") != std::string::npos);
    CHECK(plan.stateText.find("image.native-2: not saved, no image available\n") != std::string::npos);

    const DumpPlan none = MakePlan({}, "");
    CHECK(none.files.size() == 1 && none.files[0].fileName == "state.txt" && none.bytes == 0);
}

void TestFolderNames(const fs::path& root)
{
    const fs::path a = CreateUniqueFolder(root / "names", "20261007-030405");
    const fs::path b = CreateUniqueFolder(root / "names", "20261007-030405");
    const fs::path c = CreateUniqueFolder(root / "names", "20261007-030405");
    CHECK(a.filename() == "20261007-030405" && b.filename() == "20261007-030405-2" && c.filename() == "20261007-030405-3");
    CHECK(fs::is_directory(a) && fs::is_directory(b) && fs::is_directory(c));
    // a different stamp is its own folder
    CHECK(CreateUniqueFolder(root / "names", "20261007-030406").filename() == "20261007-030406");
    // a root that is a file cannot hold folders
    {
        std::ofstream(root / "afile.txt") << "x";
        CHECK(CreateUniqueFolder(root / "afile.txt", "x").empty());
    }
}

void TestWriteDump(const fs::path& root)
{
    const auto bgra = PaddedImage(1, 2, 3);
    const auto rgba = PaddedImage(50, 60, 70);
    std::vector<DumpImage> images;
    images.push_back(MakeImage("native", 87, "B8G8R8A8_UNORM(87)", bgra));
    images.push_back(MakeImage("aa", 29, "R8G8B8A8_UNORM_SRGB(29)", rgba));
    DumpImage dlss;
    dlss.name = "dlss";
    dlss.note = "the DLSS path has no evaluated preview image";
    images.push_back(dlss);

    OutputBudget budget;
    const fs::path out = root / "dump";
    const DumpOutcome o = WriteDump(out, "20261007-030405", images, "probe.on: yes\n", budget);
    CHECK(o.result == DumpOutcome::Result::Written);
    CHECK(o.files == 5 && o.plannedFiles == 5);
    const fs::path folder = out / "20261007-030405";
    CHECK(o.folder == Utf8(folder));
    CHECK(CountEntries(folder) == 5);

    // the files are exactly what the tested encoder makes of the converted pixels
    Planes native, aa;
    CHECK(ConvertPixels(87, 3, 2, bgra.data(), bgra.size(), 16, native) && ConvertPixels(29, 3, 2, rgba.data(), rgba.size(), 16, aa));
    std::vector<uint8_t> expected;
    EncodePng(3, 2, 3, native.rgb.data(), expected);
    CHECK(ReadAll(folder / "native.png") == expected);
    expected.clear();
    EncodePng(3, 2, 1, native.alpha.data(), expected);
    CHECK(ReadAll(folder / "alpha-native.png") == expected);
    expected.clear();
    EncodePng(3, 2, 3, aa.rgb.data(), expected);
    CHECK(ReadAll(folder / "aa.png") == expected);
    expected.clear();
    EncodePng(3, 2, 1, aa.alpha.data(), expected);
    CHECK(ReadAll(folder / "alpha-aa.png") == expected);
    CHECK(!fs::exists(folder / "dlss.png") && !fs::exists(folder / "alpha-dlss.png"));
    const std::string state = ReadText(folder / "state.txt");
    CHECK(state.rfind("probe.on: yes\n", 0) == 0);
    CHECK(state.find("image.dlss: not saved, the DLSS path has no evaluated preview image\n") != std::string::npos);
    CHECK(o.bytes == fs::file_size(folder / "native.png") + fs::file_size(folder / "alpha-native.png") + fs::file_size(folder / "aa.png") +
                         fs::file_size(folder / "alpha-aa.png") + state.size());
    CHECK(budget.files == 5 && budget.bytes == o.bytes);
    CHECK(o.message.find("ok, 5 of 5 files") == 0);

    // the same second again: a -2 folder
    const DumpOutcome o2 = WriteDump(out, "20261007-030405", images, "x: 1\n", budget);
    CHECK(o2.result == DumpOutcome::Result::Written && fs::is_directory(out / "20261007-030405-2"));
    CHECK(budget.files == 10);

    // state only
    const DumpOutcome st = WriteDump(out, "20261007-030406", {}, "only: state\n", budget);
    CHECK(st.result == DumpOutcome::Result::Written && st.files == 1 && CountEntries(out / "20261007-030406") == 1);
    CHECK(ReadText(out / "20261007-030406" / "state.txt") == "only: state\n");
    CHECK(budget.files == 11);
}

void TestBudgetRefusal(const fs::path& root)
{
    std::vector<DumpImage> images;
    images.push_back(MakeImage("native", 87, "B8G8R8A8_UNORM(87)", PaddedImage(1, 2, 3)));
    const fs::path out = root / "budget";

    // The set needs 3 files. With 38 used only 2 remain: nothing at all is written (not even state.txt), no folder is made.
    OutputBudget tight;
    tight.files = kMaxFiles - 2;
    DumpOutcome o = WriteDump(out, "20261007-040000", images, "a: b\n", tight);
    CHECK(o.result == DumpOutcome::Result::Refused);
    CHECK(!fs::exists(out) && o.files == 0 && o.folder.empty());
    CHECK(tight.files == kMaxFiles - 2 && tight.bytes == 0);
    CHECK(o.message.find("refused") == 0 && o.message.find("3 files") != std::string::npos);

    // exactly enough files: written
    tight.files = kMaxFiles - 3;
    o = WriteDump(out, "20261007-040001", images, "a: b\n", tight);
    CHECK(o.result == DumpOutcome::Result::Written && tight.files == kMaxFiles);
    // now the budget is full: even the smallest request is refused
    o = WriteDump(out, "20261007-040002", {}, "a: b\n", tight);
    CHECK(o.result == DumpOutcome::Result::Refused && !fs::exists(out / "20261007-040002"));

    // bytes: one byte short of the whole set is refused, nothing written
    OutputBudget bytesTight;
    const DumpPlan plan = MakePlan(images, "a: b\n");
    bytesTight.bytes = kMaxBytes - plan.bytes + 1;
    o = WriteDump(out, "20261007-040003", images, "a: b\n", bytesTight);
    CHECK(o.result == DumpOutcome::Result::Refused && !fs::exists(out / "20261007-040003") && bytesTight.files == 0);
    bytesTight.bytes = kMaxBytes - plan.bytes;
    o = WriteDump(out, "20261007-040004", images, "a: b\n", bytesTight);
    CHECK(o.result == DumpOutcome::Result::Written && bytesTight.bytes == kMaxBytes);

    // a failing disk operation (the root is a file) fails without a throw and without spending the budget
    std::ofstream(root / "rootfile") << "x";
    OutputBudget fresh;
    o = WriteDump(root / "rootfile", "20261007-040005", images, "a: b\n", fresh);
    CHECK(o.result == DumpOutcome::Result::Failed && fresh.files == 0 && fresh.bytes == 0 && o.message.find("failed") == 0);
}
} // namespace

int main()
{
    const fs::path root = fs::temp_directory_path() / "chara_probe_files_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);

    TestText();
    TestConvert();
    TestPlan();
    TestFolderNames(root);
    TestWriteDump(root);
    TestBudgetRefusal(root);

    fs::remove_all(root, ec);
    return TEST_RESULT("chara_probe_files");
}
