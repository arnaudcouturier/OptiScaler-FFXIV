@{
    Class       = 'cpu'
    Note        = '[PACE] source pins on the trace sites: the HiddenPresent seam call, PacingScope first in LocalPresent, the Reflex scope before the sleep, and the ext-side sites of the other phases'
    Sources     = @('frame_pacing_upstream_pins_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('UNDEBUG')
}
