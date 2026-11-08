@{
    Class       = 'cpu'
    Note        = 'Engine light controls, constant-buffer family (ext/ffxiv/enginelight/CbFamilyRules.h): the effective factors per channel (the moon and its tint, the water pass''s own moon term REPLACING the intensity, the ambient and the water block), master off / A/B / day weight / controls at 1 / never published = the engine''s values with empty tables (no patch), NaN and absurd published values, the entry tables (moon +32/+48, composite SH rows 64+16c stride 192 x64 + the header sky scale, water block 8 entries at +0..+128 with no two on one float4, all fitting the sizes the engine binds), the shader stamp truth table (directional / fake specular / water; volumes, shadow-mask readers and chara shaders are nothing), the main-vs-water classification by shape, size and pair, the frame latch, the family''s own stand-down streak, the composite validation of a lone engine-light ask, and the F9 row text'
    Sources     = @('enginelight_rules.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
