@{
    Class       = 'cpu'
    Note        = 'Engine light controls (ENG2) wiring as built, read as text: the lamp wrapper is substituted for the game''s draw function in the seam''s draw path right after the anchors are read and before any module entry (one anchors load, the native-draw record refreshed, the wrapper object outliving the call), the lamp arm restores the blend state / factor / sample mask in a destructor armed before the state call and builds nothing at an identity factor (no context call), the feed request is "wanted" and never depends on the exposure stage or mode, PublishCbFamily is null-checked and called on a change of the weight or keys and from Configure, the exposure is read at the slot the composite''s reflection stamp names, the chain hooks / F9 stage 13 / feature bit / release chain keep the retired grade''s slot, the retired grade is gone (code, keys, files; its four keys only in the schema''s retired list), every config member has a binder, an INI line, a schema entry and a read in ParamsFromConfig, and the card is in the layout and the manifest'
    Sources     = @('engine_light_pins.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
