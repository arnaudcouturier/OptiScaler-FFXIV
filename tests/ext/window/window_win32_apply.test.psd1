@{
    Class       = 'cpu'
    Note        = 'Window mode: the border fix on a real hidden window (client edge cleared, rectangle and other styles kept, WM_SIZE delivered inside the call)'
    Sources     = @('window_win32_apply.cpp', '{repo}/OptiScaler/ext/window/WindowWin32.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('user32.lib')
}
