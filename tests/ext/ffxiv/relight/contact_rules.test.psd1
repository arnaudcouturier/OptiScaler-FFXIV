@{
    Class       = 'cpu'
    Note        = 'Contact shadow rules (ext/ffxiv/relight/ContactShadowsRules.h): the key clamps, the sun space and sign names, the unit-length and rate guards, the whole draw drop list (including the cascade atlas by size and by format), the log budget and text, the sixteen-refusal switch-off and the half-resolution term size'
    Sources     = @('contact_rules.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
