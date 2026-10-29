@{
    Class       = 'cpu'
    Note        = 'Plate probe output accounting (ext/hudless/PlateProbeAccounting.h): window size measured from the stream (drops and bunching lower the percentage), first-in-first-out lag, multiplier change, restart, frame-number jump, threads, QPC merge with hold-back'
    Sources     = @('plate_probe_accounting.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
