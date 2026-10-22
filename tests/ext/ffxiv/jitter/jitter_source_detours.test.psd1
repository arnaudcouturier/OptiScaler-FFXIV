@{
    Class       = 'cpu'
    Note        = 'Jitter source fix, the two detour bodies on a fake generator and getter: only the world camera object is normalised (a foreign object and an unknown world camera pass through), argument order, record before the division, caller and switch tests, plan gates, phase count only when unbounded, camera getter learning and torn reads'
    Sources     = @('jitter_source_detours.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
