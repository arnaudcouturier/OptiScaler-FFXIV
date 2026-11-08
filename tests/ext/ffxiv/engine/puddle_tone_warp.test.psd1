@{
    Class       = 'warp'
    Note        = 'Puddles'' tone-map stage (ext/ffxiv/engine/PuddleTonePass.cpp and shaders/PuddleReflection.hlsl, with the march the water reflections share) on D3D11 WARP against a CPU reference of the same formula: a dry frame is the scene byte for byte, a pool texel with nothing above it is exactly the darkened scene (a miss is the engine''s own probe reflection, never black), a pool under a bright wall comes back brighter than the darkened scene by a gain that is the reference''s pool mask times the key''s strength times water''s Fresnel (all three channels, so the hit''s colour is the wall''s), the wall and a dry texel keep the engine''s own value, the scene the chained order hands over (the same frame with an air-light-like addition to its scene comes back with every pool texel grown by what the pool keeps of it plus the reflection''s own gain, and everything outside the pool by the addition whole; the chain''s own entries are driven in puddle_tone_chain_warp), the reflection key at off is the darkening alone, the darkening at zero with the reflection off is the stage''s identity and the scene byte for byte, the same world point seen from two cameras has the same mask (the field is anchored to the world, which is what keeps the pool still while the camera moves), two runs of a scene are byte-identical and a resize builds its own targets, the statistics target counts the pool and the march''s hits for engine.txt, only the render window''s texels are shaded while its one texel of border is the scene''s own, and a frame the pure rules refuse returns null so the caller keeps the engine''s scene'
    Prepare     = 'prepare_puddle_tone.ps1'
    Sources     = @('puddle_tone_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/engine/PuddleTonePass.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}')
    Args        = @('{repo}')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
