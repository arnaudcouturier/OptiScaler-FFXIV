// Text pins of the contact shadow module: where it hangs, what the key off costs, and what it must never do.
//
// The feature is off by default and must stay free while it is off: the seam in the draw hook is one relaxed load, the module is only called
// from that seam and from the shader creation hook, the keys are published at the config load, and the device side never writes a game
// resource (it reads the game's constant buffers through a staging copy and puts every binding it took back).
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

std::string Raw(const std::string& repo, const char* relative)
{
    std::ifstream file(repo + "/OptiScaler/ext/" + relative, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// Comments and whitespace removed, so a pin is about the code and not about its formatting.
std::string Code(const std::string& repo, const char* relative)
{
    const std::string text = Raw(repo, relative);
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

bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
size_t Find(const std::string& text, const std::string& needle, size_t from = 0) { return text.find(needle, from); }

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

    // ---- the seam: one relaxed load on the draw path, and the pass replaces the draw call, not the rest of the hook
    {
        const std::string hooks = Code(repo, "ffxiv/lighting/FfxivLightingCapture.h");
        EXPECT(Count(hooks, "ext::Active(ext::F::ContactShadows)") == 1, "the draw hook asks the key exactly once");
        EXPECT(Has(hooks, R"x(if(!inside&&c==context&&ext::Active(ext::F::ContactShadows)))x"),
               "the key test is the first thing the seam does, and only the game's own context is touched (the classification reads that "
               "context's tracked shader and the pass enters its private state on it)");
        EXPECT(Has(hooks, R"x(ext::ffxiv::relight::BoundDraw(c,original))x"), "the seam wraps the game's own draw");
        EXPECT(Has(hooks, R"x(AroundBody(c,kind,wrapped);)x") && Has(hooks, R"x(AroundBody(c,kind,original);)x"),
               "the rest of the hook runs either way: the pass only replaces the draw call");
        EXPECT(Count(hooks, "ext::ffxiv::relight::") == 4,
               "the module is called from four places: the draw-once rule, the draw seam, the shader creation and the pixel-shader seam");
        EXPECT(Has(hooks, R"x(ext::ffxiv::relight::OnShaderCreated(*out,code,size,crc);)x"), "the reflection is parsed where the bytecode is");
        EXPECT(Has(hooks, R"x(#include"ext/ffxiv/relight/Inject.h")x"), "the seam's header is included");
        // The pixel-shader seam: the draw path must not ask the context for the shader (0.65 ms a frame at 9250 draws), so the bound
        // shader is tracked on the one call that changes it.
        EXPECT(Count(hooks, "LIGHT_ATTACH(setPs,OnSetPs)") == 1 && Count(hooks, "LIGHT_DETACH(setPs,OnSetPs)") == 1,
               "the PSSetShader seam is attached with every other hook and detached with them");
        EXPECT(Has(hooks, "setPs=reinterpret_cast<SetPs>(cv[9]);"),
               "the seam is the context vtable's PSSetShader, attached in the existing hook transaction");
        EXPECT(Has(hooks, R"x(if(c==context)ext::ffxiv::relight::OnPixelShaderSet(ps,count);)x"),
               "only the game's own context is tracked");
        EXPECT(Has(hooks, R"x(ext::ffxiv::relight::OnShaderCreated(*out,code,size,crc);)x"), "the reflection is parsed where the bytecode is");
        // The wrapper hands the hook body's draw to the draw-once rule with both paths: the pass with the draw, and the draw alone for a
        // replay. Dropping the second argument (or running both) would double the frame's draws or drop the replayed one; the rule itself is
        // tested in contact_rules.
        EXPECT(Has(hooks, R"x(autowrapped=[&]{ext::ffxiv::relight::RunDrawOnce(first,ext::hdr::FfxivSceneHdr::Requested(),[&]{ext::ffxiv::relight::BoundDraw(c,original);},original);};)x"),
               "the seam hands the hook body's draw to the draw-once rule once, with both paths (running both, or neither, would double "
               "the frame's draws or drop the replayed one)");
    }

    // ---- the feature bit is its own, and the inline test is the relaxed load of ExtFeatures.h
    {
        const std::string features = Code(repo, "ExtFeatures.h");
        EXPECT(Has(features, R"x(ContactShadows=1ull<<14,)x"), "the feature bit");
        const std::string header = Code(repo, "ffxiv/relight/Inject.h");
        EXPECT(Has(header, R"x(inlinevoidBoundDraw(ID3D11DeviceContext*context,F&draw))x"), "the seam wrapper is header only");
        EXPECT(Has(header, R"x(std::atomic<uint32_t>boundLayoutId{0};)x"),
               "the layout the seam tracked is the one value the draw path reads");
        EXPECT(Has(header, R"x(voidOnPixelShaderSet(ID3D11PixelShader*shader,uint32_tinstances);)x"), "the seam's entry point");
        EXPECT(!Has(header, "std::function") && !Has(header, "std::mutex") && !Has(header, "d3d11.h"),
               "the header the draw hook includes carries no allocation, no lock and no D3D include of its own");
    }

    // ---- the module: armed through the config's hook pointer, switched off after the refusal limit, and never writing a game resource
    {
        const std::string cpp = Code(repo, "ffxiv/relight/Inject.cpp");
        EXPECT(Has(cpp, R"x(ext::FfxivConfig::contactShadowsOnLoaded=&ext::ffxiv::relight::OnConfigLoaded;)x"),
               "the key reaches the module through the config's hook pointer");
        EXPECT(Has(cpp, R"x(returnext::Active(ext::F::ContactShadows)&&state.session.Armed();)x"), "Armed() is the key and the session");
        EXPECT(Has(cpp, R"x(if(state.session.off)ext::SetActive(ext::F::ContactShadows,false);)x"),
               "the switch-off puts the draw path back to one relaxed load");
        EXPECT(Has(cpp, R"x(if(!MentionsMaskName(code,size)))x"),
               "shader creation drops the shaders that cannot be a sun light draw with a byte scan, so the index is filled while the key "
               "is off and a switch turned on in a running session finds its shader");
        EXPECT(!Has(cpp, "CopyResource") && !Has(cpp, "PSSetShaderResources"), "the index side never touches the pipeline");
        // The cost fix, pinned as the absence it is: the draw hook used to ask the context for its pixel shader on every draw.
        EXPECT(!Has(cpp, "PSGetShader"), "the draw path makes no state query of its own");
        EXPECT(Has(cpp, R"x(state.boundLayoutId.load(std::memory_order_relaxed))x"), "it reads the seam's integer instead");
        EXPECT(Has(cpp, R"x(constuint32_tid=instances==0?TrackedIdOf(shader):0;)x"),
               "a bind is classified by the seam, and a class-instance override is not a sun light draw");
        EXPECT(Has(cpp, R"x(state.boundLayoutId.store(id,std::memory_order_relaxed);)x"),
               "the seam stores the layout id the game's bind gave the draw path");
        EXPECT(Has(cpp, R"x(if(id==0||FAILED(shader->GetPrivateData(kLayoutTag,&bytes,&stamped))||stamped!=id))x"),
               "a recycled address is not trusted: the stamp is the shader object's own answer");
        EXPECT(Has(cpp, R"x(if(state.inPass.load(std::memory_order_relaxed))return;)x"),
               "the pass's own shaders do not overwrite what the game bound (the state swap puts the game's back)");
        const std::string apply = Code(repo, "ffxiv/relight/ContactShadowsApply.cpp");
        EXPECT(Has(apply, R"x(structRestore{)x"), "the game's binding is put back by a scope guard");
        EXPECT(Has(apply, R"x(context->PSSetShaderResources(slot,1,&view);)x"), "the binding that was there is what comes back");
        EXPECT(Has(apply, R"x(if(!IsShapeDecline(reason)&&state.session.NoteDecline()&&!state.loggedOff))x"),
               "the refusal limit is counted in one place, and only for a refusal that is not about the draw's shape");
        EXPECT(Has(cpp, R"x(if(!state.refusedShapes.empty()))x") &&
                   Has(cpp, R"x(ShapeRefusalActive(refusal->second.refusedQpc,Qpc(),QpcFrequency()))x"),
               "a shape refusal is remembered for a few seconds, not for the session: a re-created mask is looked at again");
        EXPECT(!Has(cpp, "constint64_tnow=Qpc();"),
               "the draw path reads the clock only for a draw whose shader has a refusal pending, never on every draw");
        EXPECT(Has(cpp, R"x(elseif(IsShapeDecline(outcome))state.refusedShapes[id]=ShapeRefusal{Qpc()};)x"),
               "and it is remembered from the refusal, with its time");
        // The three layers that keep the pass's own draws from re-entering it (the stack overflow of the first benchmark run).
        EXPECT(Has(cpp, R"x(if(state.inPass.load(std::memory_order_relaxed)){draw(user);return;})x"),
               "a draw the module's own pass makes is passed through, never classified");
        EXPECT(Has(apply, R"x(previous(state.inPass.exchange(true,std::memory_order_relaxed)))x"),
               "the pass's run raises the flag for the whole run, nesting-safe");
        EXPECT(!Has(cpp, "constint64_tnow=Qpc();") && Has(cpp, R"x(constboollogging=ext::Active(ext::F::ContactShadows);)x"),
               "shader creation logs nothing while the key is off");
        EXPECT(Has(cpp, R"x(if(outcome==Decline::None)state.refusedShapes.erase(id);)x"),
               "a shader that has run once is never remembered as refused");
        EXPECT(Has(cpp, R"x(layout=I().layouts[id-1];)x"), "the layout is copied under the mutex, never a pointer into the vector");
        EXPECT(Has(apply, R"x(state.session.NoteRun();)x"), "an accepted draw clears the refusal count");
        // Read, never map: the staging copy is the only readback, and it is taken once per session.
        EXPECT(Count(apply, "CopySubresourceRegion") == 2, "two GPU-side copies: the sun vector and the camera matrices");
        EXPECT(!Has(apply, "UpdateSubresource") && !Has(apply, "ClearRenderTargetView"), "nothing of the game is written");
        EXPECT(Has(apply, R"x(if(!state.haveSun))x"), "the sun vector and the camera layout are read once, not per frame");
        EXPECT(Has(apply, R"x(!CameraLayoutOk(cameraRows,kCameraCheckFloats))x"),
               "the camera buffer is checked against the matrix products before the pass may run");
        EXPECT(Has(apply, R"x(state.pass.HasGpuMilliseconds(),state.pass.GpuMilliseconds())x"),
               "the run line reports the pass's own GPU cost, separate from the hook's CPU cost");
    }

    // ---- the pass: two draws in a private device-context state, and the mask copy is the pass's own texture
    {
        const std::string pass = Code(repo, "ffxiv/relight/ContactShadowsPass.cpp");
        EXPECT(Count(pass, "SwapDeviceContextState(") == 3, "the private state is entered, undone on a failed swap, and left");
        EXPECT(Has(pass, R"x(context1->SwapDeviceContextState(_state.Get(),nullptr);)x"), "a failed swap is undone at once");
        // The swap back is a scope guard: one of the two draws throws through this fork's hooks, and a throw must not leave the game in the
        // module's private state (no render targets, wrong shaders) for the rest of the session.
        EXPECT(Has(pass, R"x(structStateRestore{)x") && Has(pass, R"x(if(previous!=nullptr)context->SwapDeviceContextState(previous,nullptr);)x"),
               "the game's state comes back on every path, a throwing draw included");
        EXPECT(Has(pass, R"x(if(_running)returnfalse;)x") && Has(pass, R"x(if(!TryEnter())returnnullptr;)x"),
               "a second entry into the pass while a run holds it is refused");
        EXPECT(Has(pass, R"x(constbooltimed=BeginGpuTiming(context);)x") && Has(pass, R"x(if(timed&&_end&&_disjoint))x"),
               "a run ends the timestamp pair only when it began it (no End without Begin)");
        EXPECT(Has(pass, R"x(constD3D11_VIEWPORTtermViewport{float(uint32_t(inputs.window.x*0.5f)),float(uint32_t(inputs.window.y*0.5f)),)x"),
               "the march is a viewport at the render window's own origin in the term target");
        EXPECT(Has(pass, R"x(!EnsureTerm(device.Get(),HalfExtent(float(inputs.maskWidth)),HalfExtent(float(inputs.maskHeight))))x"),
               "the term is the allocation's half, so a dynamic-resolution change creates no D3D object");
        EXPECT(Has(pass, R"x(context->Draw(3,0);)x"), "two full-screen triangles");
        EXPECT(Count(pass, "context->Draw(3,0);") == 2, "the march and the combine");
        EXPECT(Has(pass, "DXGI_FORMAT_R8_UNORM"), "the term is the 8-bit format the mask's precision allows");
        EXPECT(!Has(pass, "D3D11_MAP_READ"), "the pass maps nothing");
        // The pass's GPU cost, measured with timestamp queries and read one run later, never waited for.
        EXPECT(Has(pass, R"x(D3D11_QUERY_DESCdisjointDesc{D3D11_QUERY_TIMESTAMP_DISJOINT,0};)x"), "a timestamp-disjoint query pair");
        EXPECT(Has(pass, R"x(context->GetData(_disjoint.Get(),&disjoint,sizeofdisjoint,0)!=S_OK)x"),
               "the measurement never blocks the render thread");
        EXPECT(Has(pass, R"x(BeginGpuTiming(context);)x") && Has(pass, R"x(context->End(_end.Get());)x"),
               "the pair covers the march and the combine");
    }

    // ---- the rules stay pure, so the host tests can build them alone
    {
        const std::string rules = Code(repo, "ffxiv/relight/ContactShadowsRules.h");
        EXPECT(!Has(rules, "d3d11") && !Has(rules, "ID3D11"), "the rules are D3D free");
        EXPECT(!Has(rules, "std::filesystem") && !Has(rules, "std::ofstream"), "the rules do no I/O");
        EXPECT(Has(rules, R"x(inlineconstexpruint32_tkDeclineLimit=16;)x"), "the refusal limit the design names");
    }

    // ---- the keys: off by default, published at the config load, documented, and each with a control or a stated reason
    {
        const std::string config = Code(repo, "ffxiv/FfxivConfig.h");
        EXPECT(Has(config, R"x(CustomOptional<bool>contactShadows{false};)x"), "off by default: today's behaviour");
        EXPECT(Has(config, R"x(CustomOptional<float>contactShadowsLength{0.5f};)x"), "the length default");
        EXPECT(Has(config, R"x(CustomOptional<float>contactShadowsStrength{0.5f};)x"), "the strength default");
        EXPECT(Has(config, R"x(CustomOptional<int32_t>contactShadowsSteps{12};)x"), "the step default");
        EXPECT(Has(config, R"x(CustomOptional<std::string>contactShadowsSunSpace{"auto"};)x"), "the space default reads as the measurement");
        EXPECT(Has(config, R"x(CustomOptional<std::string>contactShadowsSunSign{"auto"};)x"), "the sign default reads as the measurement");
        EXPECT(Has(config, R"x(CustomOptional<int32_t>contactShadowsDump{0};)x"), "the dump is off by default: 0 writes nothing");
        EXPECT(Has(config, R"x(contactShadowsOnLoaded(contactShadows.value_or_default());)x"), "the key is published at the end of a load");

        const std::string bound = Code(repo, "core/ExtConfig.cpp");
        for (const char* key : { "ContactShadows", "ContactShadowsLength", "ContactShadowsStrength", "ContactShadowsSteps",
                                 "ContactShadowsSunSpace", "ContactShadowsSunSign", "ContactShadowsDump" })
            EXPECT(Has(bound, std::string(R"x(b.Add("FFXIV",")x") + key + R"x(",contactShadows)x"), "[FFXIV] %s is bound", key);

        const std::string ini = Raw(repo, "ini/FFXIV.ini");
        for (const char* key : { "\nContactShadows=auto", "\nContactShadowsLength=auto", "\nContactShadowsStrength=auto",
                                 "\nContactShadowsSteps=auto", "\nContactShadowsSunSpace=auto", "\nContactShadowsSunSign=auto",
                                 "\nContactShadowsDump=auto" })
            EXPECT(Has(ini, key), "the ini fragment documents %s", key + 1);
        // The two ini-only keys say so in their own comment block (the menu manifest test's rule).
        for (const char* key : { "ContactShadowsSunSpace=", "ContactShadowsSunSign=", "ContactShadowsDump=" })
        {
            const size_t at = ini.find(key);
            EXPECT(at != std::string::npos, "fragment line %s", key);
            if (at == std::string::npos)
                continue;
            size_t block = at;
            while (block > 0)
            {
                size_t previous = block - 1;
                while (previous > 0 && ini[previous - 1] != '\n')
                    --previous;
                if (ini.compare(previous, 1, ";") != 0)
                    break;
                block = previous;
            }
            EXPECT(ini.find("No menu control", block) < at, "the comment of %s says that it has no menu control", key);
        }

        const std::string manifest = Code(repo, "core/MenuManifest.h");
        EXPECT(Has(manifest, R"x("[FFXIV]ContactShadowsSunSpace",)x") && Has(manifest, R"x("[FFXIV]ContactShadowsSunSign",)x") && Has(manifest, R"x("[FFXIV]ContactShadowsDump",)x"),
               "both ini-only keys are listed in the manifest");
        EXPECT(!Has(manifest, R"x("[FFXIV]ContactShadows",)x"), "the switch itself has a menu control and is not listed as ini-only");

        const std::string menu = Code(repo, "ffxiv/relight/RelightMenu.cpp");
        EXPECT(Has(menu, R"x(cfg.contactShadows=shadows;)x") && Has(menu, R"x(cfg.contactShadowsLength=ClampLength(length);)x") &&
                   Has(menu, R"x(cfg.contactShadowsStrength=ClampStrength(strength);)x") &&
                   Has(menu, R"x(cfg.contactShadowsSteps=int32_t(ClampSteps(steps));)x"),
               "every player-facing key has a control that clamps it");
        EXPECT(Has(menu, R"x(if(changed)Publish(shadows);)x"),
               "every control publishes its value, so a switch turned on in a running session arms the pass and the sliders reach the "
               "next light draw");
        EXPECT(Has(menu, R"x({{"ffxiv_contact_shadows","FFXIV","Contactshadows(FFXIV)",Draw,"output_scaling",Visible})x"),
               "the card is registered");
    }

    if (g_failures)
    {
        std::printf("FAIL: %d expectation(s)\n", g_failures);
        return 1;
    }
    std::puts("PASS: contact shadow wiring pins (one relaxed load while off, two call sites, the game's binding restored, off by default, "
              "each key with a control or a reason)");
    return 0;
}
