@{
    Class       = 'cpu'
    Note        = 'tools\test.ps1 itself: class local is not run without -Local, Requires (unset variable, missing path, missing product build) gives NOT-RUN and never a failure, {env:NAME} expands in Args/Sources/IncludeDirs, -GpuBlocked, the summary counts and the exit code, manual unchanged (fixture recipes generated at run time, -TestsDir)'
    Runner      = 'run_test_runner_local_class.ps1'
    Sources     = @()
}
