#pragma once

#include <Config.h>

#include "ext/ffxiv/chara/CharaConfig.h"

namespace ext
{
namespace config
{
class Binder;
}

struct FfxivConfig
{
    CustomOptional<bool> jitterProbe { false };      // [FFXIV] JitterProbe: read-only log of the game's applied versus reported jitter (ext/ffxiv/jitter)
    CustomOptional<bool> jitterCorrection { false }; // [FFXIV] JitterCorrection: experimental, learns the ratio the game applies and scales the reported jitter by it
    CustomOptional<float> jitterScaleOverride { 0.0f }; // [FFXIV] JitterScaleOverride: experiment, a fixed scale (0.25 to 1.0) instead of the learned one; 0 = off
    CustomOptional<bool> jitterSource { false };     // [FFXIV] JitterSource: experimental, fixes the jitter at the source (full lattice, repeating cycle, told what was drawn); ext/ffxiv/jitter
    // Game build handling (ext/ffxiv/resolve). Persistent keys, read once when the game's first device is hooked; no menu control.
    CustomOptional<bool> legacyGameBuild { false };   // [FFXIV] FfxivLegacyGameBuild: rollback, exactly the behaviour before pattern search (exact build match, compile-time table, no resolve)
    CustomOptional<bool> unknownBuildHooks { false }; // [FFXIV] FfxivUnknownBuildHooks: allow the features that read game addresses on a game build the table does not know
    CustomOptional<bool> forceUnknownBuild { false }; // [FFXIV] FfxivForceUnknownBuild: debug, ignore the table so the resolver's values alone can be tried on today's exe
    CustomOptional<bool> automaticReShadeDepth { false }; // [FFXIV] AutomaticReShadeDepth: ReShade effects get a copy of the scene depth at the output size (ext/reshade/ReShadeDepth.h)
    // Developer draw diagnostic (ext/ffxiv/diag), ini only, off by default: `off` | `capture`. Recorded when the game creates its shaders,
    // so it is read once at start; anything but `capture` reads as off, which is today's behaviour.
    CustomOptional<std::string> drawDiagnostics { "off" };     // [FFXIV] DrawDiagnostics
    CustomOptional<int32_t> drawDiagnosticsFrames { 3 };       // [FFXIV] DrawDiagnosticsFrames: how many frames one capture writes (1..16)
    // When the capture starts (ext/ffxiv/diag/DrawDiagFormat.h has the rules): seconds after the config load, or a key press. Both
    // default to today's behaviour, the capture collecting from the first draw.
    CustomOptional<int32_t> drawDiagnosticsDelaySeconds { 0 }; // [FFXIV] DrawDiagnosticsDelaySeconds: 0..3600, 0 = at the first draw
    CustomOptional<std::string> drawDiagnosticsKey { "off" };  // [FFXIV] DrawDiagnosticsKey: a letter, a digit or F1..F24; off = no key
    // One captured frame's targets as raw bytes next to the capture, for the developer check: off (today's behaviour) | frame.
    CustomOptional<std::string> drawDiagnosticsDump { "off" }; // [FFXIV] DrawDiagnosticsDump: off, frame, or ssr-pair
    // The one-frame dump's byte budget in MiB, 64 to 4096: 256 is today's bound, and a 4K frame's full set needs about 500.
    CustomOptional<int32_t> drawDiagnosticsDumpBudgetMiB { 256 }; // [FFXIV] DrawDiagnosticsDumpBudgetMiB
    // Contact shadows before the sun light pass (ext/ffxiv/relight): the switch and its tuning, off by default, and the two keys that
    // settle what the measurement of the sun vector could not (ext/ffxiv/relight/ContactShadowsRules.h has the ranges).
    CustomOptional<bool> contactShadows { false };            // [FFXIV] ContactShadows
    CustomOptional<float> contactShadowsLength { 0.5f };      // [FFXIV] ContactShadowsLength: metres of march, 0.05..2.0
    CustomOptional<float> contactShadowsStrength { 0.5f };    // [FFXIV] ContactShadowsStrength: 0..1 of the sun's visibility
    CustomOptional<int32_t> contactShadowsSteps { 32 };       // [FFXIV] ContactShadowsSteps: 4..24
    CustomOptional<std::string> contactShadowsSunSpace { "auto" }; // [FFXIV] ContactShadowsSunSpace: auto | world | view
    CustomOptional<std::string> contactShadowsSunSign { "auto" };  // [FFXIV] ContactShadowsSunSign: auto | toward | from
    // [FFXIV] ContactShadowsDump: seconds after the game start at which one frame is written out, 0 = never (the developer check)
    CustomOptional<int32_t> contactShadowsDump { 0 };
    // Engine pass overrides (ext/ffxiv/engine): the master switch of the module that patches one of the engine's own passes for one draw, and
    // the first knob it carries. Off and at the vanilla value by default, so a fresh ini is today's picture.
    CustomOptional<bool> enginePassOverrides { false };       // [FFXIV] EnginePassOverrides
    CustomOptional<float> ambientSkyVisibilityScale { 1.0f }; // [FFXIV] AmbientSkyVisibilityScale: 0..2, 1.0 = the engine's own value
    // [FFXIV] WetnessOverride: the wetness the engine applies to the ground. -1 = the engine's own value (off); 0..1 = the amount to
    // force, with the engine's own dryness blend following it.
    CustomOptional<float> wetnessOverride { -1.0f };
    // Lighting tab (ext/ffxiv/lighting): the chara draws' own ambient block - the character-at-night knob. Off and at the engine's picture by
    // default, so a fresh ini is today's picture (chara::CharaAmbientRules.h has the ranges).
    CustomOptional<bool> charaAmbient { false };     // [FFXIV] CharaAmbient
    CustomOptional<float> charaAmbientLift { 1.0f }; // [FFXIV] CharaAmbientLift: 0.5..2.0, 1.0 = the engine's own block
    // Ambient occlusion (ext/ffxiv/engine/AoRules.h has the ranges): this fork's own occlusion in place of the engine's screen-space
    // occlusion texture, computed from the game's depth and G-buffer normal. Off, at the engine's picture, by default.
    CustomOptional<bool> ambientOcclusion { false };         // [FFXIV] AmbientOcclusion
    CustomOptional<float> ambientOcclusionRadius { 1.0f };   // [FFXIV] AmbientOcclusionRadius: metres of world searched, 0.1..4
    CustomOptional<float> ambientOcclusionStrength { 1.0f }; // [FFXIV] AmbientOcclusionStrength: how much of it is applied, 0..2
    CustomOptional<float> ambientOcclusionBlend { 0.5f };    // [FFXIV] AmbientOcclusionBlend: ours against the engine's own, 0..1
    CustomOptional<std::string> ambientOcclusionQuality { "medium" }; // [FFXIV] AmbientOcclusionQuality: low | medium | high
    // Puddles (ext/ffxiv/engine/GlossPuddlesRules.h has the ranges): the engine's own G-buffer target, copied and shaped into puddles on the
    // up-facing ground, bound at every reader of that target; and, at the tone-map draw, the pool's colour darkened and a marched reflection of
    // the scene added to it. Off by default, and a switch left on with the coverage at zero is today's picture.
    CustomOptional<bool> puddles { false };                    // [FFXIV] Puddles
    CustomOptional<float> puddlesCoverage { 0.42f };           // [FFXIV] PuddlesCoverage: 0..1, the share of the up-facing ground that becomes puddle at full rain
    CustomOptional<float> puddlesScale { 6.0f };               // [FFXIV] PuddlesScale: 0.05..16, metres one noise cell spans
    CustomOptional<float> puddlesSoftness { 0.03f };           // [FFXIV] PuddlesSoftness: 0..0.5, the half-width of a puddle's edge
    CustomOptional<int32_t> puddlesGlossTarget { 1 };          // [FFXIV] PuddlesGlossTarget: 1..2, which G-buffer target carries the gloss channel
    CustomOptional<std::string> puddlesGlossChannel { "a" };    // [FFXIV] PuddlesGlossChannel: r | g | b | a, the channel this pass replaces (a: the gloss; r on target 1 is a selector and is refused)
    CustomOptional<float> puddlesGlossWet { 1.0f };            // [FFXIV] PuddlesGlossWet: 0..1, the channel value of a mirror-wet surface
    CustomOptional<bool> puddlesReflection { true };           // [FFXIV] PuddlesReflection: the marched mirror at the tone map (off: the darkening alone)
    CustomOptional<float> puddlesReflectionStrength { 0.6f };  // [FFXIV] PuddlesReflectionStrength: 0..1 of the Fresnel reflection the hit is added with
    CustomOptional<float> puddlesDarkening { 0.45f };          // [FFXIV] PuddlesDarkening: 0..1 of the lit colour a full puddle takes away
    CustomOptional<float> puddlesRipple { 0.35f };             // [FFXIV] PuddlesRipple: 0..1, how much the static ripple tilts the mirror
    CustomOptional<int32_t> puddlesReflectionSteps { 24 };     // [FFXIV] PuddlesReflectionSteps: 8..48, the march's taps
    CustomOptional<float> puddlesReach { 24.0f };              // [FFXIV] PuddlesReach: metres a pool may sit at (0 = no limit); beyond it open water keeps the engine's texel
    // Light shafts before the tone map (ext/ffxiv/engine/ShaftsDraw.cpp): the master switch and the four tuning keys, off by default so a fresh
    // ini is today's picture (ShaftsRules.h has the ranges and what each key does to the picture).
    CustomOptional<bool> lightShafts { false };          // [FFXIV] LightShafts
    CustomOptional<float> lightShaftsIntensity { 1.0f }; // [FFXIV] LightShaftsIntensity: 0..2, 0 = off
    CustomOptional<float> lightShaftsDecay { 0.96f };    // [FFXIV] LightShaftsDecay: 0.80..1.0
    CustomOptional<int32_t> lightShaftsSamples { 48 };   // [FFXIV] LightShaftsSamples: 16..96
    CustomOptional<float> lightShaftsDensity { 0.9f };   // [FFXIV] LightShaftsDensity: 0.1..1.5
    CustomOptional<std::string> lightShaftsColor { "auto" }; // [FFXIV] LightShaftsColor: auto | white
    // Shadow mapping (ext/ffxiv/shadows/ShadowsRules.h has the ranges): the sun/shadow mask's lift at the light draws and the composite's
    // night ambient lift, keyed on the frame's own light colour. Off and at the engine's picture by default, so a fresh ini is today's picture.
    CustomOptional<bool> shadowMapping { false };          // [FFXIV] ShadowMapping
    CustomOptional<float> shadowLift { 0.0f };             // [FFXIV] ShadowLift: 0..0.9, 0 = the engine's own mask
    CustomOptional<float> shadowEdgeSoftness { 0.0f };     // [FFXIV] ShadowEdgeSoftness: 0..2 mask pixels of tent
    CustomOptional<float> nightAmbientLift { 1.0f };       // [FFXIV] NightAmbientLift: 1.0..4.0, 1.0 = the engine's own
    CustomOptional<bool> nightCurveToggle { true };        // [FFXIV] NightCurveToggle: the day/night curve on (an off pins the full effect)
    // God rays: the volumetric march (ext/ffxiv/engine/GodraysRules.h has the ranges): a quarter-res per-pixel ray march along the shadow
    // light's direction, each sample's shadow read from the engine's own cascade shadow atlas, the density from the engine's own live fog
    // state, added to a copy of the scene the tone-map chain hands it. Off by default, and every key at the engine's own value, so a fresh
    // ini is today's picture.
    CustomOptional<bool> godraysVolumetric { false };         // [FFXIV] GodraysVolumetric
    CustomOptional<int32_t> godraysSteps { 48 };              // [FFXIV] GodraysSteps: the march's samples, 16..96
    CustomOptional<std::string> godraysResolution { "quarter" }; // [FFXIV] GodraysResolution: quarter | half
    CustomOptional<float> godraysFogBoost { 1.0f };           // [FFXIV] GodraysFogBoost: 0..2, 1.0 = the engine's own density
    CustomOptional<float> godraysHeightLift { 1.0f };         // [FFXIV] GodraysHeightLift: 0..2, 1.0 = the engine's own height falloff
    CustomOptional<float> godraysIntensity { 1.0f };          // [FFXIV] GodraysIntensity: 0..2, 0 = off
    CustomOptional<std::string> godraysColor { "auto" };      // [FFXIV] GodraysColor: auto | white | enginefog
    CustomOptional<int32_t> godraysShadowSoftness { 0 };      // [FFXIV] GodraysShadowSoftness: 0..2 taps
    CustomOptional<bool> godraysJitter { true };              // [FFXIV] GodraysJitter
    CustomOptional<float> godraysPhaseBoost { 1.0f };         // [FFXIV] GodraysPhaseBoost: 0..2, 1.0 = neutral (scales the lobe's narrowness)
    CustomOptional<float> godraysDensity { 20.0f };           // [FFXIV] GodraysDensity: 0..200, units of 0.001 per metre at the camera's height
    CustomOptional<float> godraysAnisotropy { 0.8f };         // [FFXIV] GodraysAnisotropy: 0..0.95, the Henyey-Greenstein g
    // Screen-space light bounce (ext/ffxiv/engine/GiRules.h has the ranges): the GI tab's surface-bounce stage on the tone-map chain the march
    // hangs on, chained after it. K cosine-distributed rays per half-res pixel, each ray marched against the composite's own view positions up
    // to a range key, a hit's radiance its own linear scene value ((scene rgb)^2, the surface's colour left in), the estimate
    // added through the game's own white-bounded addition. Off by default, and every key at today's picture.
    CustomOptional<bool> gi { false };                // [FFXIV] Gi
    CustomOptional<int32_t> giRays { 4 };             // [FFXIV] GiRays: the rays one casts, 1..8
    CustomOptional<int32_t> giSteps { 16 };           // [FFXIV] GiSteps: the march's steps, 8..32
    CustomOptional<float> giRange { 5.0f };           // [FFXIV] GiRange: the march's reach in metres, 1.0..20.0
    CustomOptional<float> giStrength { 0.55f };       // [FFXIV] GiStrength: 0..1, 1 = the full diffuse bounce, 0.55 = the subtle default
    CustomOptional<std::string> giResolution { "half" }; // [FFXIV] GiResolution: half | quarter
    // Lens flare (ext/ffxiv/engine/FlareDraw.cpp): the engine's own flare family with each part's amount in your hands - the ring's colour,
    // the sun sprite's colour, the step the ghost orbs march by, and the veil's skip. Off and at the engine's picture by default, so a
    // fresh ini is today's picture (FlareRules.h has the ranges and the parts bitfield's meaning).
    CustomOptional<bool> flareCard { false };      // [FFXIV] FlareCard
    CustomOptional<int32_t> flareParts { 7 };      // [FFXIV] FlareParts: the parts bitfield, ring 1, sun+orbs 2, veil 4
    CustomOptional<float> flareRingScale { 1.0f }; // [FFXIV] FlareRingScale: 0..2, 1.0 = the engine's own
    CustomOptional<float> flareSunScale { 1.0f };  // [FFXIV] FlareSunScale: 0..2, 1.0 = the engine's own (the orbs follow their sprite)
    CustomOptional<float> flareGhostScale { 1.0f };// [FFXIV] FlareGhostScale: 0..2, 1.0 = the engine's own step
    // Screen-space reflections (ext/ffxiv/engine/SsrRules.h has the ranges): this fork's own reflection for the water surfaces, computed from
    // the engine's view-position buffer and the scene the frame has already rendered, in place of the engine's planar reflection map. Off, at
    // the engine's picture, by default.
    CustomOptional<bool> screenSpaceReflections { false };            // [FFXIV] ScreenSpaceReflections
    CustomOptional<bool> screenSpaceReflectionsDiagnostic { false };  // [FFXIV] ScreenSpaceReflectionsDiagnostic: F7 cycles engine-SSR-private-copy map sources
    CustomOptional<int32_t> screenSpaceReflectionsSteps { 48 };        // [FFXIV] ScreenSpaceReflectionsSteps: ray march taps, 16..128
    CustomOptional<float> screenSpaceReflectionsThickness { 0.3f };    // [FFXIV] ScreenSpaceReflectionsThickness: metres, 0.05..2
    CustomOptional<float> screenSpaceReflectionsBlend { 0.5f };       // [FFXIV] ScreenSpaceReflectionsBlend: ours against the engine's, 0..1
    CustomOptional<float> screenSpaceReflectionsFade { 0.15f };         // [FFXIV] ScreenSpaceReflectionsFade: the picture's edge, 0..0.5
    // Developer probe, ini only: a constant half-screen mask handed to the upscaler's mask input, to see whether the backend reads
    // one at all. off | left | right, anything else reads as off (ext/ffxiv/vfxmask).
    CustomOptional<std::string> vfxMaskProbe { "off" }; // [FFXIV] VfxMaskProbe
    // Depth of field (ext/ffxiv/engine/DofRules.h has the ranges): this fork's own DoF chained at the tone-map owner as the
    // chain's last stage before t0, mirroring the game's own Weight2/DownScale4x4/DiscBlurNear/Merge2 family with an
    // analytic CoC in place of the engine's LUT. Off by default, and a strength of 0 (or an aperture at the floor) is
    // today's picture even with the switch on.
    CustomOptional<bool> depthOfField { false };         // [FFXIV] DepthOfField
    CustomOptional<std::string> dofFocusMode { "auto" };    // [FFXIV] DofFocusMode: auto | manual
    CustomOptional<float> dofFocusDistance { 3.0f };        // [FFXIV] DofFocusDistance: 0.5..500 m (manual only)
    CustomOptional<float> dofAperture { 0.05f };            // [FFXIV] DofAperture: 0.005..0.12 of the window height, 0 = off
    CustomOptional<float> dofStrength { 0.8f };             // [FFXIV] DofStrength: 0..1, 0 = off
    CustomOptional<int32_t> dofSteps { 24 };                // [FFXIV] DofSteps: the disc's taps, 16..48
    CustomOptional<std::string> dofMode { "distance" };     // [FFXIV] DofMode: distance (gameplay, the default) | cinematic (the first release's look)
    ffxiv::chara::CharaConfig chara; // ext/ffxiv/chara: [FFXIV] Preview* and [HDR] PreviewNRTrace

    void Bind(config::Binder& b);

    // End of config::Load (every load and reload). Hands the AutomaticReShadeDepth key to ext/reshade/ReShadeDepth.cpp, which registers
    // the hook at start-up. A pointer rather than a call so that ExtConfig.cpp keeps no link dependency on the glue: the unit tests that
    // build ExtConfig.cpp alone find it null and nothing happens. The hook only STORES the value (this can run inside DllMain).
    using BoolHook = void (*)(bool on);
    static inline BoolHook automaticReShadeDepthOnLoaded = nullptr;
    // The developer draw diagnostic's key (ext/ffxiv/diag): arms one capture. Same pointer pattern and same reason.
    static inline BoolHook drawDiagnosticsOnLoaded = nullptr;

    // Same for the VFX mask probe (ext/ffxiv/vfxmask/VfxMaskProbe.cpp): the text of [FFXIV] VfxMaskProbe, parsed there.
    using TextHook = void (*)(const char* text);
    static inline TextHook vfxMaskProbeOnLoaded = nullptr;
    // And for the contact shadow switch (ext/ffxiv/relight/Inject.cpp): the key arms the seam's feature bit.
    static inline BoolHook contactShadowsOnLoaded = nullptr;
    // And for the shadow mapping module (ext/ffxiv/shadows/ShadowsDispatch.cpp): the key arms the module's bit and
    // publishes its keys (the same hook-pointer pattern; the module reads its own remainder itself).
    static inline BoolHook shadowMappingOnLoaded = nullptr;
    // And for the engine pass overrides (ext/ffxiv/engine/EnginePass.cpp): the key arms the seam's feature bit.
    static inline BoolHook enginePassOverridesOnLoaded = nullptr;
    // And for the ambient occlusion pass (ext/ffxiv/engine/AoApply.cpp): the keys and the seam's feature bit. It publishes before the module's
    // own hook, because that hook's arming asks this pass whether it wants a draw.
    static inline BoolHook ambientOcclusionOnLoaded = nullptr;
    // And for the puddles pass (ext/ffxiv/engine/GlossPuddlesApply.cpp): the keys and the seam's feature bit, published for the same reason.
    static inline BoolHook puddlesOnLoaded = nullptr;
    // And for the light shafts (ext/ffxiv/engine/ShaftsDraw.cpp): its own key, tuning keys and feature bit. It publishes before the module's own
    // hook for the same reason: that hook's arming asks this module whether it wants a draw too.
    static inline BoolHook lightShaftsOnLoaded = nullptr;
    // And for the screen-space reflections pass (ext/ffxiv/engine/SsrApply.cpp): the keys and the seam's feature bit. It publishes before the
    // module's own hook for the same reason.
    static inline BoolHook screenSpaceReflectionsOnLoaded = nullptr;
    static inline BoolHook screenSpaceReflectionsDiagnosticOnLoaded = nullptr;
    // And for the volumetric god rays march (ext/ffxiv/engine/GodraysDraw.cpp): its own key, tuning keys and feature bit.
    static inline BoolHook godraysOnLoaded = nullptr;
    // And for the lighting tab's chara ambient (ext/ffxiv/lighting/CharaAmbientApply.cpp): the key and the lift, published from the hook chain.
    static inline BoolHook charaAmbientOnLoaded = nullptr;
    // And for the screen-space light bounce (ext/ffxiv/engine/GiDraw.cpp): its own key, tuning keys and feature bit. It publishes before the
    // march's own hook: the march's chain entry composes onto the bounce's answers when this stage wants the chain's output.
    static inline BoolHook giOnLoaded = nullptr;
    // And for the depth of field stage (ext/ffxiv/engine/DofDraw.cpp): its own key, tuning keys and feature bit, the
    // chain's last member.
    static inline BoolHook dofOnLoaded = nullptr;
    // And for the lens flare's card (ext/ffxiv/engine/FlareDraw.cpp): its key, tuning keys and feature bit, published for the same reason.
    static inline BoolHook lensFlareOnLoaded = nullptr;

    void PostLoad()
    {
        if (automaticReShadeDepthOnLoaded != nullptr)
            automaticReShadeDepthOnLoaded(automaticReShadeDepth.value_or_default());
        if (drawDiagnosticsOnLoaded != nullptr)
            drawDiagnosticsOnLoaded(drawDiagnostics.value_or_default() == "capture");
        if (vfxMaskProbeOnLoaded != nullptr)
            vfxMaskProbeOnLoaded(vfxMaskProbe.value_or_default().c_str());
        if (contactShadowsOnLoaded != nullptr)
            contactShadowsOnLoaded(contactShadows.value_or_default());
        if (shadowMappingOnLoaded != nullptr)
            shadowMappingOnLoaded(shadowMapping.value_or_default());
        if (ambientOcclusionOnLoaded != nullptr)
            ambientOcclusionOnLoaded(ambientOcclusion.value_or_default());
        if (lightShaftsOnLoaded != nullptr)
            lightShaftsOnLoaded(lightShafts.value_or_default());
        if (puddlesOnLoaded != nullptr)
            puddlesOnLoaded(puddles.value_or_default());
        if (screenSpaceReflectionsOnLoaded != nullptr)
            screenSpaceReflectionsOnLoaded(screenSpaceReflections.value_or_default());
        if (screenSpaceReflectionsDiagnosticOnLoaded != nullptr)
            screenSpaceReflectionsDiagnosticOnLoaded(screenSpaceReflectionsDiagnostic.value_or_default());
        if (godraysOnLoaded != nullptr)
            godraysOnLoaded(godraysVolumetric.value_or_default());
        if (giOnLoaded != nullptr)
            giOnLoaded(gi.value_or_default());
        if (dofOnLoaded != nullptr)
            dofOnLoaded(depthOfField.value_or_default());
        if (lensFlareOnLoaded != nullptr)
            lensFlareOnLoaded(flareCard.value_or_default());
        if (enginePassOverridesOnLoaded != nullptr)
            enginePassOverridesOnLoaded(enginePassOverrides.value_or_default());
        if (charaAmbientOnLoaded != nullptr)
            charaAmbientOnLoaded(charaAmbient.value_or_default());
    }
};
} // namespace ext
