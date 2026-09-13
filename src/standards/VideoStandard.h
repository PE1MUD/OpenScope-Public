#pragma once

enum class VideoColorStandard
{
    Rec601_625,
    Rec601_525,
    Rec709,
    Rec2020
};

enum class VideoRange
{
    Legal,
    Full
};

struct VideoStandard
{
    VideoColorStandard colorStandard;
    VideoRange range;
    int sampleWidth;
    int sampleHeight;
    int outputWidth;
    int outputHeight;
    double sampleClockHz;
    double pixelAspectRatio;
    double safeWidthScale;
    double safeHeightScale;

    static constexpr VideoStandard pal625()
    {
        return
        {
            VideoColorStandard::Rec601_625,
            VideoRange::Legal,
            720,
            576,
            720,
            576,
            13'500'000.0,
            16.0 / 15.0,
            0.80,
            0.90
        };
    }

    static constexpr VideoStandard ntsc525()
    {
        return
        {
            VideoColorStandard::Rec601_525,
            VideoRange::Legal,
            720,
            486,
            720,
            486,
            13'500'000.0,
            1.0,
            1.0,
            1.0
        };
    }
};

struct YuvToRgbCoefficients
{
    double rCr;
    double gCb;
    double gCr;
    double bCb;
};


struct YuvLevels
{
    int yBlack;
    int yWhite;

    int chromaNeutral;
    int chromaMin;
    int chromaMax;

    int chromaNegativeExcursion;
    int chromaPositiveExcursion;
};

struct AnalogVideoLevels
{
    double syncTipVolts;
    double blackVolts;
    double whiteVolts;
    double graticuleMaxVolts;
    double chromaPeakVolts;
};

struct PalChromaCoefficients
{
    double cbToU;
    double crToV;
};

constexpr PalChromaCoefficients palChromaCoefficients(
    VideoColorStandard standard)
{
    switch (standard)
    {
    case VideoColorStandard::Rec601_625:
        return
        {
            0.8736,
            1.2294
        };

    default:
        return
        {
            1.0,
            1.0
        };
    }
}

constexpr YuvToRgbCoefficients yuvToRgbCoefficients(
    VideoColorStandard standard)
{
    switch (standard)
    {
    case VideoColorStandard::Rec601_625:
    case VideoColorStandard::Rec601_525:
        return
        {
            1.402,
            -0.344136,
            -0.714136,
            1.772
        };

    case VideoColorStandard::Rec709:
        return
        {
            1.5748,
            -0.187324,
            -0.468124,
            1.8556
        };

    case VideoColorStandard::Rec2020:
        return
        {
            1.4746,
            -0.164553,
            -0.571353,
            1.8814
        };
    }

    return
    {
        1.402,
        -0.344136,
        -0.714136,
        1.772
    };
}

constexpr YuvLevels levels(
    VideoStandard standard)
{
    if (standard.range == VideoRange::Legal)
    {
        return
        {
            64,   // Y black
            940,  // Y white

            512,  // Cb/Cr neutral
            64,   // Cb/Cr nominal minimum
            960,   // Cb/Cr nominal maximum

            448,    // max neg chroma
            448     // max pos chroma
        };
    }

    return
    {
        0,
        1023,

        512,
        0,
        1023,

        512,
        511 
    };
}

constexpr AnalogVideoLevels analogLevels(
    VideoColorStandard standard)
{
    switch (standard)
    {
    case VideoColorStandard::Rec601_625:
        return
        {
            0.0,   // sync tip
            0.3,   // black
            1.0,   // white
            1.3,   // graticule max
            0.35   // nominal chroma component peak
        };

    default:
        return
        {
            0.0,
            0.0,
            1.0,
            1.3
        };
    }
}