#pragma once

enum class ColorBar
{
    White,
    Yellow,
    Cyan,
    Green,
    Magenta,
    Red,
    Blue,
    Black
};

constexpr const char* colorBarShortName(
    ColorBar colorBar)
{
    switch (colorBar)
    {
    case ColorBar::White:   return "W";
    case ColorBar::Yellow:  return "Yl";
    case ColorBar::Cyan:    return "Cy";
    case ColorBar::Green:   return "G";
    case ColorBar::Magenta: return "Mg";
    case ColorBar::Red:     return "R";
    case ColorBar::Blue:    return "B";
    case ColorBar::Black:   return "Bk";
    }

    return "";
}

enum class ColorBarLevel
{
    Percent75,
    Percent100
};

struct Rgb
{
    double r;
    double g;
    double b;
};

constexpr Rgb colorBar(
    ColorBar bar,
    ColorBarLevel level)
{
    const double amplitude =
        level == ColorBarLevel::Percent75
        ? 0.75
        : 1.0;

    switch (bar)
    {
    case ColorBar::White:
        return { amplitude, amplitude, amplitude };

    case ColorBar::Yellow:
        return { amplitude, amplitude, 0.0 };

    case ColorBar::Cyan:
        return { 0.0, amplitude, amplitude };

    case ColorBar::Green:
        return { 0.0, amplitude, 0.0 };

    case ColorBar::Magenta:
        return { amplitude, 0.0, amplitude };

    case ColorBar::Red:
        return { amplitude, 0.0, 0.0 };

    case ColorBar::Blue:
        return { 0.0, 0.0, amplitude };

    case ColorBar::Black:
        return { 0.0, 0.0, 0.0 };
    }

    return {};
}