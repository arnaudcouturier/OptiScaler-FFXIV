@{
    Class       = 'cpu'
    Note        = 'NR runtime scan (NrRuntimeScan.h) on temporary folders: legacy search keeps the FindFilePath order but never enters NR.* folders, folder scan, file version, SHA-256 (streamed equals one-shot)'
    Sources     = @('nr_runtime_scan.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
