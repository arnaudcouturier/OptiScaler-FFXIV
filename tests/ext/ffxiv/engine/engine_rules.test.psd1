@{
    Class       = 'cpu'
    Note        = 'Engine pass override rules (ext/ffxiv/engine/EnginePassRules.h): the name table and a layout lookup at any slot, the shape test that only a shader declaring both of the composite''s anchors (at a slot D3D11 exposes) is looked at and that the frame''s other indexed draws never reach the refusal counter, the ambient probe offset and stride, the scale entry fitting (or not) both buffer sizes, the identity rule that makes 1.0 today''s picture, the 64-bit anchors word (the shipped bits and slot fields at their old values in the low word, the later knobs'' fields disjoint above it, and a word that carries them changing no shipped answer), the whole draw validation (both accepted sizes, one render target, a window inside the target, a character-preview replay), the sixty-four-refusal switch-off and the log budget and text, the independence of the two features'' switch-offs (a stood-down module patches nothing while the ambient occlusion pass keeps the seam and its own arming), and the master key''s fold into both (the four combinations of master and occlusion pass: neither knob may patch and the card reads off while the key is off, whatever keeps the seam entered)'
    Sources     = @('engine_rules.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
