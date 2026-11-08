@{
    Class       = 'cpu'
    Note        = 'INT1: the tone-map chain''s order as built (light bounce, night grade, atmosphere, depth of field), read as text: the order is written only in ChainHooks.h (four hook pairs declared in chain order, four dispatchers whose bodies are pinned exactly), each stage names only the dispatcher behind it (the light bounce none of the three, the night grade neither the atmosphere nor depth of field, the atmosphere only depth of field, depth of field none, the earlier stages none), the seam''s fallback-owner order and the five arming lists in EnginePass.cpp, the harness''s seam model calls its stages in the product''s order, the feature bits are distinct (atmosphere 23, night grade 24), the stage-dump ids are appended (12, 13) and the menu cards follow the chain'
    Sources     = @('look_chain_order_pins.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
