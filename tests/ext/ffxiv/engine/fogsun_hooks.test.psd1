@{
    Class       = 'cpu'
    Note        = 'Fog sun accessor (FogSun.h), the one link between the god rays fog-post capture and the shadow mapping night curve: the published sun and its publish count (a NaN is neither stored nor counted), the wanted flag off by default, the arming hook called once per change and never per call, in both config-load orders (the consumer first and the capture registering after, and the other way round)'
    Sources     = @('fogsun_hooks.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Std         = 'c++20'
}
