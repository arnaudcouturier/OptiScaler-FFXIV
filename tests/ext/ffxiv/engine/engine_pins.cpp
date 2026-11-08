// Text pins of the engine pass override module: where it hangs, what the key off costs, and what it must never do.
//
// The feature is off by default and must stay free while it is off: the seam is one relaxed load, the module is only called from the seam and
// the two shader hooks, the keys are published at the config load, and the patch only ever binds its own buffers - the game's buffer is read
// with CopyResource, never mapped or written, and every binding is put back.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
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
    const std::string text = Comments(Raw(repo, relative));
    std::string out;
    for (char c : text)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            out += c;
    return out;
}

bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

size_t Count(const std::string& text, const std::string& needle)
{
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
        ++n;
    return n;
}
} // namespace

int main(int, char** argv)
{
    const std::string repo = argv[1];

    // ---- the seam: one relaxed load while both keys are off, and the patch replaces the draw call, not the rest of the hook
    {
        const std::string hooks = Code(repo, "ffxiv/lighting/FfxivLightingCapture.h");
        EXPECT(Has(hooks, R"x(if(!inside&&c==context&&ext::ActiveAny(ext::F::ContactShadows,ext::F::EnginePassOverrides,ext::F::LightShafts))x"),
               "the draw-path features are tested together, so all of them off costs one relaxed load");
        EXPECT(Count(hooks, "ext::Active(ext::F::EnginePassOverrides)") == 0,
               "the engine's key is always one of the draw-path tests now, never a second load of its own");
        EXPECT(Has(hooks, R"x(if(!ext::ActiveAny(ext::F::EnginePassOverrides,ext::F::LightShafts)){shadows();return;})x"),
               "the engine's patch and the light shafts share the wrapping draw, so either key opens it and both off runs the game's call alone");
        EXPECT(Has(hooks, R"x(ext::ffxiv::engine::BoundDraw(c,shadows);)x"),
               "the patch wraps the game's own draw, outside the contact shadow swap");
        EXPECT(Count(hooks, "ext::ffxiv::relight::RunDrawOnce(") == 1,
               "one draw-once rule covers both patches: a replay of a draw runs the game's call alone");
        EXPECT(Count(hooks, "ext::ffxiv::engine::") == 4,
               "the module is called from four places: the draw seam, the pixel-shader seam, the shader creation and the teardown");
        EXPECT(Has(hooks, R"x(#include"ext/ffxiv/engine/EnginePass.h")x"), "the seam's header is included");
        EXPECT(Has(hooks, R"x(if(c==context)ext::ffxiv::engine::OnPixelShaderSet(ps,count);)x"),
               "the bound shader is tracked where the game binds it, not asked for on every draw");
        EXPECT(Has(hooks, R"x(ext::ffxiv::engine::OnShaderCreated(*out,code,size,crc);)x"), "the index is filled where the bytecode is");
        EXPECT(Has(hooks, R"x(ext::ffxiv::engine::Release();)x"), "the teardown path releases the buffers while the device is alive");
    }

    // ---- the feature bit is its own, and the inline test is the relaxed load of ExtFeatures.h
    {
        const std::string features = Code(repo, "ExtFeatures.h");
        EXPECT(Has(features, R"x(EnginePassOverrides=1ull<<15,)x"), "the feature bit");
        const std::string header = Code(repo, "ffxiv/engine/EnginePass.h");
        EXPECT(Has(header, R"x(inlinevoidBoundDraw(ID3D11DeviceContext*context,F&draw))x"), "the seam wrapper is header only");
        EXPECT(Has(header, R"x(std::atomic<uint32_t>boundLayoutId{0};)x") == false,
               "the header the draw hook includes carries no state: the seam's one value lives in the module");
        EXPECT(!Has(header, "std::mutex") && !Has(header, "d3d11.h"),
               "the header the draw hook includes carries no lock and no D3D include of its own");
    }

    // ---- the module: armed through the config's hook pointer, switched off after the refusal limit, and never writing a game resource
    {
        const std::string cpp = Code(repo, "ffxiv/engine/EnginePass.cpp");
        EXPECT(Has(cpp, R"x(ext::FfxivConfig::enginePassOverridesOnLoaded=&ext::ffxiv::engine::OnConfigLoaded;)x"),
               "the key reaches the module through the config's hook pointer");
        EXPECT(Has(cpp, R"x(returnext::Active(ext::F::EnginePassOverrides)&&state.session.Armed();)x"), "Armed() is the key and the session");
        EXPECT(Has(cpp, R"x(if(ext::Active(ext::F::EnginePassOverrides)!=armed))x") &&
                   Has(cpp, R"x(S().boundAnchors.store(0,std::memory_order_relaxed);)x"),
               "every arm and disarm transition forgets the seam's value for the bound shader, so a knob moved from 1.0 to 0.7 or a re-arm "
               "after the session stood down never patches a draw the seam classified under another state");
        EXPECT(Count(cpp, "SetArmed(") == 5,
               "the feature bit changes in one place: the config publish, the feature-bit rule, the switch-off, the draw that finds every feature "
               "stood down, and the release of the key");
        EXPECT(Count(cpp, "state.boundAnchors.store(0,") == 2 &&
                   Has(cpp, R"x(state.boundAnchors.store(0,std::memory_order_relaxed);if(!on))x"),
               "a config load forgets the seam's value before it looks at the key, and the teardown forgets it too: the first draw after either "
               "is the game's own call, so no draw is patched with anchors the seam stored under another state");
        EXPECT(Has(cpp, R"x(if(!state.session.Armed())SetArmed(AnyFeatureArmed(ao::Armed(),gloss::Armed(),ext::ffxiv::shafts::Armed(),ssr::Armed()));)x"),
               "and it happens on the draw that reached the limit, not on the next config load - and it leaves the feature bit on while another "
               "feature still wants draws, so a stood-down knob never switches the ambient occlusion, the puddles, the light shafts or the "
               "reflections off");
        EXPECT(Has(cpp, R"x(SetArmed(FeatureBitWanted(on,AnyKnobOn(S().params),ao::Armed(),gloss::Armed(),ext::ffxiv::shafts::Armed(),ssr::Armed()));)x"),
               "the feature bit is set while the master is on with at least one knob off the engine's own value - a master left on with both "
               "sliders at vanilla costs nothing per draw - or while any feature wants a draw on its own switch");
        EXPECT(Count(cpp, "SetArmed(AnyFeatureArmed(ao::Armed(),gloss::Armed(),ext::ffxiv::shafts::Armed(),ssr::Armed()));") == 3,
               "every branch that outlives the master key - the config publish that turned it off, the switch-off and the draw that found every "
               "feature stood down - asks every feature, so the bit is cleared only when the last of them stands down");
        EXPECT(Has(cpp, R"x(if(!MentionsAnyName(code,size)))x"),
               "shader creation drops the shaders that cannot name one of the buffers with a byte scan, so the index is filled while the key "
               "is off and a switch turned on later finds its shader");
        EXPECT(Has(cpp, R"x(state.boundAnchors.store(instances==0?TrackedAnchorsOf(shader):0,std::memory_order_relaxed);)x"),
               "a bind is classified by the seam - the ambient slot and the both-anchors bit in one value - and a class-instance override is "
               "not one of these draws");
        EXPECT(Has(cpp, R"x(std::atomic<Anchors>boundAnchors{0};)x"),
               "the seam's value is one 64-bit atomic, so the anchors map grew without a second load: a draw and a bind each still cost one "
               "relaxed load and one relaxed store");
        EXPECT(Has(cpp, R"x(SeenTable<kSeenSlots>*table=SeenIfAny();if(table==nullptr)return0;)x") &&
                   Has(cpp, R"x(table->Find(reinterpret_cast<uintptr_t>(shader),)x") && Has(cpp, R"x(returnStampedIdOf(shader);)x"),
               "a bind walks the index's own table through the pointer the first insert published - never allocating on the bind path - and it "
               "asks the shader object for its stamp only on a pointer hit");
        EXPECT(Has(cpp, R"x(if(anchors==0)return;)x") &&
                   Has(cpp, R"x(if(SUCCEEDED(shader->SetPrivateData(kLayoutTag,sizeofid,&id)))full=SeenForInsert().Insert(reinterpret_cast<uintptr_t>(shader),id,anchors)==SeenInsert::Full;)x") &&
                   Has(cpp, R"x(if(full&&!S().indexFullLogged))x"),
               "only a layout that carries an anchor bit for a knob is stored, only when the object accepted its stamp, and a table that ran out of probe room writes one line and fails "
               "open for that shader instead of evicting one the knobs patch");
        {
            // The table's own contract, and the draw path's cost: the index read happens on the bind, while the draw pays one relaxed load
            // of the seam's value and takes no lock.
            const std::string rules = Code(repo, "ffxiv/engine/EnginePassRules.h");
            EXPECT(Has(rules, R"x(if(id==0||stampedId(shader)!=id)return0;)x") && Has(rules, R"x(if(recorded!=shader)continue;)x"),
                   "a recycled address is not trusted: the stamp is the shader object's own answer, and a bind of an address the table does not "
                   "hold asks nothing");
            EXPECT(Has(rules, R"x(slot.anchors.store(anchors,std::memory_order_relaxed);slot.id.store(id,std::memory_order_release);slot.shader.store(shader,std::memory_order_release);)x"),
                   "a record is published anchors first, then the id, then the pointer: a bind can never see a new id with the previous anchors");
            EXPECT(Count(rules, "slot.anchors.store(anchors,std::memory_order_relaxed);slot.id.store(id,std::memory_order_release);") == 2,
                   "and both paths that write a record publish it in that order, the new slot and the one replaced in place");
            // The cap a NEW bytecode is refused at, and the one rule that decides it: the index keeps its first kMaxIndexedShaders bytecodes and a
            // later one is counted and written instead of dropped without a line - the late shaders of a long session (a water material, the
            // composite of a new zone) are exactly the ones that used to disappear. The table is sized from the same cap (3 MiB at 131,072 slots).
            EXPECT(Has(rules, R"x(inlineconstexpruint32_tkMaxIndexedShaders=40960;)x") && Has(rules, R"x(inlineconstexpruint32_tkSeenSlots=131072;)x"),
                   "the index's cap is above the ~33,000 pixel shaders a session creates, and the table has twice that many slots");
            EXPECT(Has(rules, R"x(constexprboolIndexHasRoom(size_tkept)noexcept{returnkept<kMaxIndexedShaders;})x"),
                   "whether a new bytecode may be stored is one rule, so both the insert and a test read it");
            EXPECT(Has(cpp, R"x(elseif(IndexHasRoom(I().byCrc.size())))x"),
                   "the insert asks the room rule rather than comparing the cap inline, so a mutation of the rule changes the index");
            EXPECT(Has(cpp, R"x(refused=true;)x") && Has(cpp, R"x(S().indexRefused.fetch_add(1,std::memory_order_relaxed);)x") &&
                       Has(cpp, R"x(LOG_WARN("{}",FormatIndexRefused());)x") && Has(cpp, R"x(if(!S().indexRefusedLogged))x"),
                   "a new bytecode past the cap is counted and written once, with its own line, instead of being dropped silently");
            EXPECT(Has(rules, R"x(inlineconstexpruint32_tkSeenProbeLength=16;)x"),
                   "the probe bound the table's own fullness depends on stays at 16");
            EXPECT(Has(rules, R"x(constuint32_tid=slot.id.load(std::memory_order_acquire);if(id==0||stampedId(shader)!=id)return0;returnslot.anchors.load(std::memory_order_relaxed);)x"),
                   "a find reads the pointer, then the id, then the anchors - the reverse of the write order - so the stamp check refuses the "
                   "one pair a replace can leave visible, the previous id beside the new anchors");
            const std::string rawRules = Raw(repo, "ffxiv/engine/EnginePassRules.h");
            EXPECT(Has(rawRules, "the previous id with the new anchors") && Has(rawRules, "never see a new id with the previous anchors"),
                   "the record's comment states the publication-order argument those two orders pin");
            const size_t drawAt = cpp.find("voidOnGameDraw(");
            const size_t bindAt = cpp.find("voidOnPixelShaderSet(");
            const std::string drawPath = drawAt != std::string::npos && bindAt != std::string::npos && drawAt < bindAt
                                             ? cpp.substr(drawAt, bindAt - drawAt)
                                             : std::string();
            EXPECT(!drawPath.empty(), "the draw path is read from OnGameDraw");
            EXPECT(!Has(drawPath, "lock_guard") && !Has(drawPath, "std::mutex"), "the draw path takes no lock");
            EXPECT(Count(drawPath, "state.boundAnchors.load(std::memory_order_relaxed)") == 1,
                   "the draw path reads the seam's one value with one relaxed load, whatever the index does on the bind");
        }
        EXPECT(!Has(cpp, "PSGetShader"), "the draw path makes no state query of its own");
        EXPECT(Has(cpp, R"x(state.boundAnchors.load(std::memory_order_relaxed))x") &&
                   Has(cpp, R"x(if(!IsAmbientAnchors(anchors)&&!IsWetnessAnchors(anchors)&&!IsOcclusionAnchors(anchors)&&!gloss::IsGlossAnchors(anchors)&&!IsReflectionAnchors(anchors))x"),
               "the draw reads the seam's one value, and a shader that declares none of the features' names is passed on at that load");
        EXPECT(Has(cpp, R"x(if(context==nullptr||!DrawPathWanted(state.session.Armed(),ao::Armed(),gloss::Armed(),ext::ffxiv::shafts::Armed(),ssr::Armed()))x"),
               "the seam is entered while any feature wants a draw: the occluder's own switch-off is neither the occlusion pass's, nor the "
               "puddles', nor the light shafts', nor the reflections'");
        {
            // The gate's own shape: one arming source per feature, so a feature added to the module is one more argument and no call site can
            // keep a fixed two-source form; and the feature bit is the master's knobs OR every feature, in one rule.
            const std::string gateRules = Code(repo, "ffxiv/engine/EnginePassRules.h");
            EXPECT(Has(gateRules, R"x(template<typename...Features>constexprboolDrawPathWanted(boolsessionArmed,Features...featuresArmed)noexcept{returnsessionArmed||AnyFeatureArmed(featuresArmed...);})x"),
                   "the draw path's gate takes one arming source per feature and is the session's arming OR any of them");
            EXPECT(Has(gateRules, R"x(template<typename...Features>constexprboolFeatureBitWanted(boolmasterOn,boolanyKnobOff,Features...featuresArmed)noexcept{return(masterOn&&anyKnobOff)||AnyFeatureArmed(featuresArmed...);})x"),
                   "the feature bit is the master's two knobs or any other feature's own arming, so a feature's own switch arms the seam with "
                   "the master key off and the bit is cleared only when the last feature stands down");
        }
        EXPECT(Has(cpp, R"x(gloss::BoundReaderpuddles=gloss::SwapReader(context,anchors);)x"),
               "a draw the seam classified as a reader of a G-buffer target binds the puddles pass's texture for that one call");
        EXPECT(Has(cpp, R"x(ssr::BoundReaderreflection=ssr::SwapReader(context,anchors);)x"),
               "and a draw classified as a sampler of the engine's planar reflection map binds the reflections pass's texture for its one call");
        EXPECT(Has(cpp, R"x(constKnobIntentintent=IntentOfSession(state.params,anchors,state.session.Armed(),state.keyOn.load(std::memory_order_relaxed));)x"),
               "a stood-down module patches nothing even when the seam is entered for the occlusion pass, and neither does either knob while "
               "the master key is off: the key is folded into the intent, not left to the feature bit the occlusion pass keeps on");
        EXPECT(Has(cpp, R"x(status.armed=KnobsArmed(state.keyOn.load(std::memory_order_relaxed),state.params,state.session.Armed());)x"),
               "the card's and a capture's arming word is the master key's, not the feature bit's: a master turned off reads off while the "
               "seam is still entered for the other features");
        EXPECT(Has(cpp, R"x(ao::BoundReaderocclusion=ao::SwapReader(context,anchors);)x"),
               "a draw the seam classified as a reader of the engine's occlusion texture binds this module's texture for that one call");
        EXPECT(Has(cpp, R"x(if(state.inPatch.load(std::memory_order_relaxed)){draw(user);)x") &&
                   Count(cpp, "state.inPatch.load(std::memory_order_relaxed)") == 2,
               "the draw path calls the game's draw unchanged while this module's own GPU work runs, and the shader-bind hook stores nothing "
               "then either: a copy or a dispatch of the patch is not a draw of the frame and not a bind the seam may classify");
        EXPECT(Has(cpp, R"x(if(!drew)draw(user);)x"),
               "a patch that fails after the game's draw already ran returns without calling it again: one game draw per game draw, whatever "
               "fails around it");
        EXPECT(Count(cpp, "lock_guard") == 1,
               "only the shader index takes a lock: the draw path of a draw whose shader is not the composite takes none, and pays no "
               "state query and no refusal count either");
        EXPECT(Has(cpp, R"x(guard{state};)x") && Has(cpp, R"x(state.inPatch.exchange(true,std::memory_order_relaxed))x"),
               "the module's own GPU work is marked, so its copies never reach the draw classification");
        EXPECT(Has(cpp, R"x(ext::ffxiv::chara::ReplayScopeown;)x"),
               "the character preview's marker is raised around the patch, so its hooks do not count the fork's copies as the game's");
        EXPECT(Has(cpp, R"x(facts.replay=ext::ffxiv::chara::t_replay;)x"), "a replayed game draw is never patched");
        EXPECT(Has(cpp, R"x(if(buffers[i]!=nullptr&&i!=ambientSlot))x"),
               "the inspection keeps the ambient slot's reference alone and gives every other slot back by index, so a buffer the game binds "
               "at two slots leaks nothing");
        EXPECT(Has(cpp, R"x(BoundConstantBufferfirst(context,swaps[0].slot,swaps[0].buffer);)x") &&
                   Has(cpp, R"x(BoundConstantBuffersecond(context,swapCount>1?swaps[1].slot:swaps[0].slot,swapCount>1?swaps[1].buffer:nullptr);)x"),
               "the patched buffers are bound by scope guards, and a draw with one patch binds nothing through the second one");
        EXPECT(Has(cpp, R"x(patched=state.patch.Scale(context,ambient.Get(),facts.boundBytes,entries,count);)x"),
               "the GPU scale runs on the game's own buffer and gives back what the draw binds");
        EXPECT(Has(cpp, R"x(if(state.session.NoteDecline(knob)&&!state.loggedOff))x") && Count(cpp, "NoteDecline(") == 1,
               "the refusal limit is counted in one place, and a refusal names the knob it belongs to");
        EXPECT(Has(cpp, R"x(if(uint32_t(reason)<kDeclineReasonCount)state.declines[uint32_t(reason)]++;)x"),
               "a refusal is counted against the reason it was refused for, in the table a capture reads: engine.txt names the reason that "
               "dominated a window, so a count kept per knob would report the wrong reason");
        EXPECT(Has(cpp, R"x(for(uint32_ti=0;i<kDeclineReasonCount;++i)diagnostics.declines[i]=state.declines[i];)x"),
               "the export copies every reason's count in the enum's order: a capture reads the same reason out of engine.txt that the module "
               "counted, and the last reason is reported like the others");
        EXPECT(Has(cpp, R"x(if(constuint32_tcount=AmbientEntries(state.params,entries,kMaxScaleEntries);count!=0))x"),
               "a knob at the engine's own value is not a decline: the draw is the game's own call and the module is not counted");
        EXPECT(Has(cpp, R"x(constboollogging=ext::Active(ext::F::EnginePassOverrides);)x"), "shader creation logs nothing while the key is off");
        EXPECT(Has(cpp, R"x(state.session.NoteRun(Knob::Ambient);)x") && Has(cpp, R"x(state.session.NoteRun(Knob::Wetness);)x"),
               "an accepted draw clears the refusal count of the knob that patched it, not the other knob's");
    }

    // ---- the wetness knob: one GPU patch per frame and buffer, a slot swap per material draw, and a frame token both present paths tick
    {
        const std::string cpp = Code(repo, "ffxiv/engine/EnginePass.cpp");
        EXPECT(Has(cpp, R"x(state.wetnessCache.Begin(ext::present::FrameCount(),CacheEpoch());)x"),
               "the frame token is the output-frame counter, one relaxed load per candidate draw: a session with frame generation off and SDR "
               "output never runs the bridge, so a bridge present count would not move there");
        EXPECT(!Has(cpp, "catalog::"), "the module does not reach into the catalog for a frame token");
        const std::string seams = Code(repo, "present/PresentSeams.h");
        EXPECT(Has(seams, R"x(voidFrameTick()noexcept;)x") && Has(seams, R"x(uint64_tFrameCount()noexcept;)x"),
               "the frame counter is a seam of ext/present, so another module can use the same boundary");
        const std::string pacing = Code(repo, "present/FramePacingTrace.cpp");
        EXPECT(Has(pacing, R"x(if(phase==Phase::OutputPresent)FrameTick();)x"),
               "the plain wrapped swapchain's present ticks the frame at its entry, through the scope it already constructs there (no new "
               "call site in the upstream present)");
        EXPECT(Has(pacing, R"x(g_frameTicks.fetch_add(1,std::memory_order_relaxed);)x") &&
                   Has(pacing, R"x(returng_frameTicks.load(std::memory_order_relaxed);)x"),
               "the tick is one relaxed increment and the read one relaxed load");
        const std::string bridge = Code(repo, "hdr/BridgeHdr.cpp");
        EXPECT(Count(bridge, "ext::present::NativeBegin();") == 1,
               "the bridge's present still enters through the one seam that ticks the frame on that path");
        const std::string rules = Code(repo, "ffxiv/engine/EnginePassRules.h");
        EXPECT(Has(rules, R"x(constexprVerdictWetnessVerdict(constKnobIntent&intent,constWetnessFacts&facts,boolcached,boolmayPatch)noexcept)x") &&
                   Has(rules, R"x(Verdict{KnobVerdict::Refused,Decline::TooManyBuffers})x"),
               "a frame that binds more wetness buffers than the cache patches refuses the draw with its own reason, and the pure rule says so");
        EXPECT(Has(cpp, R"x(constVerdictverdict=AmbientVerdict(intent,facts);)x") &&
                   Has(cpp, R"x(constVerdictverdict=WetnessVerdict(intent,wetnessFacts,cached,state.wetnessCache.MayPatch());)x") &&
                   Count(cpp, "if(verdict.kind==KnobVerdict::Refused)") == 2,
               "both knobs ask the pure rule for their verdict and refuse only through it, so which draws are refusals is the rule's answer");
        EXPECT(Count(cpp, "returndecline(") == 6 &&
                   Has(cpp, R"x(returndecline(Knob::Ambient,verdict.reason);)x") &&
                   Has(cpp, R"x(returndecline(Knob::Wetness,verdict.reason);)x"),
               "a refusal is counted at exactly six places: the two verdicts, the three patch failures and the throw - a draw that no knob asked for is never counted");
        EXPECT(!Has(cpp, "IsWetnessCandidate(") && !Has(cpp, "Classify("),
               "the draw path no longer decides the candidate test or the validation itself: it asks the rules");
        EXPECT(Has(cpp, R"x(patched=state.patch.Set(context,wetness.Get(),wetnessFacts.boundBytes,setEntries,setCount);)x"),
               "the set patch runs on the game's own buffer and gives back the buffer the frame's draws bind");
        EXPECT(Has(cpp, R"x(state.wetnessCache.Record(wetness.Get(),patched);)x") &&
                   Has(cpp, R"x(ID3D11Buffer*cachedCopy=reinterpret_cast<ID3D11Buffer*>(const_cast<void*>(state.wetnessCache.Dst()));)x") &&
                   cpp.find("cachedCopy=reinterpret_cast") < cpp.find("state.wetnessCache.Serve();"),
               "the patched copy is remembered per frame and pointer; every later draw of the frame serves it, and the cached buffer is read "
               "before the bind that can expire the entry, so the draw at the expiry still binds a copy");
        EXPECT(Has(cpp, R"x(context->PSGetConstantBuffers(wetnessSlot,1,&wetness);)x"),
               "the wetness slot comes from the shader's reflection, and only that slot is read: a full 14-slot read per material draw would cost "
               "more than the patch it saves");
        EXPECT(Count(cpp, "OMGetRenderTargets") == 1,
               "the composite's validation makes the module's only render-target query: the wetness knob has no shape rule");
        EXPECT(Has(cpp, R"x(state.wetnessCache.Begin(ext::present::FrameCount(),CacheEpoch());)x"),
               "the cache is begun with the frame token and the epoch, so a knob that moved or a re-created buffer clears it on the draw thread");        EXPECT(Count(cpp, "state.cacheEpoch.fetch_add(1,std::memory_order_relaxed);") == 1 &&
                   Count(cpp, "S().cacheEpoch.fetch_add(1,std::memory_order_relaxed);") == 1 &&
                   !Has(cpp, "wetnessCache.Reset("),
               "the knob and the master publish an epoch instead of writing the cache: the draw thread owns the entry, and a menu or config load "
               "on another thread cannot tear it");
        EXPECT(Has(cpp, R"x(returnstate.cacheEpoch.load(std::memory_order_relaxed)+state.patch.Generation();)x"),
               "the patch primitive's generation is part of the epoch, so a device change or a size cache eviction that frees the cached buffer "
               "is seen by the next Begin()");
        const std::string patchHeader = Code(repo, "ffxiv/engine/CbPatch.h");
        const std::string patchCpp = Code(repo, "ffxiv/engine/CbPatch.cpp");
        EXPECT(Has(patchHeader, R"x(uint32_tGeneration()constnoexcept{return_generation.load(std::memory_order_relaxed);})x") &&
                   Has(patchHeader, R"x(std::atomic<uint32_t>_generation{0};)x"),
               "the primitive counts its own re-initialisation, and the draw thread reads the count with one relaxed load");
        EXPECT(Count(patchCpp, "_generation.fetch_add(1,std::memory_order_relaxed);") == 2,
               "one bump where the buffers are released and one where the size cache evicts an entry");
        EXPECT(Has(cpp, R"x(state.wetnessRuns++;)x") && Has(cpp, R"x(state.wetnessSwaps++;)x"),
               "the card counts wetness patches and binds separately");
    }

    // ---- the ambient occlusion pass keeps no table of the game's shaders: the stamp on the shader object is the whole of it
    {
        const std::string ao = Code(repo, "ffxiv/engine/AoApply.cpp");
        const std::string swap = Code(repo, "ffxiv/engine/AoSwap.cpp");
        EXPECT(Has(ao, R"x(detail::StampShader(shader,PackInputs(InputsOfShader(code,size)));)x"),
               "the slots one shader declares are packed and stamped on the shader object itself, which lives exactly as long as the shader does");
        EXPECT(Has(ao, R"x(returnInputsOfBindings(parsed,count);)x"),
               "the draw path's parse hands the binding table to the pure rule (AoRules.h), so the selection a stamp carries is the one the host test "
               "covers; a second copy inside the draw path would be the one no test reaches");
        EXPECT(Has(ao, R"x(inputs=UnpackInputs(detail::ReadStamp(pixelShader.Get()));)x"),
               "the frame's first reader reads that stamp with one call, and nothing else in the module asks a shader anything");
        EXPECT(Has(swap, R"x(shader->SetPrivateData(kInputsTag,sizeofpacked,&packed))x") &&
                   Has(swap, R"x(shader->GetPrivateData(kInputsTag,&bytes,&packed))x") && Count(swap, "GetPrivateData(") == 1,
               "the write and the read are one pair on the object, in the unit a device test drives: a stamp written on one shader is read back "
               "after five thousand more are created, which is the behaviour a bounded table of shaders could not give");
        EXPECT(Count(ao, "GetPrivateData(") == 0 && Count(ao, "SetPrivateData(") == 0,
               "the module's own draw and creation paths never touch a shader's private data themselves");
        EXPECT(!Has(ao, "g_seen") && !Has(ao, "SeenSlotOf") && Count(ao, "std::atomic<ID3D11PixelShader*>") == 0,
               "no fixed table of shaders exists: the game creates tens of thousands a session, every material shader declares the camera, and a "
               "bounded table loses the reader's entry before the first reader asks for it");
        EXPECT(Has(ao, R"x(state.frame.Begin(token);)x"),
               "the per-frame memory is begun with the token the frame counter gave: the engine alternates its occlusion texture from one "
               "frame to the next, so the first reader of a frame forgets what the last one recorded");
        EXPECT(Has(ao, R"x(if(state.frame.hasFrame&&state.frame.frame!=token)state.servedLastFrame=state.frame.served;)x"),
               "the count of the last finished frame is taken before the frame is begun: the served-per-frame line and the card's number are "
               "the frame that just ended, never the one in progress that Begin() is about to clear");
        EXPECT(Has(ao, R"x(state.frame.Record(occlusion.resource,occlusion.view,state.pass.OutputTexture());)x"),
               "the pass records the resource and the view the engine bound and the texture it computed, which is the record the frame's "
               "later readers are served from");
        EXPECT(Has(ao, R"x(constboolsameBinding=occlusion.view==state.frame.engineView||state.frame.Has(occlusion.resource);)x"),
               "a later reader is served only when it binds the view or the resource the frame recorded: the engine's other texture is not "
               "answered with this frame's");
        EXPECT(Has(ao, R"x(constFrameInputs::Contributiontaken=state.inputs.Merge(reader.cameraBound,reader.depthBound,reader.normalBound);)x") &&
                   Has(ao, R"x(if(taken.depth)state.depthHeld.Take(reader.depth);)x") &&
                   Has(ao, R"x(if(taken.normal)state.normalHeld.Take(reader.normal);)x") &&
                   Has(ao, R"x(if(taken.camera)state.cameraHeld=reader.camera;)x"),
               "one merge per reader takes the camera table with the textures it attributes, holds exactly the views the frame kept, and "
               "the camera table is the view's identity so the three merged inputs cannot come from two views");
        EXPECT(Has(ao, R"x(if(taken.otherView){)x") && Has(ao, R"x(Refuse(Decline::ViewMismatch,FormatRefusedReader(Decline::ViewMismatch,facts,slots));)x"),
               "a reader that binds another view's camera table refuses the frame: a second view's depth has the same allocation's size, so "
               "the identity of the table is what tells the two apart, and the draw is left with the engine's texture");
        EXPECT(Has(ao, R"x(reader.cameraBound.resource=reader.camera.Get();)x"),
               "the identity is the camera buffer's own device object, read from the same binding the pass copies its matrices from");
        EXPECT(Has(ao, R"x(state.token=token;)x") &&
                   Has(ao, R"x(constLookSteplook=state.session.Look(token);if(look==LookStep::Skip)returnnone;)x") &&
                   Has(ao, R"x(if(look==LookStep::ProbeBegin){state.frame.Reset();state.inputs.Reset();})x"),
               "the frame token the draw path read is the pause's clock, and a paused frame is passed on at that one test (before any state "
               "query), so the pause costs the key-off path plus one load; a look starts its frame from scratch, so the frame it inspects is "
               "read again instead of reaching a spent attempt (the frame that paused the module, or a token that never advances), and every "
               "later reader of that frame is part of the same look");
        EXPECT(Has(ao, R"x(if(NoteFrame(state.session,state.inputs.noted,state.token,false)&&!state.loggedOff))x") &&
                   Has(ao, R"x(if(NoteFrame(state.session,state.inputs.noted,state.token,true)&&!state.loggedOff))x") &&
                   Count(ao, "LOG_WARN(\"{}\",FormatPaused());") == 2,
               "both declines of a frame go through the one count: a wait and a frame-level refusal of the same frame are one frame, a wait "
               "on the pause's own look frame counts nothing, and the frame that reaches the limit writes the pause's line once");
        EXPECT(Has(ao, R"x(voidWaitForInputs(Declinereason,constReaderFacts&facts,constInputs&inputs){State&state=S();state.declined++;if(uint32_t(reason)<kDeclineReasonCount)state.declines[uint32_t(reason)]++;if(state.log.Take(uint32_t(reason)))LOG_INFO("{}",FormatWaiting(reason,facts,inputs));})x"),
               "a wait is counted in the per-reason table as well as in the total, so a capture's breakdown adds up to the draws it says were "
               "left alone - and a wait itself decides nothing about the pause: whether the frame counts is the frame's own business (NoteFrame)");
        EXPECT(Has(ao, R"x(if(CollectFrame(context,state,occlusion)!=FrameStep::Ran)returnnone;)x") &&
                   Has(ao, R"x(if(IsWaitingDecline(reason)&&state.inputs.readers<=kMaxFrameInputReaders))x") &&
                   Has(ao, R"x(if(!state.inputs.noted){WaitForInputs(reason,facts,slots);if(NoteFrame(state.session,state.inputs.noted,state.token,true)&&!state.loggedOff){state.loggedOff=true;LOG_WARN("{}",FormatPaused());}state.inputs.noted=true;})x") &&
                   Count(ao, "state.frame.TakeAttempt();") == 3,
               "a reader that supplies no input leaves the frame waiting without spending its attempt (the waiting is counted and logged "
               "once per frame, and bounded by kMaxFrameInputReaders); only a frame no reader completed, a frame whose inputs cannot agree and "
               "a frame whose readers span two views take it - so the inputs a frame can collect and the view they belong to decide, not "
               "its first draw");
        const std::string rules = Code(repo, "ffxiv/engine/EnginePassRules.h");
        const std::string cpp = Code(repo, "ffxiv/engine/EnginePass.cpp");
        EXPECT(Has(rules, R"x(inlineconstexprAnchorskOcclusionReaderBit=1ull<<32;)x") &&
                   Has(rules, R"x(anchors|=kOcclusionReaderBit|((Anchors(layout.SlotOf(5))<<kOcclusionReaderSlotShift)&kOcclusionReaderSlotMask);)x"),
               "a reader of the occlusion texture takes the map's own reader bit and slot field, so the shared index stores it and the draw path "
               "reads its slot through the same map as the shipped knobs");
        EXPECT(cpp.find("ao::NoteShader(shader,code,size);") < cpp.find("SeenForInsert().Insert("),
               "the ambient occlusion stamp is written before the shader index decides anything, so a shader the index does not store (no anchor "
               "bit, a full index) still answers for the slots its own reflection declares");
    }

    // ---- the primitive: three buffers, two copies and one dispatch, and nothing of the game written
    {
        const std::string patch = Code(repo, "ffxiv/engine/CbPatch.cpp");
        EXPECT(Count(patch, "CopyResource") == 2, "the game's buffer in, the scaled one out");
        EXPECT(Has(patch, "Dispatch("), "one dispatch per patched draw");
        EXPECT(Count(patch, "D3D11_BIND_CONSTANT_BUFFER") >= 2, "the source copy and the destination are constant buffers");
        EXPECT(Has(patch, "D3D11_BIND_UNORDERED_ACCESS") && Has(patch, "D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS") &&
                   Has(patch, "D3D11_BUFFER_UAV_FLAG_RAW"),
               "the buffer between them is a raw UAV, because a constant buffer cannot carry another bind flag");
        EXPECT(Has(patch, "CSSetShader") && Has(patch, "CSSetUnorderedAccessViews") && Has(patch, "CSSetConstantBuffers"),
               "the dispatch sets its own compute state");
        EXPECT(Has(patch, "CSGetShader") && Has(patch, "CSGetUnorderedAccessViews") && Has(patch, "CSGetConstantBuffers"),
               "and saves what it found, so the game's compute state comes back");
        EXPECT(Has(patch, "ComputeStateRestore"), "the restore is a scope guard, so a throwing call cannot leave it bound");
        EXPECT(!Has(patch, "UpdateSubresource"), "nothing of the game is written through the context");
        EXPECT(Has(patch, "D3D11_MAP_WRITE_DISCARD"), "the CPU-written mode writes its own dynamic buffer");
        EXPECT(Has(patch, "if(request.equal)returnnullptr;"), "a target equal to the engine's value swaps nothing");
        EXPECT(Has(patch, "if(slot->written&&slot->frame==request.frame)returnslot->buffer.Get();"),
               "a frame writes its buffer once, not once per draw, and a buffer that was never written is written even in frame 0");
        EXPECT(Has(patch, "PSSetConstantBuffers(_slot,1,&_original);") && Has(patch, "PSGetConstantBuffers(_slot,1,&_original)"),
               "the game's buffer is read on the way in and is what comes back");
        EXPECT(Has(patch, R"x(context->End(_disjoint.Get());)x"),
               "the timestamp-disjoint query is ended with its pair: without that End no measurement ever lands");
        EXPECT(Has(patch, R"x(if(timed)EndGpuTiming(context);)x"), "the pair is ended after the second copy, so it covers the whole patch");
        EXPECT(Has(patch, R"x(std::lock_guardlock(_mutex);)x"),
               "a config load can publish from another thread: the primitive serialises its entries so a release never runs under a dispatch");
        EXPECT(Count(patch, "std::lock_guardlock(_mutex);") == 3,
               "the release, the patch (which the scale and the set both go through) and the CPU write each take the one lock, so every public "
               "entry of the primitive serialises with a release");
        EXPECT(Has(patch, "static_assert(sizeof(ParamsBuffer)==16+5*8*16,"), "the parameter block the shader declares is pinned");
        const std::string shader = Code(repo, "ffxiv/engine/shaders/CbScale.hlsl");
        EXPECT(Has(shader, "uint4g_source[1024];"), "the game's buffer arrives as raw bits, so a component beside a knob is never converted");
        EXPECT(Has(shader, R"x(if(multiplier!=1.0f))x"),
               "a component whose multiplier is exactly 1 keeps its bits: a denormal, an integer-as-float or a NaN payload beside the knob "
               "comes out unchanged");
        EXPECT(!Has(shader, "i+=64u"), "one thread per float4: no group redoes another's work");
        EXPECT(Has(shader, R"x(if((mask&1u)!=0u)value.x=g_setvalue[s].x;)x"),
               "a set entry replaces only the components its mask addresses: every other component keeps the bits the game wrote");
    }

    // ---- the rules stay pure, so the host tests can build them alone
    {
        const std::string rules = Code(repo, "ffxiv/engine/EnginePassRules.h");
        EXPECT(!Has(rules, "d3d11") && !Has(rules, "ID3D11"), "the rules are D3D free");
        EXPECT(!Has(rules, "std::filesystem") && !Has(rules, "std::ofstream"), "the rules do no I/O");
        EXPECT(Has(rules, R"x(inlineconstexpruint32_tkConsecutiveDeclineLimit=64;)x"), "the refusal limit the design names");
        EXPECT(Has(rules, R"x(if(recorded==0)return0;)x") && Count(rules, "if(recorded==0)return0;") == 1,
               "a find stops at the first empty slot: nothing is ever removed and an insert takes the first empty slot of its own probe, so no "
               "address sits beyond an empty slot and a miss costs the probe the table's density gives it rather than the whole probe bound");
        EXPECT(Has(rules, R"x(uint32_tambientDeclines=0;)x") && Has(rules, R"x(uint32_twetnessDeclines=0;)x") &&
                   Has(rules, R"x(boolNoteDecline(Knobknob))x") && Has(rules, R"x(voidNoteRun(Knobknob))x"),
               "each knob keeps its own refusal streak, so one knob's successes cannot keep the other from standing down");
        EXPECT(Has(rules, R"x(constexprKnobIntentIntentOf(constParams&params,Anchorsanchors)noexcept)x") &&
                   Has(rules, R"x(constexprVerdictAmbientVerdict(constKnobIntent&intent,constDrawFacts&facts)noexcept)x"),
               "which knobs ask for a draw, and what their refusal is, are pure rules the host tests cover");
        EXPECT(Has(rules, R"x(constexprboolIsAmbientDraw(constPassLayout&layout)noexcept)x"),
               "the shape test is a pure rule, so the host tests cover which shaders reach the draw path");
        EXPECT(Has(rules, R"x(constexprAnchorsDrawAnchors(constPassLayout&layout)noexcept)x"),
               "both knobs' slots and bits travel to the draw path in one value");
        EXPECT(Has(rules, R"x(constexpruint32_tWetnessSlotOf(Anchorsanchors)noexcept)x"),
               "the wetness slot travels in the same value as the ambient one");
        EXPECT(Has(rules, R"x(usingAnchors=uint64_t;)x"),
               "the anchors word is 64 bits wherever it travels: the index's record, the seam's one value and the draw path");
        EXPECT(Has(rules, R"x(inlineconstexprAnchorskOcclusionReaderBit=1ull<<32;)x") &&
                   Has(rules, R"x(inlineconstexprAnchorskShaftsLightBit=1ull<<41;)x") &&
                   Has(rules, R"x(inlineconstexprAnchorskShaftsCompositeBit=1ull<<42;)x") &&
                   Has(rules, R"x(inlineconstexprAnchorskShaftsToneMapBit=1ull<<43;)x") &&
                   Has(rules, R"x(inlineconstexprAnchorskGlossReaderBit=1ull<<52;)x") &&
                   Has(rules, R"x(inlineconstexprAnchorskReflectionReaderBit=1ull<<61;)x"),
               "the map is the fixed one: ambient occlusion at bit 32, light shafts at 41..43, the puddles reader at 52 and the reflections reader "
               "at 61, each with its slot field above the shipped word where it has one");
        EXPECT(Has(rules, R"x(static_assert(AnchorsMapIsSound(),)x"),
               "the map is asserted at compile time - no two flag bits or slot fields share a bit, every slot field is 8 bits - because a "
               "collision would classify one knob's draw with another knob's slot");
        EXPECT(Has(rules, R"x(inlineconstexprfloatkWetnessOverrideDefault=-1.0f;)x"),
               "the wetness knob's default is the engine's own value, off");
        EXPECT(Has(rules, R"x(structFrameCache)x") && Has(rules, R"x(if(++served<kMaxServedDraws))x"),
               "the per-frame cache and its expire rule are pure, so the host tests cover the frame transitions");
        EXPECT(Has(rules, R"x(inlineconstexpruint32_tkAmbientScaleOffset=240;)x"), "the probe layout's offset");
        EXPECT(Has(rules, R"x(inlineconstexpruint32_tkAmbientSkyRecipOffset=112;)x"),
               "the second field's offset: 0x40 header + 0x30, the float4 whose .w carries shRecipScale");
        EXPECT(Has(rules, R"x(entry.multiplier[3]=scale;)x"),
               "the second entry scales .w alone: .xyz is the probe's colour and keeps its multiplier at 1");
        EXPECT(Has(rules, R"x(out[1]=AmbientSkyRecipEntry(params.ambientScale);)x") &&
                   Has(rules, R"x(capacity<kAmbientEntryCount)x"),
               "the knob writes both entries or none: one field alone is a knob that dims nothing on a one-probe table");
        EXPECT(Has(rules, R"x(EntryFits(out[0],kAmbientDeclaredBytes)&&EntryFits(out[1],kAmbientDeclaredBytes))x"),
               "both entries are checked against the declared table before the dispatch, not one of them");
        EXPECT(Has(rules, R"x(inlineconstexprfloatkIdentityEpsilon=0.001f;)x"), "the identity epsilon");
    }

    // ---- the keys: off and at vanilla by default, published at the config load, documented, and each with a control
    {
        const std::string config = Code(repo, "ffxiv/FfxivConfig.h");
        EXPECT(Has(config, R"x(CustomOptional<bool>enginePassOverrides{false};)x"), "off by default: today's behaviour");
        EXPECT(Has(config, R"x(CustomOptional<float>ambientSkyVisibilityScale{1.0f};)x"), "the knob's default is the engine's own value");
        EXPECT(Has(config, R"x(CustomOptional<float>wetnessOverride{-1.0f};)x"),
               "the wetness knob's default is the engine's own value, so a fresh ini is today's picture");
        EXPECT(Has(config, R"x(enginePassOverridesOnLoaded(enginePassOverrides.value_or_default());)x"),
               "the key is published at the end of a load");

        const std::string bound = Code(repo, "core/ExtConfig.cpp");
        const std::string overridesLine = std::string("b.Add(\"FFXIV\",\"EnginePassOverrides\",enginePassOverrides)");
        const std::string scaleLine = std::string("b.Add(\"FFXIV\",\"AmbientSkyVisibilityScale\",ambientSkyVisibilityScale)");
        EXPECT(Has(bound, overridesLine), "[FFXIV] EnginePassOverrides is bound");
        EXPECT(Has(bound, scaleLine), "[FFXIV] AmbientSkyVisibilityScale is bound");
        EXPECT(Has(bound, std::string("b.Add(\"FFXIV\",\"WetnessOverride\",wetnessOverride)")), "[FFXIV] WetnessOverride is bound");

        const std::string ini = Raw(repo, "ini/FFXIV.ini");
        for (const char* key : { "\nEnginePassOverrides=auto", "\nAmbientSkyVisibilityScale=auto", "\nWetnessOverride=auto" })
            EXPECT(Has(ini, key), "the ini fragment documents %s", key + 1);
        // both have a menu control, so neither says "No menu control" nor sits in the manifest
        const std::string manifest = Code(repo, "core/MenuManifest.h");
        EXPECT(!Has(manifest, R"x("[FFXIV]EnginePassOverrides",)x") && !Has(manifest, R"x("[FFXIV]AmbientSkyVisibilityScale",)x") &&
                   !Has(manifest, R"x("[FFXIV]WetnessOverride",)x"),
               "the three keys have menu controls, so they are not listed as ini-only");
        EXPECT(Has(manifest, R"x("ffxiv_engine_passes",)x"), "the card id is FFXIV-only and lives in the tab");
        const std::string features = Code(repo, "ExtFeatures.h");
        EXPECT(Has(features, R"x(EnginePassOverrides=1ull<<15,)x"), "the feature bit");

        const std::string menu = Code(repo, "ffxiv/engine/EngineMenu.cpp");
        EXPECT(Has(menu, R"x(cfg.enginePassOverrides=overrides;)x") &&
                   Has(menu, R"x(cfg.ambientSkyVisibilityScale=ClampAmbientScale(scale);)x") &&
                   Has(menu, R"x(cfg.wetnessOverride=ClampWetnessOverride(wetness);)x"),
               "every player-facing key has a control that clamps it");
        EXPECT(Has(menu, R"x(kMinWetnessOverride,kMaxWetnessOverride)x") &&
                   Has(menu, R"x(WetnessAmount(CurrentParams().wetnessOverride))x"),
               "the wetness slider spans the knob's whole range and the state line shows the forced amount and its counters");
        EXPECT(Has(menu, R"x(if(changed)Publish(overrides);)x"),
               "every control publishes its value, so a switch turned on in a running session arms the draw path");
        EXPECT(Has(menu, R"x({{"ffxiv_engine_passes","FFXIV","Enginepassoverrides(FFXIV)",Draw,"output_scaling",Visible})x"),
               "the card is registered");

        const std::string layout = Code(repo, "core/MenuLayout.h");
        EXPECT(Has(layout, R"x({"ffxiv_engine_passes","FFXIV","ffxiv_contact_shadows"},)x"), "the card has its place in the tab");
        EXPECT(Has(layout, R"x({"ffxiv_ambient_occlusion","FFXIV","ffxiv_engine_passes"},)x"),
               "the occlusion card follows the engine card in the FFXIV tab: a card's place in the tab is its `after` field, and the tab's order "
               "is read from that field rather than from the registration order");

        const std::string schema = ReadFile(repo + "/tools/settings-schema.json");
        for (const char* key : { "\"[FFXIV] EnginePassOverrides\"", "\"[FFXIV] AmbientSkyVisibilityScale\"", "\"[FFXIV] WetnessOverride\"" })
            EXPECT(Has(schema, key), "the schema carries %s", key);
        EXPECT(Has(schema, "\"card\": \"ffxiv_engine_passes\""), "both entries point at the card");
    }

    // ---- the puddles and the reflections keys: both cards off by default, and every other key at the value its own rules were tuned to
    {
        // A card that ships on is not today's picture: the two switches are pinned squashed exactly as the config writes them, and the rest of each
        // card is pinned too, because a default that drifts changes what a player sees when they turn the card on and no test above reads the key.
        const std::string config = Code(repo, "ffxiv/FfxivConfig.h");
        EXPECT(Has(config, R"x(CustomOptional<bool>puddles{false};)x"), "the puddles ship off: a fresh ini is today's picture");
        EXPECT(Has(config, R"x(CustomOptional<float>puddlesCoverage{0.42f};)x"),
               "the coverage default: about a third of the up-facing ground, the value the field was measured at");
        EXPECT(Has(config, R"x(CustomOptional<float>puddlesScale{6.0f};)x"),
               "the noise scale default: the measured field's median pool is 4.3 m across at six-metre cells");
        EXPECT(Has(config, R"x(CustomOptional<bool>puddlesReflection{true};)x"), "the mirror ships on");
        EXPECT(Has(config, R"x(CustomOptional<float>puddlesDarkening{0.45f};)x"), "the darkening default");
        EXPECT(Has(config, R"x(CustomOptional<float>puddlesReflectionStrength{0.6f};)x"), "the reflection strength default");
        EXPECT(Has(config, R"x(CustomOptional<float>puddlesRipple{0.35f};)x"), "the ripple default");
        EXPECT(Has(config, R"x(CustomOptional<int32_t>puddlesReflectionSteps{24};)x"), "the march step default");
        EXPECT(Has(config, R"x(CustomOptional<float>puddlesSoftness{0.03f};)x"), "the edge width default: 0.81 m at the measured scale");
        EXPECT(Has(config, R"x(CustomOptional<int32_t>puddlesGlossTarget{1};)x"), "the G-buffer target default");
        EXPECT(Has(config, R"x(CustomOptional<std::string>puddlesGlossChannel{"a"};)x"),
               "the channel default is the material pack's alpha, the gloss the engine's composite reads (its red is a selector)");
        EXPECT(Has(config, R"x(CustomOptional<float>puddlesGlossWet{1.0f};)x"), "the wet value default");
        EXPECT(Has(config, R"x(CustomOptional<bool>screenSpaceReflections{false};)x"),
               "the reflections ship off: a fresh ini is today's picture");
        EXPECT(Has(config, R"x(CustomOptional<int32_t>screenSpaceReflectionsSteps{48};)x"), "the step count default");
        EXPECT(Has(config, R"x(CustomOptional<float>screenSpaceReflectionsThickness{0.3f};)x"), "the thickness default");
        EXPECT(Has(config, R"x(CustomOptional<float>screenSpaceReflectionsBlend{0.5f};)x"),
               "the blend default: the traced reflection with a share of the engine's own soft map still under it");
        EXPECT(Has(config, R"x(CustomOptional<float>screenSpaceReflectionsFade{0.15f};)x"), "the edge fade default");
    }

    // ---- the shader hook stamps every pass that answers at the pixel-shader seam, and the teardown gives every pass's textures back
    {
        const std::string cpp = Code(repo, "ffxiv/engine/EnginePass.cpp");
        EXPECT(Count(cpp, "gloss::NoteShader(shader,code,size);") == 1 && Count(cpp, "ssr::NoteShader(shader,code,size);") == 1,
               "the puddles and the reflections stamp the shader object, once each, on the same terms as the occlusion's");
        const size_t indexAt = cpp.find("SeenForInsert().Insert(");
        EXPECT(indexAt != std::string::npos && cpp.find("gloss::NoteShader(shader,code,size);") < indexAt &&
                   cpp.find("ssr::NoteShader(shader,code,size);") < indexAt,
               "both stamps are written before the index decides anything: a shader the index does not store (no anchor bit, a full index) still "
               "answers for the slots its own reflection declares, which is what makes a water draw recognisable at all");
        const size_t releaseAt = cpp.find("voidRelease(){");
        const size_t nextAt = cpp.find("voidOnConfigLoaded(", releaseAt == std::string::npos ? 0 : releaseAt);
        EXPECT(releaseAt != std::string::npos && nextAt != std::string::npos, "the teardown is one function with the config hook after it");
        const std::string body =
            releaseAt == std::string::npos || nextAt == std::string::npos ? std::string() : cpp.substr(releaseAt, nextAt - releaseAt);
        EXPECT(Has(body, R"x(ao::Release();)x") && Has(body, R"x(gloss::Release();)x") && Has(body, R"x(ssr::Release();)x") &&
                   Has(body, R"x(ext::ffxiv::shafts::Release();)x"),
               "the teardown releases the ambient occlusion, the puddles, the shafts' and the reflections' owned textures while the device is still "
               "valid: a pass left out keeps a device resource across a device change and for the rest of the process on the unload path");
        EXPECT(Has(body, R"x(state.patch.Release();)x"), "and the scaling primitive's own buffers");
    }

    // ---- the puddles pass reads its camera table without waiting: the layout check is a no-wait map of a copy an earlier frame took, so it
    //      never drains the queue inside one of the game's draws
    {
        const std::string pass = Code(repo, "ffxiv/engine/GlossPuddlesPass.cpp");
        const std::string passHeader = Code(repo, "ffxiv/engine/GlossPuddlesPass.h");
        EXPECT(Count(pass, "D3D11_MAP_READ") == 1 && Count(pass, "DXGI_ERROR_WAS_STILL_DRAWING") == 0,
               "the pass makes one read map, and a busy answer is a wait for a later frame rather than a reason to give up");
        EXPECT(Count(pass, "D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT") == Count(pass, "D3D11_MAP_READ"),
               "that map is a no-wait map: a blocking map of the camera copy would stall the render thread inside a draw");
        EXPECT(Has(pass, R"x(_cameraNotReady=true;)x") &&
                   Has(passHeader, R"x(boolCameraNotReady()constnoexcept{return_cameraNotReady;})x"),
               "a copy that has not landed is reported as its own state, so the caller counts it apart from a refused table");
    }

    // ---- the puddles' gate asks a replay before anything else: a character preview, a diagnostic replay, never gets the frame's water and never
    //      spends the frame's one attempt
    {
        const std::string apply = Code(repo, "ffxiv/engine/GlossPuddlesApply.cpp");
        const std::string rules = Code(repo, "ffxiv/engine/GlossPuddlesRules.h");
        const size_t replayAt = apply.find("if(ext::ffxiv::chara::t_replay)returnnone;");
        const size_t gateAt = apply.find("ServeAnswer(inputs,targetIndex,");
        EXPECT(replayAt != std::string::npos, "the per-draw path reads the character preview's replay marker");
        EXPECT(gateAt != std::string::npos, "the per-draw path asks the gate of one reader draw");
        EXPECT(replayAt != std::string::npos && gateAt != std::string::npos && replayAt < gateAt,
               "the replay is passed on before the gate, so a replayed reader is never served the frame's puddles");
        EXPECT(Count(apply, "ServeAnswer(") == 1 &&
                   Has(apply, R"x(ServeAnswer(inputs,targetIndex,state.frame.valid,state.frame.MayRun(),ext::ffxiv::chara::t_replay);)x"),
               "the gate is the only decision, and it asks the same marker, so a replay that reaches it some other way is passed on too");
        EXPECT(Has(apply, R"x(if(answer==Serve::PassOn)returnnone;)x"),
               "a replay is passed on like any other draw the frame cannot answer: it is not counted as a refusal");
        const size_t attemptAt = apply.find("state.frame.TakeAttempt();");
        EXPECT(Count(apply, "TakeAttempt(") == 1 && attemptAt != std::string::npos && attemptAt > gateAt,
               "the frame's one attempt is taken after the gate, so a replay that is passed on leaves it unspent");
        // In the rule the replay is the first branch of the body: before the target test, before the served frame's texture and before the attempt.
        const size_t bodyAt = rules.find("constexprServeServeAnswer(");
        const size_t ruleReplay = rules.find("if(replay)returnServe::PassOn;");
        EXPECT(bodyAt != std::string::npos && ruleReplay != std::string::npos && ruleReplay > bodyAt,
               "the pure gate carries the replay test as its first branch");
        for (const char* later : { "if(!inputs.HasTarget(targetIndex))", "if(frameValid)", "returnServe::Attempt;" })
            EXPECT(ruleReplay != std::string::npos && ruleReplay < rules.find(later), "the replay test comes before %s", later);
    }

    // ---- the puddles' two refused key values and the wetness they gate on: the material pack's selector channel is refused once per value, and the
    //      amount the shape follows is the one the engine's own draws read (the wetness knob's, while it is armed)
    {
        const std::string apply = Code(repo, "ffxiv/engine/GlossPuddlesApply.cpp");
        const std::string rules = Code(repo, "ffxiv/engine/GlossPuddlesRules.h");
        // The channel that is a selector in the engine's own shaders is refused next to the target that cannot be shaped, and the refusal is
        // guarded by the same rule the identity test uses.
        const size_t targetRefusal = apply.find("FormatTargetRefused(");
        const size_t channelRefusal = apply.find("FormatChannelRefused(");
        EXPECT(targetRefusal != std::string::npos && channelRefusal != std::string::npos && targetRefusal < channelRefusal,
               "a refused channel is logged with its target, after the target's own refusal line");
        EXPECT(Has(apply, R"x(!ChannelServable(state.params.target,state.params.channel))x") &&
                   Has(rules, R"x(|!ChannelServable(params.target,params.channel);)x"),
               "the channel refusal and the identity rule ask the same function, so a refused pair never arms the pass");
        // The wetness the shape follows is chosen once, from the engine's own amount and the knob, and written into the pass's own buffer at the
        // point the draw path notes the frame.
        // Three uses: the frame's copy-and-knob choice, and the tone stage's own gate, which reads the same pair through the same rule.
        EXPECT(Count(apply, "WetnessFor(") == 3 && Has(apply, R"x(if(copied)state.wetnessKnown=true;)x"),
               "the engine's copy still happens (with the knob off it is the value), the knob's branch is beside it, and the tone stage's gate asks"
               " the same function");
        EXPECT(Has(apply, R"x(constfloatknob=engine::ClampWetnessOverride(ext::Cfg().ffxiv.wetnessOverride.value_or_default());)x") &&
                   Has(apply, R"x(returnWetnessKnobFor(ext::Cfg().ffxiv.enginePassOverrides.value_or_default(),knob);)x") &&
                   Has(apply, R"x(if(knob>=0.0f)forced=state.pass.SetWetness(context,WetnessFor(state.pass.EngineWetness(),knob));)x") &&
                   Has(apply, R"x(if(!state.pass.WetLatched())returnnone;)x"),
               "the pass's wetness buffer is written with WetnessFor(engine, knob) only while the engine pass overrides switch is on, a dry frame is "
               "skipped before any reader is swapped (through the dwell: once a reading lands above the floor the gate stays armed until an exact 0.0), "
               "and the run line reads the same pair");
        EXPECT(Has(apply, R"x(LOG_INFO("{}",FormatRun(state.params,facts.targetWidth,facts.targetHeight,WetnessFor(engineWetness,knob),engineWetness,)x"),
               "the run line prints the amount the shader gated on, not only the channel value of a mirror-wet surface");
        // The rule itself: the knob wins while it is armed, the engine's amount otherwise.
        EXPECT(Has(rules, R"x(constexprfloatWetnessFor(floatengine,floatknob)noexcept{returnknob>=0.0f?knob:engine;})x"),
               "WetnessFor is the pure rule the two call sites share");
    }

    if (g_failures)
    {
        std::printf("FAIL: %d expectation(s)\n", g_failures);
        return 1;
    }
    std::puts("PASS: engine pass override wiring pins (one relaxed load while off, four call sites, the game's binding restored, off by "
              "default, the identity value costing nothing, each key with a control)");
    return 0;
}
