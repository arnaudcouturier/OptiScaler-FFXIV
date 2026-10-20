@{
    Class       = 'cpu'
    Note        = '[REL] The narrow lock over the DX11 NGX feature table (ext/dx11bridge/NgxFeatureTable.h): reference-stable storage, detach under the lock and destroy outside it, locked iteration, in-flight/closed handle registry (bounded wait, self-inside, reopen, forget), many-thread stress with a deadlock watchdog, lock order against a fake NR lock'
    Sources     = @('ngx_feature_table_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('UNDEBUG')
}
