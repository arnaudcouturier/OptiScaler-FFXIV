@{
    Class       = 'cpu'
    Note        = 'Window mode: source-level check that the seam bodies and the ReShade handler use only the inline guards and the event is subscribed only under a mode check'
    Sources     = @('window_call_sites.cpp')
    Args        = @('{repo}')
}
