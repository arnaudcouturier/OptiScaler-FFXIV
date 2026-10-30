@{
    Class    = 'cpu'
    Note     = 'Concurrent LoadLibrary skip windows: six threads open and close two nested windows each while two threads that opened none check that nothing leaks into them, under a 90 s deadline; built with the debug CRT (/MDd) so the map operations run under the checked containers'
    Runner   = 'run_skip_window_race.ps1'
    Sources  = @('skip_window_race.cpp')
}
