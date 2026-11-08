@{
    Class       = 'cpu'
    Note        = 'Engine pass override wetness knob rules (ext/ffxiv/engine/EnginePassRules.h): the packed word the pixel-shader seam stores carries both knobs'' bits and both slots (a composite, a material draw, a shader declaring both, slot zero, a slot past the last the engine can bind, and the four fields never overlapping), the set entry writes .x and .y as exact bit patterns and addresses no other component, the set table fitting (or not) a 32-byte buffer, the -1.0-is-off default with the whole dry half off and a NaN reading as off, the wetness entry table and the forced ends of the range, the feature bit needing at least one knob off the engine''s own value, the candidate rule (the engine''s own 32 bytes at the reflected slot, never a replay, and no render-target or viewport fact at all), the per-frame cache (one pointer per frame, a new token clearing the entry and the patch budget, four patches then TooManyBuffers, the expire-after-4096-binds fail-safe, and every reset point), and the log text of both knobs'
    Sources     = @('engine_wetness.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = @('c++20')
}
