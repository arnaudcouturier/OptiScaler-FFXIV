// Text pins of the reflections' frame-exact A/B: where it hangs in the draw path, what it must never do, and that it is free off the capture frame.
//
// The A/B is a diagnostic of the one-frame dump. It ships in the build the user installs, so what it may touch is pinned in the text: the draw
// path calls it right after the swap guard and right after the draw path, the extra draw runs inside the explicit-state guard with a minimal spec,
// the tessellation stages are never neutralised, depth and stencil writes are off for the extra draw, there is no new key, and every entry point
// costs one relaxed load of the open mask until a capture is open.
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

// Comments removed and every whitespace character dropped, so a pin is about the code and not about its formatting.
std::string Squeezed(const std::string& text)
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
        if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n')
            out += text[i];
    }
    return out;
}

std::string Code(const std::string& repo, const char* relative) { return Squeezed(ReadFile(repo + "/OptiScaler/ext/" + relative)); }

bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

size_t Count(const std::string& text, const std::string& needle)
{
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
        ++n;
    return n;
}

size_t At(const std::string& text, const std::string& needle) { return text.find(needle); }
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    const std::string pass = Code(repo, "ffxiv/engine/EnginePass.cpp");
    const std::string ab = Code(repo, "ffxiv/engine/SsrAb.cpp");
    const std::string abHeader = Code(repo, "ffxiv/engine/SsrAb.h");
    const std::string apply = Code(repo, "ffxiv/engine/SsrApply.cpp");
    const std::string stage = Code(repo, "ffxiv/diag/StageDump.h");
    const std::string diagCpp = Code(repo, "ffxiv/diag/FfxivDrawDiag.cpp");

    // ---- the draw path: after the swap guard, around ApplyDraw, only for a served reader ---------------------------------------------------------------------
    {
        const size_t swap = At(pass, "ssr::BoundReaderreflection=ssr::SwapReader(context,anchors);");
        const size_t begin = At(pass, "ssr::ab::Begin(reflectionAb,context,reflection,gameDraw,gameUser);");
        const size_t apply = At(pass, "ApplyDraw(context,state,anchors,gameDraw,gameUser);");
        const size_t end = At(pass, "ssr::ab::End(reflectionAb);");
        EXPECT(swap != std::string::npos && begin != std::string::npos && apply != std::string::npos && end != std::string::npos,
               "the draw path calls the A/B's two entries around ApplyDraw");
        EXPECT(swap < begin && begin < apply && apply < end, "Begin follows the swap guard, End follows the draw path");
        EXPECT(Has(pass, "if(reflection.Bound())ssr::ab::Begin(") && Count(pass, "ssr::ab::Begin(") == 1 && Count(pass, "ssr::ab::End(") == 1,
               "only a reader the reflections served is offered to the A/B, from one place");
        EXPECT(Has(pass, "DrawFngameDraw=native.trace?forwardNative:draw;void*gameUser=native.trace?&native:user;"),
               "the A/B wraps the draw the draw path would have run, whichever route that is");
        EXPECT(Count(pass, "PSGetShader") == 1, "the draw path makes no state query of its own: the A/B reads its shader in its own file");
        EXPECT(Has(pass, "#include\"ext/ffxiv/engine/SsrAb.h\""), "the draw path includes the A/B's header");
    }

    // ---- the extra draw: the explicit-state guard, a minimal spec, tessellation untouched, depth and stencil writes off ------------------------------------
    {
        EXPECT(Has(ab, "es::Guardguard(c,c1.Get(),fl111,spec);"), "the extra draw runs inside the explicit-state guard");
        EXPECT(!Has(ab, "Neutralise("), "the tessellation stages of a water draw stay bound: the guard is never asked to neutralise them");
        for (const char* off : { "spec.inputAssembler=false;", "spec.vertexShader=false;", "spec.pixelShader=false;", "spec.rasterizerState=false;",
                                 "spec.viewports=false;", "spec.blend=false;", "spec.hullDomainGeometry=false;", "spec.streamOut=false;",
                                 "spec.predication=false;", "spec.psConstantBuffers=0;", "spec.psSrvCount=0;" })
            EXPECT(Has(ab, off), "the guard's spec touches only the output merger and the depth-stencil state (%s)", off);
        EXPECT(Has(ab, "spec.depthStencil=true;") && Has(ab, "spec.outputMerger=true;"), "... which are exactly what the extra draw changes");
        EXPECT(Has(ab, "ShadowDepthStencilDesc(gameDssDesc)") && Has(ab, "out.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;out.StencilWriteMask=0;"),
               "the depth-stencil state of the extra draw is the game's with depth writes off and the stencil write mask zero");
        const size_t guard = At(ab, "es::Guardguard(c,c1.Get(),fl111,spec);");
        const size_t swapIn = At(ab, "detail::SwapSlot(c,slot,engineView)");
        const size_t putBack = At(ab, "BoundReaderputOursBack(c,slot,ours);");
        const size_t copy = At(ab, "c->CopyResource(privateTexture.Get(),texture.Get());");
        const size_t setTargets = At(ab, "c->OMSetRenderTargets(1,targets,gameDsv.Get());");
        const size_t setDepth = At(ab, "c->OMSetDepthStencilState(shadowDss.Get(),stencilRef);");
        const size_t queryBegin = At(ab, "c->Begin(extraQuery.Get());");
        const size_t draw = At(ab, "ExtraDrawScopescope;call.draw(call.user);");
        const size_t queryEnd = At(ab, "c->End(extraQuery.Get());");
        EXPECT(guard < swapIn && swapIn < putBack && putBack < copy && copy < setTargets && setTargets < setDepth && setDepth < queryBegin &&
                   queryBegin < draw && draw < queryEnd,
               "the order: guard, the engine's map in, its restore guard, the copy of the destination immediately before, the private target, the write-off "
               "state, the pixel count around the draw");
        EXPECT(Has(ab, "ExtraDrawScopescope;call.draw(call.user);"), "the extra draw is another module's replay and this module's own work");
        EXPECT(Has(ab, "ext::ffxiv::chara::ReplayScopereplay;") && Has(ab, "engine::SetOwnWork(true);") && Has(ab, "engine::SetOwnWork(false);"),
               "... both markers are raised for the draw and dropped after it");
        EXPECT(Has(ab, "voidThunk(void*opaque){Call&call=*static_cast<Call*>(opaque);Shadow(call);ID3D11Query*realQuery=call.realQuery.Get();"
                       "if(realQuery!=nullptr)call.context->Begin(realQuery);call.draw(call.user);if(realQuery!=nullptr)call.context->End(realQuery);}"),
               "the thunk is the extra draw and then the real draw, once, with the real draw's pixels counted when the draw was kept");
        EXPECT(Has(ab, "voidShadow(Call&call)noexcept{try{") && Has(ab, "catch(...){}}"), "the extra draw never throws: the real draw that follows runs whatever happened");
        EXPECT(Has(ab, "privateDesc.BindFlags=D3D11_BIND_RENDER_TARGET;"), "the private target is a render target and nothing else");
        EXPECT(Has(ab, "CreateRenderTargetView(privateTexture.Get(),&rtvDesc,&privateRtv)"), "its view is made from the game's own view description");
    }

    // ---- the gate: the open mask first, the served reader, the diagnostic off, then the capture's one draw -----------------------------------------------------
    {
        const size_t open = At(ab, "if(!ext::ffxiv::diag::StageOpen(ext::ffxiv::diag::StageId::Ssr))returnfalse;");
        const size_t served = At(ab, "if(context==nullptr||!reader.Bound()||draw==nullptr)returnfalse;");
        const size_t take = At(ab, "if(!ext::ffxiv::diag::AbTake(&framePosition,&tryIndex))returnfalse;");
        const size_t diagnostic = At(ab, "if(DiagnosticOn()){Refuse(call,Decline::SsrDiagnostic);returnfalse;}");
        const size_t decide = At(ab, "constDeclineverdict=Decide(facts);");
        EXPECT(open != std::string::npos && served != std::string::npos && take != std::string::npos && diagnostic != std::string::npos &&
                   decide != std::string::npos,
               "Begin's gate reads in order");
        EXPECT(open < served && served < take && take < diagnostic && diagnostic < decide,
               "the open mask first (one relaxed load), then the served reader, then the capture's one draw, then the diagnostic, then the draw's own facts");
        EXPECT(Has(apply, "boolDiagnosticOn(){returnS().diagnosticOn.load(std::memory_order_relaxed);}"), "the diagnostic's state is one relaxed load");
        EXPECT(Has(stage, "inlineboolAbTake(uint32_t*framePosition,uint32_t*tryIndex){if(!StageOpen(StageId::Ssr)||detail::g_abTake==nullptr)returnfalse;"),
               "the offer entry tests the mask first");
        EXPECT(Has(stage, "inlinevoidAbAccept(){if(!StageOpen(StageId::Ssr)||detail::g_abAccept==nullptr)return;"), "the accept entry too");
        EXPECT(Has(stage, "inlinevoidAbImage(constchar*name,ID3D11DeviceContext*context,ID3D11Texture2D*texture,constStageWindow&window,uint32_tpsCrc){if(!StageOpen(StageId::Ssr)||"),
               "the picture entry tests the mask first");
        EXPECT(Has(stage, "inlinevoidAbNote(constchar*text){if(!StageOpen(StageId::Ssr)||"), "the note entry tests the mask first");
    }

    // ---- which draw is kept: the pixels the extra draw wrote decide, the capture is told only of a kept draw, the pictures follow the decision ----------------------
    {
        const size_t read = At(ab, "constboolknown=extraQuery&&ReadSamples(c,extraQuery.Get(),extraSamples);");
        const size_t judge = At(ab, "constCoveragecoverage=Judge(known,extraSamples,windowPixels);");
        const size_t line = At(ab, "AbNote(FormatTry(call.tryIndex,call.framePosition,call.psCrc,coverage,extraSamples,windowPixels).c_str());");
        const size_t keep = At(ab, "if(!Keep(coverage)){");
        const size_t accept = At(ab, "ext::ffxiv::diag::AbAccept();");
        const size_t before = At(ab, "AbImage(\"ssr-ab-before\",c,texture.Get(),call.window,call.psCrc);");
        const size_t engine = At(ab, "AbImage(\"ssr-ab-engine\",c,privateTexture.Get(),call.window,call.psCrc);");
        EXPECT(read != std::string::npos && judge != std::string::npos && line != std::string::npos && keep != std::string::npos &&
                   accept != std::string::npos && before != std::string::npos && engine != std::string::npos,
               "the keep decision reads in order");
        EXPECT(read < judge && judge < line && line < keep && keep < accept && accept < before && before < engine,
               "the extra draw's pixels are read, judged and written as a try line (kept or skipped) before anything is kept; the capture is told and the pictures "
               "queued only for a kept draw, the target before the real draw first");
        EXPECT(Count(ab, "ext::ffxiv::diag::AbAccept();") == 1, "one place tells the capture a draw was kept");
        EXPECT(Has(ab, "ReadSamples(call.context,call.realQuery.Get(),realSamples)") && Has(ab, "call.realQuery=MakeQuery(device.Get());"),
               "the kept draw's real draw is counted too, and both counts reach the ran line");
        EXPECT(Has(ab, "if(FAILED(hr)||GetTickCount64()-start>500)returnfalse;"), "the wait for a query is bounded");
        EXPECT(Has(ab, "desc.Query=D3D11_QUERY_OCCLUSION;"), "the pixels are counted by an occlusion query");
        const std::string rules = Code(repo, "ffxiv/engine/SsrAbRules.h");
        EXPECT(Has(rules, "constexprboolKeep(Coveragecoverage)noexcept{returncoverage==Coverage::Enough||coverage==Coverage::Unknown;}"),
               "an empty or small draw is never kept; an unknown coverage is");
        EXPECT(Has(rules, "constexprCoverageJudge(boolknown,uint64_tsamples,uint64_twindowPixels)noexcept{if(!known)returnCoverage::Unknown;if(samples==0)returnCoverage::Empty;"),
               "a draw that wrote no pixel is empty, an unknown count is not zero");
    }

    // ---- the raw term: the capture frame's fourth named picture, taken after the pass ran and only while the capture is open -----------------------------------------
    {
        const size_t ran = At(apply, "ext::ffxiv::diag::StageRan(ext::ffxiv::diag::StageId::Ssr,passContext,planar.view,ours,ext::ffxiv::diag::StageWindow{});");
        const size_t term = At(apply, "ext::ffxiv::diag::AbImage(\"ssr-term\",passContext,state.pass.TermTexture(),ext::ffxiv::diag::StageWindow{},0u);");
        EXPECT(ran != std::string::npos && term != std::string::npos && ran < term, "the raw term is queued right after the stage's own pictures, on the context the pass ran on");
        EXPECT(Has(apply, "if(ext::ffxiv::diag::StageOpen(ext::ffxiv::diag::StageId::Ssr))ext::ffxiv::diag::AbImage(\"ssr-term\""),
               "and only while the capture frame is open (one relaxed load otherwise)");
        EXPECT(Count(apply, "\"ssr-term\"") == 1, "one place names it");
    }

    // ---- the runtime: tries, a kept draw ends them, four places of their own, stage-image rank, the hooks installed by the runtime only -----------------------------
    {
        EXPECT(Has(diagCpp, "if(!s.dumpActive||s.dumpWritten||s.abAccepted||s.abTries>=kAbTries)returnfalse;++s.abTries;"), "an offer is a try, while no draw was kept");
        EXPECT(Has(diagCpp, "voidAbAcceptHook(){S().abAccepted=true;}"), "a kept draw ends the offers");
        EXPECT(Has(diagCpp, "s.stageWantedBytes=0;s.abAccepted=false;s.abTries=0;detail::g_stageOpen.store(0,std::memory_order_relaxed);"), "a capture starts the offers over");
        EXPECT(Has(diagCpp, "item.rank=uint8_t(DumpRank::Stage);item.stage=true;item.ab=true;item.copied=true;"),
               "the A/B's pictures are stage-image rank, with a count of their own");
        EXPECT(Has(diagCpp, "held+=(item.ab&&item.note.empty())?1:0;if(held>=kAbImages)"), "four places of their own");
        EXPECT(Has(diagCpp, "constexprsize_tkAbImages=4;"), "the A/B's three pictures and the reflections' raw term");
        EXPECT(Has(Code(repo, "ffxiv/engine/SsrAbRules.h"), "inlineconstexpruint32_tkMaxTries=128;"), "a capture frame offers every reader draw it has");
        EXPECT(Has(diagCpp, "count+=(item.stage&&!item.ab&&item.note.empty())?1:0;"), "... which are not the stages' places");
        EXPECT(Has(diagCpp, "detail::g_abTake=&AbTakeHook;detail::g_abAccept=&AbAcceptHook;detail::g_abImage=&AbImageHook;detail::g_abNote=&AbNoteHook;"),
               "the hooks are installed by the runtime only");
        const size_t section = At(diagCpp, "voidRefreshStageOpen(State&s)");
        const size_t pending = At(diagCpp, "voidDumpCopyPending(");
        const size_t hook = At(diagCpp, "voidAbImageHook(");
        EXPECT(section < pending && pending < hook, "the A/B's picture hook sits after the stage section (the stage copy pins read that section alone)");
        EXPECT(Has(diagCpp, "item.factsExtra=AbImageNote(name,s.drawsInFrame,psCrc,"), "each picture is followed by its comment line");
    }

    // ---- no key, no new file in the install, no attribution --------------------------------------------------------------------------------------------------
    {
        const std::string schema = Code(repo, "ffxiv/FfxivConfig.h");
        EXPECT(!Has(schema, "SsrAb") && !Has(schema, "ssr-ab") && !Has(schema, "ReflectionAb"), "FfxivConfig.h has no A/B key: the one-frame dump is the switch");
        const std::string ini = ReadFile(repo + "/OptiScaler/ext/ffxiv/ini/FFXIV.ini");
        EXPECT(ini.find("SsrAb") == std::string::npos && ini.find("ssr-ab") == std::string::npos, "FFXIV.ini has no A/B key");
        EXPECT(Has(abHeader, "namespaceext::ffxiv::engine::ssr::ab"), "the A/B lives in the reflections' namespace");
    }

    if (g_failures)
    {
        std::printf("ssr_ab_pins: %d pin(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ssr_ab_pins: all pins passed\n");
    return 0;
}
