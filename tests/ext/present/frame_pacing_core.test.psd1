@{
    Class       = 'cpu'
    Note        = '[PACE] pure core of the frame-pacing trace: accumulator thresholds, 10 s windows, span tracker, log line text, enable decision with its reason text'
    Sources     = @('frame_pacing_core_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('UNDEBUG')
}
