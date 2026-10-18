@{
    Class       = 'cpu'
    Note        = 'Window mode: the Win32 reads on a real hidden window (styles, rects, client edge arithmetic, owner thread)'
    Sources     = @('window_win32_read.cpp', '{repo}/OptiScaler/ext/window/WindowWin32.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Libs        = @('user32.lib')
}
