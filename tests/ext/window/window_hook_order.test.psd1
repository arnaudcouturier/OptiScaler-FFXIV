@{
    Class       = 'cpu'
    Note        = 'Window mode: read-only source check that upstream hkSetFullscreenState in hooks/FG_Hooks.cpp still does fgChanged = true, UpdateTarget(), Deactivate() in that order, before anything else (the module RequestFgOff mirrors it)'
    Sources     = @('window_hook_order.cpp')
    Args        = @('{repo}')
}
