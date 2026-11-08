// Text pins of the developer draw diagnostic: where it hangs, what the key off costs, and that it stays a developer switch.
//
// The diagnostic is a log-only probe, so what must not drift is its wiring: one relaxed atomic load on the draw path (and nothing else
// before the branch), the reflection recorded where the shaders are created, the mark of the upscale boundary, the frame closed at the
// Present entry whatever else is armed, the key defaults of today's behaviour, the ini-only entries and the bounded re-armed captures of
// one session.
// Comments and whitespace are ignored, so an edit that changes any of those lines removes its pin.
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

std::string Code(const std::string& repo, const char* relative)
{
    std::ifstream file(repo + "/OptiScaler/ext/" + relative, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::string out;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text.compare(i, 2, "//") == 0)
        {
            while (i < text.size() && text[i] != '\n')
                ++i;
            continue;
        }
        if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n')
            out += text[i];
    }
    return out;
}

std::string Raw(const std::string& repo, const char* relative)
{
    std::ifstream file(repo + "/OptiScaler/ext/" + relative, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

size_t Find(const std::string& text, const std::string& needle, size_t from = 0) { return text.find(needle, from); }
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

size_t Count(const std::string& text, const std::string& needle)
{
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
        ++n;
    return n;
}

// The text in front of the statement that contains `at`, in the comment- and whitespace-free source: a call written as the body of a
// condition ("if (x) call();") carries that condition here, a statement of a block carries nothing.
std::string StatementPrefix(const std::string& code, size_t at)
{
    size_t start = at;
    while (start > 0)
    {
        const char c = code[start - 1];
        if (c == ';' || c == '{' || c == '}')
            break;
        --start;
    }
    return code.substr(start, at - start);
}

struct Pin
{
    const char* fragment;
    const char* what;
};

void Pins(const std::string& code, const char* file, std::initializer_list<Pin> pins)
{
    for (const Pin& pin : pins)
    {
        const size_t n = Count(code, pin.fragment);
        EXPECT(n == 1, "%s: %s (%zu occurrences of %s)", file, pin.what, n, pin.fragment);
    }
}
} // namespace

int main(int, char** argv)
{
    const std::string repo = argv[1];

    // ---- the draw path: one relaxed atomic load, taken before any of the hook's other work, and only once
    {
        const std::string hooks = Code(repo, "ffxiv/lighting/FfxivLightingCapture.h");
        EXPECT(Has(hooks, "#include\"ext/ffxiv/diag/DrawDiag.h\""), "include");
        Pins(hooks, "lighting", {
            { R"x(if(!inside&&ext::ffxiv::diag::Capturing())ext::ffxiv::diag::OnDraw(c,kind);)x", "the draw hook asks the key first" },
            { R"x(ext::ffxiv::diag::OnPixelShaderCreated(*out,code,size,crc);)x", "the reflection is recorded where the shaders are created" },
        });
        EXPECT(Count(hooks, "ext::ffxiv::diag::") == 3, "the diagnostic is called from the two places only");
        const size_t ui = Find(hooks, "ext::ffxiv::ui::OnDraw(c,c==context);");
        const size_t diag = Find(hooks, "ext::ffxiv::diag::Capturing()");
        const size_t earlyOut = Find(hooks, "constboolwanted=");
        EXPECT(ui != std::string::npos && diag != std::string::npos && earlyOut != std::string::npos && ui < diag && diag < earlyOut,
               "the diagnostic sees every draw: after the UI detector and before the hook's first early-out");
        const size_t inside = Find(hooks, "if(SUCCEEDED(hr)&&out&&*out&&code&&size>=32");
        const size_t created = Find(hooks, "ext::ffxiv::diag::OnPixelShaderCreated");
        EXPECT(inside != std::string::npos && created != std::string::npos && inside < created,
               "the reflection is recorded on the same path as the bytecode CRC, not in a second pass");
        // The reflection is recorded for every created pixel shader that has bytecode, not only for the few shapes the lighting scan
        // identifies: a shader the diagnostic cannot name is a row it cannot describe. So the call is a statement of the created-shader
        // block, and the only thing between that block's guard and the call is the CRC the other modules share.
        EXPECT(created != std::string::npos && StatementPrefix(hooks, created).empty(),
               "the reflection is unconditional: a created shader that is not a lighting-scan shape is reflected too");
        const size_t guard = Find(hooks, "){constUINTcrc=Crc(code,size);");
        EXPECT(guard != std::string::npos && guard < created && !Has(hooks.substr(guard, created - guard), "Identify("),
               "no shader-identity test stands inside the created-shader block before the reflection");
    }

    // ---- the key test itself: one load, no state, no D3D in the header
    {
        const std::string header = Code(repo, "ffxiv/diag/DrawDiag.h");
        Pins(header, "DrawDiag.h", {
            { R"x(inlineboolCapturing(){returnext::Active(ext::F::DrawDiag);})x", "the key off is one relaxed atomic load" },
        });
        EXPECT(!Has(header, "std::atomic"), "the hot header keeps no state of its own");
        EXPECT(!Has(header, "#include<d3d11"), "the hot header pulls in no D3D");
        const std::string features = Code(repo, "ExtFeatures.h");
        EXPECT(Has(features, "DrawDiag=1ull<<12,"), "the feature bit");
        EXPECT(Has(features, "inlineboolActive(Ff){return(detail::g_activeFeatures.load(std::memory_order_relaxed)"), "Active() is a relaxed load");
    }

    // ---- the frame and the capture: closed at the Present entry, one capture per session, disarmed when it is written
    {
        const std::string ui = Code(repo, "ffxiv/ui/FfxivUiBoundary.cpp");
        Pins(ui, "present", {
            { R"x(ext::ffxiv::diag::OnPresent();)x", "the frame of the diagnostic is closed at the Present entry" },
        });
        const size_t catalog = Find(ui, "ext::ffxiv::catalog::OnPresent();");
        const size_t diag = Find(ui, "ext::ffxiv::diag::OnPresent();");
        const size_t armed = Find(ui, "RefreshArmed();");
        EXPECT(catalog != std::string::npos && diag != std::string::npos && armed != std::string::npos && catalog < diag && diag < armed,
               "the diagnostic is closed before the Armed() test: it must not need the UI detector to be armed");

        const std::string seams = Code(repo, "Seams.cpp");
        Pins(seams, "evaluate", {
            { R"x(ext::ffxiv::diag::OnUpscaleBoundary();)x", "the upscale boundary is marked at the evaluate" },
        });
        const size_t jitter = Find(seams, "ext::ffxiv::jitter::OnUpscaleBoundary(parameters);");
        const size_t boundary = Find(seams, "ext::ffxiv::diag::OnUpscaleBoundary();");
        EXPECT(jitter != std::string::npos && boundary != std::string::npos && jitter < boundary, "the marker sits in ext::dx11bridge::OnUpscaleBoundary");

        const std::string cpp = Code(repo, "ffxiv/diag/FfxivDrawDiag.cpp");
        Pins(cpp, "runtime", {
            { R"x(constuint8_tphase=S().phase.load(std::memory_order_relaxed);if(context==nullptr||(phase!=kCapturing&&phase!=kWaiting))return;)x", "the draw path leaves after one load when the diagnostic is idle or done" },
            { R"x(voidOnPresent(){if(S().phase.load(std::memory_order_relaxed)!=kCapturing)return;)x", "the present path leaves after one load" },
            { R"x(if(s.phase.load(std::memory_order_relaxed)!=kIdle)return;)x", "a config reload neither starts nor disturbs a capture (a waiting one stays waiting)" },
            { R"x(if(s.arm.trigger!=Trigger::Key||s.captureArm.Exhausted())return;)x",
              "a key capture re-arms once its files are written, up to the session's bound" },
            { R"x(s.captureArm.Poll(KeyDown(s.arm.key)))x", "the key poll is edge triggered: one press, one capture" },
            { R"x(voidResetForNextCapture(State&s))x", "one place resets what the next capture must not inherit" },
            { R"x(s.split=FrameSplitter{};)x", "the splitter starts fresh, so the next capture drops its own partial frame" },
            { R"x(s.arm=PlannedArm();)x", "the delay and the key are read with the frame count, at the config load" },
            { R"x(s.phase.store(s.arm.trigger==Trigger::Immediate?kCapturing:kWaiting,std::memory_order_relaxed);ext::SetActive(ext::F::DrawDiag,true);)x", "the bit is set last, so a draw never sees it without a capture behind it" },
            { R"x(s.phase.store(kDone,std::memory_order_relaxed);ext::SetActive(ext::F::DrawDiag,false);)x", "a written capture puts the draw path back to one load" },
            { R"x(if(s.drawsInFrame>=kMinDrawsPerFrame))x", "only a rendered frame (not a loading screen) counts" },
            { R"x(constexprconstchar*kLightParamName="g_LightParam";)x", "the constant buffer of the lighting passes is found by its reflected name" },
            { R"x(constexprconstchar*kCameraParamName="g_CameraParameter";)x", "the camera buffer beside it gives the direction's space" },
            { R"x(MapConstant(c,s.cameraStaging,camera,kCameraParamProjectionAt+kCameraParamMatrixBytes);)x",
              "one map of the camera buffer covers the view, the view-projection and the projection the sun's direction is projected through" },
            { R"x(std::memcpy(read.viewProjection,cameraBytes+kCameraParamViewProjectionAt,sizeofread.viewProjection);)x",
              "and the row's view-projection is copied from +0x060, so a capture separates a wrong direction from a wrong matrix" },
            { R"x(std::memcpy(read.projection,cameraBytes+kCameraParamProjectionAt,sizeofread.projection);)x",
              "the projection at +0x120 is copied with it: the light's direction is view space, so this is the matrix that places it" },
            { R"x(boolSampleDue(State&s))x", "the constant buffers are read at most once a second" },
            { R"x(boolSampleWanted(State&s))x", "the second is checked without spending it, so a draw that is not the light pass leaves it open" },
            { R"x(voidNoteSampled(State&s){s.lastReadQpc=Qpc();})x", "and only the draw the sample is taken at stamps the clock" },
            { R"x(boolIsLightPass(constDrawTargets&targets,boolblends))x",
              "the light pass is recognised by the shape of its targets (two R11G11B10 buffers, one size, not blended)" },
            { R"x(if(!lightName.empty()&&IsLightPass(targets,blends)&&SampleDue(s))x",
              "the light's own parameters are read at that pass only, not at whichever draw bound g_LightParam first" },
            { R"x(voidDumpSunDrawConstants(ID3D11DeviceContext*c,State&s,constCbSlot(&cb)[8],conststd::string(&names)[8],uint32_tcrc,boolhasCrc))x",
              "the sun draw's own 128-byte constant buffers are dumped with their reflected names" },
            { R"x(if(!cb[i].buffer||cb[i].bytes!=0x80)continue;)x",
              "the dump takes every 128-byte pixel-shader buffer the draw binds, which is the size of g_DirectionalShadowParameter" },
            { R"x(DumpSunDrawConstants(c,s,cb,cbName,crc,row.hasCrc);)x",
              "and it runs at the light pass that writes the frame's buffers, beside the light's own sample, so the neighbour's bytes are in the capture too" },
            { R"x(if(!IsLightPass(ReadTargets(c),blends))return;}NoteSampled(s);)x",
              "the waiting path applies the same rule before it spends the second" },
            { R"x(IsLightPassShape(targets.bound,format,targets.bound!=0&&same,blends))x",
              "and the decision itself is the pure rule the host test drives" },
            { R"x(constuint8_tsplit=s.split.NoteDraw(row.stage,s.drawsInFrame);)x", "the tone map draw closes the frame: a capture needs no bridge Present" },
            { R"x(if(phase==kCapturing)Draw(context,api);elseWaitTick(context);)x",
              "a waiting capture collects no draw row: the key poll and the constant-buffer sample are the whole waiting path" },
            { R"x(if(start){StartCollecting();return;})x",
              "the key or the delay starts the collection on the waiting path, which then returns for that draw" },
            { R"x(if(s.drawsInFrame>kMaxRowsPerFrame))x", "the per-frame row cap" },
            { R"x(constuint64_tsequence=++s.drawSequence;)x", "the dump's draw order is the capture's own counter, which a frame close does not restart" },
            { R"x(DumpCopyPending(c,s,sequence);)x", "the readback copies are queued by that counter, not by the per-frame ordinal" },
            { R"x(DumpCopySampled(c,s,srv,sequence);)x", "the G-buffer is copied at the first later draw that samples it" },
            { R"x(DumpNoteDraw(s,row,targets,srv,reflection,sequence);)x", "the dump is identified from the state this draw already read" },
            { R"x(DumpIdentify(s,"vposition",srv[i].resource.Get(),srv[i].texture,row.ordinal,sequence,DumpRank::ViewPosition);)x",
              "the view-position buffer is one of the dump's targets, so the reflections' ray origin can be read offline" },
            { R"x(elseif(IsViewPositionName(name)))x",
              "the dump identifies it by either name the engine gives it, so it does not depend on which pass a capture named" },
            { R"x(elseif(name=="g_SamplerShadow")DumpIdentify(s,"atlas",srv[i].resource.Get(),srv[i].texture,row.ordinal,sequence,DumpRank::Atlas);)x",
              "the cascade shadow atlas is one of the dump's targets, so a session can read the texels the god-rays march compares against" },
            { R"x(elseif(name=="g_SamplerShadowMask")DumpIdentify(s,"sunmask",srv[i].resource.Get(),srv[i].texture,row.ordinal,sequence,DumpRank::SunMask);)x",
              "the screen-space sun mask is one of the dump's targets named apart from the atlas, so the five-stop capture can read the mask's own texels (the shadow-mapping lever's U-a)" },
            { R"x(elseif(name=="g_SamplerReflectionMap")DumpIdentify(s,"planar",srv[i].resource.Get(),srv[i].texture,row.ordinal,sequence,DumpRank::WaterGate);)x",
              "the water draws' planar map is a dump target: its alpha traffic is one candidate the puddles' water gate reads" },
            { R"x(constboolwaterPair=DumpHeld(s,"light_diffuse")&&ReflectionHas(s,reflection,"g_SamplerReflectionMap")&&!HoldsResource(s,"light_diffuse",srv[i].resource.Get())&&!HoldsResource(s,"light_specular",srv[i].resource.Get());)x",
              "the transparency-light pair is taken only above the plain light names, only at a water reader, and only when the main pair is already held on another resource - the reviewer's F4: the same names make a chain below unreachable" },
            { R"x(ReflectionHas(s,reflection,"g_AmbientParamArray")&&ReflectionHas(s,reflection,"g_SamplerLightDiffuse"))x",
              "the composite's own occlusion is identified by the composite's shape, so no other occlusion binding takes its place" },
            { R"x(elseif(FogDumpTargetOf(name,true)!=nullptr&&ReflectionHas(s,reflection,"cFogParam")))x",
              "the fog post's two textures are dump targets only at a draw whose shader also declares the post's parameter buffer; the sun sprite's sky texture is not" },
            { R"x(DumpIdentify(s,FogDumpTargetOf(name,true),srv[i].resource.Get(),srv[i].texture,row.ordinal,sequence,FogDumpRankOf(FogDumpTargetOf(name,true)));)x",
              "the LUT, the sky and the depth take the ranks the rules give them (with the scene and the view positions: no stage image displaces them)" },
            { R"x(elseif(IsBufferView(srv[i].viewDimension)&&IsStructuredDumpName(StructuredSrvName(s,reflection,i))))x",
              "the composite's per-material shading table is identified at the one buffer view the name names, by the name the structured kind answers; every other bind of the name is not the row table" },
            { R"x(constchar*name=NameAt(s,reflection,BindKind::Other,static_cast<uint32_t>(slot));)x",
              "a structured buffer is filed under the kind the slot decisions do not ask, so its name is looked up through that kind (the texture question never answers it)" },
            { R"x(DumpIdentifyStructured(s,srv[i].buffer.Get(),srv[i].bufferBytes,srv[i].bufferStride,srv[i].bufFirstElement,)x",
              "the structured identification reads the description the slot read took (view first/count, buffer width/stride), so no D3D call is under the reflection mutex" },
            { R"x(if(CbDumpPointersPerSecond(ResourceBaseName(name))>1))x",
              "the shadow parameter's cascade instances are sampled per pointer, so one window carries the frame's four differently" },
            { R"x(constext::ffxiv::shafts::SunProjectionsun=ext::ffxiv::shafts::ProjectSun(light.projection,light.direction,window);)x",
              "engine.txt's sun line is projected here from the diagnostic's own light read, so it is there with the light shafts card off" },
            { R"x(constState::LightSample&light=S().lastLight;)x",
              "that read is the diagnostic's own sample of the light pass, kept session-wide for the line" },
            { R"x(std::memcpy(sample.projection,read.projection,sizeofsample.projection);)x",
              "the camera the light draw carried is what the sun's direction is projected through" },
            { R"x(if(WindowCounter(diagnostics.passedOn[kReadPassedOn],baseline.passedOn[kReadPassedOn])>0))x",
              "the reflections' water-draw line answers for this capture's window, not the session's last read" },
            { R"x(if(!s.dumpActive||s.dumpWritten||!DumpIdentifies(s.split.fresh))return;)x",
              "nothing is identified in the partial frame the capture started in" },
            { R"x([&s,name,rank](std::stringreason))x", "a skipped texture is held once with its reason and its priority, so facts.txt names it once" },
            { R"x(constFrameTallytally=TallyFrame(s.drawsInFrame,s.omittedInFrame,AppendDraws(s,text));)x",
              "the trailer counts the rows and frames the file holds, not the rows a capped frame dropped" },
            { R"x(constDumpAddVerdictverdict=DumpAddWhat(s.dumpActive,s.dumpWritten,resource,DumpHeld(s,name),DumpCount(s));if(verdict==DumpAddVerdict::Ignore)return;)x",
              "a name is identified once, and twenty-four engine files bound the set (the stage images have places of their own)" },
            { R"x(if(verdict==DumpAddVerdict::NoPlace){skip(kDumpNoPlaceReason);return;})x",
              "a texture that finds the engine places full is held as a skipped file with its reason, never refused in silence (how scene.bin, fog_lut.bin and sky.bin went missing)" },
            { R"x(count+=(item.note.empty()&&!item.stage)?1:0;)x",
              "the engine places count the written engine items only: a stage image never uses one" },
            { R"x(DumpHoldholds[kMaxDumpTextures+kMaxStageImages];)x",
              "the make-room list covers the engine items and the stage images together" },
            { R"x(elseif(IsAoDepthReader(ReflectionHas(s,reflection,"g_SamplerOcclusion"),name)){DumpIdentify(s,kAoDepthDumpFile,srv[i].resource.Get(),srv[i].texture,row.ordinal,sequence,FogDumpRankOf(kAoDepthDumpFile));DumpMarkDepth(s,kAoDepthDumpFile,srv[i].resource.Get(),srv[i].format);})x",
              "the first draw that binds the occlusion and a g_SamplerDepth identifies the AO readers' own depth copy (aodepth.bin), cropped like depth.bin and ranked with it" },
            { R"x(item.cropped=true;)x", "the depth is kept whole until the write and cropped to the draw's window there (a depth texture cannot be copied in part)" },
            { R"x(DepthImageNote(item->name,item->viewFormat,)x", "the cropped depth carries its window, box and texture in a comment line" },
            { R"x(s.dumpWritten=true;++s.dumpsWritten;)x",
              "a dump that ran counts against the session's allowance (one that never ran spends nothing)" },
            { R"x(s.dumpItems.clear();s.dumpHeldBytes=0;s.dumpWritten=false;s.dumpActive=DumpEnabled(ext::Cfg().ffxiv.drawDiagnosticsDump.value_or_default())&&DumpMayStart(s.dumpsWritten);)x",
              "the next capture of a session re-opens the dump while the key asks for it and the allowance holds; its flag and holds start empty" },
            { R"x(constuint64_tfreed=DumpMakeRoom(holds,count,rank,item.bytes,s.dumpHeldBytes,s.dumpBudget);)x",
              "a texture of a higher priority releases the holds of a lower one instead of being refused, inside the configured budget" },
            { R"x(voidDumpIdentifyStructured(State&s,ID3D11Buffer*buffer,uint32_tbufferBytes,uint32_tstride,uint32_tfirstElement,)x",
              "the structured row table has one place of its own, and it is identified in one place like every texture's shape is" },
            { R"x(skip("notthe128-Browtable");)x",
              "a structured view whose stride is not the 128-B row is refused with its reason, so facts.txt says why the table is not there" },
            { R"x(if(!DumpIdentFits(s.dumpHeldBytes,item.bytes,s.dumpBudget)){skip("overthedump'sbound");return;}s.dumpHeldBytes+=item.bytes;)x",
              "the row table is bounded by the same budget rules; it never displaces a texture to fit (the make-room path is not asked)" },
            { R"x(held.note="displacedby"+std::string(name);)x", "a released hold says which texture took its place" },
            { R"x(if(DumpSampledSlot(bound,kMaxSlots,item.resource.Get())<0)continue;)x",
              "the G-buffer copy's match is the row of bound resources, not a second scan" },
            { R"x(if(!keepTone&&s.dumpToneFrame!=0&&frame==s.dumpToneFrame)s.dumpToneAtPresent=true;)x",
              "the tone-map frame is whole once a Present closed it" },
            { R"x(s.dumpToneAtPresent=false;)x", "the frame-over flag belongs to one capture" },
            { R"x(caseDumpEnd::Skip:item.note="notsampled";continue;)x",
              "a G-buffer target no draw sampled is skipped at the end of a capture" },
            { R"x(FinishDump(s);)x", "the end of a capture writes a pending dump with the game's context" },
            { R"x(s.shadersWithoutReflection=0;)x", "the missing-reflection count belongs to one capture" },
            { R"x(voidKeepCreationFixture(ID3D11PixelShader*shader,constvoid*code,size_tsize,uint32_tcrc))x",
              "the shaders kept from creation are kept in one place, bounded, and carry their CRC from there" },
            { R"x(if(shader!=nullptr)shader->SetPrivateData(kShaderCrcTag,sizeofcrc,&crc);)x",
              "a shader kept from creation is stamped with its CRC, so a draw of it can be named at all" },
            { R"x(if(s.creationReflections.find(crc)==s.creationReflections.end()&&s.creationReflections.size()<kMaxCreationFixtures))x",
              "and its reflected names and slots are kept in their own bounded table, outside the session table's bound" },
            { R"x(s.creationReflections.emplace(crc,std::move(reflection));)x",
              "which is what names its draws and samples its buffers when the session's table had no room" },
            { R"x(constReflection*ReflectionFor(constState&s,uint32_tcrc))x",
              "one lookup answers for a draw's shader: the session table, then the shaders kept from creation" },
            { R"x(reflection=ReflectionFor(s,crc);)x", "the draw names a shader through that lookup" },
            { R"x(constReflection*reflection=crc!=0?ReflectionFor(s,crc):nullptr;)x", "and so does the waiting path" },
            { R"x(if(BindingAnswers(probe,kind,slot))x",
              "the name a slot carries is decided by the typed rule, never by a comparison written here" },
            { R"x(dumpName[i]=CbDumpNameIndex(name);)x", "a named buffer of the cbdump list is what gets sampled into cbdump.csv" },
            { R"x(c->PSGetConstantBuffers(static_cast<UINT>(kHighCbFirst),static_cast<UINT>(kDumpCbSlots-kHighCbFirst),highBuffers);)x",
              "the slots past the first eight are fetched for the byte dump's names (the chara shaders bind their ambient block at b8 or b9)" },
            { R"x(dumpName[kHighCbFirst+i]=CbDumpNameIndex(name);)x",
              "a named buffer bound past the first eight slots reaches the byte dump, without a draw-row entry" },
            { R"x(for(size_ti=0;i<kDumpCbSlots;++i){if(dumpName[i]<0)continue;)x",
              "the byte dump's sampling loop covers every slot the API has, not only the first eight" },
            { R"x(s.creationOn.store(capture,std::memory_order_relaxed);)x",
              "the creation fixtures follow the key, not the capture phase" },
            { R"x(crc==0||(!capturing&&!S().creationOn.load(std::memory_order_relaxed)))x",
              "a created shader costs two relaxed loads while the draw diagnostic is off, and no scan" },
            { R"x(MentionsCreationFixtureName(code,size))x",
              "the byte scan runs before the reflection parse of a shader kept from creation" },
            { R"x(FixtureListLine(fixture.crc,fixture.code.size(),true,fixture.names))x",
              "the shaders kept from creation are listed with their origin, so the offline tools can tell them apart" },
            { R"x(s.dumpBudget=DumpBudgetBytes(ext::Cfg().ffxiv.drawDiagnosticsDumpBudgetMiB.value_or_default());)x",
              "the one-frame dump's budget is the key, clamped" },
            { R"x(if(!WriteFile(dir/kEngineFile,EngineStatusText(EngineStatusNow())+SsrStatusText(SsrStatusNow())+PuddleStatusText(PuddleStatusNow())))x",
              "every capture writes the engine pass overrides' counters to engine.txt, the screen-space reflections' after them and the puddles' last" },
            { R"x(TakeEngineBaseline(s);TakeSsrBaseline(s);TakePuddleBaseline(s);s.lastReadQpc=0;s.phase.store(kCapturing,std::memory_order_relaxed);)x",
              "a delayed or key capture takes the window baselines and clears the light sample's clock where it arms, before its first patchable draw" },
            { R"x(if(s.arm.trigger==Trigger::Immediate){TakeEngineBaseline(s);TakeSsrBaseline(s);TakePuddleBaseline(s);})x",
              "an immediate capture takes the window baselines before the first draw it collects" },
            { R"x(status.windowRuns=WindowCounter(diagnostics.runs,base.runs);)x",
              "engine.txt's window counts are the session counters minus the baseline the capture took" },
            { R"x(status.windowReaders=WindowCounter(diagnostics.readers,baseline.readers);)x",
              "the reflections' window reader count is their session count minus the baseline the capture took, so a capture answers for its own frames" },
            { R"x(ext::FfxivConfig::drawDiagnosticsOnLoaded=&ext::ffxiv::diag::OnConfigLoaded;)x", "the key reaches the diagnostic through the config's hook pointer" },
        });
        // The creation scan's own name list lives in the pure header (the runtime only calls it): the fog post's two
        // parameter names and the depth-of-field weight parameter ride the `c` branch (the flare family's pattern). The post's
        // texture names are not scanned for: a short lower-case name could be any post shader's.
        {
            const std::string format = Code(repo, "ffxiv/diag/DrawDiagFormat.h");
            Pins(format, "format bits", {
                { R"x(std::string_view("cFogParam"),std::string_view("cExpHeightFogParam"),std::string_view("cWeightSamplingParam")})x",
                  "the scan list carries the fog post's two parameter names and the depth-of-field weight parameter, so those shaders are kept while the game loads them" },
            });
            EXPECT(!Has(format, "bytes[i]=='s'"), "the scan has no lower-case-s arm: the post's texture names never qualify a shader for the creation list");
        }

        // Markers are noted before the cap: a frame that loses rows still reports its tone map, UI and boundary ordinals.
        const size_t note = Find(cpp, "s.markers.NoteDraw(row.stage,row.ordinal);");
        const size_t cap = Find(cpp, "if(s.drawsInFrame>kMaxRowsPerFrame)");
        EXPECT(note != std::string::npos && cap != std::string::npos && note < cap, "markers are noted before the row cap is applied");
        // A skipped texture carries the rank of its kind exactly like a written one: facts.txt is written in the dump's rank order, so a skip
        // that lost its rank would sort in front of the wetness target and name the wrong texture first.
        EXPECT(Count(cpp, "item.rank=uint8_t(rank);") == 2,
               "the held texture and the skipped one both record the rank of their kind, which is the order facts.txt is written in");
        // The split is asked after the row is kept, so the tone map draw is the closed frame's last row, not the next one's first.
        const size_t kept = Find(cpp, "s.frameRows+=DrawsRowText(row);");
        const size_t split = Find(cpp, "constuint8_tsplit=s.split.NoteDraw(row.stage,s.drawsInFrame);");
        EXPECT(kept != std::string::npos && split != std::string::npos && kept < split, "the tone map row is kept before its frame closes");
        // The marker state and the splitter are reset at a frame boundary. The splitter is reset only by the bridge Present: the tone map
        // that closes a window opens the next one too, so a splitter reset at a close made every second window span two game frames.
        const size_t close = Find(cpp, "voidCloseFrame(ID3D11DeviceContext*c,boolkeepTone)");
        const size_t splitReset = Find(cpp, "if(!keepTone)s.split.Reset();");
        EXPECT(close != std::string::npos && splitReset != std::string::npos && close < splitReset,
               "the frame is reset with it, and only a Present resets the splitter");
        EXPECT(Count(cpp, "s.markers.Reset();") == 3, "the marker plan is reset at both frame boundaries and before the next capture");
        // The first tone map drops the partial frame the capture started in, so the first window is one whole frame.
        EXPECT(Count(cpp, "if((split&kSplitArm)!=0)DropPartialFrame(s);") == 1, "the first tone map drops the partial frame");
        EXPECT(Has(cpp, "voidDropPartialFrame(State&s)"), "the partial frame is dropped in one place");
        // One close, one finish test: the frame end exists in two places only (the split and the bridge Present), and both ask the same.
        EXPECT(Count(cpp, "if(s.framesCaptured>=s.wantedFrames)Finish();") == 2, "a frame end always tests whether the capture is complete");
        EXPECT(Count(cpp, "CloseFrame(c,true);") == 1 && Count(cpp, "CloseFrame(nullptr,false);") == 1,
               "the split keeps the tone map armed and the Present starts the splitter over");
        // Waiting for the capture costs one draw in kWaitDrawsPerPoll: the key poll and the sample sit behind that counter, never in Draw().
        const size_t waitTick = Find(cpp, "voidWaitTick(ID3D11DeviceContext*c){");
        const size_t keyDown = Find(cpp, "KeyDown(s.arm.key)");
        const size_t drawFn = Find(cpp, "voidDraw(ID3D11DeviceContext*c,constchar*api){");
        EXPECT(waitTick != std::string::npos && keyDown != std::string::npos && drawFn != std::string::npos && waitTick < keyDown,
               "the capture key is read on the waiting path only");
        EXPECT(drawFn < waitTick, "the collecting path is written first and the waiting path after it, so the key is not in the collecting one");
        EXPECT(Count(cpp, "GetAsyncKeyState(") == 1, "one key read, in one place");
        // The staging copies are queued before the reflection mutex is taken, never under it.
        const size_t srvLoop = Find(cpp, "c->PSGetShaderResources(0,kMaxSlots,views);");
        const size_t sampled = Find(cpp, "DumpCopySampled(c,s,srv,sequence);");
        const size_t lock = Find(cpp, "std::lock_guardlock(s.mutex);");
        EXPECT(srvLoop != std::string::npos && sampled != std::string::npos && lock != std::string::npos && srvLoop < sampled &&
                   sampled < lock,
               "the G-buffer copy is queued after the draw's slots are read and before the reflection lock is taken");
        // Every path that ends or writes the dump releases its holds on game textures: the write (it moves the items out), the capture's
        // reset, and the end of a capture (the no-key path returns before the reset).
        EXPECT(Count(cpp, "s.dumpItems.clear();") == 3, "the dump's references are released at the write, the reset and the end of a capture");
        // The structured row dump rides the same three-path release: its staging is a buffer, released wherever the
        // texture's is (the write's two tails and the bound check), and one map branch carries the buffer itself.
        EXPECT(Count(cpp, "item->bufferStaging.Reset();") == 3,
               "the buffer's staging is released at the write's two tails and the bound check, like the texture's is");
        // The structured copy is a staging buffer of just the view's rows, copied from the view's first row (a region, not the
        // whole buffer): the staging width, the box and the guard all carry the first row's byte offset.
        EXPECT(Has(cpp, "stagingDesc.ByteWidth=static_cast<UINT>(item.bytes);"),
               "the buffer's staging holds just the rows the view covers, not the whole game buffer");
        EXPECT(Has(cpp, "constD3D11_BOXrows{item.srcByteOffset,0,0,static_cast<UINT>(item.srcByteOffset+item.bytes),1,1};") &&
                   Has(cpp, "c->CopySubresourceRegion(staging.Get(),0,0,0,0,buffer.Get(),0,&rows);"),
               "the buffer's readback copies the byte range from the view's first row");
        EXPECT(Has(cpp, "item.srcByteOffset=StructuredDumpByteOffset(firstElement,stride);") &&
                   Has(cpp, "uint64_t(item.srcByteOffset)+item.bytes>desc.ByteWidth"),
               "the first row's offset comes from the pure rule and the guard checks offset plus length against the allocation");
        EXPECT(Count(cpp, "caseD3D11_SRV_DIMENSION_BUFFER:") == 1 && Count(cpp, "caseD3D11_SRV_DIMENSION_BUFFEREX:") == 1 &&
                   Count(cpp, "D3D11_SRV_DIMENSION_BUFFER") == 4,
               "one case each reads the plain and the extended buffer view's own span, and the two other references are IsBufferView's");
        // A displaced structured table gives its staging back like a displaced texture does, so it is never written over the bound.
        EXPECT(Has(cpp, "held.note=\"displacedby\"+std::string(name);held.staging.Reset();held.bufferStaging.Reset();"),
               "a displaced hold releases both kinds of staging");
        // The readback image is allocated before the copy is mapped, so an allocation failure cannot leave it mapped.
        const size_t pixels = Find(cpp, "std::vector<unsignedchar>pixels(");
        const size_t map = Find(cpp, "c->Map(item->staging.Get()");
        const size_t bufferMap = Find(cpp, "c->Map(item->bufferStaging.Get()");
        EXPECT(pixels != std::string::npos && map != std::string::npos && pixels < map, "the dump's image is allocated before its copy is mapped");
        EXPECT(bufferMap != std::string::npos && pixels < bufferMap,
               "the structured stage is mapped in the same place its row copy' image is allocated");
        // Both writers of the dump ask one frame-over rule: a two-frame capture whose tone-map frame closed at a bridge Present is written at
        // that Present, and a tone-map split's close is not enough.
        EXPECT(Count(cpp, "!DumpFrameOver(s.frames,s.dumpToneFrame,s.dumpToneAtPresent)") == 2,
               "MaybeWriteDump and FinishDump share the frame-over rule");
        EXPECT(Has(cpp, "switch(DumpAtEnd(DumpRank(item.rank),item.sequence,s.drawSequence,s.dumpToneAtPresent))"),
               "the end of a capture asks the shared end rule for every pending copy (skip, copy or leave)");
        EXPECT(Count(cpp, "DumpReleaseBytes(s,item);") == 3,
               "a staging copy that failed gives its bytes back in all three copy paths");
        // A 16-bit depth target is two bytes per texel: a 4 there made its byte count, and the file, twice the texture's size.
        EXPECT(Has(cpp, "DXGI_FORMAT_D16_UNORM:return2;"), "the dump's pixel-size table counts a 16-bit depth target as two bytes");
        const size_t finishDump = Find(cpp, "FinishDump(s);");
        const size_t finishItems = Find(cpp, "s.dumpItems.clear();", finishDump);
        const size_t trigger = Find(cpp, "if(s.arm.trigger!=Trigger::Key||s.captureArm.Exhausted())return;");
        EXPECT(finishDump != std::string::npos && finishItems != std::string::npos && trigger != std::string::npos && finishItems < trigger,
               "the end of a capture releases the dump's references before it returns to the waiting state");
        // Nothing in the output names a path, a user or a machine.
        EXPECT(!Has(cpp, "GetUserName") && !Has(cpp, "GetComputerName") && !Has(cpp, "C:\\\\"), "no user, machine or absolute path in the output");
        EXPECT(!Has(cpp, "CreateFileA(\"C:") && !Has(cpp, "getenv"), "no environment path");
    }

    // ---- the draw arguments are remembered for both probes behind the same one atomic test
    {
        const std::string plate = Code(repo, "ffxiv/ui/PlateDraws.h");
        Pins(plate, "NoteDraw", {
            { R"x(if(ext::ActiveAny(ext::F::PlateProbe,ext::F::DrawDiag))g_args={kind,count,instances,start,baseVertex};)x", "one load serves both probes" },
        });
        EXPECT(Count(plate, "ext::hudless::plate::Enabled()") == 1, "the argument note no longer tests one probe's bit only (the vertex shader stamp still does)");
    }

    // ---- the keys: today's behaviour by default, ini only, and the frame count is clamped
    {
        const std::string ffxivConfig = Code(repo, "ffxiv/FfxivConfig.h");
        Pins(ffxivConfig, "FfxivConfig.h", {
            { R"x(CustomOptional<std::string>drawDiagnostics{"off"};)x", "the default is off: today's behaviour" },
            { R"x(CustomOptional<int32_t>drawDiagnosticsFrames{3};)x", "three frames by default" },
            { R"x(CustomOptional<int32_t>drawDiagnosticsDelaySeconds{0};)x", "no delay by default: today's behaviour" },
            { R"x(CustomOptional<std::string>drawDiagnosticsKey{"off"};)x", "no key by default" },
            { R"x(drawDiagnosticsOnLoaded(drawDiagnostics.value_or_default()=="capture");)x", "any other value counts as off, and the key is published at the end of every config load" },
        });
        const std::string configCpp = Code(repo, "core/ExtConfig.cpp");
        Pins(configCpp, "ExtConfig.cpp", {
            { R"x(b.Add("FFXIV","DrawDiagnostics",drawDiagnostics);)x", "the key" },
            { R"x(b.Add("FFXIV","DrawDiagnosticsFrames",drawDiagnosticsFrames);)x", "the frame count key" },
            { R"x(b.Add("FFXIV","DrawDiagnosticsDelaySeconds",drawDiagnosticsDelaySeconds);)x", "the delay key" },
            { R"x(b.Add("FFXIV","DrawDiagnosticsKey",drawDiagnosticsKey);)x", "the capture key" },
        });
        const std::string readme = Raw(repo, "ini/FFXIV.ini");
        EXPECT(Has(readme, "\nDrawDiagnostics=auto"), "the ini fragment documents the key");
        EXPECT(Has(readme, "\nDrawDiagnosticsFrames=auto"), "the ini fragment documents the frame count");
        EXPECT(Has(readme, "\nDrawDiagnosticsDelaySeconds=auto"), "the ini fragment documents the delay");
        EXPECT(Has(readme, "\nDrawDiagnosticsKey=auto"), "the ini fragment documents the capture key");
        // The ini-only rule of the menu manifest test: a key without a control says so in its own comment.
        for (const char* key : { "DrawDiagnostics=", "DrawDiagnosticsFrames=", "DrawDiagnosticsDelaySeconds=", "DrawDiagnosticsKey=" })
        {
            const size_t at = readme.find(key);
            EXPECT(at != std::string::npos, "fragment line %s", key);
            if (at == std::string::npos)
                continue;
            size_t begin = at;
            while (begin > 0 && readme[begin - 1] != '\n')
                --begin;
            // walk the comment block above the key line
            size_t block = begin;
            while (block > 0)
            {
                size_t previous = block - 1;
                while (previous > 0 && readme[previous - 1] != '\n')
                    --previous;
                if (readme.compare(previous, 1, ";") != 0)
                    break;
                block = previous;
            }
            EXPECT(readme.find("No menu control", block) < at, "the comment of %s says that it has no menu control", key);
        }
        const std::string manifest = Code(repo, "core/MenuManifest.h");
        EXPECT(Has(manifest, "\"[FFXIV]DrawDiagnostics\",") && Has(manifest, "\"[FFXIV]DrawDiagnosticsFrames\","),
               "both keys are listed as ini-only");
        EXPECT(Has(manifest, "\"[FFXIV]DrawDiagnosticsDelaySeconds\",") && Has(manifest, "\"[FFXIV]DrawDiagnosticsKey\","),
               "the two keys that start a capture are listed as ini-only as well");
        // The frame count is clamped where it is used: a key of 0 or 999 cannot make a capture write no frames or a hundred.
        const std::string cpp = Code(repo, "ffxiv/diag/FfxivDrawDiag.cpp");
        EXPECT(Has(cpp, "static_cast<uint32_t>(v<1?1:(v>kMaxFrames?kMaxFrames:v))"), "the frame count is clamped to 1..16");
    }

    // ---- the two pure headers stay pure: no D3D, no file, no clock beyond the run id
    for (const char* pure : { "ffxiv/diag/DrawDiagFormat.h", "ffxiv/diag/ShaderBindings.h" })
    {
        const std::string text = Code(repo, pure);
        EXPECT(!Has(text, "#include<d3d11") && !Has(text, "ID3D11"), "%s is D3D free (the unit test builds it alone)", pure);
        EXPECT(!Has(text, "std::ofstream") && !Has(text, "std::filesystem"), "%s does no I/O (the writer is in the runtime)", pure);
    }

    if (g_failures)
    {
        std::printf("FAIL: %d expectation(s)\n", g_failures);
        return 1;
    }
    std::puts("PASS: draw diagnostic wiring pins (one load on the draw path, reflection at shader creation, boundary and Present marks, "
              "bounded re-armed captures per session, defaults off, ini only)");
    return 0;
}
