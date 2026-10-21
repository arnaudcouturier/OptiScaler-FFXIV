@{
    Class       = 'cpu'
    Note        = '[PACE] hidden-present seam body (hidden::SeamPresent, the code HiddenPresent runs) against a fake swapchain: key off = Present(0, callerFlags) and the chain''s own answer; key on = flags | DO_NOT_WAIT, skip reads as S_OK; key read per present; latch and episode through the seam'
    Sources     = @('hidden_present_seam_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('UNDEBUG')
}
