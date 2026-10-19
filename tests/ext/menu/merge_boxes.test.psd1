@{
    Class       = 'cpu'
    Note        = 'The real MergeBoxes template run offline with fake cards: upstream boxes copied unchanged in the layout order, each fork box carries its own card and runs it with its registration index and the menu scale, same menu for every registration order, built once, untouched upstream array while no card is registered'
    Sources     = @('merge_boxes.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
