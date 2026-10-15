#pragma once

#include <dxgi1_6.h>

enum class ColorTransfer
{
    Unknown,
    Linear,
    SRGB,
    PQ,
    HLG
};

enum class ColorPrimaries
{
    Unknown,
    Rec709,
    Rec2020
};

enum class ColorRange
{
    Unknown,
    Full
};

enum class ColorModel
{
    Unknown,
    RGB,
    YCbCr
};

struct OutputColorSpace
{
    DXGI_COLOR_SPACE_TYPE dxgiColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    ColorTransfer transfer = ColorTransfer::Unknown;
    ColorPrimaries primaries = ColorPrimaries::Unknown;
    ColorRange range = ColorRange::Unknown;
    ColorModel model = ColorModel::Unknown;
    bool valid = false;
};

struct State
{
    OutputColorSpace outputColorSpace;
    bool hdrOutputActive = false;

    static State& Instance()
    {
        static State value;
        return value;
    }
};
