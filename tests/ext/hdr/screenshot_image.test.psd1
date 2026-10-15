@{
    Class       = 'cpu'
    Note        = 'Production PNG writer, SDR/HDR bit depth and colour metadata, CRC validation'
    Sources     = @('screenshot_image.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
