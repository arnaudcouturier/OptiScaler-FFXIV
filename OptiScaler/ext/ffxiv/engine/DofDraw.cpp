#include "pch.h"

#include "ext/ffxiv/engine/Dof.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/chara/CharaDispatch.h" // the character preview's replay marker
#include "ext/ffxiv/engine/ChainHooks.h"   // the chain's stage hooks, which this module registers
#include "ext/ffxiv/engine/EnginePass.h"   // OwnWork: this module's own draws are never re-classified
#include "ext/ffxiv/engine/EnginePassRules.h"
#include "ext/ffxiv/engine/DofCamera.h"
#include "ext/ffxiv/engine/DofPass.h"
#include "ext/ffxiv/engine/RecogRules.h" // FrameStep: one count per rendered frame, the step line, the pixel-to-NDC mapping
#include "ext/hdr/scene/FfxivSceneHdr.h" // the scene HDR path replaces the tone chain this stage draws before
#include "ext/present/PresentSeams.h"    // the output-frame counter, the scene-HDR sample's token
#include "ext/ffxiv/diag/ShaderBindings.h" // the DXBC reflection reader: the stamp rule of the engine's own depth of field
#include "ext/ffxiv/diag/StageDump.h" // the per-stage image dump: this stage reports its run and its pictures on the capture frame

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>
#include <thread>

// Depth of field, Route B: the draw path, the frame's captures and the module's session (Dof.h). The pure decisions are
// in DofRules.h, the frame's captures in DofCamera.cpp, the three draws in DofPass.cpp, the shaders in shaders/Dof.hlsl,
// the card in DofMenu.cpp.
//
// What it costs while it is off: the engine module's one call per draw into OnSeamDraw, which returns at two bit tests -
// no state query, no allocation and no log line. While it is on, per frame: the camera is copied at the frame's first
// light draw, the depth (and the viewport buffer's row, for one log line) at the fog post's draw, the scene at the composite, and at the tone map
// the stage packs, blurs and merges the chain's current output and the chain binds this module's copy of the scene at t0
// for that one draw (the owner role) or takes this module's output as its own (the chained role).
//
// Nothing of the game is written: the scene is copied into an owned texture, and the depth is read through a view the
// fog post itself bound. The tone map's own binding is put back after its one draw, also when that draw throws.
//
// The frame's count opens at the frame's first composite after the previous frame's tone map (the game draws the composite
// twice a frame), which also resets the camera copy's taken flag: the composite is the one draw a rendered frame reliably
// makes before its tone map, and every capture this stage makes stamps the count it was made in. The captures' rings are this module's own - never the march's, never the radial's, never the puddle
// stage's stamp (the stamping contract: this module's answered-tracking is its own rendered-frame counter, dofFrame).
namespace ext::ffxiv::dof
{
namespace
{
using Microsoft::WRL::ComPtr;
using ext::ffxiv::engine::Knob;
using ext::ffxiv::engine::Session;
using ext::ffxiv::engine::kConsecutiveDeclineLimit;

// D3D11's own maximum of render targets, the march's own measurement bound.
inline constexpr uint32_t kMaxRenderTargets = 8;

// The camera bytes the light draws bind: the march's own rule, stated at its capture (the largest bound buffer in 0x160
// to 0x400 bytes that is not the light's). This module copies the buffer's 0x160-byte window the march's rules name.
inline constexpr uint32_t kMaxCameraBufferBytes = 0x400;

// The light draws the camera is hunted among, bounded like the march's own capture: the camera copy is done after the
// first light draw of the frame that carries one.
inline constexpr uint32_t kMaxCaptureAttempts = 24;

// A read that has not landed for this long is not a GPU that is behind any more: one line says so (the march's own
// staleness rule and its own bound).
inline constexpr uint64_t kMaxReadbackAge = 90;
inline constexpr uint32_t kReadbackStallFrames = 64;

// The tone map's own parameter buffer (`cCommonTexParam`) sits in its pixel shader's first constant slot, the march's own
// slot for the same buffer - the facts the tone map's shape answers from are the shafts' rules'.
inline constexpr uint32_t kToneMapSlot = 0;

// The 32-byte candidates the fog post binds beside the fog constants: the pixel-to-NDC row this stage's pack walks (the
// march's own FogReader stores the same buffer as its third piece; this module has its own ring).
inline constexpr uint32_t kViewportParamBytes = 0x20;

// ---- the engine's own depth of field ---------------------------------------------------------------------------------------------------
// The game's classic depth of field (Weight2 and its siblings) writes its result back into the scene texture the tone map reads, so this stage would
// blur a blurred picture. The pass that makes the picture is the weight pass: a pixel shader that declares the family's parameter buffer AND reads
// an sInput (DofRules.h IsEngineDofLayout; the 256x1 CoC LUT update declares the buffer alone and never counts). Its objects are stamped at shader
// creation (the lens flare card's pattern: the anchors word is full), the bind seam reads the stamp, and the seam notes a stamped draw that draws to a
// scene-sized target as "the engine's depth of field ran in this frame" (the stage's own frame counter, so the flag needs no reset).
constexpr GUID kWeightStampTag { 0x7a2e4c19, 0x35d8, 0x4b60, { 0x8e, 0x17, 0xc4, 0x5d, 0x09, 0x6b, 0xf3, 0xa2 } };
inline constexpr uint32_t kWeightSeenSlots = 256;
inline constexpr uint32_t kWeightMaxShaders = 32;
static_assert((kWeightSeenSlots & (kWeightSeenSlots - 1)) == 0, "the slot count is a power of two, so the hash's low bits are the slot");
static_assert(kWeightSeenSlots >= 2 * kWeightMaxShaders, "the stamp index leaves the table at most half full at the cap");

std::atomic<engine::SeenTable<kWeightSeenSlots>*> g_weightTable { nullptr };

engine::SeenTable<kWeightSeenSlots>* WeightTableIfAny() noexcept { return g_weightTable.load(std::memory_order_acquire); }

engine::SeenTable<kWeightSeenSlots>& WeightTableForInsert()
{
    engine::SeenTable<kWeightSeenSlots>* table = g_weightTable.load(std::memory_order_acquire);
    if (table == nullptr)
    {
        table = new engine::SeenTable<kWeightSeenSlots>; // never destroyed: no destructor may run under the loader lock
        g_weightTable.store(table, std::memory_order_release);
    }
    return *table;
}

struct WeightStamps
{
    std::mutex mutex;      // one stamp and one table insert per shader object, whatever loader thread the creation runs on
    uint32_t assigned = 0; // the stamps handed out
    bool fullLogged = false;
};

WeightStamps& Stamps()
{
    static WeightStamps* stamps = new WeightStamps; // never destroyed: no destructor may run under the loader lock
    return *stamps;
}

// The stamp the shader object itself carries, 0 when it has none (a find asks it only on a pointer hit).
uint32_t StampedWeightOf(const void* shader)
{
    if (shader == nullptr)
        return 0;
    UINT bytes = sizeof(uint32_t);
    uint32_t stamped = 0;
    if (FAILED(static_cast<ID3D11PixelShader*>(const_cast<void*>(shader))->GetPrivateData(kWeightStampTag, &bytes, &stamped)))
        return 0;
    return stamped;
}

// The clock the smoothing runs on: time since the pass last RAN, not a frame count.
int64_t Qpc()
{
    LARGE_INTEGER value {};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

int64_t QpcFrequency()
{
    static const int64_t frequency = [] {
        LARGE_INTEGER value {};
        QueryPerformanceFrequency(&value);
        return value.QuadPart > 0 ? value.QuadPart : int64_t(1);
    }();
    return frequency;
}

// One accepted camera: the last usable read, held across gaps (the pack's reconstruction never needs more than a plane
// pair, and one does not move inside a session). A camera cut changes the picture, not the projection, so there is no
// cut to drop on - the hold carries the pack's metres whatever the view does.
struct CameraHold
{
    bool have = false;
    float inverseProjection[16] {};
    DepthBand band;

    void Accept(const CameraRead& cameraRead)
    {
        if (cameraRead.landed && cameraRead.usable)
        {
            std::memcpy(inverseProjection, cameraRead.inverseProjection, sizeof inverseProjection);
            band = cameraRead.band;
            have = true;
        }
    }

    void Drop()
    {
        have = false;
        band = DepthBand {};
    }
};

struct State
{
    Pass pass;
    Session session;
    engine::LogGate log;
    Params params;
    bool enabled = false;
    CameraReader camera;  // the camera buffer's staging ring, copied at the frame's first light draw
    RowReader row;        // the fog post's pixel-to-NDC row, its own ring
    DepthKeep depth;      // the scene depth, as the fog post binds it
    bool haveRenderThread = false;
    std::thread::id renderThread;

    // The frame's rendered count, opened at the frame's first composite after the previous frame closed at its tone map (the game
    // draws the composite twice a frame, so the count must not move at every composite draw): the stage's own counter, never the
    // chain's shared stamp.
    uint64_t frames = 0;
    shafts::FrameStep step;
    shafts::ChangeLine stepLine; // the step line's own once-per-state memory
    // The ages the last read-back took at the tone map, in counts (kRingNotLanded when nothing landed): what the step line names.
    uint32_t cameraAge = shafts::kRingNotLanded;
    uint32_t rowAge = shafts::kRingNotLanded;
    // The frame's lit scene: the deferred composite's single render target, which the tone map then reads as its t0.
    void* sceneResource = nullptr;
    uint32_t sceneWidth = 0, sceneHeight = 0, sceneFormat = 0;
    uint64_t sceneFrames = 0;
    // The stage answers at most one tone map a frame, by its own counter: a second tone-shaped draw of the frame is the
    // engine's own post-tone copy, and the game's own call (the march's own rule, this module's own counter).
    uint64_t dofFrame = 0;
    bool haveDofFrame = false;
    // The camera and the row the stage draws with: the newest usable read each, held across gaps.
    CameraHold hold;
    bool cameraTaken = false; // the frame's camera copied (one object is one copy a frame)
    // What the card shows and what the log says.
    uint32_t attempts = 0;
    uint32_t runs = 0;
    uint32_t declined = 0;
    SkipCounts skips;
    uint32_t refusals = 0; // the camera refusals (a build mismatch), counted toward the stand-down
    bool lastReadCameraRefused = false;
    uint64_t nextCountsLog = 0;
    bool loggedOff = false;
    bool loggedCost = false;
    bool loggedCameraRefused = false;
    bool loggedStall = false;
    bool loggedStale = false;
    bool loggedDepth = false;
    bool loggedRow = false;
    bool loggedViewportRow = false;
    shafts::SceneHdrWatch hdr;
    // The engine's own depth of field: the bind seam's value (non-zero while the pixel shader the game bound last is one of the weight passes) and the
    // stage's own frame counter value of the last scene-sized weight draw.
    std::atomic<uint32_t> boundWeightStamp { 0 };
    uint64_t engineDofFrame = 0;
    bool haveEngineDofFrame = false;
    // The smoothing's clock: the QPC value of the last run of the pass (0 = it never ran), and the numbers the capture frame names (read by the probe's
    // formatter when the dump is written).
    int64_t lastRunQpc = 0;
    Measured measured;
    uint64_t hdrSampleFrame = 0;
    bool haveHdrSample = false;
    uint32_t lastWindowWidth = 0, lastWindowHeight = 0;
    uint32_t lastQuarterWidth = 0, lastQuarterHeight = 0;
};

State& S()
{
    static State* state = new State; // never destroyed: no destructor may run under the loader lock
    return *state;
}

// The frame's composite: the one draw a rendered frame reliably makes before its tone map, and the stage's frame
// boundary (the march's own RecordScene, this module's own recorded facts).
void RecordScene(ID3D11DeviceContext* context, State& state)
{
    // The count opens once a frame, at the first composite after the previous frame's tone map: the captured frame draws the composite
    // twice in a row, and a count that moved at both draws stepped by two a frame, which pushed the previous frame's camera copy out of
    // the ring's read window. The camera copy's taken flag and the capture budget are tied to the real advance.
    if (state.step.OpenAtComposite(state.frames))
    {
        state.attempts = 0;
        state.cameraTaken = false;
    }
    ID3D11RenderTargetView* views[2] { nullptr, nullptr };
    context->OMGetRenderTargets(2, views, nullptr);
    if (views[0] != nullptr && views[1] == nullptr)
    {
        ComPtr<ID3D11Resource> resource;
        views[0]->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (resource != nullptr && SUCCEEDED(resource.As(&texture)) && texture != nullptr)
        {
            D3D11_TEXTURE2D_DESC desc {};
            texture->GetDesc(&desc);
            D3D11_RENDER_TARGET_VIEW_DESC viewDesc {};
            views[0]->GetDesc(&viewDesc);
            if (desc.ArraySize == 1 && desc.SampleDesc.Count == 1 && shafts::IsSceneFormat(uint32_t(viewDesc.Format)))
            {
                state.sceneResource = texture.Get();
                state.sceneWidth = desc.Width;
                state.sceneHeight = desc.Height;
                state.sceneFormat = uint32_t(viewDesc.Format);
                state.sceneFrames = state.frames;
            }
        }
    }
    for (ID3D11RenderTargetView*& view : views)
        if (view != nullptr)
        {
            view->Release();
            view = nullptr;
        }
}

// The light draws: the camera buffer the CoC's metres reconstruct through, one copy per frame (the march's own shape
// capture: the largest bound buffer in 0x160..0x400 bytes that is not the light's). The light draws also carry the four
// cascade instances and the atlas, which the march's own capture reads at the same anchors; this module copies only its
// own window of their bindings.
void CaptureCameraAtLightDraw(ID3D11DeviceContext* context, engine::Anchors anchors, State& state)
{
    if (state.cameraTaken || state.attempts >= kMaxCaptureAttempts)
        return;
    ++state.attempts;
    ID3D11Buffer* buffers[ext::ffxiv::engine::kConstantBufferSlots] {};
    context->PSGetConstantBuffers(0, ext::ffxiv::engine::kConstantBufferSlots, buffers);
    ComPtr<ID3D11Buffer> bound[ext::ffxiv::engine::kConstantBufferSlots];
    for (uint32_t i = 0; i < ext::ffxiv::engine::kConstantBufferSlots; ++i)
        if (buffers[i] != nullptr)
            bound[i].Attach(buffers[i]);

    const uint32_t lightSlot = ext::ffxiv::engine::ShaftsSlotOf(anchors);
    if (lightSlot >= ext::ffxiv::engine::kConstantBufferSlots)
        return;

    // The camera: the largest bound buffer that is neither the light's own nor smaller than the window this module's
    // decode readsw. The march's own rule, its own instance (the light slot's buffer is the one under the anchors' slot;
    // the cascades' 128-byte buffers and the shafts' other bindings sit below the floor).
    ID3D11Buffer* camera = nullptr;
    uint32_t cameraBytes = shafts::kCameraBytes - 1;
    for (const ComPtr<ID3D11Buffer>& buffer : bound)
    {
        if (!buffer || buffer.Get() == bound[lightSlot].Get())
            continue;
        D3D11_BUFFER_DESC desc {};
        buffer->GetDesc(&desc);
        if (desc.ByteWidth < shafts::kCameraBytes || desc.ByteWidth > kMaxCameraBufferBytes || desc.ByteWidth <= cameraBytes)
            continue;
        cameraBytes = desc.ByteWidth;
        camera = buffer.Get();
    }
    if (camera == nullptr)
        return;
    if (state.camera.Copy(context, camera, state.frames))
    {
        state.cameraTaken = true;
        return;
    }
    if (state.log.Take(0x80000001u))
        LOG_INFO("Depth of field: this frame's camera copy could not be staged, so the last accepted camera stands");
}

// The fog post: the scene depth (its own t0, the same depth the march bounds its rays with) and the pixel-to-NDC row.
void CaptureFogPost(ID3D11DeviceContext* context, State& state)
{
    ID3D11Buffer* buffers[ext::ffxiv::engine::kConstantBufferSlots] {};
    context->PSGetConstantBuffers(0, ext::ffxiv::engine::kConstantBufferSlots, buffers);
    ID3D11ShaderResourceView* textureViews[8] {};
    context->PSGetShaderResources(0, 8, textureViews);
    ComPtr<ID3D11Buffer> bound[ext::ffxiv::engine::kConstantBufferSlots];
    for (uint32_t i = 0; i < ext::ffxiv::engine::kConstantBufferSlots; ++i)
        if (buffers[i] != nullptr)
            bound[i].Attach(buffers[i]);
    ComPtr<ID3D11ShaderResourceView> textures[8];
    for (uint32_t i = 0; i < 8; ++i)
        if (textureViews[i] != nullptr)
            textures[i].Attach(textureViews[i]);

    // The pixel-to-NDC row: the 32-byte buffer the fog post binds beside the fog constants (the march's own third
    // reader piece, this module's own ring).
    for (const ComPtr<ID3D11Buffer>& buffer : bound)
    {
        if (!buffer)
            continue;
        D3D11_BUFFER_DESC desc {};
        buffer->GetDesc(&desc);
        if (desc.ByteWidth != kViewportParamBytes)
            continue;
        if (state.row.Copy(context, buffer.Get(), state.frames) && !state.loggedRow)
        {
            state.loggedRow = true;
            LOG_INFO("Depth of field: the fog post's viewport buffer is staged per frame (its second row is logged once it lands)");
        }
        break;
    }

    // The scene depth: the fog post's own t0, the same depth the march bounds its rays with. A depth of another size
    // than the scene the composite wrote is another draw's binding, and the stage keeps the last one it kept (the
    // march's own KeepDepth).
    for (const ComPtr<ID3D11ShaderResourceView>& texture : textures)
    {
        if (!texture)
            continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc {};
        texture->GetDesc(&viewDesc);
        if (viewDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || !shafts::IsDepthFormat(uint32_t(viewDesc.Format)))
            continue;
        ComPtr<ID3D11Resource> resource;
        texture->GetResource(&resource);
        ComPtr<ID3D11Texture2D> depthTexture;
        if (resource == nullptr || FAILED(resource.As(&depthTexture)) || depthTexture == nullptr)
            continue;
        D3D11_TEXTURE2D_DESC desc {};
        depthTexture->GetDesc(&desc);
        if (desc.SampleDesc.Count != 1 || desc.ArraySize != 1)
            continue;
        if (state.sceneWidth != 0 && (desc.Width != state.sceneWidth || desc.Height != state.sceneHeight))
            continue;
        state.depth.Keep(texture.Get(), uint32_t(viewDesc.Format), desc.Width, desc.Height);
        if (!state.loggedDepth)
        {
            state.loggedDepth = true;
            LOG_INFO("Depth of field: the fog post's scene depth ({}x{}, format {}) is the depth the CoC reads",
                     desc.Width, desc.Height, uint32_t(viewDesc.Format));
        }
        break;
    }
}

// A stamped weight draw of the engine's own depth of field: it counts only when it draws to a scene-sized target (the picture, not the family's smaller
// passes), in a frame whose composite this stage has seen (that is where the scene's size comes from).
void NoteEngineDofDraw(ID3D11DeviceContext* context, State& state)
{
    if (state.sceneResource == nullptr || state.sceneFrames != state.frames)
        return;
    ID3D11RenderTargetView* view = nullptr;
    context->OMGetRenderTargets(1, &view, nullptr);
    if (view == nullptr)
        return;
    ComPtr<ID3D11RenderTargetView> held;
    held.Attach(view); // the query's reference, owned here
    ComPtr<ID3D11Resource> resource;
    held->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (resource == nullptr || FAILED(resource.As(&texture)) || texture == nullptr)
        return;
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    if (!IsSceneSized(desc.Width, desc.Height, state.sceneWidth, state.sceneHeight))
        return;
    state.engineDofFrame = state.frames;
    state.haveEngineDofFrame = true;
}

// How the probed focus state reads as words: the capture frame's facts with the state's two floats filled in (the dump calls this when it writes).
std::string FormatProbe(const float* values, size_t count)
{
    Measured measured = S().measured;
    if (values != nullptr && count >= 1)
        measured.focus = values[0];
    if (values != nullptr && count >= 2)
        measured.sample = values[1];
    return FormatMeasured(measured);
}

// The capture frame's numbers for the stage line: the focus the frame used and the raw centre sample (read from the one-texel state at the write, no
// readback of ours), the start, the far limit, the radius, whether the engine's depth of field ran, and the white point of the chain with the stage's fade.
void NoteCaptureNumbers(ID3D11DeviceContext* context, State& state, const shafts::ToneMapFacts& facts)
{
    Measured measured;
    measured.mode = state.params.mode;
    measured.autoFocus = state.params.focusMode == FocusMode::Auto;
    measured.focus = measured.autoFocus ? 0.0f : state.params.focusDistance;
    measured.windowHeight = facts.window.height;
    measured.aperture = state.params.aperture;
    measured.engineDof = false; // a frame the engine's depth of field ran in never gets here: the stage declined it
    state.measured = measured;
    if (measured.mode == Mode::Distance && measured.autoFocus && state.pass.FocusView() != nullptr)
        ext::ffxiv::diag::StageProbe(ext::ffxiv::diag::StageId::Dof, context, state.pass.FocusView(), &FormatProbe);
    else
        ext::ffxiv::diag::StageNoteText(ext::ffxiv::diag::StageId::Dof, FormatMeasured(measured));
    // The stage has no white point or fade of its own: it names the white point an earlier stage of the chain noted this frame (n/a when none ran).
    const float white = ext::ffxiv::diag::StageFrameWhite();
    if (white > 0.0f)
    {
        ext::ffxiv::diag::StageFrameValues values;
        values.haveWhite = true;
        values.white = white;
        values.fade = 1.0f;
        ext::ffxiv::diag::StageNoteFrame(ext::ffxiv::diag::StageId::Dof, values);
    }
    else
        ext::ffxiv::diag::StageNoteText(ext::ffxiv::diag::StageId::Dof, "white=n/a fade=1.00");
}

// One declined tone-map draw: the per-reason count, one line per change of reason with the outcome the chain's hold
// semantics name, the module's own refusal streak, and the game's own draw left exactly as the chain left it. Nothing
// here touches the chain's shared state - the composite's capture stamp is the puddle stage's own file-local state, and
// this module's counters are its own (the march's own discipline). A decline NEVER draws: the owner role returns false
// and the caller draws the game's call once on the binding it already has (the chain's owner-return rule); the chained
// role returns null and the chain's hold stands.
void NoteDecline(State& state, Decline reason, Outcome outcome)
{
    // A later tone-shaped draw of the frame (the engine's own post-tone copy) is not a decision about the capture frame: it never replaces the real reason.
    if (reason != Decline::NotToneMap)
        ext::ffxiv::diag::StageReport(ext::ffxiv::diag::StageId::Dof, ext::ffxiv::diag::StageOutcome::Declined, DeclineName(reason));
    state.declined++;
    if (CountsTowardsStandDown(reason))
    {
        if (reason == Decline::NoCamera)
            ++state.refusals;
        if (state.session.NoteDecline(Knob::DoF) && !state.loggedOff)
        {
            state.loggedOff = true;
            LOG_WARN("{}", ext::ffxiv::engine::FormatSwitchedOff());
            ext::SetActive(ext::F::DepthOfField, false); // the draw path is back to two bit tests
        }
    }
    if (state.log.Take(uint32_t(reason) | (uint32_t(outcome) << 8)))
        LOG_INFO("{}", FormatDecline(reason, outcome));
}

// One skipped frame: nothing is counted against the module (a frame without the stage in it is not a failure), the
// skip's own count moves, and the game's draw is left exactly as the chain left it. A skip never draws either: the
// owner role returns false and the caller draws the game's call once.
void NoteSkip(State& state, SkipReason reason)
{
    ext::ffxiv::diag::StageReport(ext::ffxiv::diag::StageId::Dof, ext::ffxiv::diag::StageOutcome::Declined, SkipName(reason));
    CountSkip(state.skips, reason);
    if (state.log.Take(0x80000010u | uint32_t(reason)))
        LOG_INFO("{}", FormatSkip(reason));
}

// One run of the stage, whichever role reached it: the pass on the scene it was handed, the counters and the one line a
// session gets (the march's own RunMarch shape, this module's own facts).
ID3D11ShaderResourceView* RunStage(State& state, ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts,
                                   ID3D11ShaderResourceView* scene)
{
    Inputs inputs;
    inputs.scene = scene;
    inputs.depth = state.depth.View();
    inputs.sceneFormat = facts.sceneFormat;
    inputs.sceneWidth = facts.sceneWidth;
    inputs.sceneHeight = facts.sceneHeight;
    inputs.depthFormat = state.depth.Format();
    inputs.depthWidth = state.depth.Width();
    inputs.depthHeight = state.depth.Height();
    inputs.window = facts.window;
    std::memcpy(inputs.inverseProjection, state.hold.inverseProjection, sizeof inputs.inverseProjection);
    // The pixel-to-NDC mapping, derived from the render window (the fog post's bytecode hard-codes it; see RecogRules.h).
    const shafts::NdcRow ndc = shafts::PixelToNdcRow(facts.window);
    inputs.ndcScale[0] = ndc.scale[0];
    inputs.ndcScale[1] = ndc.scale[1];
    inputs.ndcOffset[0] = ndc.offset[0];
    inputs.ndcOffset[1] = ndc.offset[1];
    inputs.params = state.params;
    // The smoothing's clock is the time since the pass last RAN (a stand-down, a menu or a cutscene is a gap, and a gap of a second or more makes the
    // focus adopt the next sample whole), never a frame count.
    const int64_t now = Qpc();
    inputs.deltaSeconds = state.lastRunQpc != 0 ? float(double(now - state.lastRunQpc) / double(QpcFrequency())) : 0.0f;

    ID3D11ShaderResourceView* ours = nullptr;
    {
        ext::ffxiv::chara::ReplayScope own;
        ext::ffxiv::engine::OwnWork ownWork;
        ours = state.pass.Run(context, inputs);
    }
    if (ours == nullptr)
    {
        const char* failure = state.pass.Failure();
        if (failure != nullptr && state.log.Take(0x80000002u))
            LOG_INFO("Depth of field: the stage's pass did not run ({})", failure);
    }
    else
        state.lastRunQpc = now;
    return ours;
}

// The card's numbers into the log about once a minute of rendered frames, from the frame's close (the march's cadence).
void LogCountsIfDue(State& state)
{
    if (state.frames < state.nextCountsLog)
        return;
    state.nextCountsLog = state.frames + shafts::kCountsLogFrames;
    CardCounts numbers;
    numbers.frames = state.frames;
    numbers.runs = state.runs;
    numbers.declined = state.declined;
    numbers.skipped = state.skips.skipped;
    numbers.noDepth = state.skips.noDepth;
    numbers.noCamera = state.skips.noCamera;
    numbers.refusals = state.refusals;
    numbers.armed = Armed();
    numbers.strength = state.params.strength;
    numbers.aperture = state.params.aperture;
    numbers.steps = state.params.steps;
    numbers.focusMode = state.params.focusMode;
    numbers.mode = state.params.mode;
    numbers.windowWidth = state.lastWindowWidth;
    numbers.windowHeight = state.lastWindowHeight;
    numbers.quarterWidth = state.lastQuarterWidth;
    numbers.quarterHeight = state.lastQuarterHeight;
    LOG_INFO("{}", FormatCard(numbers));
}

// The frame's reads, and what the stage draws with: the newest copies each ring still holds and the last usable camera held across
// gaps. False when the frame has no camera this stage can reconstruct through. The pixel-to-NDC mapping is not a read: it is derived
// from the render window at the draw (RunStage).
bool Readback(ID3D11DeviceContext* context, State& state, const shafts::ToneMapFacts& facts)
{
    const CameraRead cameraRead = state.camera.Read(context, state.frames);
    const RowRead rowRead = state.row.Read(context, state.frames);
    state.cameraAge = cameraRead.landed ? shafts::AgeOf(state.frames, cameraRead.writtenFrame) : shafts::kRingNotLanded;
    state.rowAge = rowRead.landed ? shafts::AgeOf(state.frames, rowRead.writtenFrame) : shafts::kRingNotLanded;
    // The fog post's viewport buffer is not what the pixel-to-NDC mapping comes from (the fog post's bytecode reads only its first row and
    // hard-codes the mapping): its second row is logged once next to the mapping this stage derives from the window, so a session settles
    // what the row holds.
    if (rowRead.landed && !state.loggedViewportRow)
    {
        const shafts::NdcRow derived = shafts::PixelToNdcRow(facts.window);
        if (derived.ok)
        {
            state.loggedViewportRow = true;
            const float second[4] { rowRead.ndcScale[0], rowRead.ndcScale[1], rowRead.ndcOffset[0], rowRead.ndcOffset[1] };
            LOG_INFO("{}", shafts::FormatViewportRow("Depth of field", second, derived));
        }
    }
    // A camera whose bytes arrived and are not this build's camera is a build mismatch, counted toward the stand-down;
    // a read that never landed is a GPU that is behind or a build without the light draws, and its own line says which.
    state.lastReadCameraRefused = cameraRead.landed && !cameraRead.usable;
    state.hold.Accept(cameraRead);
    if (state.lastReadCameraRefused)
        ++state.refusals; // the march's end-note: the refusal's own count, toward the stand-down's streak
    // The staleness lines, the march's own two states on this module's own rings.
    if (state.camera.Reads() == 0 && state.camera.NotReady() >= kReadbackStallFrames && !state.loggedStall)
    {
        state.loggedStall = true;
        LOG_INFO("Depth of field: the camera read-back has not started landing while this stage is armed");
    }
    else if (state.camera.HaveRead() && state.frames >= state.camera.LastReadFrame() &&
             state.frames - state.camera.LastReadFrame() > kMaxReadbackAge && !state.loggedStale)
    {
        state.loggedStale = true;
        LOG_INFO("Depth of field: the camera read-back has stopped landing, so the last accepted camera stands");
    }
    return state.hold.have;
}

// The frame's tone map closes the frame (RecogRules.h ClosesFrame): the next frame's first composite then opens the next count. It runs
// at the top of every entry that can see the tone map, before the stage's one-answer gate and whatever the stage then does with the draw,
// so a frame the stage declines or skips still moves the count on.
void CloseFrameAtToneMap(State& state, const shafts::ToneMapFacts& facts, bool chained)
{
    if (shafts::ClosesFrame(facts, chained))
        state.step.CloseAtToneMap();
}

// The one line a live session needs to see the mechanism: how often the count moved for the frame that just closed, and the ages of the
// copies its read-backs took.
void NoteFrameStep(State& state)
{
    if (!state.step.HaveSample())
        return;
    const shafts::RingAge rings[] { { "camera copy", state.cameraAge }, { "mapping row", state.rowAge } };
    const uint32_t advances = state.step.AdvancesPerFrame();
    const uint32_t composites = state.step.CompositesPerFrame();
    if (state.stepLine.Take(shafts::StepKey(advances, composites, rings, 2)))
        LOG_INFO("{}", shafts::FormatFrameStep("Depth of field", advances, composites, rings, 2));
}

// The stage at one tone-map draw, in either role. The game's draw runs exactly once, whoever owns it: the chained role
// returns this module's texture (or null, which leaves the chain's hold standing), and the owner role binds its own
// output at t0 for the one draw and puts the game's own binding back, also when that draw throws.
ID3D11ShaderResourceView* StageAtToneMap(State& state, ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts,
                                         ID3D11ShaderResourceView* scene, bool owner, DrawFn draw, void* user, bool& drew)
{
    const Outcome declineOutcome = owner ? Outcome::EngineOwn : Outcome::HoldStands;
    // The scene HDR path replays the engine's own tone chain in its own branch, so the scene this stage would blur is
    // not the picture that path presents (the march's own decline, this stage's own state's sample).
    if (shafts::SceneHdrInactive(state.enabled, ext::hdr::FfxivSceneHdr::Requested()))
    {
        NoteDecline(state, Decline::SceneHdr, declineOutcome);
        return nullptr;
    }
    if (facts.replay)
    {
        NoteDecline(state, Decline::CharaReplay, declineOutcome);
        return nullptr;
    }
    const shafts::Decline shape = shafts::ClassifyToneMap(facts);
    if (shape != shafts::Decline::None)
    {
        Decline reason = Decline::NotToneMap;
        if (shape == shafts::Decline::RenderTarget)
            reason = Decline::RenderTarget;
        else if (shape == shafts::Decline::TargetFormat)
            reason = Decline::TargetFormat;
        else if (shape == shafts::Decline::SceneFormat)
            reason = Decline::SceneFormat;
        else if (shape == shafts::Decline::SizeMismatch)
            reason = Decline::SizeMismatch;
        else if (shape == shafts::Decline::Window)
            reason = Decline::Window;
        NoteDecline(state, reason, declineOutcome);
        return nullptr;
    }
    // The engine's own depth of field drew in this frame (its weight pass, before the tone map): its chain already blurred the scene the tone map reads, and a
    // second blur would take the high frequencies to a third. Not a failure of this stage: the frame is left as it is and nothing counts against the stage.
    if (EngineDofRanThisFrame(state.haveEngineDofFrame, state.engineDofFrame, state.frames)) { NoteDecline(state, Decline::EngineDof, declineOutcome);
        if (ext::ffxiv::diag::StageOpen(ext::ffxiv::diag::StageId::Dof))
            ext::ffxiv::diag::StageNoteText(ext::ffxiv::diag::StageId::Dof, "engineDof=1");
        return nullptr;
    }
    // The frame's depth has to be this frame's scene's own: a depth of another size is another draw's binding, and the
    // pack's CoC would read the wrong pixels (the march's own SizeMismatch, this module's own capture).
    if (state.depth.View() == nullptr || state.depth.Width() != facts.sceneWidth || state.depth.Height() != facts.sceneHeight)
    {
        NoteDecline(state, Decline::NoDepth, declineOutcome);
        return nullptr;
    }
    // The frame's reads, and what the stage draws with: the newest copies each ring still holds, the last usable camera
    // held across gaps.
    Readback(context, state, facts);
    NoteFrameStep(state);
    // A camera whose bytes arrived and are not this build's camera is a build mismatch, counted toward the stand-down;
    // a read that has never landed (or has stopped landing) is a GPU that is behind or a build without the light draws,
    // and its own line says which (the march's own two lines, on this module's own rings).
    if (!state.hold.have)
    {
        if (state.lastReadCameraRefused)
        {
            if (!state.loggedCameraRefused)
            {
                state.loggedCameraRefused = true;
                LOG_INFO("{}", FormatCameraRefused());
            }
            NoteDecline(state, Decline::NoCamera, declineOutcome);
            return nullptr;
        }
        // The read never landed at all: the frames are skipped uncounct - a frame without the stage in it, the march's
        // own skip semantics, until a camera lands. (The two staleness log lines ran in Readback.)
        NoteSkip(state, SkipReason::NoCamera);
        return nullptr;
    }
    ID3D11ShaderResourceView* ours = RunStage(state, context, facts, scene);
    if (ours == nullptr)
    {
        NoteDecline(state, Decline::PassFailed, declineOutcome);
        return nullptr;
    }
    // The capture frame's pictures of this stage: the scene it was handed and the answer it made, copied now (its pass is over and its guards
    // are gone). Depth of field is the chain's last member, so its answer is what the tone map reads.
    ext::ffxiv::diag::StageRan(ext::ffxiv::diag::StageId::Dof, context, scene, ours,
                               ext::ffxiv::diag::StageWindow { facts.window.x, facts.window.y, facts.window.width, facts.window.height });
    if (ext::ffxiv::diag::StageOpen(ext::ffxiv::diag::StageId::Dof))
        NoteCaptureNumbers(context, state, facts);

    // Answered: the counters and the one line a session gets. The chained role hands its texture back for the caller to
    // bind; the owner role binds it here, for exactly one draw, and puts the game's own binding back (the march's own
    // draw-once rule).
    state.runs++;
    state.lastWindowWidth = uint32_t(facts.window.width);
    state.lastWindowHeight = uint32_t(facts.window.height);
    state.lastQuarterWidth = QuarterExtent(facts.window.width);
    state.lastQuarterHeight = QuarterExtent(facts.window.height);
    state.session.NoteRun(Knob::DoF);
    if (state.log.Take(0x80000003u | ((facts.sceneWidth & 0x7FFFu) << 11)))
    {
        RunFacts run;
        run.mode = state.params.mode;
        run.focusMode = FocusModeName(state.params.focusMode);
        run.focus = state.params.focusDistance;
        run.strength = state.params.strength;
        run.aperture = state.params.aperture;
        run.steps = state.params.steps;
        run.windowWidth = state.lastWindowWidth;
        run.windowHeight = state.lastWindowHeight;
        run.quarterWidth = state.lastQuarterWidth;
        run.quarterHeight = state.lastQuarterHeight;
        LOG_INFO("{}", FormatRun(run));
    }
    if (!state.loggedCost && state.pass.HasGpuMilliseconds())
    {
        state.loggedCost = true;
        LOG_INFO("{}", FormatCost(true, state.pass.GpuMilliseconds(), state.frames, state.runs, state.declined));
    }
    if (!owner)
        return ours;

    // The owner's one bind: this module's copy of the scene at t0 for the one draw, and the game's own view back
    // afterwards, also when that draw throws. The chained role's caller does the same for its own texture (the march's
    // own restore).
    ID3D11ShaderResourceView* original = nullptr;
    context->PSGetShaderResources(0, 1, &original);
    context->PSSetShaderResources(0, 1, &ours);
    struct Restore
    {
        ID3D11DeviceContext* context = nullptr;
        ID3D11ShaderResourceView* view = nullptr;
        ~Restore()
        {
            context->PSSetShaderResources(0, 1, &view);
            if (view != nullptr)
                view->Release(); // the reference the read above took
        }
    } restore { context, original };
    draw(user);
    drew = true;
    return ours;
}

// The tone map's own shape, measured at the draw, defined below (the march's own measurement, this stage's own recorded
// scene).
shafts::ToneMapFacts MeasureToneMap(ID3D11DeviceContext* context, State& state, ComPtr<ID3D11ShaderResourceView>& scene);

// The stage as the chain's fallback owner: the entry the shafts' chain and the seam's own call reach when neither
// earlier stage answered. True only if this stage drew the game's call; a decline or a skip draws nothing and returns
// false, and the caller draws the game's call once on the binding it already has (the chain's owner-return rule). The
// catch is the one place this function draws without having drawn: a throw before the draw, so the call still runs once.
bool RunToneMapAsOwner(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene,
                       DrawFn draw, void* user)
{
    State& state = S();
    bool drew = false;
    try
    {
        LogCountsIfDue(state);
        StageAtToneMap(state, context, facts, scene, true, draw, user, drew);
        return drew;
    }
    catch (...)
    {
        LOG_INFO("{}", FormatDecline(Decline::Exception, Outcome::EngineOwn));
        if (!drew)
            draw(user); // the game's draw, once, and not twice (the march's own catch)
        return true;
    }
}

// The stage as a chain member: the entry the light bounce's chained entry reaches (with the bounce's own answer, or with the texture the
// bounce was handed on every exit where it has no answer - the march's answer, or what the march was handed) with the chain's current
// output. A texture comes back for the caller to bind at t0; null means the stage has nothing for this frame, and the caller's own texture -
// the chain's last good picture - stays at t0 (the hold stands). Registered as ext::ffxiv::engine::chainDofScene by this module's
// registration below, through the exported ChainScene.
ID3D11ShaderResourceView* ChainSceneInternal(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts,
                                             ID3D11ShaderResourceView* scene)
{
    State& state = S();
    if (context == nullptr || scene == nullptr || !Armed())
        return nullptr;
    if (!state.haveRenderThread)
    {
        state.renderThread = std::this_thread::get_id();
        state.haveRenderThread = true;
    }
    CloseFrameAtToneMap(state, facts, true);
    // The chained role answers at most one tone map a frame, by this module's own counter: a second tone-shaped draw of
    // the frame is the engine's own post-tone copy, and the game's own call.
    if (state.haveDofFrame && state.dofFrame == state.frames)
        return nullptr;
    state.haveDofFrame = true;
    state.dofFrame = state.frames;
    try
    {
        bool drew = false; // the chained role never draws: the caller binds and draws
        return StageAtToneMap(state, context, facts, scene, false, nullptr, nullptr, drew);
    }
    catch (...)
    {
        LOG_INFO("{}", FormatDecline(Decline::Exception, Outcome::HoldStands));
        return nullptr; // the chain's hold stands
    }
}

// The stage as the chain's fallback owner, through the chain's own hook: the light bounce's owner entry reaches here at both of its early
// exits and after its own owner role declined. True only if this stage drew the game's call; false obliges the caller to draw it once on the
// binding it already has (the chain's owner-return rule).
bool ToneMapOwnerInternal(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene, DrawFn draw,
                          void* user)
{
    State& state = S();
    if (context == nullptr || scene == nullptr || draw == nullptr || !Armed())
        return false;
    if (!state.haveRenderThread)
    {
        state.renderThread = std::this_thread::get_id();
        state.haveRenderThread = true;
    }
    CloseFrameAtToneMap(state, facts, true);
    if (state.haveDofFrame && state.dofFrame == state.frames)
        return false; // this frame's tone map was already answered, by this stage
    state.haveDofFrame = true;
    state.dofFrame = state.frames;
    return RunToneMapAsOwner(context, facts, scene, draw, user);
}

// The seam's own entry: the captures at the fog post, the light draws and the composite, and the tone map's fallback
// owner when neither earlier stage took the draw.
bool SeamDraw(ID3D11DeviceContext* context, engine::Anchors anchors, DrawFn draw, void* user)
{
    State& state = S();
    if (draw == nullptr)
        return false;
    if (!Armed() || context == nullptr)
        return false;
    if (!state.haveRenderThread)
    {
        state.renderThread = std::this_thread::get_id();
        state.haveRenderThread = true;
    }
    try
    {
        if (state.boundWeightStamp.load(std::memory_order_relaxed) != 0)
            NoteEngineDofDraw(context, state);
        if (ext::ffxiv::engine::IsFogPostAnchors(anchors))
        {
            CaptureFogPost(context, state);
            return false; // the game's own draw still has to run: the engine module's draw path runs it
        }
        if (ext::ffxiv::engine::IsShaftsLightAnchors(anchors))
        {
            CaptureCameraAtLightDraw(context, anchors, state);
            return false;
        }
        if (ext::ffxiv::engine::IsShaftsCompositeAnchors(anchors))
        {
            RecordScene(context, state);
            return false;
        }
        if (ext::ffxiv::engine::IsShaftsToneMapAnchors(anchors))
        {
            // The shafts and the puddles' and the march's stages had their chances above this entry in the dispatch;
            // reaching here with a tone map means none took it, and this stage is the fallback owner. The shape is
            // measured here, and a draw that is not the tone map is the game's own call.
            ComPtr<ID3D11ShaderResourceView> scene;
            const shafts::ToneMapFacts facts = MeasureToneMap(context, state, scene);
            if (shafts::ClassifyToneMap(facts) == shafts::Decline::NotToneMap)
                return false;
            CloseFrameAtToneMap(state, facts, false);
            if (state.haveDofFrame && state.dofFrame == state.frames)
                return false;
            state.haveDofFrame = true;
            state.dofFrame = state.frames;
            return RunToneMapAsOwner(context, facts, scene.Get(), draw, user);
        }
    }
    catch (...)
    {
        LOG_INFO("{}", FormatDecline(Decline::Exception, Outcome::EngineOwn));
    }
    return false;
}

// The tone map's own shape, measured at the draw (the march's own measurement, this stage's own recorded scene).
shafts::ToneMapFacts MeasureToneMap(ID3D11DeviceContext* context, State& state, ComPtr<ID3D11ShaderResourceView>& scene)
{
    shafts::ToneMapFacts facts;
    ID3D11RenderTargetView* views[2] { nullptr, nullptr };
    context->OMGetRenderTargets(2, views, nullptr);
    facts.renderTargets = (views[0] != nullptr ? 1u : 0u) + (views[1] != nullptr ? 1u : 0u);
    if (views[0] != nullptr && views[1] == nullptr)
    {
        ComPtr<ID3D11Resource> resource;
        views[0]->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (resource != nullptr && SUCCEEDED(resource.As(&texture)) && texture != nullptr)
        {
            D3D11_TEXTURE2D_DESC desc {};
            texture->GetDesc(&desc);
            D3D11_RENDER_TARGET_VIEW_DESC viewDesc {};
            views[0]->GetDesc(&viewDesc);
            facts.targetWidth = desc.Width;
            facts.targetHeight = desc.Height;
            facts.targetFormat = uint32_t(viewDesc.Format);
            facts.targetIsScene = texture.Get() == state.sceneResource;
        }
    }
    for (ID3D11RenderTargetView*& view : views)
        if (view != nullptr)
        {
            view->Release();
            view = nullptr;
        }

    ID3D11ShaderResourceView* first = nullptr;
    context->PSGetShaderResources(0, 1, &first);
    scene.Attach(first);
    if (scene != nullptr)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc {};
        scene->GetDesc(&viewDesc);
        facts.sceneSampled = viewDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
        facts.sceneFormat = uint32_t(viewDesc.Format);
        ComPtr<ID3D11Resource> resource;
        scene->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (resource != nullptr && SUCCEEDED(resource.As(&texture)) && texture != nullptr)
        {
            D3D11_TEXTURE2D_DESC desc {};
            texture->GetDesc(&desc);
            facts.sceneWidth = desc.Width;
            facts.sceneHeight = desc.Height;
            // The scene the composite wrote has to be this frame's: the test is this module's own recorded frame, never
            // the chain's shared stamp (the stamping contract).
            facts.sceneIsScene = texture.Get() == state.sceneResource && state.sceneFrames == state.frames;
        }
    }

    D3D11_VIEWPORT viewport {};
    UINT count = 1;
    context->RSGetViewports(&count, &viewport);
    facts.window = { viewport.TopLeftX, viewport.TopLeftY, viewport.Width, viewport.Height };
    facts.replay = ext::ffxiv::chara::t_replay;
    return facts;
}

// The live config expression's own answer, the march's own rule (a session that switches HDR on or off with the
// stage's key untouched stops and starts this stage without arming anything again). Defined before its callers and
// again reused below the anonymous namespace through this namespace's members.
bool SceneHdrRequested() { return ext::hdr::FfxivSceneHdr::Requested(); }

void ObserveSceneHdr(State& state)
{
    if (state.hdr.Sample(state.enabled, SceneHdrRequested()))
        LOG_INFO("{}", kSceneHdrInactive);
}

void Apply(bool on)
{
    State& state = S();
    auto& cfg = ext::Cfg().ffxiv;
    state.enabled = on;
    state.params = BuildParams(cfg.dofStrength.value_or_default(), cfg.dofAperture.value_or_default(),
                               cfg.dofSteps.value_or_default(), cfg.dofFocusMode.value_or_default(),
                               cfg.dofFocusDistance.value_or_default(), cfg.dofMode.value_or_default());
    ObserveSceneHdr(state);
    if (!on)
    {
        state.session = Session {};
        state.log = engine::LogGate {};
        state.loggedOff = false;
        state.loggedCost = false;
        state.loggedCameraRefused = false;
        state.loggedStall = false;
        state.loggedStale = false;
        state.loggedDepth = false;
        state.loggedRow = false;
        state.loggedViewportRow = false;
        state.step.Reset();
        state.stepLine = shafts::ChangeLine {};
        state.cameraAge = state.rowAge = shafts::kRingNotLanded;
        state.sceneResource = nullptr;
        state.cameraTaken = false;
        state.haveDofFrame = false;
        state.dofFrame = 0;
        state.haveEngineDofFrame = false;
        state.engineDofFrame = 0;
        state.lastRunQpc = 0;
        state.hold = CameraHold {};
        state.nextCountsLog = 0;
        state.runs = 0;
        state.declined = 0;
        state.skips = SkipCounts {};
        state.refusals = 0;
        state.depth.Reset();
    }
    else if (state.session.off)
    {
        state.session = Session {}; // the player turned the switch back on: the counters and the streaks start again
        state.nextCountsLog = 0;
    }
    const bool wanted = Wanted(state.enabled, state.params) && state.session.Armed();
    ext::SetActive(ext::F::DepthOfField, wanted);
    if (!wanted || state.hdr.inactive)
    {
        state.pass.Reset();
        state.camera.Reset();
        state.row.Reset();
        state.depth.Reset();
        state.hold = CameraHold {};
    }
}
} // namespace

void NoteShader(void* shaderVoid, const void* code, size_t size)
{
    auto* shader = static_cast<ID3D11PixelShader*>(shaderVoid);
    if (shader == nullptr || code == nullptr || size < 32)
        return;
    try
    {
        ext::ffxiv::diag::ShaderBinding parsed[ext::ffxiv::diag::kMaxBindings];
        const size_t count = ext::ffxiv::diag::ParseBindings(code, size, parsed, ext::ffxiv::diag::kMaxBindings);
        const bool declaresParam = ext::ffxiv::diag::BindPointOf(parsed, count, ext::ffxiv::diag::BindKind::ConstantBuffer, "cWeightSamplingParam") !=
                                   ext::ffxiv::diag::kNoBindPoint;
        const bool declaresInput = ext::ffxiv::diag::BindPointOf(parsed, count, ext::ffxiv::diag::BindKind::Texture, "sInput") != ext::ffxiv::diag::kNoBindPoint;
        if (!IsEngineDofLayout(declaresParam, declaresInput))
            return; // not the pass that makes the picture (the CoC LUT update, or a shader that merely reads an sInput)
        bool full = false;
        {
            std::lock_guard lock(Stamps().mutex);
            if (Stamps().assigned >= kWeightMaxShaders)
                full = true;
            else
            {
                const uint32_t id = ++Stamps().assigned;
                if (SUCCEEDED(shader->SetPrivateData(kWeightStampTag, sizeof id, &id)))
                    full = WeightTableForInsert().Insert(reinterpret_cast<uintptr_t>(shader), id, engine::Anchors(1)) == engine::SeenInsert::Full;
            }
        }
        if (full && !Stamps().fullLogged)
        {
            Stamps().fullLogged = true;
            LOG_WARN("Depth of field: the engine's weight-pass stamp index is full, so one shader's draws are not noticed (the stage may blur on top of the engine's own)");
        }
    }
    catch (...)
    {
    }
}

void OnPixelShaderSet(const void* shader, uint32_t instances)
{
    State& state = S();
    // A class-instance override is never one of these draws, and a null shader binds nothing: both answer 0 until the next plain bind.
    if (instances != 0 || shader == nullptr)
    {
        state.boundWeightStamp.store(0, std::memory_order_relaxed);
        return;
    }
    engine::SeenTable<kWeightSeenSlots>* table = WeightTableIfAny();
    const engine::Anchors found =
        table != nullptr ? table->Find(reinterpret_cast<uintptr_t>(shader), [shader](uintptr_t) { return StampedWeightOf(shader); }) : 0;
    state.boundWeightStamp.store(uint32_t(found), std::memory_order_relaxed);
}

bool Armed()
{
    State& state = S();
    if (!Wanted(state.enabled, state.params) || !state.session.Armed())
        return false;
    return !shafts::SceneHdrInactive(state.enabled, SceneHdrRequested());
}

void SampleSceneHdr()
{
    State& state = S();
    const uint64_t token = ext::present::FrameCount();
    if (state.haveHdrSample && state.hdrSampleFrame == token)
        return;
    state.haveHdrSample = true;
    state.hdrSampleFrame = token;
    ObserveSceneHdr(state);
}

bool OnSeamDraw(ID3D11DeviceContext* context, engine::Anchors anchors, DrawFn draw, void* user)
{
    return SeamDraw(context, anchors, draw, user);
}

// The chained role, behind the light bounce's stage (ChainHooks.h's own hook pointer, this module's registration at start-up).
ID3D11ShaderResourceView* ChainScene(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene)
{
    try
    {
        return ChainSceneInternal(context, facts, scene);
    }
    catch (...)
    {
        return nullptr; // nothing escapes into the light bounce's entries: the chain's hold stands
    }
}

// The fallback-owner role, behind the light bounce's stage (ChainHooks.h's hook pointer, this module's registration at start-up). The owner
// contract: true only when this stage drew the game's call. The internals catch around everything that can run after the draw
// (RunToneMapAsOwner); an exception that gets out of them happened before it, so false (the caller draws once) is the honest answer.
bool ToneMapOwner(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene, DrawFn draw, void* user)
{
    try
    {
        return ToneMapOwnerInternal(context, facts, scene, draw, user);
    }
    catch (...)
    {
        return false;
    }
}

Status CurrentStatus()
{
    State& state = S();
    Status status;
    status.armed = Armed();
    status.hdrInactive = shafts::SceneHdrInactive(state.enabled, SceneHdrRequested());
    status.params = state.params;
    status.frames = state.frames;
    status.runs = state.runs;
    status.declined = state.declined;
    status.skipped = state.skips.skipped;
    status.noDepth = state.skips.noDepth;
    status.noCamera = state.skips.noCamera;
    status.refusals = state.refusals;
    status.windowWidth = state.lastWindowWidth;
    status.windowHeight = state.lastWindowHeight;
    status.quarterWidth = state.lastQuarterWidth;
    status.quarterHeight = state.lastQuarterHeight;
    return status;
}

void Publish() { Apply(ext::Cfg().ffxiv.depthOfField.value_or_default()); }

void OnConfigLoaded(bool on) { Apply(on); }

void Release()
{
    State& state = S();
    state.pass.Reset();
    state.camera.Reset();
    state.row.Reset();
    state.depth.Reset();
    state.sceneResource = nullptr;
    state.hold = CameraHold {};
}

// The stage dump's two questions about this stage, asked at the capture frame: is it armed (Armed()), and what are its values.
std::string StageValues()
{
    const Params& params = S().params;
    return std::format("mode={} focus={} distance={:.2f} strength={:.2f} aperture={:.2f} steps={}", ModeName(params.mode), FocusModeName(params.focusMode),
                       double(params.focusDistance), double(params.strength), double(params.aperture), params.steps);
}
} // namespace ext::ffxiv::dof

namespace
{
// The registration of the config hook and the chain's stage hooks, set when this DLL's static initialisers run: the
// [FFXIV] DepthOfField key arms the seam's feature bit at every config load, and the light bounce's entries reach the stage
// through the pointers the chain's own header declares - null in every build that does not link this file, which is the stage
// being off.
struct RegisterDof
{
    RegisterDof()
    {
        ext::FfxivConfig::dofOnLoaded = &ext::ffxiv::dof::OnConfigLoaded;
        ext::ffxiv::engine::chainDofScene = &ext::ffxiv::dof::ChainScene;
        ext::ffxiv::engine::toneMapDofOwner = &ext::ffxiv::dof::ToneMapOwner;
        ext::ffxiv::diag::StageRegister(ext::ffxiv::diag::StageId::Dof, &ext::ffxiv::dof::Armed, &ext::ffxiv::dof::StageValues);
    }
} g_registerDof;
} // namespace
