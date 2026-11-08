// Recognition and frame-step fixes: the wiring pins, read as text from the sources (no device), the house pins' shape.
//
// What a wrong wiring would break, and what is pinned here:
//   one count per rendered frame - each of depth of field, god rays and the light bounce opens its count through FrameStep at the composite and
//   never advances it by itself (the composite is drawn twice a frame), the frame closes at the top of all three tone-map entries
//   (chained, owner and seam) before the stage's one-answer gate, and the close goes through ClosesFrame, which refuses a replay;
//   the step line - every stage names the step and the ring ages once a state, after its read-back;
//   the fog height profile - god rays pick the buffer by the stamped slot (ProfileSlotFor), the stamp is written at shader creation inside the
//   creation hook's try before the layout parse, the read checks the declared 48 bytes before decoding, and no shape rule is left;
//   the pixel-to-NDC mapping - depth of field and god rays derive it from the window and the fog post's viewport row is only logged;
//   the decline lines - every new wait or refusal has a line of its own;
//   the slot_kind rule - the one new parser caller goes through the typed lookup and reads no binding type itself.
#include <cctype>
#include <cstdio>
#include <fstream>
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
            std::printf(__VA_ARGS__);                                                                                   \
            std::printf("\n");                                                                                         \
        }                                                                                                              \
    } while (0)

std::string ReadFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

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

// Comments and whitespace removed, so a pin is about the code and not about its formatting.
std::string Code(const std::string& repo, const char* relative)
{
    const std::string text = Comments(ReadFile(repo + "/OptiScaler/ext/" + relative));
    std::string out;
    for (char c : text)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            out += c;
    return out;
}

bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

size_t At(const std::string& text, const std::string& needle) { return text.find(needle); }

// The body of the function whose (whitespace-free) signature text is `signature`, braces included; empty when it is not there.
std::string Body(const std::string& code, const std::string& signature)
{
    const size_t at = code.find(signature);
    if (at == std::string::npos)
        return {};
    const size_t open = code.find('{', at + signature.size());
    if (open == std::string::npos)
        return {};
    int depth = 0;
    for (size_t i = open; i < code.size(); ++i)
    {
        if (code[i] == '{')
            ++depth;
        else if (code[i] == '}' && --depth == 0)
            return code.substr(open, i - open + 1);
    }
    return {};
}

size_t Count(const std::string& text, const std::string& needle)
{
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
        ++count;
    return count;
}

// `first` is in the text and `second` follows it.
bool Before(const std::string& text, const std::string& first, const std::string& second)
{
    const size_t firstAt = text.find(first);
    if (firstAt == std::string::npos)
        return false;
    return text.find(second, firstAt + first.size()) != std::string::npos;
}

// One tone-map entry: the close is there with the right chained flag and comes before the stage's one-answer gate.
void ClosesBeforeGate(const char* entry, const std::string& body, const char* flag, const char* gate)
{
    const std::string close = std::string("CloseFrameAtToneMap(state,facts,") + flag + ");";
    EXPECT(!body.empty(), "%s: the entry is there\n", entry);
    EXPECT(Count(body, "CloseFrameAtToneMap(") == 1 && Has(body, close), "%s: one close, with the chained flag %s\n", entry, flag);
    EXPECT(Before(body, close, gate), "%s: the close comes before the one-answer gate\n", entry);
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: recog_pins <repo>\n");
        return 2;
    }
    const std::string repo = argv[1];
    const std::string dofDraw = Code(repo, "ffxiv/engine/DofDraw.cpp");
    const std::string godraysDraw = Code(repo, "ffxiv/engine/GodraysDraw.cpp");
    const std::string godraysSun = Code(repo, "ffxiv/engine/GodraysSun.cpp");
    const std::string godraysSunHeader = Code(repo, "ffxiv/engine/GodraysSun.h");
    const std::string godraysRules = Code(repo, "ffxiv/engine/GodraysRules.h");
    const std::string giDraw = Code(repo, "ffxiv/engine/GiDraw.cpp");
    const std::string enginePass = Code(repo, "ffxiv/engine/EnginePass.cpp");
    const std::string shaftsDraw = Code(repo, "ffxiv/engine/ShaftsDraw.cpp");
    const std::string recog = Code(repo, "ffxiv/engine/RecogRules.h");
    const std::string dofCameraHeader = Code(repo, "ffxiv/engine/DofCamera.h");
    EXPECT(!dofDraw.empty() && !godraysDraw.empty() && !godraysSun.empty() && !giDraw.empty() && !enginePass.empty() && !recog.empty(),
           "every source this pin reads is there\n");

    // ---- one count per rendered frame: the composite -----------------------------------------------------------------------------------
    {
        EXPECT(Has(dofDraw, "if(state.step.OpenAtComposite(state.frames)){state.attempts=0;state.cameraTaken=false;}"),
               "depth of field: the count opens through FrameStep, and the capture budget and the camera's taken flag reset with the real advance\n");
        EXPECT(Has(godraysDraw, "if(state.step.OpenAtComposite(state.frames)){state.attempts=0;state.sunDrawTaken=false;}"),
               "god rays: the count opens through FrameStep, and the capture budget and the sun draw's taken flag reset with the real advance\n");
        EXPECT(Has(giDraw, "state.step.OpenAtComposite(state.frames);"), "the light bounce: the count opens through FrameStep\n");
        for (const std::string* draw : { &dofDraw, &godraysDraw, &giDraw })
        {
            EXPECT(Count(*draw, "OpenAtComposite(") == 1, "one open, at the composite capture\n");
            EXPECT(!Has(*draw, "AdvanceFrame("), "and the stage never advances its count by itself\n");
        }
        // The one place the count moves is the open, behind the closed flag.
        EXPECT(Count(recog, "AdvanceFrame(frames);") == 1, "FrameStep advances the count in exactly one place\n");
        EXPECT(Has(Body(recog, "constexprboolOpenAtComposite(uint64_t&frames)noexcept"), "if(!_closed)returnfalse;_closed=false;AdvanceFrame(frames);"),
               "and only while the previous frame is closed\n");
        EXPECT(Has(Body(recog, "constexprvoidCloseAtToneMap()noexcept"), "_closed=true;"), "and the tone map closes the frame\n");
        // The old shafts module keeps its own rule and does not use this header.
        EXPECT(!Has(shaftsDraw, "RecogRules") && Has(shaftsDraw, "AdvanceFrame(state.frames);"), "the light shafts module is untouched\n");
    }

    // ---- one count per rendered frame: the tone map closes it, before the gate ------------------------------------------------------------
    {
        ClosesBeforeGate("dof chained", Body(dofDraw, "ID3D11ShaderResourceView*ChainSceneInternal(ID3D11DeviceContext*context,"), "true", "state.dofFrame==state.frames");
        ClosesBeforeGate("dof owner", Body(dofDraw, "boolToneMapOwnerInternal(ID3D11DeviceContext*context,"), "true", "state.dofFrame==state.frames");
        ClosesBeforeGate("dof seam", Body(dofDraw, "boolSeamDraw(ID3D11DeviceContext*context,engine::Anchorsanchors,DrawFndraw,void*user)"), "false", "state.dofFrame==state.frames");
        ClosesBeforeGate("god rays chained", Body(godraysDraw, "ID3D11ShaderResourceView*ChainScene(ID3D11DeviceContext*context,"), "true", "state.marchFrame==state.frames");
        ClosesBeforeGate("god rays owner", Body(godraysDraw, "boolToneMapOwner(ID3D11DeviceContext*context,"), "true", "state.marchFrame==state.frames");
        ClosesBeforeGate("god rays seam", Body(godraysDraw, "boolSeamDraw(ID3D11DeviceContext*context,engine::Anchorsanchors,DrawFndraw,void*user)"), "false", "state.marchFrame==state.frames");
        ClosesBeforeGate("gi chained", Body(giDraw, "ID3D11ShaderResourceView*ChainSceneInternal(ID3D11DeviceContext*context,"), "true", "state.giFrame==state.frames");
        ClosesBeforeGate("gi owner", Body(giDraw, "boolToneMapOwnerInternal(ID3D11DeviceContext*context,"), "true", "state.giFrame==state.frames");
        ClosesBeforeGate("gi seam", Body(giDraw, "boolOnSeamDrawInternal(ID3D11DeviceContext*context,engine::Anchorsanchors,DrawFndraw,void*user)"), "false", "state.giFrame==state.frames");
        // The seam's close follows its own classification: another draw of the tone map's shader family does not close the frame.
        for (const auto& seam : { Body(dofDraw, "boolSeamDraw(ID3D11DeviceContext*context,engine::Anchorsanchors,DrawFndraw,void*user)"),
                                  Body(godraysDraw, "boolSeamDraw(ID3D11DeviceContext*context,engine::Anchorsanchors,DrawFndraw,void*user)"),
                                  Body(giDraw, "boolOnSeamDrawInternal(ID3D11DeviceContext*context,engine::Anchorsanchors,DrawFndraw,void*user)") })
            EXPECT(Before(seam, "ClassifyToneMap(facts)==shafts::Decline::NotToneMap)returnfalse;", "CloseFrameAtToneMap(state,facts,false);"),
                   "the seam entry closes only after its classification says tone map\n");
        // Each stage's close goes through the rule that refuses a replay and closes a declined draw.
        for (const std::string* draw : { &dofDraw, &godraysDraw, &giDraw })
        {
            const std::string close = Body(*draw, "voidCloseFrameAtToneMap(State&state,constshafts::ToneMapFacts&facts,boolchained)");
            EXPECT(close == "{if(shafts::ClosesFrame(facts,chained))state.step.CloseAtToneMap();}", "the close goes through ClosesFrame\n");
        }
        const std::string closes = Body(recog, "constexprboolClosesFrame(constToneMapFacts&facts,boolchained)noexcept");
        EXPECT(closes == "{if(facts.replay)returnfalse;returnchained||ClassifyToneMap(facts)!=Decline::NotToneMap;}", "ClosesFrame refuses a replay first\n");
    }

    // ---- the step line ---------------------------------------------------------------------------------------------------------------------
    {
        const std::string dofStage = Body(dofDraw, "ID3D11ShaderResourceView*StageAtToneMap(State&state,ID3D11DeviceContext*context,");
        const std::string marchStage = Body(godraysDraw, "ID3D11ShaderResourceView*MarchAtToneMap(State&state,ID3D11DeviceContext*context,");
        const std::string giStage = Body(giDraw, "ID3D11ShaderResourceView*StageAtToneMap(State&state,ID3D11DeviceContext*context,");
        EXPECT(Before(dofStage, "Readback(context,state,facts);", "NoteFrameStep(state);"), "depth of field writes its step line after the read-back\n");
        EXPECT(Before(marchStage, "Readback(context,state,marchState,fade,facts);", "NoteFrameStep(state);"), "god rays write their step line after the read-back\n");
        EXPECT(Before(giStage, "Readback(context,state,fade,cameraRefused);", "NoteFrameStep(state);"), "the light bounce writes its step line after the read-back\n");
        EXPECT(Has(dofDraw, "shafts::FormatFrameStep(\"Depthoffield\",advances,composites,rings,2)") &&
                   Has(godraysDraw, "shafts::FormatFrameStep(\"Godrays\",advances,composites,rings,3)") &&
                   Has(giDraw, "shafts::FormatFrameStep(\"Lightbounce\",advances,composites,rings,1)"),
               "each stage names its own rings\n");
        EXPECT(Has(dofDraw, "state.stepLine.Take(shafts::StepKey(") && Has(godraysDraw, "state.stepLine.Take(shafts::StepKey(") &&
                   Has(giDraw, "state.stepLine.Take(shafts::StepKey("),
               "and writes it once a state, on its own memory and not the shared log gate\n");
        // The ages come from the reads' own written frames.
        EXPECT(Has(dofDraw, "state.cameraAge=cameraRead.landed?shafts::AgeOf(state.frames,cameraRead.writtenFrame)") &&
                   Has(godraysDraw, "state.frameAge=frameRead.landed?shafts::AgeOf(state.frames,frameRead.writtenFrame)") &&
                   Has(giDraw, "state.cameraAge=read.landed?shafts::AgeOf(state.frames,read.writtenFrame)"),
               "the ages are the count at the tone map minus the count the copy was written at\n");
        EXPECT(Has(godraysDraw, "state.step.Reset();") && Has(dofDraw, "state.step.Reset();") && Has(giDraw, "state.step.Reset();"),
               "switching a stage off puts its frame step back\n");
    }

    // ---- god rays: the profile is chosen by the stamped slot -------------------------------------------------------------------------------
    {
        const std::string fogPost = Body(godraysDraw, "voidCaptureFogPost(ID3D11DeviceContext*context,engine::Anchorsanchors,State&state)");
        EXPECT(!fogPost.empty(), "the fog post capture is there\n");
        EXPECT(Before(fogPost, "constuint32_tstamped=BoundProfileSlot(context);", "constuint32_tprofileSlot=ProfileSlotFor(stamped,boundBytes);"),
               "the profile's slot is the bound shader's stamp, checked against what the draw binds\n");
        EXPECT(Before(fogPost, "ProfileSlotFor(stamped,boundBytes);", "state.fog.Copy(context,fogParam.Get(),profile,viewportParam,state.frames);"),
               "and the copy takes exactly the buffer at that slot\n");
        EXPECT(Has(fogPost, "ID3D11Buffer*profile=profileSlot!=kNoProfileSlot?bound[profileSlot].Get():nullptr;"), "no slot, no profile\n");
        const std::string readStamp = Body(godraysDraw, "uint32_tBoundProfileSlot(ID3D11DeviceContext*context)");
        EXPECT(Has(readStamp, "PSGetShader(") && Has(readStamp, "UnpackProfileSlot(ReadStampRaw(shader.Get()))"), "the stamp is read from the bound pixel shader\n");
        EXPECT(Has(godraysDraw, "shader->SetPrivateData(kProfileStampTag,sizeofpacked,&packed)") &&
                   Has(godraysDraw, "shader->GetPrivateData(kProfileStampTag,&bytes,&packed)"),
               "the stamp is one 64-bit word on the shader object under the module's own tag\n");
        EXPECT(Has(godraysDraw, "BindPointOf(parsed,count,ext::ffxiv::diag::BindKind::ConstantBuffer,\"cExpHeightFogParam\")") &&
                   Has(godraysDraw, "BindPointOf(parsed,count,ext::ffxiv::diag::BindKind::ConstantBuffer,\"cFogParam\")"),
               "the slot comes from the bytecode's own reflection, through the typed lookup\n");
        EXPECT(!Has(godraysDraw, ".type") && !Has(godraysDraw, "->type") && !Has(godraysDraw, "BindingCovers"),
               "and this parser caller reads no binding's type itself (the slot_kind rule)\n");
        const std::string noteShader = Body(godraysDraw, "voidNoteShader(ID3D11DeviceChild*shader,constvoid*code,size_tsize)");
        EXPECT(Has(noteShader, "StampShader(shader,PackProfileSlot(ProfileSlotOfShader(code,size)));") && Has(noteShader, "catch(...)"),
               "the stamp is written inside a try\n");
        EXPECT(Has(noteShader, "||!MentionsProfile(code,size))return;") &&
                   Before(noteShader, "!MentionsProfile(code,size)", "ProfileSlotOfShader(code,size)"),
               "and only for a shader whose bytes name the profile, before any parse\n");
        EXPECT(Has(Body(godraysDraw, "boolMentionsProfile(constvoid*code,size_tsize)"), "std::memcmp(bytes+i,kName,kLength)==0"),
               "by a byte scan\n");
        // The shared creation path: one line, inside the try, after the light bounce's stamp and before the layout parse.
        const std::string created = Body(enginePass, "voidOnShaderCreated(ID3D11PixelShader*shader,constvoid*code,size_tsize,uint32_tcrc)");
        EXPECT(Count(created, "ext::ffxiv::godrays::NoteShader(shader,code,size);") == 1, "the creation hook calls the stamp once\n");
        EXPECT(Before(created, "try{", "ext::ffxiv::godrays::NoteShader(shader,code,size);") &&
                   Before(created, "ext::ffxiv::engine::gi::NoteShader(shader,code,size);", "ext::ffxiv::godrays::NoteShader(shader,code,size);") &&
                   Before(created, "ext::ffxiv::godrays::NoteShader(shader,code,size);", "constPassLayoutlayout=PassLayoutOfShader(code,size);"),
               "inside the try, after the light bounce's stamp, before the layout parse\n");
        {
            const size_t from = At(created, "ext::ffxiv::godrays::NoteShader(shader,code,size);");
            const size_t to = At(created, "constPassLayoutlayout=PassLayoutOfShader(code,size);");
            EXPECT(from != std::string::npos && to != std::string::npos && from < to && !Has(created.substr(from, to - from), "return"),
                   "and with no early return between it and the layout parse\n");
        }
        // The reader: the declared 48 bytes are checked before the decode, and the shape rule is gone.
        const std::string read = Body(godraysSun, "FogReadFogReader::Read(ID3D11DeviceContext*context,uint64_tframe,boolblock)");
        EXPECT(Before(read, "if(ProfileRowUsable(profileBytes))", "DecodeFog(fog,profileBytes)"), "the copy is checked before it is decoded\n");
        EXPECT(!Has(godraysSun, "RigidTransform4x4") && !Has(godraysSun, "kHeightCandidates") && !Has(godraysSunHeader, "kHeightCandidates") &&
                   !Has(godraysDraw, "kHeightCandidates") && !Has(godraysDraw, "RigidTransform4x4"),
               "no shape rule is left\n");
        EXPECT(Has(godraysSunHeader, "boolCopy(ID3D11DeviceContext*context,ID3D11Buffer*fogParam,ID3D11Buffer*profile,ID3D11Buffer*viewportParam,uint64_tframe);"),
               "the reader's copy takes the one profile buffer\n");
        // The cascade ring: a slot reused by a later frame starts that frame's set over before anything is keyed on its old pointers.
        const std::string cascadeCopy = Body(godraysSun, "boolCascadeReader::Copy(ID3D11DeviceContext*context,ID3D11Buffer*shadow,uint64_tframe)");
        EXPECT(Before(cascadeCopy, "if(slot.written&&slot.frame!=frame){slot.count=0;slot.written=false;}",
                      "for(uint32_ti=0;i<slot.count;++i)if(slot.source[i]==shadow)returntrue;"),
               "the cascade ring resets a reused slot before it keys on the slot's old pointers\n");
        // LIVE1 item 4: the cascade read walks the ring to an older copy until one LANDED (the shared WalkRing), it does not stop at the first candidate
        // that merely existed. The light and fog readers walk the same way (a failed map is a `continue`).
        const std::string cascadeRead = Body(godraysSun, "CascadeReadCascadeReader::Read(ID3D11DeviceContext*context,uint64_tframe,boolblock)");
        EXPECT(Has(cascadeRead, "shafts::WalkRing(frame,") && Has(cascadeRead, "if(result.count==0)returnfalse;") && !Has(cascadeRead, "break;"),
               "the cascade read goes on to an older copy while none of the newest copy's maps landed, and has no unconditional break\n");
        EXPECT(Has(cascadeRead, "returntrue;") && Has(cascadeRead, "_lastAcceptedFrame=slot.frame;"),
               "a copy that landed is the read, and it is the newest the ring may take from then on\n");
        // The profile lines.
        EXPECT(Has(fogPost, "LOG_INFO(\"{}\",kNoProfileStamp);") && Has(fogPost, "FormatProfileSlotEmpty(stamped,boundBytes[stamped])"),
               "a draw without a stamp, and a stamped slot with nothing there, each say so once\n");
        EXPECT(Has(godraysDraw, "FormatProfileRefused(BoundProfileSlotMemo(state),fogRead.profileFields)"), "a copy that fails the profile check says so once\n");
        EXPECT(Has(godraysRules, "constexpruint64_tPackProfileSlot(uint32_tslot)noexcept{returnslot<ext::ffxiv::engine::kConstantBufferSlots?uint64_t(slot)+1u:0u;}"),
               "the stamp word is the slot plus one\n");
    }

    // ---- the pixel-to-NDC mapping and the viewport row line --------------------------------------------------------------------------------
    {
        EXPECT(Has(Body(dofDraw, "ID3D11ShaderResourceView*RunStage(State&state,ID3D11DeviceContext*context,"), "shafts::PixelToNdcRow(facts.window)"),
               "depth of field maps pixels to NDC from the render window\n");
        EXPECT(Has(godraysDraw, "constshafts::NdcRowderived=shafts::PixelToNdcRow(facts.window);") && Has(godraysDraw, "marchState.ndcScale[0]=derived.scale[0];") &&
                   Has(godraysDraw, "marchState.ndcOffset[1]=derived.offset[1];"),
               "so does the march\n");
        EXPECT(!Has(dofDraw, "haveRow") && !Has(dofDraw, "hold.ndcScale") && !Has(godraysDraw, "hold.ndcScale") && !Has(godraysDraw, "hold.ndcOffset"),
               "and neither holds a row read from the fog post's viewport buffer\n");
        EXPECT(Has(dofDraw, "shafts::FormatViewportRow(\"Depthoffield\",second,derived)") && Has(godraysDraw, "shafts::FormatViewportRow(\"Godrays\",second,derived)"),
               "the buffer's second row is logged once next to the derived mapping\n");
        EXPECT(Has(dofDraw, "if(rowRead.landed&&!state.loggedViewportRow)") && Has(godraysDraw, "if(fogRead.viewportLanded&&derived.ok&&!state.loggedViewportRow)"),
               "once a session\n");
        EXPECT(Has(dofCameraHeader, "structRowRead"), "the viewport row's reader stays (it feeds the line)\n");
    }

    // ---- the lines for the waits ---------------------------------------------------------------------------------------------------------
    {
        const std::string missing = Body(godraysDraw, "voidNoteMissingInput(State&state)");
        EXPECT(Has(missing, "FirstMissingInput(state.seenLight,state.seenCascades,state.seenFog,state.seenFogUsable)") &&
                   Has(missing, "LOG_INFO(\"{}\",MissingInputLine(missing));") && Has(missing, "LOG_INFO(\"{}\",kReadbacksNotTogether);"),
               "a march that has never accepted a frame names the read-back it lacks, or that they never landed together\n");
        EXPECT(Has(godraysDraw, "if(!state.hold.have)NoteMissingInput(state);"), "and the wait is noted where the march gives up for want of a state\n");
        EXPECT(Has(godraysDraw, "LOG_INFO(\"{}\",kCascadesUnusable);"), "cascade copies that landed without a usable matrix say so\n");
        EXPECT(Has(godraysDraw, "state.loggedStall=false;"), "the stall line is allowed again after a re-enable\n");
        EXPECT(Has(giDraw, "state.frames-state.stallBase>kMaxReadbackAge/2"), "the light bounce's never-landed line counts from the ring's reset\n");
    }

    if (g_failures)
    {
        std::printf("recog_pins: %d pin(s) failed\n", g_failures);
        return 1;
    }
    std::printf("recog_pins: all pins hold\n");
    return 0;
}
