#pragma once

#include "VideoStandard.h"
#include "ColorBars.h"

struct ColorMatrix
{
    double kr;
    double kb;
};

struct CbCr
{
    double cb;
    double cr;
};

constexpr ColorMatrix colorMatrix(
    VideoColorStandard standard)
{
    switch (standard)
    {
    case VideoColorStandard::Rec601_625:
    case VideoColorStandard::Rec601_525:
        return
        {
            0.299,
            0.114
        };

    case VideoColorStandard::Rec709:
        return
        {
            0.2126,
            0.0722
        };

    case VideoColorStandard::Rec2020:
        return
        {
            0.2627,
            0.0593
        };
    }

    return
    {
        0.299,
        0.114
    };
}

constexpr CbCr rgbToCbCr(
    const Rgb& rgb,
    VideoColorStandard standard)
{
    const ColorMatrix matrix =
        colorMatrix(standard);

    const double kg =
        1.0 -
        matrix.kr -
        matrix.kb;

    const double y =
        matrix.kr * rgb.r +
        kg * rgb.g +
        matrix.kb * rgb.b;

    const double cb =
        (rgb.b - y) /
        (2.0 * (1.0 - matrix.kb));

    const double cr =
        (rgb.r - y) /
        (2.0 * (1.0 - matrix.kr));

    return
    {
        cb,
        cr
    };
}