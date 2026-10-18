@{
    Class       = 'cpu'
    Note        = 'CharaView preview NR production entry (NR on the super-resolution output), pure parts: the model call (output-size colour, render-size guides built by GuidesOf from the same inputs as the super-resolution evaluate, the upscaler depth convention and motion scale), the recipe without the zero-motion pass, the stream shape per mode (a mode change rebuilds, a bring-up consumer never gets a production result), the checks on the copied images'
    Sources     = @('chara_nr_production.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
