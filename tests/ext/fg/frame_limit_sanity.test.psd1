@{
    Class       = 'cpu'
    Note        = 'Frame limit sanity: NaN, infinite and non-positive caps mean no cap (FrameLimitSanity.h); FrameInterval and FgConfig::PostLoad use the one predicate'
    Sources     = @('frame_limit_sanity.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
}
