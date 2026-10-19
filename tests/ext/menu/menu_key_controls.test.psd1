@{
    Class       = 'cpu'
    Note        = 'Menu manifest key rule: every config key the fork binds (b.Add literals under ext) is exposed by a control (its member is named in a file that draws menu widgets; upstream-owned keys in upstream menu_common.cpp) or is on kIniOnlyKeys; keys whose member name is shared (Mode of HDR, HudLess, Window) carry explicit evidence in MenuManifest.h kKeyEvidence; stale entries and negative controls (the generated NR Pass keys are outside the rule)'
    Sources     = @('menu_key_controls.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
