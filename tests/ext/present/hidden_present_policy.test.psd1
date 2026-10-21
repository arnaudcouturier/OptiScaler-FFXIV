@{
    Class       = 'cpu'
    Note        = '[PACE] hidden DX11 present policy: DO_NOT_WAIT flag handling, skip classification, latch on invalid argument, episode log, with a fake busy swapchain'
    Sources     = @('hidden_present_policy_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('UNDEBUG')
}
