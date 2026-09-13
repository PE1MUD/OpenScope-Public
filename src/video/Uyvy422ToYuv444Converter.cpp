#include "Uyvy422ToYuv444Converter.h"

#include <cstddef>
#include <cstdlib>

bool Uyvy422ToYuv444Converter::convert(
    const std::uint8_t* source,
    int sourceRowBytes,
    int width,
    int height,
    Yuv444Frame& destination) const
{
    if (source == nullptr ||
        width <= 0 ||
        height <= 0 ||
        (width & 1) != 0 ||
        sourceRowBytes < width * 2)
    {
        return false;
    }

    if (destination.width != width ||
        destination.height != height)
    {
        destination.resize(width, height);
    }

    destination.sampleClockHz =
        13'500'000.0;

    for (int line = 0; line < height; ++line)
    {
        const auto* src =
            source +
            static_cast<std::ptrdiff_t>(line) * sourceRowBytes;

        const std::size_t destinationOffset =
            static_cast<std::size_t>(line) *
            static_cast<std::size_t>(width);

        auto* dstY = destination.y.data() + destinationOffset;
        auto* dstU = destination.u.data() + destinationOffset;
        auto* dstV = destination.v.data() + destinationOffset;

        for (int x = 0; x < width; x += 2)
        {
            // DeckLink bmdFormat8BitYUV:
            // U0 Y0 V0 Y1
            const std::uint8_t u0 = src[0];
            const std::uint8_t y0 = src[1];
            const std::uint8_t v0 = src[2];
            const std::uint8_t y1 = src[3];

            // The next chroma sample is co-sited with Y[x + 2].
            // At the right edge there is no next sample, so hold
            // the final chroma value instead of reading past the line.
            std::uint8_t u2 = u0;
            std::uint8_t v2 = v0;

            if (x + 2 < width)
            {
                u2 = src[4];
                v2 = src[6];
            }

            dstY[x]     = expand8To16(y0);
            dstY[x + 1] = expand8To16(y1);

            // Even pixel: original co-sited chroma sample.
            dstU[x] = expand8To16(u0);
            dstV[x] = expand8To16(v0);

            // Odd pixel: halfway between adjacent chroma samples.
            dstU[x + 1] = interpolate(u0, u2);
            dstV[x + 1] = interpolate(v0, v2);
            src += 4;
        }
    }

    return true;
}