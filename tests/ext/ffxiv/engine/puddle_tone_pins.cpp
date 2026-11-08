// Text pins of the puddles' tone-map stage: where it hangs, what it must never do, and that its keys default to today's picture.
//
// The things pinned here are the ones a picture cannot separate from their absence: the stage answers one composite per tone map (the engine's
// post-tone copy of the scene is not the tone map), a frame with no wetness is skipped before the copy rather than copied and drawn, a replay of
// a game draw never reaches the pass, the game's own t0 comes back after the one draw, and the light shafts own the tone map's binding - they run
// this stage on their own air-lit output before binding t0, and the seam's own call is the fallback for a frame they left alone. The scene HDR key
// and the wetness both stages read are pinned here too, because neither can be seen in a picture that is not running.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
int g_failures = 0;
#define EXPECT(cond, ...)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::printf("    FAIL line %d: %s  ", __LINE__, #cond);                                                    \
            std::printf(__VA_ARGS__);                                                                                  \
            std::printf("\n");                                                                                         \
        }                                                                                                              \
    } while (0)

std::string ReadFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::string Raw(const std::string& repo, const char* relative) { return ReadFile(repo + "/OptiScaler/ext/" + relative); }

// Comments and whitespace removed, so a pin is about the code and not about its formatting.
std::string Comments(const std::string& text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text.compare(i, 2, "//") == 0)
        {
            while (i < text.size() && text[i] != '\n')
                ++i;
            continue;
        }
        if (text.compare(i, 2, "/*") == 0)
        {
            const size_t end = text.find("*/", i + 2);
            i = end == std::string::npos ? text.size() : end + 1;
            continue;
        }
        out += text[i];
    }
    return out;
}

std::string Code(const std::string& repo, const char* relative)
{
    std::string text = Comments(Raw(repo, relative));
    std::string out;
    for (char c : text)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            out += c;
    return out;
}

bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

size_t Count(const std::string& text, const std::string& needle)
{
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
        ++count;
    return count;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: puddle_tone_pins <repo>\n");
        return 2;
    }
    const std::string repo = argv[1];
    const std::string draw = Code(repo, "ffxiv/engine/PuddleToneDraw.cpp");
    const std::string pass = Code(repo, "ffxiv/engine/PuddleTonePass.cpp");
    const std::string passHeader = Code(repo, "ffxiv/engine/PuddleTonePass.h");
    const std::string rules = Code(repo, "ffxiv/engine/GlossPuddlesRules.h");
    const std::string apply = Code(repo, "ffxiv/engine/GlossPuddlesApply.cpp");
    const std::string glossPassHeader = Code(repo, "ffxiv/engine/GlossPuddlesPass.h"); // where the pairing's state lives
    const std::string enginePass = Code(repo, "ffxiv/engine/EnginePass.cpp");
    const std::string menu = Code(repo, "ffxiv/engine/GlossPuddlesMenu.cpp");
    const std::string shader = Code(repo, "ffxiv/engine/shaders/PuddleReflection.hlsl");
    const std::string march = Code(repo, "ffxiv/engine/shaders/SceneMarch.hlsl");
    const std::string ssrShader = Code(repo, "ffxiv/engine/shaders/Ssr.hlsl");
    const std::string config = Code(repo, "ffxiv/FfxivConfig.h");
    const std::string ini = Raw(repo, "ini/FFXIV.ini");

    // ---- the one owner of the tone map's t0: the shafts run the stage on their own output, and the seam's call is the fallback
    {
        EXPECT(Count(enginePass, "gloss::OnToneStageDraw(") == 1, "the stage is called from exactly one place");
        EXPECT(Has(enginePass, R"x(if(ext::ffxiv::shafts::OnEngineDraw(context,anchors,draw,user))return;if(gloss::OnToneStageDraw(context,anchors,draw,user))return;)x"),
               "the shafts' branch comes first: it owns the tone map, and its air light is part of the scene the pool reflects");
        // The chain itself: the shafts ask whether the stage has anything to do, and run it on their own output before t0 is rebound.
        const std::string shafts = Code(repo, "ffxiv/engine/ShaftsDraw.cpp");
        EXPECT(Has(shafts, R"x(ext::ffxiv::engine::gloss::OnShaftsSceneDraw(context,facts,copy,draw,user);)x"),
               "the shafts' pass hands its own air-lit scene to this stage and draws through its one binding of t0, which keeps the shafts' copy when "
               "the stage answers nothing");
        EXPECT(Has(shafts, R"x(ext::ffxiv::engine::gloss::OnShaftsSceneDraw(context,facts,scene,draw,user);)x"),
               "and every frame the shafts leave as they were draws through it too, so turning the shafts on cannot turn the pools off");
        EXPECT(Count(shafts, "OnShaftsSceneDraw(") == 2, "one module owns t0: the shafts' own run and their skipped frames share the stage's entry");
        EXPECT(Has(draw, R"x(boolToneArmedForChain(){State&state=S();)x"), "the chain's gate is the stage's own (the card, the session, a composite no tone map has answered)");
        EXPECT(Has(draw, R"x(returnToneWants(state.params)&&state.session.Armed()&&state.capture.taken&&state.capture.id!=state.usedCapture;)x"),
               "and it asks for a composite this frame that no tone map has spent yet");
        EXPECT(Has(draw, R"x(state.usedCapture=state.capture.id;if(ran)NoteToneRun(state,mine);returnours;)x"),
               "a chained run stamps the composite it answered, so the seam's own call cannot run a second pass over the same frame (the run line's "
               "counters move on a run, never on a held frame)");
        EXPECT(Has(draw, R"x(if(!state.capture.taken||state.capture.id==state.usedCapture){)x"),
               "and the fallback passes on a tone map the chain already answered");
        EXPECT(Has(draw, R"x(constToneFactsmine=ToneFactsOf(facts,state.capture);if(ClassifyTone(mine)!=ToneDecline::None)returnnullptr;)x"),
               "the chain classifies the shape the shafts measured with the same rule, and answers nothing it cannot serve");
    }

    // ---- scene HDR: the fallback asks the key the shafts' own arming asks, and nothing is counted against the module for it
    {
        EXPECT(Has(draw, R"x(if(shafts::SceneHdrInactive(state.params.on,ext::hdr::FfxivSceneHdr::Requested())){)x"),
               "the fallback declines while the scene HDR path owns the tone chain");
        EXPECT(Has(draw, R"x(state.loggedSceneHdr=true;LOG_INFO("{}",kToneSceneHdrInactive);)x"), "with one line, once per publish");
        EXPECT(!Has(rules, R"x(ToneDecline::SceneHdr)x"), "and not as a counted refusal: a configuration is not a build this module cannot serve");
    }

    // ---- the capture: the composite's own draw, and only when it declares everything the stage reads
    {
        EXPECT(Has(draw, R"x(if(!inputs.HasViewPosition()||!inputs.HasCamera()||!inputs.HasTarget(3))return;)x"),
               "the capture needs the view positions, the camera table and the G-buffer normal");
        EXPECT(Has(draw, R"x(if(!engine::IsShaftsCompositeAnchors(anchors)&&!engine::IsShaftsToneMapAnchors(anchors))returnfalse;)x"),
               "every other draw of the frame costs one bit test");
        EXPECT(Has(draw, R"x(engine::IsShaftsCompositeAnchors(anchors))x") && Has(draw, R"x(NoteComposite(context,state);)x"),
               "the composite is where the frame's inputs are recorded");
    }

    // ---- one answer per composite: the engine's post-tone copy of the scene is not the tone map
    {
        EXPECT(Has(draw, R"x(if(!state.capture.taken||state.capture.id==state.usedCapture){)x"),
               "a frame with no new composite answers nothing, and a second tone-shaped draw is the game's own");
        EXPECT(Has(draw, R"x(state.usedCapture=state.capture.id;)x"), "the answer is stamped with the composite it answered");
        EXPECT(Has(draw, R"x(if(shape==ToneDecline::NotToneMap)returnfalse;)x"),
               "a draw that is not the tone map is passed on before anything is counted");
    }

    // ---- the dry identity is skipped before the copy, and the replay gate is the draw's own fact
    {
        EXPECT(Has(draw, R"x(if(!ToneWet(state)){if(!drew)draw(user);drew=true;returntrue;})x"),
               "a frame with no wetness is skipped before the copy, with one line that says so");
        // The floor itself: a frame whose engine amount is read and zero is as dry as one whose amount has not landed, and neither may be copied.
        // The latch is the shared one the pass owns (the pairing block below pins its rule); the stage's own copy is gone.
        EXPECT(Has(draw, R"x(if(WetLatched())returntrue;)x"),
               "the wetness floor is what makes a dry frame the scene rather than a copy of it, and one latch answers it for the two stages");
        // The chained order's own gate: the shafts' output is the picture on a dry frame, so the stage answers nothing and nothing is copied.
        EXPECT(Has(draw, R"x(if(!ToneWet(state))returnnullptr;)x"),
               "the chained run is skipped before the copy when the frame has no wetness");
        EXPECT(Has(draw, R"x(facts.replay=ext::ffxiv::chara::t_replay;)x"), "the replay marker is read at the draw");
        EXPECT(Has(rules, R"x(if(facts.replay)returnToneDecline::Replay;)x"), "and the pure rule refuses a replay first");
    }

    // ---- the game's own t0 comes back after the one draw, also when it throws
    {
        EXPECT(Has(draw, R"x(context->PSGetShaderResources(0,1,&original);context->PSSetShaderResources(0,1,&ours);)x"),
               "the copy is bound at t0 for one draw");
        EXPECT(Has(draw, R"x(context->PSSetShaderResources(0,1,&view);)x"), "and the game's own view is put back");
        EXPECT(Has(draw, R"x(context->PSSetShaderResources(0,1,&bind);)x"),
               "the chained entry binds the stage's texture at t0 for the one draw, or the scene it was handed when the stage answered nothing");
        EXPECT(Count(draw, "draw(user);") == 6,
               "the game's draw runs exactly once on every path (five branches - the shape refusal, the dry skip, the hold that has no answer yet, "
               "the run, and the catch - with its own call each, and the chained entry)");
    }

    // ---- the frame's answers pair: a run rides a shaped composite, a skip holds the last answer, the hold answers
    //      a refused gloss attempt; and the wetness latch is one bit the pass owns
    {
        EXPECT(Has(rules, R"x(enumclassToneAnswer:uint8_t{None=0,Hold,Run,};)x"), "the stage's wet answer is a rule the host can pin: run, hold, or none");
        EXPECT(Has(rules, R"x(constexprToneAnswerToneAnswerFor(boolwet,boolshapeOnFrame,boolglossAlive,boolhaveLastAnswer)noexcept)x"),
               "a wet frame runs when the composite carries the gloss stage's shape, holds the last answer when it does not, and answers nothing only "
               "when there is no last answer yet");
        EXPECT(Has(rules, R"x(constexprboolAnswersSameBinding(constvoid*holdResource,constvoid*holdView,constvoid*boundResource,constvoid*boundView)noexcept)x"),
               "the hold answers the reader draws that still bind the object the shape was computed for (the same view first, then the same resource)");
        EXPECT(Has(rules, R"x(constexprboolHoldAnswersShape(boollatched,boolhaveShape,boolcameraServable)noexcept)x"),
               "the hold needs the wetness latched, a whole output and a camera the layout check has not refused");
        // The gloss pass owns the pairing state (one object, the same wetness buffer between the two stages), so a stage cannot disagree with the other.
        EXPECT(Has(glossPassHeader, R"x(voidNoteWetLatch(boollatched)noexcept{_wetLatch=latched;})x") && Has(glossPassHeader, R"x(boolWetLatched()constnoexcept{return_wetLatch;})x"),
               "the wetness latch is the gloss pass's own bit, so the two stages read one answer");
        EXPECT(Has(glossPassHeader, R"x(voidNoteShapeFrame(uint64_ttoken,boolshaped)noexcept)x") && Has(glossPassHeader, R"x(boolShapePresent(uint64_ttoken)constnoexcept)x"),
               "the frame whose composite reads the shape is stamped on the gloss pass, for the tone stage's pairing to ask");
        EXPECT(Has(glossPassHeader, R"x(voidNoteHold(constvoid*engineResource,constvoid*engineView)noexcept)x"), "the hold lives with the pass that serves it");
        // The draw path stamps the frame at the composite and asks about that frame - both stages answer one frame.
        EXPECT(Has(draw, R"x(state.capture.frameToken=ext::present::FrameCount();)x"),
               "the capture carries the frame token, the same counter the draw path's frame cache is stamped with");
        EXPECT(Has(draw, R"x(state.capture.shapeWet=ShapeOnFrame(state.capture.frameToken);)x") &&
                   Count(draw, "ShapeOnFrame(") == 1,
               "the pairing is evaluated ONCE, at the composite, and carried in the capture: a present between the gloss stamp and the tone map cannot tick the counter between the two reads (the review's token-skew finding)");
        EXPECT(Has(draw, R"x(if(WetLatched())returntrue;)x"),
               "the tone stage's wetness gate is the pass's shared latch, not a bit of its own");
        EXPECT(Has(apply, R"x(NoteShapeFrame(token,state.frame.valid);)x") && Has(apply, R"x(NoteShapeFrame(token,false);)x"),
               "the swap path stamps the frame shaped (its own answer or the hold's) and unshapes it when neither is there");
        // The refused attempt answers from the hold instead of the engine's unshaped target - the pools' two answers
        // then never show the flip between shaped and unshaped on the ramp.
        EXPECT(Has(apply, R"x(HoldAnswersShape(state.pass.WetLatched(),state.pass.OutputReady(target.width,target.height),!state.pass.CameraRejected)x") &&
                   Has(apply, R"x(state.pass.OnDevice(holdDevice.Get()))x"), "the gloss refuse's gates are the pure rule, of the frame's own size, on the live device");
        EXPECT(Has(apply, R"x(AnswersSameBinding(state.pass.HoldEngineResource(),state.pass.HoldEngineView(),target.resource,target.view))x"), "and so is the binding it answers on");
        EXPECT(Has(apply, R"x(state.frame.Record(target.resource,target.view,state.pass.OutputView());)x"),
               "the hold serves this frame's later readers through the frame cache it was recorded in");
        EXPECT(Has(apply, R"x(NoteHold(target.resource,target.view);)x") && Count(apply, "NoteHold(") == 2,
               "every pass that ran records its shape as the next frame's hold, and the refused one re-records its own");
        EXPECT(Has(apply, R"x(NoteWetLatch(WetEnoughDwell(state.pass.WetLatched(),wet.known,wet.amount)))x"),
               "the latch is answered on the frame's own copy, once, wherever it landed");
        EXPECT(Has(apply, R"x(boolWetLatched(){returnS().pass.WetLatched();})x") && Has(apply, R"x(boolShapeOnFrame(uint64_ttoken){returnS().pass.ShapePresent(token);})x"),
               "the tone stage reads the pairing through the apply module's door, like the wetness buffer it binds");
        // The skips hold their answer on the tone side (the fallback and the chain both do; the count above is two).
        EXPECT(Has(passHeader, R"x(boolOutputReady(uint32_twidth,uint32_theight)constnoexcept{return_outValid&&_width==width&&_height==height;})x"), "the tone pass says whether its last answer is a whole picture of the frame's own size (a hold bound across a resize is the review's F5)");
        EXPECT(Count(draw, "ours=state.pass.OutputView();state.held++;") == 2,
               "the fallback and the chain hold the last answer and count the frame as held, not as a run");
        EXPECT(Has(draw, R"x(if(ran)NoteToneRun(state,mine);)x") && Has(draw, R"x(if(ran)NoteToneRun(state,facts);)x"),
               "the run's counters move on a run, never on a held frame");
    }

    // ---- the pass itself: an explicit state guard, no readback on the draw path, the window's copy, the statistics only while a capture is armed
    {
        EXPECT(Has(pass, R"x(ext::core::explicit_state::GuardstateGuard(context,context1.Get(),device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1,kSpec);)x"),
               "the draw runs between an explicit save and restore of the state the pass touches, and the game's state comes back through a scope guard");
        EXPECT(Has(pass, R"x(stateGuard.Neutralise();)x") && !Has(pass, "SwapDeviceContextState") && !Has(pass, "CreateDeviceContextState"),
               "what the private state's defaults provided (no HS, DS, GS, stream-out or predicate) is set explicitly, and no state swap remains (it does nothing when the draw hook is nested)");
        EXPECT(Has(pass, R"x(ext::core::explicit_state::OmSnapshotom(context1.Get(),device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1);)x") && Has(pass, R"x(om.Restore();)x"),
               "the scene ring's copy takes the render targets off and puts them back together with the UAV slots (OMSetRenderTargets drops a bound UAV)");
        EXPECT(Has(pass, R"x(OMSetRenderTargets(0,nullptr,nullptr);_outValid=false;for(uint32_ti=0;i<border.count;++i){)x"),
               "the scene's ring is copied with the game's render targets unbound, and the hold's flag drops while this frame's copy is in flight (the review's N2)");
        EXPECT(Has(pass, R"x(constD3D11_BOXbandBox=RegionBox(border.band[i]);context->CopySubresourceRegion(_out.Get(),0,border.band[i].x,border.band[i].y,0,inputs.sceneTexture,0,&bandBox);)x"),
               "into the window's own texels: the ring around it is all the copy takes, because the draw writes every texel inside it");
        EXPECT(Has(rules, R"x(constexprRegionBandsToneBorderOf(constToneFacts&facts)noexcept)x"),
               "the ring is a rule the host can pin, not arithmetic in the draw path");
        EXPECT(Has(pass, R"x(context->CopySubresourceRegion(_camera.Get(),0,0,0,0,inputs.camera,0,&cameraBox);)x"),
               "the camera table is copied GPU side");
        EXPECT(!Has(pass, "Map(") || Has(pass, "D3D11_MAP_FLAG_DO_NOT_WAIT"),
               "every map of the statistics copy is a no-wait one");
        EXPECT(Has(draw, R"x(inputs.stats=ext::ffxiv::diag::Capturing();)x"), "the shares are written only while a capture is armed");
        EXPECT(Has(passHeader, R"x(uint32_tTargetsCreated()constnoexcept)x"), "a test can pin that a frame creates no D3D objects");
    }

    // ---- the stage's arming is the puddles module's, and the wetness is one buffer for both stages
    {
        EXPECT(Has(apply, R"x(return(state.armed.load(std::memory_order_relaxed)||ToneWants(state.params))&&state.session.Armed();)x"),
               "either stage arms the shared seam");
        EXPECT(Has(apply, R"x(ID3D11ShaderResourceView*WetnessResource(){returnS().pass.WetnessView();})x"),
               "the tone stage binds the gloss pass's own wetness buffer");
        // The refused-target case: a gloss target or channel the pass will not shape still hands the tone stage the frame's weather, or the shader
        // would read an all-zero buffer while the card and engine.txt said the stage was armed.
        EXPECT(Has(apply, R"x(constboolweather=engine::IsWetnessAnchors(anchors);)x"),
               "the wetness draw is read for both stages, whatever the gloss keys say");
        EXPECT(Has(apply, R"x(if(!Armed())returnnone;)x"), "and its gate is either stage's arming, not the gloss stage's alone");
        EXPECT(Has(apply, R"x(constboolgloss=state.armed.load(std::memory_order_relaxed)&&reader;)x"),
               "while the swap itself stays the gloss stage's own, so a refused pair is never shaped");
        EXPECT(Has(apply, R"x(PublishTone(state.params);)x"), "one publish arms both stages from one set of keys");
        EXPECT(Has(apply, R"x(ReleaseTone();)x"), "a teardown gives the tone textures back while the device is alive");
    }

    // ---- the shaders: the march is shared with the water reflection, and the dry gate and the fresnel are the picture's own rules
    {
        EXPECT(Has(ssrShader, R"x(#include"SceneMarch.hlsl")x") && Has(shader, R"x(#include"SceneMarch.hlsl")x"),
               "both reflections march the same implementation");
        EXPECT(Has(shader, R"x(if(!(wetness>Control.z))returnoutput;)x"), "the shader keeps a dry frame to the bit");
        EXPECT(Has(shader, R"x(constfloatm=1.0-cosine;)x"), "water's Fresnel is taken from the angle");
        EXPECT(Has(shader, R"x(colour+=SceneMarchColour(Scene,hitWindow,Window)*gain;)x"), "the hit is added with the gain, never on its own (the colour read at the crossing's own fraction)");
        EXPECT(Has(march, R"x(if(!(projected.x>=window.x))x"), "the march stops where the ray leaves the window");
        // The region test cannot change a picture, because the viewport clips every fragment outside it - so the statement is pinned, exactly as the
        // water reflection pins its own. Either one alone would leave a future viewport (or a future region rule) free to shade the engine's texels.
        EXPECT(Has(shader, R"x(if(at.x<regionLow.x||at.y<regionLow.y||at.x>=regionHigh.x||at.y>=regionHigh.y)returnoutput;)x"),
               "the shader keeps the scene's texel outside the render window");
        EXPECT(Has(shader, R"x(if(dot(N,P)>0.0)N=-N;)x"), "the mirror's normal faces the camera's side");
    }

    // ---- the keys: shipped off, the new five written out, documented and on the card
    {
        EXPECT(Has(config, R"x(CustomOptional<bool>puddles{false};)x"), "the puddles ship off: a fresh ini is today's picture");
        EXPECT(Has(config, R"x(CustomOptional<bool>puddlesReflection{true};)x"), "the mirror ships on");
        EXPECT(Has(config, R"x(CustomOptional<float>puddlesReflectionStrength{0.6f};)x"), "the strength default");
        EXPECT(Has(config, R"x(CustomOptional<float>puddlesDarkening{0.45f};)x"), "the darkening default");
        EXPECT(Has(config, R"x(CustomOptional<float>puddlesRipple{0.35f};)x"), "the ripple default");
        EXPECT(Has(config, R"x(CustomOptional<int32_t>puddlesReflectionSteps{24};)x"), "the step default");
        for (const char* key : { "PuddlesReflection", "PuddlesReflectionStrength", "PuddlesDarkening", "PuddlesRipple", "PuddlesReflectionSteps" })
        {
            EXPECT(Has(ini, std::string(key) + "=auto"), "the ini fragment carries %s", key);
            std::string member = key;
            member[0] = char(member[0] - 'A' + 'a');
            EXPECT(Has(menu, "cfg." + member + "="), "the card writes %s from its control", member.c_str());
        }
        EXPECT(Has(menu, R"x(cfg.puddlesDarkening=ClampKey(darkening,kMinDarkening,kMaxDarkening,kDarkeningDefault);)x"),
               "the darkening slider publishes the value it shows");
        EXPECT(Has(rules, R"x(if(value<int32_t(kMinSteps)||value>int32_t(kMaxSteps))returnkStepsDefault;)x"),
               "a step count out of range reads as the default rather than as the key's own value");
    }

    // ---- the tone stage's gate: ONE added binding (t5, the engine's material pack of this frame), the spec that goes with it, and nothing else bound
    {
        const std::string toneShader = Code(repo, "ffxiv/engine/shaders/PuddleReflection.hlsl");
        const std::string gateShader = Code(repo, "ffxiv/engine/shaders/PuddleGate.hlsli");
        EXPECT(Has(pass, R"x(constexpruint32_tkSlotGate=5;constexpruint32_tkSlotCount=6;)x"), "the tone pass binds six shader-resource slots, the sixth being the gate's");
        EXPECT(Has(pass, R"x(kSpec{kSlotCount,0,1};)x"), "its explicit-state spec is six slots, no sampler and one constant buffer: the guard saves and restores t5 too");
        EXPECT(Has(pass, R"x(views[kSlotScene]=inputs.scene;views[kSlotVPosition]=inputs.viewPosition;views[kSlotNormal]=inputs.normal;views[kSlotCamera]=_cameraView.Get();views[kSlotWetness]=inputs.wetness;views[kSlotGate]=inputs.gate;)x"),
               "with the six views in them; a null gate is bound as null, which reads as exposure 0");
        EXPECT(Count(pass, "PSSetShaderResources") == 2, "one bind and one unbind of them");
        EXPECT(Has(passHeader, R"x(ID3D11ShaderResourceView*gate=nullptr;)x"), "the pass's inputs carry the gate view");
        EXPECT(Has(toneShader, R"x(Texture2D<float4>Gate:register(t5);)x") && Count(toneShader, ":register(t") == 6 && Count(toneShader, ":register(b") == 1,
               "the shader declares the gate at t5, six textures and buffers in all, one constant buffer");
        EXPECT(Has(toneShader, R"x(#include"PuddleGate.hlsli")x") && !Has(toneShader, "staticconstfloatkGate"),
               "the tone shader includes the one gate file and declares no gate constant of its own");
        EXPECT(Has(toneShader, R"x(mask*=PuddleExposure(Gate.Load(int3(at,0)).w,wetness);)x"),
               "the pool mask is scaled by the same exposure the gloss shader applies, read from the gate at the texel");
        EXPECT(Has(gateShader, R"x(floatPuddleExposure(floatengineGloss,floatwetness))x"), "the exposure is one function in one file");
        // This frame's gate: the composite's own binding, written at every composite (a composite with none leaves none), read at the pass's inputs.
        EXPECT(Has(draw, R"x(state.capture.gate=gateReason==ToneGateReason::None?gate:ComPtr<ID3D11ShaderResourceView>();)x"),
               "the gate view is assigned at EVERY composite, null when this composite gave none, so a frame never inherits the previous frame's");
        EXPECT(Has(draw, R"x(inputs.gate=state.capture.gate.Get();)x"), "and the pass is handed that capture's view");
        EXPECT(Has(draw, R"x(ToneGateReasonOf(GateReadable(state.params.target),gate!=nullptr,pack.twoD?pack.format:0u,pack.width,pack.height,scene.width,scene.height);)x"),
               "the gate's reason is the pure rule's, asked with the key's target readability, whether a view is bound at the slot, and the bound texture's own format and size");
        EXPECT(Has(draw, R"x(FormatToneGateMissing(state.capture.gateReason)x"), "and each missing-gate reason is logged once per state");
    }

    // ---- nothing a player can read names a private word or an absolute path
    {
        // The banned words are assembled from pieces so this file never carries them itself (the publish scanner reads tests too).
        const std::string banned[] { std::string("ps-") + "ledger", std::string("phase3-") + "ledger", std::string("BRI") + "EF",
                                     std::string("C:\\") + "Users" };
        for (const char* relative : { "ffxiv/engine/PuddleToneDraw.cpp", "ffxiv/engine/PuddleTonePass.cpp", "ffxiv/engine/PuddleTonePass.h",
                                      "ffxiv/engine/PuddleTone.h", "ffxiv/engine/shaders/PuddleReflection.hlsl",
                                      "ffxiv/engine/shaders/SceneMarch.hlsl", "ffxiv/engine/GlossPuddlesRules.h" })
            for (const std::string& word : banned)
                EXPECT(!Has(Raw(repo, relative), word), "%s does not carry the private word %s", relative, word.c_str());
    }

    if (g_failures != 0)
    {
        std::printf("puddle_tone_pins: %d expectation(s) failed\n", g_failures);
        return 1;
    }
    std::printf("puddle_tone_pins: all checks passed\n");
    return 0;
}
