#include "pch.h"

#include "ext/ffxiv/engine/PuddleTone.h"

#include "ext/ffxiv/chara/CharaDispatch.h"      // the character preview's replay marker
#include "ext/ffxiv/diag/DrawDiag.h"            // Capturing(): the pool and hit shares are only measured while a capture is armed
#include "ext/ffxiv/engine/GlossPuddlesApply.h" // the module's own stamp (the composite's slots), the wetness both stages gate on and the pairing
#include "ext/ffxiv/engine/PuddleTonePass.h"
#include "ext/hdr/scene/FfxivSceneHdr.h"        // the scene HDR path replaces the tone chain this stage draws before
#include "ext/present/PresentSeams.h"           // the output-frame counter, the frame token the pairing names the shaped frame by

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <thread>

// Puddles at the tone map: the draw path (PuddleTone.h). The pure decisions are in GlossPuddlesRules.h, the GPU work in PuddleTonePass.cpp, the
// shader in shaders/PuddleReflection.hlsl.
//
// Two things happen here, one draw each:
//  - at the frame's deferred composite (the one draw that binds g_SamplerViewPosition), the frame's view positions, its G-buffer normal, its
//    camera table and the scene it writes are recorded. The tone map declares only t0 and its colour LUT, so nothing of this can be read at that
//    draw; a frame whose composite was not seen has no pool, which is the fail-open answer.
//  - at the frame's tone-map draw, the pass runs once and the game's draw runs with this module's copy of the scene at its t0, and the game's own
//    view back afterwards (also when that draw throws).
// The light shafts own that draw while their card is on: their tone-map branch calls OnShaftsScene with its own air-lit output before it binds t0,
// so one module rebinds the scene and the pool's mirror reflects the air light as well. The seam's own call is then the fallback, and it answers
// only a tone map the chain did not (a frame with the shafts off, or one the shafts left alone).
namespace ext::ffxiv::engine::gloss
{
namespace
{
using Microsoft::WRL::ComPtr;

// The raindrops' clock, in seconds: the tone stage's shader animates the splash rings on the pools from it, and it is the only number in this
// stage that moves with real time. A start stamp is taken on the first frame the module runs, so the rings begin at phase zero on the frame the
// effect is armed (not wherever the machine's uptime happens to be) and cannot jump when a long-loading frame or a pause happens: both ends are
// QueryPerformanceCounter reads of the same clock. The subtraction is in integer counts before it becomes a float, so the value a QPC clock of a
// few days of uptime carries never loses the sub-millisecond precision the animation's phase needs.
float RippleSeconds()
{
    static const int64_t start = [] {
        LARGE_INTEGER value {};
        QueryPerformanceCounter(&value);
        return value.QuadPart;
    }();
    LARGE_INTEGER now {};
    LARGE_INTEGER frequency {};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    if (frequency.QuadPart <= 0 || now.QuadPart < start)
        return 0.0f; // a clock this build cannot read: the still ripple, which is the effect the keys drew before the animation
    return float(double(now.QuadPart - start) / double(frequency.QuadPart));
}

// One bound texture, as the facts need it: its view's format and dimension, the resource behind it and its own description. The caller keeps the
// view's reference; nothing here owns it.
struct BoundTexture
{
    ID3D11ShaderResourceView* view = nullptr;
    ID3D11Texture2D* texture = nullptr;
    const void* resource = nullptr;
    uint32_t format = 0, width = 0, height = 0, samples = 0;
    bool twoD = false;
};

BoundTexture Describe(ID3D11ShaderResourceView* view)
{
    BoundTexture out;
    if (view == nullptr)
        return out;
    out.view = view;
    D3D11_SHADER_RESOURCE_VIEW_DESC desc {};
    view->GetDesc(&desc);
    out.format = uint32_t(desc.Format);
    out.samples = 1;
    out.twoD = desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    if (!resource)
        return out;
    out.resource = resource.Get();
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resource->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
    {
        out.twoD = false;
        return out;
    }
    D3D11_TEXTURE2D_DESC texture {};
    static_cast<ID3D11Texture2D*>(resource.Get())->GetDesc(&texture);
    out.texture = static_cast<ID3D11Texture2D*>(resource.Get());
    out.width = texture.Width;
    out.height = texture.Height;
    out.samples = texture.SampleDesc.Count;
    out.twoD = out.twoD && texture.ArraySize == 1;
    return out;
}

// The frame's composite inputs, recorded once per frame. `id` counts the composites this module saw, and the tone map answers a capture once:
// that pair is what makes "the scene the composite wrote" this frame's scene without a clock (the output-frame counter steps inside a rendered
// frame while frame generation presents a generated image, so a "same counter" test would drop every other frame's pool).
struct Capture
{
    bool taken = false;
    uint64_t id = 0;
    uint64_t frameToken = 0; // the output-frame counter at the composite: the pair the tone stage's answer is given to
    bool shapeWet = false;   // the pairing's own answer, evaluated HERE once (the review's token-skew finding): a present between the
                             // gloss stamp and this read would otherwise tick the counter between the two FrameCount loads
    ComPtr<ID3D11ShaderResourceView> viewPosition;
    ComPtr<ID3D11ShaderResourceView> normal;
    ComPtr<ID3D11Buffer> camera;
    uint32_t viewPositionFormat = 0, viewPositionWidth = 0, viewPositionHeight = 0;
    uint32_t normalFormat = 0, normalWidth = 0, normalHeight = 0;
    uint32_t cameraBytes = 0;
    const void* sceneResource = nullptr;
    uint32_t sceneWidth = 0, sceneHeight = 0, sceneFormat = 0;
};

struct State
{
    TonePass pass;
    Session session;
    LogGate log;
    Params params;
    Capture capture;
    std::atomic<bool> inPass { false };
    uint32_t runs = 0;
    uint32_t captures = 0;
    uint32_t declined = 0;
    uint32_t declines[kToneDeclineReasonCount] {};
    uint64_t usedCapture = 0; // the capture the tone map last answered: a frame with no new composite answers nothing
    bool loggedRun = false;
    bool loggedOff = false;
    bool loggedDry = false;
    bool loggedFailure = false;
    bool loggedInputs = false;
    bool loggedSceneHdr = false; // the scene HDR path is on: one line, once per publish
    const char* failure = nullptr;
    uint32_t lastSceneWidth = 0, lastSceneHeight = 0;
    float lastWindow[4] {};
    float lastRippleSeconds = 0.0f; // the clock the last run handed the shader (LastRippleSeconds)
    RainTracker rain;               // whether it is raining, from the direction of the engine's wetness
    float rainStamp = 0.0f;         // the clock at the last rain step
    // The wetness gate's dwell is no longer this stage's own field: it is the gloss pass's shared latch (NoteWetnessFrame
    // answers it on the frame's copy), so this stage's answer cannot disagree with the gloss stage's inside a frame.
    bool glossAlive = false;        // the gloss side can still shape (the card on, a servable target/channel): a dead pairing answers None, never the frozen hold
    uint32_t held = 0;              // the frames answered with the last good output instead of a run (the hold)
    bool loggedRaining = false;     // the rain line of the log, one per change
    bool haveRun = false;
    std::thread::id renderThread;
    bool haveRenderThread = false;
};

State& S()
{
    static State* state = new State; // never destroyed: no destructor may run under the loader lock
    return *state;
}

// Marks this module's own GPU work: its draw re-enters the draw seam (it is drawn on the game's own immediate context) and the shared module must
// never classify or count it. A nested guard restores, never clears.
struct OwnWork
{
    const bool previous;
    OwnWork() : previous(S().inPass.exchange(true, std::memory_order_relaxed))
    {
        if (!previous)
            engine::SetOwnWork(true);
    }
    ~OwnWork()
    {
        if (!previous)
            engine::SetOwnWork(false);
        S().inPass.store(previous, std::memory_order_relaxed);
    }
    OwnWork(const OwnWork&) = delete;
    OwnWork& operator=(const OwnWork&) = delete;
};

// One refused tone map. Every reason leaves the draw exactly as the game made it, and each is one line of the log the first time it happens. Only a
// refusal that says this build cannot do the work walks the stage towards its switch-off.
void Refuse(ToneDecline reason)
{
    State& state = S();
    state.declined++;
    if (uint32_t(reason) < kToneDeclineReasonCount)
        state.declines[uint32_t(reason)]++;
    if (state.log.Take(uint32_t(reason)))
        LOG_INFO("Puddles: the tone map was left alone ({})", ToneDeclineName(reason));
    if (ToneCountsTowardsSwitchOff(reason) && state.session.NoteDecline() && !state.loggedOff)
    {
        state.loggedOff = true;
        LOG_WARN("Puddles: {} tone maps in a row could not run the reflection, so the pool's colour is the engine's own from here", kDeclineLimit);
    }
}

// The frame's composite: the one draw that declares g_SamplerViewPosition, writes the scene the tone map reads and binds the camera the stage
// reflects. Read from the module's own stamp, so no reflection is parsed here.
void NoteComposite(ID3D11DeviceContext* context, State& state)
{
    ComPtr<ID3D11PixelShader> pixelShader;
    context->PSGetShader(&pixelShader, nullptr, nullptr);
    const Inputs inputs = UnpackInputs(detail::ReadStamp(pixelShader.Get()));
    if (!inputs.HasViewPosition() || !inputs.HasCamera() || !inputs.HasTarget(3))
        return; // not the composite: a frame without one leaves the previous capture until a new one arrives, which the frame id refuses

    // The scene this draw writes: exactly one render target, a single-sample 2D texture of a scene format at a scene size. It is what the tone
    // map's t0 has to be for this stage to answer, which is what makes the pair this frame's.
    BoundTexture scene;
    {
        ID3D11RenderTargetView* views[2] { nullptr, nullptr };
        context->OMGetRenderTargets(2, views, nullptr);
        if (views[0] != nullptr && views[1] == nullptr)
        {
            D3D11_RENDER_TARGET_VIEW_DESC viewDesc {};
            views[0]->GetDesc(&viewDesc);
            ComPtr<ID3D11Resource> resource;
            views[0]->GetResource(&resource);
            ComPtr<ID3D11Texture2D> texture;
            if (resource != nullptr && SUCCEEDED(resource.As(&texture)) && texture != nullptr)
            {
                D3D11_TEXTURE2D_DESC desc {};
                texture->GetDesc(&desc);
                if (IsToneSceneFormat(uint32_t(viewDesc.Format)) && desc.ArraySize == 1 && desc.SampleDesc.Count == 1 &&
                    desc.Width >= kMinToneWidth && desc.Height >= kMinToneHeight)
                {
                    scene.resource = texture.Get();
                    scene.format = uint32_t(viewDesc.Format);
                    scene.width = desc.Width;
                    scene.height = desc.Height;
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
    if (scene.resource == nullptr)
        return;

    ComPtr<ID3D11ShaderResourceView> viewPosition;
    ComPtr<ID3D11ShaderResourceView> normal;
    ComPtr<ID3D11Buffer> camera;
    context->PSGetShaderResources(inputs.vposition, 1, &viewPosition);
    context->PSGetShaderResources(inputs.targets[3], 1, &normal);
    context->PSGetConstantBuffers(inputs.camera, 1, &camera);
    const BoundTexture positions = Describe(viewPosition.Get());
    const BoundTexture normals = Describe(normal.Get());
    if (!positions.twoD || !normals.twoD || !camera)
        return;
    // The positions are the march's own buffer and the normal is the up-facing test's: both at the scene's size, or the tone map is a shape this
    // stage cannot read.
    if (positions.width != scene.width || positions.height != scene.height || normals.width != scene.width ||
        normals.height != scene.height)
        return;
    if (!IsToneSceneFormat(positions.format))
        return;
    D3D11_BUFFER_DESC cameraDesc {};
    camera->GetDesc(&cameraDesc);
    if (cameraDesc.ByteWidth < kCameraBytes)
        return;

    state.capture.taken = true;
    ++state.capture.id;
    state.capture.frameToken = ext::present::FrameCount(); // the frame token the pairing asks the gloss pass about
    state.capture.shapeWet = ShapeOnFrame(state.capture.frameToken); // the pairing evaluated ONCE, here at the composite
    state.capture.viewPosition = viewPosition;
    state.capture.normal = normal;
    state.capture.camera = camera;
    state.capture.viewPositionFormat = positions.format;
    state.capture.viewPositionWidth = positions.width;
    state.capture.viewPositionHeight = positions.height;
    state.capture.normalFormat = normals.format;
    state.capture.normalWidth = normals.width;
    state.capture.normalHeight = normals.height;
    state.capture.cameraBytes = cameraDesc.ByteWidth;
    state.capture.sceneResource = scene.resource;
    state.capture.sceneWidth = scene.width;
    state.capture.sceneHeight = scene.height;
    state.capture.sceneFormat = scene.format;
    state.captures++;
}

// The tone map's own shape, measured at the draw: one FP16 render target that is not the scene, and a t0 that is the scene the frame's composite
// wrote. The five inputs the stage reads come from the capture, not from this draw.
ToneFacts MeasureToneMap(ID3D11DeviceContext* context, const State& state, ComPtr<ID3D11ShaderResourceView>& scene)
{
    const Capture& capture = state.capture;
    ToneFacts facts;
    ID3D11RenderTargetView* views[2] { nullptr, nullptr };
    context->OMGetRenderTargets(2, views, nullptr);
    facts.renderTargets = (views[0] != nullptr ? 1u : 0u) + (views[1] != nullptr ? 1u : 0u);
    if (views[0] != nullptr && views[1] == nullptr)
    {
        D3D11_RENDER_TARGET_VIEW_DESC viewDesc {};
        views[0]->GetDesc(&viewDesc);
        ComPtr<ID3D11Resource> resource;
        views[0]->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (resource != nullptr && SUCCEEDED(resource.As(&texture)) && texture != nullptr)
        {
            D3D11_TEXTURE2D_DESC desc {};
            texture->GetDesc(&desc);
            facts.targetWidth = desc.Width;
            facts.targetHeight = desc.Height;
            facts.targetFormat = uint32_t(viewDesc.Format);
            facts.targetIsScene = texture.Get() == capture.sceneResource;
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
            facts.sceneIsScene = texture.Get() == capture.sceneResource;
        }
    }
    if (capture.taken)
    {
        facts.viewPositionSampled = true;
        facts.viewPositionWidth = capture.viewPositionWidth;
        facts.viewPositionHeight = capture.viewPositionHeight;
        facts.normalSampled = true;
        facts.normalWidth = capture.normalWidth;
        facts.normalHeight = capture.normalHeight;
        facts.cameraBytes = capture.cameraBytes;
    }

    D3D11_VIEWPORT viewport {};
    UINT count = 1;
    context->RSGetViewports(&count, &viewport);
    facts.window[0] = viewport.TopLeftX;
    facts.window[1] = viewport.TopLeftY;
    facts.window[2] = viewport.Width;
    facts.window[3] = viewport.Height;
    facts.replay = ext::ffxiv::chara::t_replay;
    return facts;
}

// Whether this frame has a wetness to shape at all. With none, the scene the game bound is the picture, so nothing is copied and nothing is drawn - and
// the line says the stage was skipped for the weather, not that it ran.
bool ToneWet(State& state)
{
    const Wetness wetness = CurrentWetness();
    // Stepped on dry frames too: the frame the rain starts must see the amount it last saw.
    const float now = RippleSeconds();
    const float delta = now - state.rainStamp;
    state.rainStamp = now;
    RainStep(state.rain, wetness.engine, wetness.engineKnown, delta);
    if (state.rain.raining != state.loggedRaining)
    {
        state.loggedRaining = state.rain.raining;
        LOG_INFO("Puddles: {} (the engine's wetness {:.2f}), so the pools' splash rings {}", state.rain.raining ? "it is raining" : "it is not raining",
                 double(wetness.engine), state.rain.raining ? "come in" : "die away");
    }
    // The dwell (GlossPuddlesRules.h's WetEnoughDwell) is the gloss pass's shared latch now: one update a frame, on
    // the frame's own wetness copy, and both stages read it. A stale-low read-back at the ramp's crossing therefore
    // holds both stages armed together - the tone stage cannot answer a frame the gloss stage's readers were dry on,
    // which is one of the disagreement frames the pool's flash showed through.
    if (WetLatched())
        return true;
    if (!state.loggedDry)
    {
        state.loggedDry = true;
        LOG_INFO("Puddles: the frame has no wetness to shape ({}), so the pool's colour is the engine's own and the tone-map stage is skipped",
                 wetness.known ? "the engine's amount is 0 and the wetness knob is off" : "the engine's amount has not been read yet");
    }
    return false;
}

// The pass's inputs for one frame: the scene the caller hands over (the engine's own at the fallback, the light shafts' air-lit copy in the chain),
// the inputs the frame's composite recorded, and the keys. False when the gloss pass's wetness buffer does not exist, which is a counted refusal.
bool ToneInputs(State& state, const ToneFacts& facts, ID3D11ShaderResourceView* scene, ID3D11Texture2D* sceneTexture,
                TonePassInputs& inputs)
{
    ID3D11ShaderResourceView* wetnessView = WetnessResource();
    if (wetnessView == nullptr)
        return false;
    inputs.scene = scene;
    inputs.sceneTexture = sceneTexture;
    inputs.viewPosition = state.capture.viewPosition.Get();
    inputs.normal = state.capture.normal.Get();
    inputs.camera = state.capture.camera.Get();
    inputs.wetness = wetnessView;
    inputs.facts = facts;
    inputs.params = state.params;
    inputs.stats = ext::ffxiv::diag::Capturing();
    // The raindrops' clock: read here, once per run, so the pool's splash rings animate with real time and a run that hands the pass no clock
    // (a host test) keeps the still ripple. The value is kept, so a caller that reproduces this frame's picture can ask for it.
    state.lastRippleSeconds = RippleSeconds();
    inputs.timeSeconds = state.lastRippleSeconds;
    inputs.rain = state.rain.amount;
    return true;
}

// One run of the stage, whichever order reached it: the counters, the card's memory and the one line a session gets.
void NoteToneRun(State& state, const ToneFacts& facts)
{
    state.runs++;
    state.haveRun = true;
    state.lastSceneWidth = facts.sceneWidth;
    state.lastSceneHeight = facts.sceneHeight;
    for (size_t i = 0; i < 4; ++i)
        state.lastWindow[i] = facts.window[i];
    state.session.NoteRun();
    if (!state.loggedRun)
    {
        state.loggedRun = true;
        LOG_INFO("{}", FormatToneRun(state.params, facts, state.pass.Stats(), state.pass.StatsKnown(), state.pass.HasGpuMilliseconds(),
                                     state.pass.GpuMilliseconds()));
    }
}

// The pass inside this module's own guards: the character preview's marker keeps its draws out of the replay accounting, and the module's own guard
// keeps the whole draw seam away from them. A null answer is the fail-open one and its reason is logged once a session.
ID3D11ShaderResourceView* RunPass(State& state, ID3D11DeviceContext* context, const TonePassInputs& inputs)
{
    ID3D11ShaderResourceView* ours = nullptr;
    {
        ext::ffxiv::chara::ReplayScope replay;
        OwnWork own;
        ours = state.pass.Run(context, inputs);
    }
    if (ours == nullptr)
    {
        state.failure = state.pass.Failure();
        if (state.failure != nullptr && !state.loggedFailure)
        {
            state.loggedFailure = true;
            LOG_WARN("Puddles: the tone-map pass did not run ({})", state.failure);
        }
    }
    return ours;
}

// The tone map's shape as this stage reads it, out of the light shafts' own measurement plus the composite's capture.
ToneFacts ToneFactsOf(const shafts::ToneMapFacts& facts, const Capture& capture)
{
    ToneFacts mine;
    mine.renderTargets = facts.renderTargets;
    mine.targetWidth = facts.targetWidth;
    mine.targetHeight = facts.targetHeight;
    mine.targetFormat = facts.targetFormat;
    mine.targetIsScene = facts.targetIsScene;
    mine.sceneFormat = facts.sceneFormat;
    mine.sceneWidth = facts.sceneWidth;
    mine.sceneHeight = facts.sceneHeight;
    mine.sceneSampled = facts.sceneSampled;
    mine.sceneIsScene = facts.sceneIsScene;
    mine.window[0] = facts.window.x;
    mine.window[1] = facts.window.y;
    mine.window[2] = facts.window.width;
    mine.window[3] = facts.window.height;
    mine.replay = facts.replay;
    if (capture.taken)
    {
        mine.viewPositionSampled = true;
        mine.viewPositionWidth = capture.viewPositionWidth;
        mine.viewPositionHeight = capture.viewPositionHeight;
        mine.normalSampled = true;
        mine.normalWidth = capture.normalWidth;
        mine.normalHeight = capture.normalHeight;
        mine.cameraBytes = capture.cameraBytes;
    }
    return mine;
}

// One frame at the tone map. The game's draw runs exactly once whatever happens below: a throw after it must not let the seam run it a second time.
bool RunToneMap(ID3D11DeviceContext* context, engine::DrawFn draw, void* user)
{
    State& state = S();
    if (state.inPass.load(std::memory_order_relaxed) || !state.session.Armed())
        return false;
    bool drew = false;
    try
    {
        ComPtr<ID3D11ShaderResourceView> scene;
        const ToneFacts facts = MeasureToneMap(context, state, scene);
        const ToneDecline shape = ClassifyTone(facts);
        if (shape == ToneDecline::NotToneMap)
            return false; // another draw of the tone map's family (or the post-tone copy): the game's own call, nothing counted
        // The scene HDR path replays the engine's own tone chain in its own branch, so the scene this stage would shade is not the picture that path
        // presents. The shafts' branch never sees a tone-map draw while HDR is on; this fallback asks the same key itself, as the shafts' own arming
        // does. Nothing is counted against the module - a configuration is not a build it cannot serve - and a session that turns HDR off gets the
        // stage back without arming anything again.
        if (shafts::SceneHdrInactive(state.params.on, ext::hdr::FfxivSceneHdr::Requested()))
        {
            if (!state.loggedSceneHdr)
            {
                state.loggedSceneHdr = true;
                LOG_INFO("{}", kToneSceneHdrInactive);
            }
            return false; // the seam runs the game's own draw
        }
        // One answer per composite: no composite since the last tone map (a cut, a menu, a second view) leaves the stage nothing to reflect, and
        // a second tone-shaped draw of the same frame is the engine's own post-tone copy. Both are the game's own call, and neither is counted.
        if (!state.capture.taken || state.capture.id == state.usedCapture)
        {
            if (!state.capture.taken && !state.loggedInputs)
            {
                state.loggedInputs = true;
                LOG_INFO("Puddles: no composite of this frame bound the view positions, so the pool's colour is the engine's own until one does");
            }
            return false;
        }
        if (shape != ToneDecline::None)
        {
            Refuse(shape);
            if (!drew)
                draw(user);
            drew = true;
            return true;
        }

        // The dry identity, at the copy: with no wetness to shape, the scene the game bound is the picture, so nothing is copied and nothing is
        // drawn (and the line says the stage was skipped for the wetness, not that it ran).
        if (!ToneWet(state))
        {
            if (!drew)
                draw(user);
            drew = true;
            return true;
        }

        // The frame's answer pairs with the gloss stage's (GlossPuddlesRules.h's ToneAnswerFor, via ShapeOnFrame): the
        // stage runs only when the frame's composite carries the gloss stage's own shape. When it does not - a gloss
        // refuse the pass's hold could not answer, or the ramp's first frame - or when the pass itself could not run,
        // the stage binds its OWN last good answer at t0 instead: a skip in the middle of the ramp must never show the
        // engine's own picture. The flip between those two looks - the pool's darkening over whatever is under it, and
        // the plain wet ground - was the pool's black frame (the ramp reads it once per stride the two stages were
        // asked to answer apart); a skip held on the last answer paints neither, so the pool's look never pops.
        ID3D11ShaderResourceView* ours = nullptr;
        bool ran = false;
        const ToneAnswer answer = ToneAnswerFor(WetLatched(), state.capture.shapeWet, state.glossAlive,
                                                state.pass.OutputReady(facts.sceneWidth, facts.sceneHeight));
        if (answer == ToneAnswer::Run)
        {
            TonePassInputs inputs;
            if (ToneInputs(state, facts, scene.Get(), Describe(scene.Get()).texture, inputs))
                ours = RunPass(state, context, inputs);
            ran = ours != nullptr;
            if (ours == nullptr)
                Refuse(ToneDecline::PassFailed);
        }
        ComPtr<ID3D11Device> holdDevice;
        context->GetDevice(&holdDevice); // the hold must never answer with a dead device's texture
        if (ours == nullptr && answer != ToneAnswer::None && !GlossCameraRejected() &&
            state.pass.OutputReady(facts.sceneWidth, facts.sceneHeight) && state.pass.OnDevice(holdDevice.Get()))
        {
            ours = state.pass.OutputView(); // the hold: the last good answer, whole picture, at t0 for this one draw
            state.held++;
        }
        if (ours == nullptr)
        {
            // No answer: the ramp's first frame, or a gloss side that can no longer shape (an unservable key, the card's
            // own identity). Both stages' misses are the same clean answer, and the engine's picture is what a dead gloss
            // pairing falls back to - the frozen pre-change look would outlive the change. The engine's picture is what
            // the tone map reads for this frame, once a frame.
            if (!drew)
                draw(user);
            drew = true;
            return true;
        }

        // The copy at t0 for exactly one draw, and the game's own binding back afterwards - also when that draw throws.
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

        // The frame is answered whatever the draw below comes to, so the next tone-shaped draw of the frame is the game's own call.
        state.usedCapture = state.capture.id;
        draw(user);
        drew = true;

        if (ran)
            NoteToneRun(state, facts); // the hold is not a run: the stats and the line read the last pass's own answer
        return true;
    }
    catch (...)
    {
        Refuse(ToneDecline::Exception);
        if (!drew)
            draw(user);
        return true;
    }
}
} // namespace

bool ToneWants(const Params& params) { return !ToneIdentity(params); }

bool OnToneStageDraw(ID3D11DeviceContext* context, engine::Anchors anchors, engine::DrawFn draw, void* user)
{
    if (draw == nullptr || context == nullptr)
        return false;
    if (!engine::IsShaftsCompositeAnchors(anchors) && !engine::IsShaftsToneMapAnchors(anchors))
        return false; // one bit test for every other draw of the frame
    State& state = S();
    if (!ToneWants(state.params) || !state.session.Armed())
        return false;
    if (!state.haveRenderThread)
    {
        state.renderThread = std::this_thread::get_id();
        state.haveRenderThread = true;
    }
    try
    {
        if (engine::IsShaftsCompositeAnchors(anchors))
        {
            NoteComposite(context, state);
            return false; // the game's own draw still has to run
        }
        return RunToneMap(context, draw, user);
    }
    catch (...)
    {
        LOG_INFO("Puddles: the tone map was left alone ({})", ToneDeclineName(ToneDecline::Exception));
    }
    return false;
}

bool ToneArmedForChain()
{
    State& state = S();
    // No device work and no key read beyond the published params: the light shafts ask this on every tone map they draw.
    return ToneWants(state.params) && state.session.Armed() && state.capture.taken && state.capture.id != state.usedCapture;
}

ID3D11ShaderResourceView* OnShaftsScene(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene)
{
    State& state = S();
    if (context == nullptr || scene == nullptr || !ToneArmedForChain())
        return nullptr;
    if (!state.haveRenderThread)
    {
        state.renderThread = std::this_thread::get_id();
        state.haveRenderThread = true;
    }
    // The shafts measured the same draw this stage would have: their shape is classified by the same rule, and a shape this stage cannot answer
    // (a replay, a window that does not fit) leaves their copy at t0 without counting anything - the frame is not one this build failed at.
    const ToneFacts mine = ToneFactsOf(facts, state.capture);
    if (ClassifyTone(mine) != ToneDecline::None)
        return nullptr;
    if (!ToneWet(state))
        return nullptr; // the shafts' own air-lit scene is the picture: a dry frame is shaped by nothing, and nothing is copied for it
    try
    {
        // The same pairing the seam's own call walks (GlossPuddlesRules.h's ToneAnswerFor): run the stage on the air-lit
        // scene when the frame's composite carries the gloss stage's shape, and hold the last answer when it does not -
        // a mid-ramp skip binding the shafts' own copy is the pool's black frame again, and has no last answer to hold
        // on the ramp's first frame, whose answer is the clean one (both stages' absence), so nothing is held.
        ID3D11ShaderResourceView* ours = nullptr;
        bool ran = false;
        const ToneAnswer answer = ToneAnswerFor(WetLatched(), state.capture.shapeWet, state.glossAlive,
                                                state.pass.OutputReady(mine.sceneWidth, mine.sceneHeight));
        if (answer == ToneAnswer::Run)
        {
            TonePassInputs inputs;
            if (ToneInputs(state, mine, scene, Describe(scene).texture, inputs))
                ours = RunPass(state, context, inputs);
            ran = ours != nullptr;
            if (ours == nullptr)
                Refuse(ToneDecline::PassFailed);
        }
        ComPtr<ID3D11Device> holdDevice;
        context->GetDevice(&holdDevice); // the hold must never answer with a dead device's texture
        if (ours == nullptr)
        {
            if (answer == ToneAnswer::None || GlossCameraRejected() ||
                !state.pass.OutputReady(mine.sceneWidth, mine.sceneHeight) || !state.pass.OnDevice(holdDevice.Get()))
                return nullptr; // the ramp's first frame, a dead gloss pairing or camera, or no whole answer: the shafts' own copy is what the tone map reads
            ours = state.pass.OutputView(); // the hold: the last answer, whole picture, over the frame's own t0
            state.held++;
        }
        // The frame is answered: the seam's own call is the fallback for a tone map the chain did not answer, never a second pass over it.
        state.usedCapture = state.capture.id;
        if (ran)
            NoteToneRun(state, mine);
        return ours;
    }
    catch (...)
    {
        Refuse(ToneDecline::Exception);
        return nullptr;
    }
}

void OnShaftsSceneDraw(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene, engine::DrawFn draw,
                       void* user)
{
    // The stage's answer, or nothing when its card is off, the frame is dry, or it cannot serve the shape the shafts measured: the game's own draw
    // then reads the scene it was handed.
    ID3D11ShaderResourceView* pooled = OnShaftsScene(context, facts, scene);
    ID3D11ShaderResourceView* bind = pooled != nullptr ? pooled : scene;
    if (context == nullptr || draw == nullptr)
        return;
    ID3D11ShaderResourceView* original = nullptr;
    context->PSGetShaderResources(0, 1, &original);
    context->PSSetShaderResources(0, 1, &bind);
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
}

void PublishTone(const Params& params)
{
    State& state = S();
    state.params = params;
    // The pairing's liveness: a gloss card that cannot shape (an unservable target/channel, the identity) ends the
    // hold - the tone stage answers the engine's picture from that frame on instead of the last pre-change look.
    state.glossAlive = params.on && Wants(params);
    state.capture = Capture {}; // the recorded inputs belong to the tuning and the session they were taken in
    state.usedCapture = 0;
    state.loggedRun = false;
    state.loggedDry = false;
    state.loggedFailure = false;
    state.loggedInputs = false;
    state.loggedSceneHdr = false;
    if (!params.on)
    {
        state.session = Session {};
        state.log = LogGate {};
        state.loggedOff = false;
        state.rain = RainTracker {};
        state.rainStamp = 0.0f;
        // the dwell is the card's own arming: a publish-off is a fresh dry as far as it is concerned, and the shared
        // latch and the hold live on the gloss pass, whose own reset the puddles publish takes care of.
        state.loggedRaining = false;
        state.runs = 0;
        state.held = 0;
        state.captures = 0;
        state.declined = 0;
        for (uint32_t& count : state.declines)
            count = 0;
        state.haveRun = false;
        state.pass.Reset();
    }
    else if (state.session.off)
    {
        state.session = Session {}; // turning the switch back on after a switch-off is the player's re-arm
    }
}

void ReleaseTone()
{
    State& state = S();
    state.capture = Capture {};
    state.usedCapture = 0;
    state.pass.Reset();
}

float LastRippleSeconds() { return S().lastRippleSeconds; }

float LastRainAmount() { return S().rain.amount; }

ToneStatus CurrentToneStatus()
{
    State& state = S();
    ToneStatus status;
    status.identity = ToneIdentity(state.params);
    status.armed = state.params.on && !status.identity && state.session.Armed();
    status.runs = state.runs;
    status.declined = state.declined;
    status.held = state.held;
    status.captures = state.captures;
    status.statsKnown = state.pass.StatsKnown();
    status.poolShare = state.pass.Stats().PoolShare();
    status.hitShare = state.pass.Stats().HitShare();
    return status;
}

ToneDiagnostics CurrentToneDiagnostics()
{
    State& state = S();
    const ToneStatus status = CurrentToneStatus();
    ToneDiagnostics diagnostics;
    diagnostics.key = state.params.on;
    diagnostics.armed = status.armed;
    diagnostics.identity = status.identity;
    diagnostics.runs = state.runs;
    diagnostics.declined = state.declined;
    for (uint32_t i = 0; i < kToneDeclineReasonCount; ++i)
        diagnostics.declines[i] = state.declines[i];
    diagnostics.statsKnown = status.statsKnown;
    diagnostics.poolShare = status.poolShare;
    diagnostics.hitShare = status.hitShare;
    diagnostics.haveRun = state.haveRun;
    diagnostics.sceneWidth = state.lastSceneWidth;
    diagnostics.sceneHeight = state.lastSceneHeight;
    return diagnostics;
}

ToneFrameObservation CurrentToneFrameObservation()
{
    State& state = S();
    ToneFrameObservation out;
    out.captureToken = state.capture.frameToken;
    out.captureTaken = state.capture.taken;
    out.shapeWet = state.capture.shapeWet;
    out.runs = state.runs;
    out.declined = state.declined;
    return out;
}
} // namespace ext::ffxiv::engine::gloss
