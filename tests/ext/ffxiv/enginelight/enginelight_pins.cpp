// Text pins of the engine light controls' constant-buffer family (ext/ffxiv/enginelight): where it hangs, what it must never do.
//
// The facts a break here would cost a player: the moon at x0.1 feeding the night weight (a reader inside the swap not mapped), the family arming the
// shipped knobs' streak (a decline charged to the wrong knob), a family ask that does not reach the seam's feature bit, a patch that runs before the
// seam modules have read the light colour, a second mechanism instead of the engine pass overrides' CbPatch, or a config key / menu entry that
// belongs to the other writer.
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

// Comments and all whitespace removed, so a pin is about the code and not its formatting.
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

size_t Count(const std::string& text, const std::string& needle)
{
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
        ++n;
    return n;
}

// The text of `body` between the first `begin` and the next `end` after it (empty when either is missing).
std::string Between(const std::string& body, const std::string& begin, const std::string& end)
{
    const size_t b = body.find(begin);
    if (b == std::string::npos)
        return {};
    const size_t e = body.find(end, b + begin.size());
    return e == std::string::npos ? std::string() : body.substr(b, e - b);
}
} // namespace

int main(int, char** argv)
{
    const std::string repo = argv[1];
    const std::string pass = Code(repo, "ffxiv/engine/EnginePass.cpp");
    const std::string family = Code(repo, "ffxiv/enginelight/CbFamily.cpp");
    const std::string familyHeader = Code(repo, "ffxiv/enginelight/CbFamily.h");
    const std::string rules = Code(repo, "ffxiv/enginelight/CbFamilyRules.h");
    const std::string swap = Code(repo, "ffxiv/enginelight/ActiveSwap.h");

    // ---- arming: the family's ask rides the seam's existing word, so every list reaches it and the feature bit follows the family, not the key
    {
        EXPECT(Has(pass, R"x(boolShadowsAmbientAsks(){return(ext::ffxiv::shadows::Hooks::ambientWants!=nullptr&&ext::ffxiv::shadows::Hooks::ambientWants())||ext::ffxiv::enginelight::cb::Asks();})x"),
               "the seam's ambient-table asker word includes the family's ask: it sits in all five arming lists and in the composite's intent, so the draw seam "
               "and the shader tracker run when only the family wants a draw, whatever [FFXIV] EnginePassOverrides says, and a lone family ask on the composite "
               "still goes through the composite's validation (intent.shadowsAmbient)");
        EXPECT(Count(pass, "ShadowsAmbientAsks()") == 8,
               "the word is read in the intent, the five arming lists, the composite-indexed line and its own definition: no list was added or dropped");
        EXPECT(Has(family, R"x(if(Asks()!=before)ext::ffxiv::engine::NotifyShadowArmingChanged();)x"),
               "a publish that changes the family's ask re-runs the seam's feature bit (the shadows module's arming-change path), so the seam is entered "
               "when the family is the only asker");
        EXPECT(Has(family, R"x(return!s.off.load(std::memory_order_relaxed)&&(s.pendingAsks.load(std::memory_order_relaxed)||s.activeAsks.load(std::memory_order_relaxed));)x"),
               "the ask covers the pending and the latched state, so a switch-off is applied at the frame boundary and never mid-frame");
    }

    // ---- the draw path: after every seam module has read the light colour, with the family's own accounting
    {
        const size_t lastModule = pass.find("if(flare::OnSeamDraw(context,draw,user))return;");
        const size_t preGate = pass.find("if(ext::ffxiv::enginelight::cb::WantsDraw(anchors)&&!IsAmbientAnchors(anchors)");
        const size_t ao = pass.find("ao::BoundReaderocclusion=ao::SwapReader(context,anchors);");
        const size_t apply = pass.find("ApplyDraw(context,state,anchors,gameDraw,gameUser);");
        EXPECT(lastModule != std::string::npos && preGate != std::string::npos && ao != std::string::npos && apply != std::string::npos,
               "the draw path's anchors are all present");
        EXPECT(lastModule < preGate && preGate < ao && ao < apply,
               "the family's draw (the swap) comes after the shafts, god rays, light bounce, night grade, atmosphere, depth of field and the flare card have "
               "seen the light draw: every seam module reads the ENGINE's colour, then the family swaps");
        const std::string dispatch = Between(pass, "DeclineApplyDraw(ID3D11DeviceContext*context,State&state,Anchorsanchors,DrawFndraw,void*user)", "constKnobIntentintent=");
        EXPECT(Has(dispatch, R"x(if(!IsAmbientAnchors(anchors)&&!IsWetnessAnchors(anchors)&&ext::ffxiv::enginelight::cb::WantsDraw(anchors))returnApplyEngineLightDraw(context,state,anchors,draw,user);)x"),
               "a light or water draw that is not the composite or a wetness draw takes the family's own path, before any knob's intent is built");
        const std::string armBody = Between(pass, "DeclineApplyEngineLightDraw(", "DeclineApplyDraw(");
        EXPECT(!armBody.empty() && !Has(armBody, "decline(") && !Has(armBody, "state.session") && !Has(armBody, "NoteDecline"),
               "the family's arm never charges a knob's streak: its failures are its own (CbFamilyRules.h Session), so 64 of them cannot switch the shipped "
               "knobs off");
        EXPECT(Has(armBody, "el::NoteFailure(") && Has(armBody, "el::NoteRun();"), "the family counts its own failures and its own runs");
        EXPECT(Has(armBody, "patched=state.patch.Scale(context,plan.game.Get(),plan.bytes,plan.entries,plan.count);"),
               "the family patches through the engine pass overrides' one CbPatch (no second primitive)");
        EXPECT(Has(armBody, "PatchGuardguard{state};") && Has(armBody, "ext::ffxiv::chara::ReplayScopeown;"),
               "its copies and its dispatch are marked as the module's own work, like every other patch");
        EXPECT(Has(armBody, "BoundConstantBufferswap(context,plan.slot,patched);") && Has(armBody, "ext::ffxiv::enginelight::ActiveSwapScopeactive(patched,swap.Original());"),
               "the swap is a scope guard (the game's buffer comes back on every path) and the readers inside it are told which buffer is the engine's");
        EXPECT(Count(armBody, "draw(user);") >= 5 && Has(armBody, "if(!drew)draw(user);"),
               "the game's draw is called on every path, once");
        EXPECT(!Has(Between(pass, "DeclineApplyDraw(", "constKnobIntentintent="), "NoteDecline"),
               "the dispatch to the family's path sits ahead of the knobs' accounting");
    }

    // ---- the composite: the family's entries are appended AFTER the shadows arm's, and the shadows lift yields
    {
        EXPECT(Has(pass, R"x(engineLightCount=ext::ffxiv::enginelight::cb::AmbientEntries(entries+knobCount+shadowsCount,kMaxScaleEntries-knobCount-shadowsCount);)x"),
               "the family's four composite entries follow the shadows arm's, so the shadows arm's first entry stays at entries[knobCount]");
        EXPECT(Has(pass, "constuint32_tcount=knobCount+shadowsCount+engineLightCount;"), "the one dispatch carries all three");
        EXPECT(Has(pass, "ext::ffxiv::enginelight::cb::RecordComposite(context,AmbientSlotOf(anchors),ambient.Get(),patched,facts.boundBytes);"),
               "the composite's patched table is recorded for the F9 file");
        EXPECT(Has(pass, "elseif(shadowsCount!=0)") && Has(pass, "ext::ffxiv::enginelight::cb::CompositeRunLine(facts.boundBytes)"),
               "the shadows run line is written only when the shadows arm ran, and the family has its own run line");
        const std::string shadows = Code(repo, "ffxiv/shadows/ShadowsDispatch.cpp");
        const std::string sink = Between(shadows, "uint32_tAmbientEntriesSink(", "if(!AmbientWantsFast(state))");
        EXPECT(Has(sink, "ext::ffxiv::enginelight::ownsAmbient!=nullptr&&ext::ffxiv::enginelight::ownsAmbient())return0;"),
               "the shadows module's night ambient lift yields while the engine light master owns the composite's ambient (the night's ambient is applied once)");
        EXPECT(Has(family, "ownsAmbient=&OwnsAmbientNow;") && Has(family, "boolOwnsAmbientNow(){returnOwnsAmbient();}"), "the family registers the hook at start-up");
        EXPECT(Has(rules, "constexprboolOwnsAmbient(constPublished&s)noexcept{returns.params.master;}"), "the owner is the published master");
    }

    // ---- the blocking reader rule: every reader inside the swap maps what it fetched
    {
        const std::string contact = Code(repo, "ffxiv/relight/ContactShadowsApply.cpp");
        const std::string route = Code(repo, "ffxiv/shadows/ShadowsDispatch.cpp");
        const std::string chara = Code(repo, "ffxiv/lighting/CharaAmbientApply.cpp");
        EXPECT(Count(contact, "ext::ffxiv::enginelight::OwnedGameBuffer(") == 1 &&
                   Has(contact, "context->PSGetConstantBuffers(0,kCbSlots,buffers);if(layout.lightParamSlot<kCbSlots)buffers[layout.lightParamSlot]=ext::ffxiv::enginelight::OwnedGameBuffer(buffers[layout.lightParamSlot]);"),
               "the contact shadows' inspection (which feeds the pass and the night curve's colour capture) maps the light buffer right after it fetched it");
        EXPECT(Count(route, "ext::ffxiv::enginelight::OwnedGameBuffer(") == 1 &&
                   Has(route, "context->PSGetConstantBuffers(0,kColourCbs,buffers);if(layout.lightParamSlot<kColourCbs)buffers[layout.lightParamSlot]=ext::ffxiv::enginelight::OwnedGameBuffer(buffers[layout.lightParamSlot]);"),
               "the shadow mapping route's inspection maps the light buffer right after it fetched it");
        EXPECT(Count(chara, "ext::ffxiv::enginelight::OwnedGameBuffer(") == 1 &&
                   Has(chara, "context->PSGetConstantBuffers(lightSlot,1,&fetched);fetched=ext::ffxiv::enginelight::OwnedGameBuffer(fetched);"),
               "the chara ambient's night feed maps the light buffer right after it fetched it");
        // Every other fetch of the light slot in the product runs before the swap (the seam modules) or outside it (the F9 dump): none is inside the draw.
        EXPECT(Has(swap, "inlineID3D11Buffer*OwnedGameBuffer(ID3D11Buffer*bound)noexcept") && Has(swap, "detail::g_original->AddRef();bound->Release();returndetail::g_original;"),
               "the mapping takes the reader's reference and returns one: the reader's own release stays balanced");
        EXPECT(Has(swap, "~ActiveSwapScope(){detail::g_patched=_previousPatched;detail::g_original=_previousOriginal;}"),
               "the mapping ends with the scope and restores the enclosing pair (nested, also on a throw)");
        // The F9 reads the buffers before the swap: its hook runs ahead of the engine's seam in the draw chain.
        const std::string hooks = Code(repo, "ffxiv/lighting/FfxivLightingCapture.h");
        const size_t diag = hooks.find("ext::ffxiv::diag::OnDraw(c,kind);");
        const size_t engine = hooks.find("ext::ffxiv::engine::BoundDraw(c,charaLight);");
        EXPECT(diag != std::string::npos && engine != std::string::npos && diag < engine,
               "the F9 draw diagnostic reads the engine's buffers in the hook body, before the engine seam's swap (draws.csv / cb.csv keep the engine's colour)");
        EXPECT(hooks.find("ext::ffxiv::chara::Around(c,") != std::string::npos, "(the chain is unchanged)");
    }

    // ---- the mechanism: one primitive, the size cache unchanged, no second source of numbers
    {
        const std::string patch = Code(repo, "ffxiv/engine/CbPatch.cpp");
        EXPECT(Has(patch, "if(_sizes.size()>=4)"), "the primitive's size cache still holds four sizes: 16,384 / 32 / 512 / 256 are exactly the four sizes the family and the shipped knobs patch");
        const std::string patchHeader = Code(repo, "ffxiv/engine/CbPatch.h");
        EXPECT(Has(patchHeader, "ID3D11Buffer*Original()constnoexcept{return_original;}"), "the guard exposes the game's buffer it will put back");
        EXPECT(!Has(family, "CreateComputeShader") && !Has(family, "Dispatch(") && !Has(family, "CbScale"), "the family builds no compute shader of its own");
        EXPECT(Has(familyHeader, "boolAsks();") && Has(familyHeader, "boolPlan(ID3D11DeviceContext*context,eng::Anchorsanchors,DrawPlan&plan);"), "the module's interface");
    }

    // ---- classification: stamps, not draw numbers; a replay and a constant-buffer range are left alone
    {
        const std::string plan = Between(family, "boolPlan(ID3D11DeviceContext*context,eng::Anchorsanchors,DrawPlan&plan)", "uint32_tAmbientEntries(");
        EXPECT(Has(plan, "if(ext::ffxiv::chara::t_replay)returnfalse;") && Has(plan, "BoundShaderKind(context)"), "a replay is never patched; the draw is classified by the shader's stamp");
        EXPECT(Has(family, "context1->PSGetConstantBuffers1(slot,1,&buffer,&first,&count);") && Has(family, "returnfirst==0&&(count==0||count*16u>=byteWidth);"),
               "a draw that binds a constant-buffer range is left alone (the plain restore would lose the range)");
        EXPECT(!Has(family, "ordinal") && !Has(family, "0x9DC59665") && !Has(family, "0xABA675C4") && !Has(family, "0xA86A2431") && !Has(rules, "0x9DC59665"),
               "no draw number and no shader crc: the draws are told apart by what they declare and write");
        EXPECT(Has(family, "s.frameLight.NoteMain(facts);"), "the main draw is noted whether or not this frame patches it (the water pass's copy is told apart by it)");
        EXPECT(Has(family, "shader->SetPrivateData(kKindTag,sizeof(packed),&packed)") || Has(family, "shader->SetPrivateData(kKindTag,sizeofpacked,&packed)"), "the stamp is written once, at shader creation");
    }

    // ---- publishing, F9
    {
        EXPECT(Has(family, "publishCbFamily=&PublishImpl;"), "the module sets the contract's publish hook at its own init");
        EXPECT(Has(rules, "Paramsparams=EngineValues();"), "a module that was never published starts at the engine's values (Params{} has master on and moon 0.1)");
        EXPECT(Has(family, "s.latch.Begin(token,pending);"), "the published state is latched at the frame boundary");
        const std::string dumpHooks = Code(repo, "ffxiv/enginelight/DumpHooks.h");
        EXPECT(Has(dumpHooks, R"x(inlineconstexprconstchar*kDumpFile="enginelight.csv";)x"), "the capture folder's file is enginelight.csv (the lab and the session notes read it by name)");
        const std::string diag = Code(repo, "ffxiv/diag/FfxivDrawDiag.cpp");
        EXPECT(Has(diag, "ext::ffxiv::enginelight::DumpHooks::text()") && Has(diag, "ext::ffxiv::enginelight::DumpHooks::reset();") &&
                   Has(diag, "ext::ffxiv::enginelight::kDumpFile"),
               "the F9 capture writes enginelight.csv and starts it over for the next capture, through hook pointers");
        EXPECT(Has(family, "if(!ext::ffxiv::diag::Collecting()||") && !Has(family, "diag::Capturing()") && Has(family, "ext::ffxiv::chara::ReplayScopeown;"),
               "the before / after copies are taken while the capture COLLECTS (not while the key merely waits for its press: the staging copies stall the GPU "
               "and would fill the row cap with frames outside the capture), and are not counted as the game's copies");
        EXPECT(Has(Code(repo, "ffxiv/diag/FfxivDrawDiag.cpp"), "boolCollecting(){returnS().phase.load(std::memory_order_relaxed)==kCapturing;}"),
               "the diagnostic answers whether its draw list is being collected");
    }

    // ---- the scene HDR path replays the tone chain's five stages only, never a light, composite or water draw
    {
        const std::string hdr = Code(repo, "hdr/scene/FfxivSceneHdr.h");
        EXPECT(Has(hdr, "inlineUINTStage(UINTcrc){switch(crc){case0x85e777ef:return1;case0xf8f57f0a:return2;case0x27ebc404:return3;case0x1f264d17:return4;case0xf6e81a1b:return5;default:return0;}}") &&
                   Has(hdr, "if(!stage||stage>5){s.aliases.erase(native.Get());return;}"),
               "FfxivSceneHdr::AfterDraw replays only draws whose shader is one of the five tone-chain stages (the tone map, the dither and gamma passes), so the light, "
               "composite and water draws the family patches are never replayed by it: the family needs no scene-HDR decline, and the patches are neither duplicated "
               "nor missed there. A sixth stage added upstream fails this pin, and the question is asked again");
    }

    // ---- what belongs to the other writer: no key, no menu, no ini in this family
    {
        for (const std::string* text : { &family, &familyHeader, &rules, &swap })
            EXPECT(!Has(*text, "ext::Cfg()") && !Has(*text, "ImGui") && !Has(*text, "value_or_default") && !Has(*text, ".ini"),
                   "the family reads no config, draws no menu and adds no ini key: the card, the keys and the schema are the other writer's");
        const std::string contract = Code(repo, "ffxiv/enginelight/EngineLightContract.h");
        EXPECT(Has(contract, "usingPublishCbFamilyFn=void(*)(constParams&params,floatweight);") &&
                   Has(contract, "constexprfloatEffective(constParams&p,floatfactor,floatweight)noexcept"),
               "the shared contract this family builds on is the one it was written against (publish signature, Effective)");
    }

    if (g_failures)
    {
        std::printf("FAIL: %d expectation(s)\n", g_failures);
        return 1;
    }
    std::puts("PASS: engine light controls constant-buffer family wiring pins");
    return 0;
}
