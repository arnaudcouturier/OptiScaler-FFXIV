@{
    Class   = 'cpu'
    Note    = '[PACE] the hidden present''s hooks (sliced out of FramePacingTrace.cpp, driven through the real hidden::SeamPresent): key off = the caller''s flags and answer, key on = DO_NOT_WAIT, a skip reads S_OK and feeds the trace, the key is read every present, a rejected flag warns once and stays off (negative control)'
    Runner  = 'run_hidden_present_hooks.ps1'
    Sources = @('hidden_present_hooks.cpp')
}
