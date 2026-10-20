@{
    Class       = 'cpu'
    Note        = '[REL] Tri-state of ext::dx11bridge::BeforeReleaseFeature: queue drain classification (fence outcome -> drained/absent/device lost/failed), release classification (drained only when every required queue drained and the handle is quiet), sleep skip decision (kill switch, shutdown)'
    Sources     = @('release_drain_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('UNDEBUG')
}
