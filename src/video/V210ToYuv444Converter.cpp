#include "V210ToYuv444Converter.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace
{
    std::uint32_t readWord(const std::uint8_t* source)
    {
        std::uint32_t word = 0;
        std::memcpy(&word, source, sizeof(word));
        return word;
    }

    std::uint16_t expand10To16(std::uint32_t value)
    {
        return static_cast<std::uint16_t>((value & 0x03FFu) << 6);
    }

    std::uint16_t average(
        std::uint16_t left,
        std::uint16_t right)
    {
        return static_cast<std::uint16_t>(
            (static_cast<std::uint32_t>(left) +
                static_cast<std::uint32_t>(right) +
                1u) >>
            1);
    }
}

bool V210ToYuv444Converter::convert(
    const std::uint8_t* source,
    int rowBytes,
    int width,
    int height,
    Yuv444Frame& destination) const
{
    if (source == nullptr ||
        width <= 0 ||
        height <= 0 ||
        rowBytes <= 0 ||
        (width % 2) != 0)
    {
        return false;
    }

    const int requiredRowBytes =
        ((width + 5) / 6) * 16;

    if (rowBytes < requiredRowBytes)
        return false;

    destination.resize(width, height);

    destination.sampleClockHz =
        13'500'000.0;

    for (int line = 0; line < height; ++line)
    {
        const auto* src =
            source +
            static_cast<std::size_t>(line) *
            static_cast<std::size_t>(rowBytes);

        const std::size_t lineOffset =
            static_cast<std::size_t>(line) *
            static_cast<std::size_t>(width);

        auto* dstY = destination.y.data() + lineOffset;
        auto* dstU = destination.u.data() + lineOffset;
        auto* dstV = destination.v.data() + lineOffset;

        int x = 0;

        /*
         * One v210 group contains six pixels in four 32-bit words:
         *
         * word 0: U0 Y0 V0
         * word 1: Y1 U2 Y2
         * word 2: V2 Y3 U4
         * word 3: Y4 V4 Y5
         */
        while (x + 5 < width)
        {
            const std::uint32_t word0 = readWord(src + 0);
            const std::uint32_t word1 = readWord(src + 4);
            const std::uint32_t word2 = readWord(src + 8);
            const std::uint32_t word3 = readWord(src + 12);

            const std::uint16_t u0 =
                expand10To16(word0);

            const std::uint16_t y0 =
                expand10To16(word0 >> 10);

            const std::uint16_t v0 =
                expand10To16(word0 >> 20);

            const std::uint16_t y1 =
                expand10To16(word1);

            const std::uint16_t u2 =
                expand10To16(word1 >> 10);

            const std::uint16_t y2 =
                expand10To16(word1 >> 20);

            const std::uint16_t v2 =
                expand10To16(word2);

            const std::uint16_t y3 =
                expand10To16(word2 >> 10);

            const std::uint16_t u4 =
                expand10To16(word2 >> 20);

            const std::uint16_t y4 =
                expand10To16(word3);

            const std::uint16_t v4 =
                expand10To16(word3 >> 10);

            const std::uint16_t y5 =
                expand10To16(word3 >> 20);

            dstY[x + 0] = y0;
            dstY[x + 1] = y1;
            dstY[x + 2] = y2;
            dstY[x + 3] = y3;
            dstY[x + 4] = y4;
            dstY[x + 5] = y5;

            // Store the original 4:2:2 chroma samples at even pixels.
            dstU[x + 0] = u0;
            dstV[x + 0] = v0;

            dstU[x + 2] = u2;
            dstV[x + 2] = v2;

            dstU[x + 4] = u4;
            dstV[x + 4] = v4;

            src += 16;
            x += 6;
        }

        /*
         * PAL D1 is 720 pixels wide and therefore consists entirely of
         * complete six-pixel v210 groups. Keep this guard so unsupported
         * widths fail explicitly rather than producing a partial frame.
         */
        if (x != width)
            return false;

        // Reconstruct 4:4:4 chroma by linear interpolation.
        for (int pixel = 1; pixel < width - 1; pixel += 2)
        {
            dstU[pixel] =
                average(dstU[pixel - 1], dstU[pixel + 1]);

            dstV[pixel] =
                average(dstV[pixel - 1], dstV[pixel + 1]);
        }

        // There is no following chroma sample at the right-hand edge.
        dstU[width - 1] = dstU[width - 2];
        dstV[width - 1] = dstV[width - 2];
    }

    return true;
}