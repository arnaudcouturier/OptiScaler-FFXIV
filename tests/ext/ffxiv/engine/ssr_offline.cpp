// Screen-space reflections, offline lab: one captured frame's own buffers (view position, scene colour, camera table and the ENGINE's planar map),
// run through the product's reflection pass as it ships (the product shader, not the diagnostic one) on the WARP adapter, and the pass's three
// results written out for analysis: the map handed to the water draws (`out.bin`, B8G8R8A8), the raw term (`term.bin`, R16G16B16A16_FLOAT:
// premultiplied rgb and alpha) and the hit's distance along its ray (`guide.bin`, R16_FLOAT).
//
// It answers what the game would otherwise have to: how the traced colour compares with the engine's own colour where the engine has a hit (the
// calibration of the replace-not-add change), how much grain the filter takes off the term and what a different guide tolerance would do - on the
// real pictures of a capture and no game run. The inputs are the dump's own files (scene.bin, vposition.bin, stage-05-ssr-in.bin), the water draw's
// window and map from draws.csv and the camera table from cbdump.csv, exactly as ssr_frame loads them.
//
// Local only: it needs a capture folder (OPTISCALER_SSR_OFFLINE_DIR) and an output folder (OPTISCALER_SSR_OFFLINE_OUT).
#include "check.h"

#include "ext/ffxiv/engine/SsrPass.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::engine::ssr;
namespace ao = ext::ffxiv::engine::ao;

namespace
{
constexpr uint32_t kCameraRows = 0x160 / 16; // the g_CameraParameter rows the shader reads
constexpr uint32_t kProjectionRow = 18;      // m_ProjectionMatrix's first row (CameraRegisters.h)
constexpr uint32_t kInverseProjectionRow = 14;

// The exit codes the diagnostic shader writes in the red channel (ext/ffxiv/engine/shaders/Ssr.hlsl, SSR_MARCH_DIAG).
struct Code
{
    uint32_t value;
    const char* name;
};
constexpr Code kCodes[] {
    { 1, "outside the region (kept the engine's texel)" },
    { 2, "the source texel is outside the scene allocation" },
    { 3, "no geometry at the source texel (sky)" },
    { 4, "no camera plane in the table" },
    { 5, "a degenerate reflection vector" },
    { 6, "degenerate tuning or window" },
    { 10, "hit" },
    { 11, "the ray left the picture" },
    { 12, "the ray reached the distance limit" },
    { 13, "the ray ran its steps out" },
    { 14, "the crossing was thicker than the thickness" },
    { 15, "the hit's confidence was below the threshold" },
    { 16, "the hit was the ray grazing its own surface (self-graze rejection)" },
};

std::string ReadFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::vector<std::string> Split(const std::string& text, char separator)
{
    std::vector<std::string> out;
    std::string field;
    for (const char c : text)
    {
        if (c == separator)
        {
            out.push_back(field);
            field.clear();
        }
        else if (c != '\r')
            field += c;
    }
    out.push_back(field);
    return out;
}

std::string Trim(const std::string& text)
{
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool EndsWith(std::string_view text, std::string_view suffix)
{
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

uint32_t Number(std::string_view text)
{
    return uint32_t(std::strtoul(std::string(text).c_str(), nullptr, 10));
}

// ---- the dump's facts.txt ------------------------------------------------------------------------------------------------------------

struct Fact
{
    std::string name; // the file is <name>.bin
    std::string format;
    uint32_t width = 0, height = 0;
    uint64_t bytes = 0;
    uint32_t rowPitch = 0;
};

std::vector<Fact> LoadFacts(const std::string& path)
{
    std::vector<Fact> facts;
    for (const std::string& line : Split(ReadFile(path), '\n'))
    {
        if (line.empty() || line[0] == '#')
            continue;
        const std::vector<std::string> fields = Split(line, ',');
        if (fields.size() < 5 || fields[0] == "file")
            continue;
        Fact fact;
        fact.name = fields[0];
        if (EndsWith(fact.name, ".bin"))
            fact.name.resize(fact.name.size() - 4);
        fact.format = fields[1];
        const size_t x = fields[2].find('x');
        if (x == std::string::npos)
            continue;
        fact.width = Number(std::string_view(fields[2]).substr(0, x));
        fact.height = Number(std::string_view(fields[2]).substr(x + 1));
        fact.bytes = std::strtoull(fields[3].c_str(), nullptr, 10);
        fact.rowPitch = Number(fields[4]);
        facts.push_back(fact);
    }
    return facts;
}

const Fact* FindFact(const std::vector<Fact>& facts, const char* name)
{
    for (const Fact& fact : facts)
        if (fact.name == name)
            return &fact;
    return nullptr;
}

// A depth target a dump could carry: the formats the diagnostic writes (DumpPixelBytes covers 4 bytes for each of them).
const Fact* FindDepthFact(const std::vector<Fact>& facts)
{
    for (const Fact& fact : facts)
        if (fact.format == "D32_FLOAT" || fact.format == "D24_UNORM_S8_UINT" || fact.format == "R24_UNORM_X8_TYPELESS" ||
            fact.format == "R32_TYPELESS")
            return &fact;
    return nullptr;
}

// ---- draws.csv: the water draw's own window and planar map -----------------------------------------------------------

struct WaterRow
{
    uint32_t frame = 0;
    uint32_t ordinal = 0;
    std::string crc;
    uint32_t planarWidth = 0, planarHeight = 0;
    float windowX = 0.0f, windowY = 0.0f, windowWidth = 0.0f, windowHeight = 0.0f;
};

// The first draw of the capture whose shader binds the engine's planar reflection map under its own name: the draw this pass exists for.
bool FindWaterRow(const std::string& path, WaterRow& out)
{
    std::string header;
    for (const std::string& line : Split(ReadFile(path), '\n'))
    {
        if (line.empty() || line[0] == '#')
            continue;
        const std::vector<std::string> fields = Split(line, ',');
        if (fields[0] == "frame")
        {
            header.clear();
            for (size_t i = 0; i < fields.size(); ++i)
                header += fields[i] + ",";
            continue;
        }
        if (fields.size() < 16)
            continue;
        // srv: `slot:name:format:xypointer|...` - the map is the entry whose name is the shared table's row. The columns are the file's header's:
        bool water = false;
        for (const std::string& entry : Split(fields[10], '|'))
        {
            const std::vector<std::string> parts = Split(entry, ':');
            if (parts.size() < 4)
                continue;
            if (parts[1] == "g_SamplerReflectionMap.T" || parts[1] == "g_SamplerReflectionMap")
            {
                water = true;
                const size_t x = parts[3].find('x');
                if (x != std::string::npos)
                {
                    out.planarWidth = Number(std::string_view(parts[3]).substr(0, x));
                    out.planarHeight = Number(std::string_view(parts[3]).substr(x + 1));
                }
            }
        }
        if (!water || out.planarWidth < 16 || out.planarHeight < 16)
            continue;
        // viewport: `count:WxH+X+Y` for the first one (the draw's own), floored to whole texels as the seam does.
        const std::vector<std::string> viewports = Split(fields[14], '|');
        const std::vector<std::string> first = Split(viewports.front(), ':');
        if (first.size() < 2)
            continue;
        const std::string& text = first[1];
        const size_t x = text.find('x');
        const size_t plus = text.find('+', x == std::string::npos ? 0 : x);
        if (x == std::string::npos || plus == std::string::npos)
            continue;
        const size_t plus2 = text.find('+', plus + 1);
        if (plus2 == std::string::npos)
            continue;
        out.windowWidth = float(std::strtod(text.substr(0, x).c_str(), nullptr));
        out.windowHeight = float(std::strtod(text.substr(x + 1, plus - x - 1).c_str(), nullptr));
        out.windowX = std::floor(float(std::strtod(text.substr(plus + 1, plus2 - plus - 1).c_str(), nullptr)));
        out.windowY = std::floor(float(std::strtod(text.substr(plus2 + 1).c_str(), nullptr)));
        out.frame = Number(fields[0]);
        out.ordinal = Number(fields[1]);
        out.crc = fields[9];
        return true;
    }
    return false;
}

// ---- cbdump.csv: the camera table the frame's draws bound -------------------------------------------------------------

// One row of the camera table, as float4s: the shader reads the projection at register 18 and the view's rows at 0..2.
struct Camera
{
    float rows[kCameraRows][4] {};
    bool valid = false;
    std::string source; // which sample the table came from, for the test's own line
};

// The row-major 4x4 products the cb.csv fallback below needs.
struct Mat4
{
    float m[4][4] {};
};

Mat4 Multiply(const Mat4& a, const Mat4& b)
{
    Mat4 out;
    for (uint32_t r = 0; r < 4; ++r)
        for (uint32_t c = 0; c < 4; ++c)
            for (uint32_t k = 0; k < 4; ++k)
                out.m[r][c] += a.m[r][k] * b.m[k][c];
    return out;
}

bool Invert(const Mat4& in, Mat4& out)
{
    double a[4][8] {};
    for (uint32_t r = 0; r < 4; ++r)
        for (uint32_t c = 0; c < 4; ++c)
        {
            a[r][c] = double(in.m[r][c]);
            a[r][4 + c] = r == c ? 1.0 : 0.0;
        }
    for (uint32_t i = 0; i < 4; ++i)
    {
        uint32_t pivot = i;
        for (uint32_t r = i + 1; r < 4; ++r)
            if (std::fabs(a[r][i]) > std::fabs(a[pivot][i]))
                pivot = r;
        if (!(std::fabs(a[pivot][i]) > 1e-12))
            return false;
        if (pivot != i)
            for (uint32_t c = 0; c < 8; ++c)
                std::swap(a[i][c], a[pivot][c]);
        const double scale = a[i][i];
        for (uint32_t c = 0; c < 8; ++c)
            a[i][c] /= scale;
        for (uint32_t r = 0; r < 4; ++r)
        {
            if (r == i)
                continue;
            const double factor = a[r][i];
            if (factor == 0.0)
                continue;
            for (uint32_t c = 0; c < 8; ++c)
                a[r][c] -= factor * a[i][c];
        }
    }
    for (uint32_t r = 0; r < 4; ++r)
        for (uint32_t c = 0; c < 4; ++c)
            out.m[r][c] = float(a[r][4 + c]);
    return true;
}

// The camera table a frame's water draws bound. cbdump.csv carries it since the diagnostic samples it; a capture from before that carries
// cb.csv's view and view-projection instead, and the projection is then VP * inverse(V) - the matrix the engine itself composed the frame with,
// so a projection rebuilt this way projects exactly like the game's. The test's own replay check (project a point, unproject it back) rejects a
// wrong convention here instead of in a silent picture.
Camera LoadCamera(const std::string& folder, uint32_t frame)
{
    Camera camera;
    for (const std::string& line : Split(ReadFile(folder + "/cbdump.csv"), '\n'))
    {
        if (line.empty() || line[0] == '#')
            continue;
        const std::vector<std::string> fields = Split(line, ',');
        if (fields.size() < 8 || fields[0] == "frame" || fields[3] != "g_CameraParameter")
            continue;
        if (Number(fields[5]) < kCameraRows * 16)
            continue;
        const std::string& hex = Trim(fields[7]);
        if (hex.size() < size_t(kCameraRows) * 32)
            continue;
        // The column is the buffer's own bytes in hex, so the four bytes of a float read back in that order.
        for (uint32_t row = 0; row < kCameraRows; ++row)
            for (uint32_t component = 0; component < 4; ++component)
            {
                const size_t at = (size_t(row) * 4 + component) * 8;
                const uint32_t bits = uint32_t(std::strtoul(hex.substr(at, 2).c_str(), nullptr, 16)) |
                                      (uint32_t(std::strtoul(hex.substr(at + 2, 2).c_str(), nullptr, 16)) << 8) |
                                      (uint32_t(std::strtoul(hex.substr(at + 4, 2).c_str(), nullptr, 16)) << 16) |
                                      (uint32_t(std::strtoul(hex.substr(at + 6, 2).c_str(), nullptr, 16)) << 24);
                std::memcpy(&camera.rows[row][component], &bits, sizeof(float));
            }
        camera.valid = true;
        camera.source = "cbdump.csv, frame " + fields[0];
        if (Number(fields[0]) == frame)
            return camera; // the water draw's own frame: the table it was drawn with
    }
    if (camera.valid)
        return camera; // the latest sample the dump carries

    // cb.csv: frame,time_s,ps_crc,cb,slot,bytes,pos_x.. and then view_00..view_33, vp_00..vp_33.
    std::vector<std::string> header;
    std::vector<std::string> latest;
    double latestSeconds = -1.0;
    for (const std::string& line : Split(ReadFile(folder + "/cb.csv"), '\n'))
    {
        if (line.empty() || line[0] == '#')
            continue;
        const std::vector<std::string> fields = Split(line, ',');
        if (fields[0] == "frame")
        {
            header = fields;
            continue;
        }
        if (fields.size() < 40 || Number(fields[0]) == 0)
            continue; // frame 0 is before the capture: not a frame this dump belongs to
        const double seconds = std::strtod(fields[1].c_str(), nullptr);
        if (seconds < latestSeconds)
            continue;
        latestSeconds = seconds;
        latest = fields;
    }
    if (latest.empty())
        return camera;
    const auto value = [&header, &latest](const std::string& name) {
        for (size_t i = 0; i < header.size() && i < latest.size(); ++i)
            if (header[i] == name)
                return std::strtod(latest[i].c_str(), nullptr);
        return 0.0;
    };
    Mat4 view, viewProjection;
    for (uint32_t r = 0; r < 4; ++r)
        for (uint32_t c = 0; c < 4; ++c)
        {
            const std::string suffix = std::string(1, char('0' + r)) + char('0' + c);
            view.m[r][c] = float(value("view_" + suffix));
            viewProjection.m[r][c] = float(value("vp_" + suffix));
        }
    // cb.csv's view column is the engine's three-row view matrix followed by the inverse view's first row: its fourth row is (0,0,0,1).
    view.m[3][0] = view.m[3][1] = view.m[3][2] = 0.0f;
    view.m[3][3] = 1.0f;
    Mat4 inverseView;
    if (!Invert(view, inverseView))
        return camera;
    const Mat4 projection = Multiply(viewProjection, inverseView); // VP = P * V, replayed by the product below
    Mat4 inverseProjection;
    if (!Invert(projection, inverseProjection))
        return camera;
    for (uint32_t c = 0; c < 4; ++c)
        for (uint32_t i = 0; i < 4; ++i)
        {
            camera.rows[i][c] = view.m[i][c];
            camera.rows[kInverseProjectionRow + i][c] = inverseProjection.m[i][c];
            camera.rows[kProjectionRow + i][c] = projection.m[i][c];
        }
    camera.valid = true;
    camera.source = "cb.csv, time " + latest[1] + " s (projection rebuilt as view-projection * inverse view)";
    return camera;
}

// ---- matrices and the unprojection the depth path and the reference frame share ---------------------------------------

Mat4 RowsOf(const Camera& camera, uint32_t first)
{
    Mat4 out;
    for (uint32_t r = 0; r < 4; ++r)
        for (uint32_t c = 0; c < 4; ++c)
            out.m[r][c] = camera.rows[first + r][c];
    return out;
}

// The view-space point of a screen position and a depth, through the inverse projection: `clip = invP * (ndc, depth, 1)`, divided by its w. A
// projection in this engine's layout maps a view point to the clip point whose z/w is the stored depth, so this is its inverse for every
// convention (a reversed z included) and needs no sign or range of its own.
bool Unproject(const Mat4& inverseProjection, float ndcX, float ndcY, float depth, float out[3])
{
    const float in[4] { ndcX, ndcY, depth, 1.0f };
    float clip[4] {};
    for (uint32_t r = 0; r < 4; ++r)
        for (uint32_t c = 0; c < 4; ++c)
            clip[r] += inverseProjection.m[r][c] * in[c];
    if (!(std::fabs(clip[3]) > 1e-20f))
        return false;
    for (uint32_t i = 0; i < 3; ++i)
        out[i] = clip[i] / clip[3];
    return true;
}

bool Project(const Mat4& projection, const float point[3], float& ndcX, float& ndcY, float& depth)
{
    const float in[4] { point[0], point[1], point[2], 1.0f };
    float clip[4] {};
    for (uint32_t r = 0; r < 4; ++r)
        for (uint32_t c = 0; c < 4; ++c)
            clip[r] += projection.m[r][c] * in[c];
    if (!(clip[3] > 1e-12f))
        return false;
    ndcX = clip[0] / clip[3];
    ndcY = clip[1] / clip[3];
    depth = clip[2] / clip[3];
    return true;
}

// ---- the frame the pass runs on --------------------------------------------------------------------------------------

struct Frame
{
    uint32_t sceneWidth = 0, sceneHeight = 0;
    uint32_t planarWidth = 0, planarHeight = 0;
    std::vector<uint16_t> position; // RGBA half, at the scene allocation's size
    std::vector<uint16_t> colour;   // RGBA half, at the scene allocation's size
    ReaderFacts facts;
};

uint16_t Half(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t exponent = int32_t((bits >> 23) & 0xFFu) - 127 + 15;
    const uint32_t mantissa = bits & 0x7FFFFFu;
    if (exponent <= 0)
        return uint16_t(sign); // the fixture's values are ordinary: a denormal reads as zero
    if (exponent >= 31)
        return uint16_t(sign | 0x7C00u);
    return uint16_t(sign | (uint32_t(exponent) << 10) | (mantissa >> 13));
}

void SetTexel(std::vector<uint16_t>& target, uint32_t width, uint32_t x, uint32_t y, float r, float g, float b, float a)
{
    const size_t at = (size_t(y) * width + x) * 4;
    target[at + 0] = Half(r);
    target[at + 1] = Half(g);
    target[at + 2] = Half(b);
    target[at + 3] = Half(a);
}

// The window's texels in the scene allocation, as the seam fills them: the viewport's fractional origin floored to whole texels.
void FillFacts(Frame& frame, const WaterRow& row)
{
    frame.facts.hasPlanar = true;
    frame.facts.planarFormat = kPlanarFormat;
    frame.facts.planarWidth = row.planarWidth;
    frame.facts.planarHeight = row.planarHeight;
    frame.facts.planarSamples = 1;
    frame.facts.hasScene = true;
    frame.facts.sceneFormat = kSceneFormat;
    frame.facts.sceneWidth = frame.sceneWidth;
    frame.facts.sceneHeight = frame.sceneHeight;
    frame.facts.sceneSamples = 1;
    frame.facts.hasVPosition = true;
    frame.facts.vpositionFormat = kVPositionFormat;
    frame.facts.vpositionWidth = frame.sceneWidth;
    frame.facts.vpositionHeight = frame.sceneHeight;
    frame.facts.vpositionSamples = 1;
    frame.facts.hasCamera = true;
    frame.facts.cameraBytes = kCameraBytes;
    frame.facts.window[0] = row.windowX;
    frame.facts.window[1] = row.windowY;
    frame.facts.window[2] = row.windowWidth;
    frame.facts.window[3] = row.windowHeight;
}

// The reference frame: the same camera table, and geometry built in view space from it through the inverse projection, so every texel's
// position is the exact point its own pixel projects to (a march over an inconsistent buffer would answer nothing about the march). A floor
// `kFloorDistance` below the camera along the view-space up, a wall `kWallDistance` ahead, far for the rest.
Frame BuildReferenceFrame(const WaterRow& row, const Camera& camera, uint32_t sceneWidth, uint32_t sceneHeight)
{
    Frame frame;
    frame.sceneWidth = sceneWidth;
    frame.sceneHeight = sceneHeight;
    frame.planarWidth = row.planarWidth;
    frame.planarHeight = row.planarHeight;
    frame.position.assign(size_t(sceneWidth) * sceneHeight * 4, 0);
    frame.colour.assign(size_t(sceneWidth) * sceneHeight * 4, 0);
    FillFacts(frame, row);

    const Mat4 inverseProjection = RowsOf(camera, kInverseProjectionRow);
    // The view-space up is the view matrix's own second column, the normal the pass reflects about (Ssr.hlsl rule 2).
    float up[3] { camera.rows[0][1], camera.rows[1][1], camera.rows[2][1] };
    const float upLength = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    if (!(upLength > 1e-6f))
        up[0] = 0.0f, up[1] = 1.0f, up[2] = 0.0f;
    else
        for (float& c : up)
            c /= upLength;
    constexpr float kFloorBelow = 1.5f;  // metres below the camera along the view-space up
    constexpr float kWallAhead = 12.0f;  // metres ahead of the camera along the view
    for (uint32_t y = 0; y < sceneHeight; ++y)
        for (uint32_t x = 0; x < sceneWidth; ++x)
        {
            const float ndcX = ((float(x) + 0.5f) - row.windowX) / row.windowWidth * 2.0f - 1.0f;
            const float ndcY = 1.0f - ((float(y) + 0.5f) - row.windowY) / row.windowHeight * 2.0f;
            float nearPoint[3] {};
            if (!Unproject(inverseProjection, ndcX, ndcY, 1.0f, nearPoint))
                continue;
            const float length = std::sqrt(nearPoint[0] * nearPoint[0] + nearPoint[1] * nearPoint[1] + nearPoint[2] * nearPoint[2]);
            if (!(length > 1e-6f))
                continue;
            const float direction[3] { nearPoint[0] / length, nearPoint[1] / length, nearPoint[2] / length };
            const float alongNormal = direction[0] * up[0] + direction[1] * up[1] + direction[2] * up[2];
            float distance = 0.0f;
            if (alongNormal < -1e-4f)
                distance = -kFloorBelow / alongNormal; // the floor under the camera
            else if (direction[2] < -1e-4f)
                distance = -kWallAhead / direction[2]; // the wall in front of it
            else
                // No texel of this reference is sky: every direction the picture contains points into the wall's plane (the camera looks down at
                // the floor), so the two branches above cover the picture and this one only keeps the reference total. A reference with sky is the
                // one thing the real frame needs and this closed box cannot produce; the distance is the far plane that branch would have used.
                distance = 60.0f;
            const float point[3] { direction[0] * distance, direction[1] * distance, direction[2] * distance };
            SetTexel(frame.position, sceneWidth, x, y, point[0], point[1], point[2], 1.0f);
            // The scene's colour: the floor dark, the wall brighter with height, so a hit is a colour the engine's does not have.
            const float shade = std::min(1.0f, 0.15f + 0.5f * std::fabs(point[2]) / 60.0f + (alongNormal < -1e-4f ? 0.0f : 0.2f));
            SetTexel(frame.colour, sceneWidth, x, y, shade, shade * 0.5f, 1.0f - shade, 1.0f);
        }
    return frame;
}

// The frame a dump carries: vposition.bin for the position, scene.bin for the colour, both at the scene's size.
bool LoadDumpedFrame(const std::string& folder, const WaterRow& row, const Fact& scene, const Fact& position, Frame& frame)
{
    frame.sceneWidth = scene.width;
    frame.sceneHeight = scene.height;
    frame.planarWidth = row.planarWidth;
    frame.planarHeight = row.planarHeight;
    const std::string sceneBytes = ReadFile(folder + "/dump/scene.bin");
    const std::string positionBytes = ReadFile(folder + "/dump/" + position.name + ".bin");
    const size_t expected = size_t(scene.width) * scene.height * 8;
    if (sceneBytes.size() < expected || positionBytes.size() < expected)
        return false;
    frame.colour.assign(reinterpret_cast<const uint16_t*>(sceneBytes.data()),
                        reinterpret_cast<const uint16_t*>(sceneBytes.data()) + expected / 2);
    frame.position.assign(reinterpret_cast<const uint16_t*>(positionBytes.data()),
                          reinterpret_cast<const uint16_t*>(positionBytes.data()) + expected / 2);
    FillFacts(frame, row);
    return true;
}

// The frame a dump without the view position can still answer: the depth through the camera table's inverse projection. A D24S8 target's
// depth is its low 24 bits (the dump writes the resource's four bytes a texel).
bool LoadDepthFrame(const std::string& folder, const WaterRow& row, const Fact& scene, const Fact& depth, const Camera& camera, Frame& frame)
{
    frame.sceneWidth = scene.width;
    frame.sceneHeight = scene.height;
    frame.planarWidth = row.planarWidth;
    frame.planarHeight = row.planarHeight;
    const std::string sceneBytes = ReadFile(folder + "/dump/scene.bin");
    const std::string depthBytes = ReadFile(folder + "/dump/" + depth.name + ".bin");
    const size_t expected = size_t(scene.width) * scene.height * 8;
    if (sceneBytes.size() < expected || depthBytes.size() < size_t(scene.width) * scene.height * 4)
        return false;
    frame.colour.assign(reinterpret_cast<const uint16_t*>(sceneBytes.data()),
                        reinterpret_cast<const uint16_t*>(sceneBytes.data()) + expected / 2);
    frame.position.assign(expected / 2, 0);
    const Mat4 inverseProjection = RowsOf(camera, kInverseProjectionRow);
    for (uint32_t y = 0; y < scene.height; ++y)
        for (uint32_t x = 0; x < scene.width; ++x)
        {
            const size_t at = (size_t(y) * scene.width + x) * 4;
            const uint32_t bits = uint32_t(uint8_t(depthBytes[at])) | (uint32_t(uint8_t(depthBytes[at + 1])) << 8) |
                                  (uint32_t(uint8_t(depthBytes[at + 2])) << 16) | (uint32_t(uint8_t(depthBytes[at + 3])) << 24);
            float stored = 0.0f;
            if (depth.format == "D32_FLOAT")
                std::memcpy(&stored, &bits, sizeof stored);
            else
                stored = float(bits & 0xFFFFFFu) / float(0xFFFFFFu);
            const float ndcX = ((float(x) + 0.5f) - row.windowX) / row.windowWidth * 2.0f - 1.0f;
            const float ndcY = 1.0f - ((float(y) + 0.5f) - row.windowY) / row.windowHeight * 2.0f;
            float point[3] {};
            if (stored <= 0.0f || !Unproject(inverseProjection, ndcX, ndcY, stored, point))
                continue;
            SetTexel(frame.position, scene.width, x, y, point[0], point[1], point[2], 1.0f);
        }
    FillFacts(frame, row);
    return true;
}

// ---- running the product's pass --------------------------------------------------------------------------------------

bool Run(ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context, Frame& frame, const Camera& camera, const std::vector<uint8_t>& planarBytes,
         const std::string& outFolder)
{
    const auto texture = [&device](uint32_t format, uint32_t width, uint32_t height, const void* pixels, uint32_t bindFlags) {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT(format);
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = bindFlags;
        D3D11_SUBRESOURCE_DATA data {};
        data.pSysMem = pixels;
        data.SysMemPitch = width * 8;
        ComPtr<ID3D11Texture2D> out;
        if (FAILED(device->CreateTexture2D(&desc, &data, &out)))
            return ComPtr<ID3D11Texture2D>();
        return out;
    };
    ComPtr<ID3D11Texture2D> position = texture(uint32_t(DXGI_FORMAT_R16G16B16A16_FLOAT), frame.sceneWidth, frame.sceneHeight,
                                              frame.position.data(), D3D11_BIND_SHADER_RESOURCE);
    ComPtr<ID3D11Texture2D> scene = texture(uint32_t(DXGI_FORMAT_R16G16B16A16_FLOAT), frame.sceneWidth, frame.sceneHeight, frame.colour.data(),
                                            D3D11_BIND_SHADER_RESOURCE);
    D3D11_TEXTURE2D_DESC planarDesc {};
    planarDesc.Width = frame.planarWidth;
    planarDesc.Height = frame.planarHeight;
    planarDesc.MipLevels = 1;
    planarDesc.ArraySize = 1;
    planarDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    planarDesc.SampleDesc.Count = 1;
    planarDesc.Usage = D3D11_USAGE_DEFAULT;
    planarDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA planarData {};
    planarData.pSysMem = planarBytes.data();
    planarData.SysMemPitch = frame.planarWidth * 4;
    ComPtr<ID3D11Texture2D> planar;
    if (FAILED(device->CreateTexture2D(&planarDesc, &planarData, &planar)))
        return false;
    ComPtr<ID3D11ShaderResourceView> positionView, sceneView, planarView;
    if (!position || !scene || FAILED(device->CreateShaderResourceView(position.Get(), nullptr, &positionView)) ||
        FAILED(device->CreateShaderResourceView(scene.Get(), nullptr, &sceneView)) ||
        FAILED(device->CreateShaderResourceView(planar.Get(), nullptr, &planarView)))
        return false;
    D3D11_BUFFER_DESC cameraDesc {};
    cameraDesc.ByteWidth = kCameraBytes;
    cameraDesc.Usage = D3D11_USAGE_DEFAULT;
    cameraDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA cameraData {};
    cameraData.pSysMem = camera.rows;
    ComPtr<ID3D11Buffer> cameraBuffer;
    if (FAILED(device->CreateBuffer(&cameraDesc, &cameraData, &cameraBuffer)))
        return false;

    PassInputs inputs;
    inputs.planar = planarView.Get();
    inputs.planarTexture = planar.Get();
    inputs.vposition = positionView.Get();
    inputs.scene = scene.Get();
    inputs.camera = cameraBuffer.Get();
    inputs.facts = frame.facts;
    inputs.params.on = true;
    inputs.params.steps = kStepsDefault;
    inputs.params.thickness = kThicknessDefault;
    inputs.params.blend = kBlendDefault;
    inputs.params.fade = kFadeDefault;

    Pass pass;
    if (pass.Run(context.Get(), inputs) == nullptr)
    {
        std::printf("    the pass refused the frame: %s\n", pass.Failure() != nullptr ? pass.Failure() : "no reason");
        return false;
    }

    // Reads a texture back whole and writes its bytes, tightly packed, to <outFolder>/<name>.bin.
    const auto dump = [&](ID3D11Texture2D* source, const char* name, uint32_t bytesPerTexel) {
        D3D11_TEXTURE2D_DESC desc {};
        source->GetDesc(&desc);
        D3D11_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.BindFlags = 0;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging)))
            return false;
        context->CopyResource(staging.Get(), source);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return false;
        std::ofstream file(outFolder + "/" + name + ".bin", std::ios::binary);
        for (uint32_t y = 0; y < desc.Height; ++y)
            file.write(static_cast<const char*>(mapped.pData) + size_t(y) * mapped.RowPitch, std::streamsize(desc.Width) * bytesPerTexel);
        context->Unmap(staging.Get(), 0);
        std::printf("    wrote %s.bin (%ux%u, %u bytes a texel)\n", name, desc.Width, desc.Height, bytesPerTexel);
        return bool(file);
    };
    return dump(pass.OutputTexture(), "out", 4) && dump(pass.TermTexture(), "term", 8) && dump(pass.GuideTexture(), "guide", 2);
}
} // namespace

int main(int argc, char** argv)
{
    CHECK(argc >= 3);
    const std::string folder = argv[1];
    const std::string outFolder = argv[2];
    std::printf("ssr_offline: %s -> %s\n", folder.c_str(), outFolder.c_str());

    const std::vector<Fact> facts = LoadFacts(folder + "/dump/facts.txt");
    CHECK(!facts.empty());

    WaterRow row;
    CHECK(FindWaterRow(folder + "/draws.csv", row));
    std::printf("    the water draw: frame %u, ordinal %u, shader %s, map %ux%u, window %.0fx%.0f at %.0f,%.0f\n", row.frame, row.ordinal,
                row.crc.c_str(), row.planarWidth, row.planarHeight, double(row.windowWidth), double(row.windowHeight), double(row.windowX),
                double(row.windowY));
    const Camera camera = LoadCamera(folder, row.frame);
    CHECK(camera.valid);
    std::printf("    the camera table: %s\n", camera.source.c_str());

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    CHECK(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)));
    CHECK(device != nullptr && context != nullptr);

    const Fact* scene = FindFact(facts, "scene");
    const Fact* position = FindFact(facts, "vposition");
    const Fact* engineMap = FindFact(facts, "stage-05-ssr-in");
    CHECK(scene != nullptr && position != nullptr && engineMap != nullptr);
    if (scene == nullptr || position == nullptr || engineMap == nullptr)
        return Finish("ssr_offline");
    CHECK(engineMap->width == row.planarWidth && engineMap->height == row.planarHeight);

    Frame frame;
    CHECK(LoadDumpedFrame(folder, row, *scene, *position, frame));
    // OPTISCALER_SSR_OFFLINE_SCENE=ab: the scene is the A/B's `ssr-ab-before` picture - the draw's render target at the first reader draw, which is
    // exactly what the pass copies its scene from - instead of scene.bin, the tone map's input (after the water, the fog and the rest of the chain).
    // The picture is the window's crop; it is placed at the window's origin in a cleared allocation (the pass only reads the window).
    const char* sceneSource = std::getenv("OPTISCALER_SSR_OFFLINE_SCENE");
    if (sceneSource != nullptr && std::string(sceneSource) == "ab")
    {
        const Fact* before = FindFact(facts, "ssr-ab-before");
        CHECK(before != nullptr && before->format == "R16G16B16A16_FLOAT");
        if (before != nullptr)
        {
            const std::string bytes = ReadFile(folder + "/dump/ssr-ab-before.bin");
            CHECK(bytes.size() >= size_t(before->width) * before->height * 8);
            CHECK(uint32_t(row.windowX) + before->width <= frame.sceneWidth && uint32_t(row.windowY) + before->height <= frame.sceneHeight);
            std::fill(frame.colour.begin(), frame.colour.end(), uint16_t(0));
            for (uint32_t y = 0; y < before->height; ++y)
                std::memcpy(&frame.colour[(size_t(uint32_t(row.windowY) + y) * frame.sceneWidth + uint32_t(row.windowX)) * 4],
                            bytes.data() + size_t(y) * before->width * 8, size_t(before->width) * 8);
            std::printf("    the scene is ssr-ab-before (%ux%u at %.0f,%.0f): the target at the first reader draw\n", before->width, before->height,
                        double(row.windowX), double(row.windowY));
        }
    }
    const std::string planarText = ReadFile(folder + "/dump/stage-05-ssr-in.bin");
    CHECK(planarText.size() >= size_t(row.planarWidth) * row.planarHeight * 4);
    std::vector<uint8_t> planarBytes(planarText.begin(), planarText.end());
    // OPTISCALER_SSR_OFFLINE_NO_ENGINE=1: an engine map with no hit anywhere, so the pass writes its filtered term at EVERY texel (the lab then reads
    // the term where the engine has hits, which the real pass leaves to the engine).
    const char* noEngine = std::getenv("OPTISCALER_SSR_OFFLINE_NO_ENGINE");
    if (noEngine != nullptr && noEngine[0] == '1')
    {
        std::fill(planarBytes.begin(), planarBytes.end(), uint8_t(0));
        std::printf("    the engine's map is replaced by an empty one (no engine hit anywhere)\n");
    }
    CHECK(Run(device, context, frame, camera, planarBytes, outFolder));
    return Finish("ssr_offline");
}
