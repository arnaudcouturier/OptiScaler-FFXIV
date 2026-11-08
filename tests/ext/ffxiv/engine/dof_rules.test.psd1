@{
    Class       = 'cpu'
    Note        = 'Depth of field Route B''s pure rules on the host: the analytic CoC (0 on the focus plane, positive toward the camera, monotone, the broken-input guards), the shaped CoC (a sharp focus band of 0.12 with a continuous linear remap above it, the near side capped at 0.25, the far side uncapped, NaN and infinities), the near/far split''s weights, the view-depth round trip through the march''s fixture camera, the camera band''s plane-pair guard (the contact pass''s own camera rule), every key''s clamp and NaN default, Wanted''s off-at-zero, the quarter extent, the reach in window pixels, the exact disc tap table at 16/24/48, the tap weight and reach rule, the merge''s amounts and NaN guards, the decline/outcome wordings and the stand-down''s counting rule, the skip decomposition, and every default key'
    Sources     = @('dof_rules.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
