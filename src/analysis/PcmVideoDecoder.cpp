#include "PcmVideoDecoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <fstream>
#include <iomanip>

namespace
{
    constexpr int kHorizontalCells = 140;
    constexpr int kSyncCells = 4;
    constexpr int kPayloadBits = 128;
    constexpr int kWordsPerBlock = 8;
    constexpr int kWordBits = 14;

    constexpr int kPalPcmLinesPerField = 294;
    constexpr int kPalPcmLinesPerFrame = 588;
    constexpr int kCapturedFieldRows = 288;

    // Blackmagic PAL 720x576 exposes a 576-row buffer, but only 575 PCM-usable
    // raster H positions: field 1 contributes 288 rows and field 2 contributes
    // 287. The final buffer row is not a second 288th H for field 2 and must
    // never be counted/tested as PCM.
    constexpr int kPcmUsableRasterRows = 575;
    constexpr int kPcmUsableField1Rows = 288;
    constexpr int kPcmUsableField2Rows = 287;

    // With conventional 576i active-picture capture, the IEC PCM audio area
    // starts before row 0.  Delta 8 deliberately uses the mapping exercised by
    // the Blackmagic loop-through test pattern: captured field row 0 is PCM
    // audio line 17, so lines 1..16 are known erasures.
    constexpr int kFirstCapturedPcmLineZeroBased = 16;
    constexpr int kCapturedPcmLinesPerField =
        kPalPcmLinesPerField - kFirstCapturedPcmLineZeroBased; // 278

    constexpr int kInterleaveD = 16;
    constexpr int kMaxInterleaveDelay = 7 * kInterleaveD; // 112 lines

    // Temporary Delta59 Sony-reference diagnostic. Keep the dump bounded so
    // it is safe to leave enabled while testing the borrowed PCM-701ES.
    constexpr int kSonyDebugMaxLines = 768;
    std::ofstream gSonyDebugDump;
    int gSonyDebugLines = 0;

    // Delta65 transient diagnostic: frame cadence + detailed nominal/final CRC failures.
    // This stays separate from the bounded raw-word dump so a several-minute
    // run can expose slow (~1 Hz) geometry/phase disturbances without making
    // an enormous per-line log.
    std::ofstream gSonyTrackingDump;

    std::uint16_t payloadCrcWord(const std::uint8_t* payload128)
    {
        std::uint16_t value = 0;
        for (int bit = 112; bit < 128; ++bit)
        {
            value = static_cast<std::uint16_t>(
                (value << 1) | (payload128[bit] & 1u));
        }
        return value;
    }

    bool checkIecCrc(const std::uint8_t* payload128)
    {
        // IEC 60841 CRC: G(x) = x^16 + x^12 + x^5 + 1.
        // The first 16 payload coefficients are complemented before division.
        std::array<std::uint8_t, 128> work{};
        for (int i = 0; i < 128; ++i)
        {
            work[static_cast<std::size_t>(i)] = payload128[i] & 1u;
        }

        for (int i = 0; i < 16; ++i)
        {
            work[static_cast<std::size_t>(i)] ^= 1u;
        }

        constexpr std::array<int, 4> taps{0, 4, 11, 16};
        for (int i = 0; i < 112; ++i)
        {
            if (work[static_cast<std::size_t>(i)] == 0u)
            {
                continue;
            }

            for (const int tap : taps)
            {
                work[static_cast<std::size_t>(i + tap)] ^= 1u;
            }
        }

        for (int i = 112; i < 128; ++i)
        {
            if (work[static_cast<std::size_t>(i)] != 0u)
            {
                return false;
            }
        }

        return true;
    }

    std::int16_t signed16(std::uint16_t value)
    {
        return static_cast<std::int16_t>(value);
    }
}

void PcmVideoDecoder::reset()
{
    geometry_ = {};
    lockFrames_ = 0;
    badFrames_ = 0;
    searchCooldownFrames_ = 0;

    firstCaptureGeneration_ = 0;
    lastProcessedGroup_ = -1;
    physicalBlocks_.clear();

    reconstructedGroups_ = 0;
    pCorrectedGroups_ = 0;
    lsbPackMissingGroups_ = 0;
    hardUncorrectableGroups_ = 0;
    cleanPChecks_ = 0;
    cleanPPasses_ = 0;
    cleanQChecks_ = 0;
    cleanQPasses_ = 0;
    controlValid_ = false;
    controlMode16_ = false;
    controlPreEmphasis_ = false;
    headerlessModeScore_ = 0;
    headerlessModeKnown_ = false;
    headerlessMode16_ = false;

    recentLeft_.clear();
    recentRight_.clear();
    currentAudioStereo_.clear();
}

std::uint16_t PcmVideoDecoder::thresholdForGeometry(
    const std::uint16_t* line,
    int width,
    const Geometry& geometry,
    bool& contrastOk)
{
    if (!geometry.valid || geometry.bitPeriod <= 0.0 || line == nullptr || width <= 0)
    {
        contrastOk = false;
        return 0;
    }

    // Delta66: derive the payload slicer only from cells whose transmitted
    // level is known by the Sony/EIAJ line format.  Do NOT estimate the two
    // data rails from payload percentiles: a sparse payload (digital mute or a
    // hard tone start/stop) can contain only a handful of '1' bits and pulled
    // the old percentile slicer down by ~10..15 luma codes, causing perfectly
    // good lines to fail CRC.
    //
    // Normal case: use the visible 1010 run-in plus the known DATA-0 separator
    // immediately before the seven peak-white reference cells.  This keeps the
    // threshold tied to the physical DATA-0/DATA-1 eye, independent of audio
    // content.
    //
    // Important: OpenScope deliberately allows the left run-in to be partly or
    // completely clipped.  If no DATA-1 run-in cell is visible, preserve that
    // behaviour by deriving DATA-1 from DATA-0 and peak white using the nominal
    // Sony level geometry (0.4 / 0.7 / 1.0 V).  Geometry acquisition itself is
    // unchanged and can still lock from the payload CRC/end marker.
    constexpr int kMinDataContrast16 = 28 * 257;

    std::vector<std::uint16_t> dataLow;
    std::vector<std::uint16_t> dataHigh;
    std::vector<std::uint16_t> peakWhite;
    dataLow.reserve(9);
    dataHigh.reserve(6);
    peakWhite.reserve(21);

    const auto addCell = [&](int cell, std::vector<std::uint16_t>& destination)
    {
        constexpr std::array<double, 3> offsets{-0.15, 0.0, 0.15};
        for (const double offset : offsets)
        {
            const double x = geometry.syncStart +
                (static_cast<double>(cell) + 0.5 + offset) * geometry.bitPeriod;
            const int ix = static_cast<int>(std::lround(x));
            if (ix >= 0 && ix < width)
            {
                destination.push_back(line[ix]);
            }
        }
    };

    // 1010 start marker.  Only cells that are actually visible contribute.
    addCell(0, dataHigh);
    addCell(1, dataLow);
    addCell(2, dataHigh);
    addCell(3, dataLow);

    // Known DATA-0 separator and peak-white end marker are normally visible
    // even when the beginning of the line is clipped.
    addCell(132, dataLow);
    for (int cell = 133; cell < kHorizontalCells; ++cell)
    {
        addCell(cell, peakWhite);
    }

    const auto median = [](std::vector<std::uint16_t>& values) -> std::uint16_t
    {
        if (values.empty())
        {
            return 0;
        }
        auto mid = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
        std::nth_element(values.begin(), mid, values.end());
        return *mid;
    };

    // Preferred path: direct DATA-0 / DATA-1 eye measurement from known cells.
    if (!dataLow.empty() && !dataHigh.empty())
    {
        const std::uint16_t low = median(dataLow);
        const std::uint16_t high = median(dataHigh);
        contrastOk = static_cast<int>(high) - static_cast<int>(low) >= kMinDataContrast16;
        if (contrastOk)
        {
            return static_cast<std::uint16_t>(
                (static_cast<unsigned int>(low) + static_cast<unsigned int>(high)) / 2u);
        }
    }

    // Clipped-start fallback.  Nominal Sony/EIAJ pseudo-video levels are
    // approximately DATA0=0.4 V, DATA1=0.7 V, REF=1.0 V, hence DATA1 lies
    // halfway between DATA0 and REF and the slicer lies one quarter of that
    // span above DATA0.  This retains the existing off-screen-start tolerance
    // without letting payload bit density influence the threshold.
    if (!dataLow.empty() && !peakWhite.empty())
    {
        const std::uint16_t low = median(dataLow);
        const std::uint16_t white = median(peakWhite);
        const int span = static_cast<int>(white) - static_cast<int>(low);
        if (span >= 2 * kMinDataContrast16)
        {
            contrastOk = true;
            return static_cast<std::uint16_t>(
                static_cast<unsigned int>(low) +
                static_cast<unsigned int>(span) / 4u);
        }
    }

    contrastOk = false;
    return 0;
}

bool PcmVideoDecoder::sampleBit(
    const std::uint16_t* line,
    int width,
    double x,
    std::uint16_t threshold)
{
    const int ix = static_cast<int>(std::lround(x));
    if (ix < 0 || ix >= width)
    {
        return false;
    }

    return line[ix] >= threshold;
}

int PcmVideoDecoder::structureScore(
    const std::uint16_t* line,
    int width,
    const Geometry& geometry,
    std::uint16_t threshold)
{
    if (!geometry.valid)
    {
        return 0;
    }

    // Do not require a complete 1010 at the left edge.  A real active-picture
    // capture can clip the first cell(s).  Score whatever part of the start
    // marker is actually visible, plus the much more useful end marker:
    // separator 0 followed by seven white reference cells.
    constexpr std::array<bool, 4> syncPattern{true, false, true, false};

    int score = 0;
    int visible = 0;

    for (int i = 0; i < kSyncCells; ++i)
    {
        const double x =
            geometry.syncStart +
            (static_cast<double>(i) + 0.5) * geometry.bitPeriod;

        if (x < 0.0 || x >= static_cast<double>(width))
        {
            continue;
        }

        ++visible;
        if (sampleBit(line, width, x, threshold) ==
            syncPattern[static_cast<std::size_t>(i)])
        {
            ++score;
        }
    }

    // Cell 132 is the black separator. Cells 133..139 are the white
    // reference. These are relative to the same 140-cell geometry and give us
    // a second anchor even when the beginning of 1010 is clipped.
    for (int cell = 132; cell < kHorizontalCells; ++cell)
    {
        const double x =
            geometry.syncStart +
            (static_cast<double>(cell) + 0.5) * geometry.bitPeriod;

        if (x < 0.0 || x >= static_cast<double>(width))
        {
            continue;
        }

        ++visible;
        const bool expected = cell != 132;
        if (sampleBit(line, width, x, threshold) == expected)
        {
            // Give the end marker a little extra weight: unlike audio data it
            // is fixed on every PCM line.
            score += 2;
        }
    }

    // No visible fixed structure means this candidate has no useful support.
    return visible > 0 ? score : 0;
}

bool PcmVideoDecoder::decodePayload(
    const std::uint16_t* line,
    int width,
    const Geometry& geometry,
    std::uint16_t threshold,
    std::uint8_t* payload128)
{
    if (!geometry.valid)
    {
        return false;
    }

    // Important: payload sampling is deliberately independent of a perfect
    // 1010 match.  The first sync cells may be clipped by 720-active capture.
    // Candidate geometry is validated by fixed start/end structure and,
    // decisively, the 16-bit IEC CRC over the 128 payload bits.
    for (int bit = 0; bit < kPayloadBits; ++bit)
    {
        const double x =
            geometry.syncStart +
            (static_cast<double>(kSyncCells + bit) + 0.5) *
                geometry.bitPeriod;

        if (x < 0.0 || x >= static_cast<double>(width))
        {
            return false;
        }

        payload128[bit] =
            sampleBit(line, width, x, threshold) ? 1u : 0u;
    }

    return true;
}

bool PcmVideoDecoder::crcValid(
    const std::uint8_t* payload128)
{
    return checkIecCrc(payload128);
}

PcmVideoDecoder::PhysicalBlock PcmVideoDecoder::payloadToWords(
    const std::uint8_t* payload128)
{
    PhysicalBlock block{};

    for (int word = 0; word < kWordsPerBlock; ++word)
    {
        std::uint16_t value = 0;
        for (int bit = 0; bit < kWordBits; ++bit)
        {
            value = static_cast<std::uint16_t>(
                (value << 1) |
                (payload128[word * kWordBits + bit] & 1u));
        }
        block.words[static_cast<std::size_t>(word)] = value;
    }

    return block;
}


std::uint16_t PcmVideoDecoder::gfMulX14(std::uint16_t v)
{
    v &= 0x3FFFu;
    const bool carry = (v & 0x2000u) != 0;
    v = static_cast<std::uint16_t>((v << 1) & 0x3FFFu);
    if (carry)
    {
        v ^= 0x0101u;
    }
    return v;
}

std::uint16_t PcmVideoDecoder::gfMul14(std::uint16_t a, std::uint16_t b)
{
    a &= 0x3FFFu;
    b &= 0x3FFFu;
    std::uint16_t r = 0;
    while (b != 0u)
    {
        if ((b & 1u) != 0u)
        {
            r ^= a;
        }
        b = static_cast<std::uint16_t>(b >> 1);
        a = gfMulX14(a);
    }
    return static_cast<std::uint16_t>(r & 0x3FFFu);
}

std::uint16_t PcmVideoDecoder::gfInv14(std::uint16_t v)
{
    v &= 0x3FFFu;
    if (v == 0u)
    {
        return 0u;
    }

    // In GF(2^14), v^-1 = v^(2^14-2).
    std::uint16_t result = 1u;
    std::uint16_t base = v;
    unsigned int exponent = (1u << 14) - 2u;
    while (exponent != 0u)
    {
        if ((exponent & 1u) != 0u)
        {
            result = gfMul14(result, base);
        }
        exponent >>= 1;
        if (exponent != 0u)
        {
            base = gfMul14(base, base);
        }
    }
    return result;
}

std::uint16_t PcmVideoDecoder::q14(const std::array<std::uint16_t, 6>& words)
{
    std::uint16_t q = 0;
    for (int i = 0; i < 6; ++i)
    {
        std::uint16_t v = static_cast<std::uint16_t>(words[static_cast<std::size_t>(i)] & 0x3FFFu);
        for (int power = 6 - i; power > 0; --power)
        {
            v = gfMulX14(v);
        }
        q ^= v;
    }
    return static_cast<std::uint16_t>(q & 0x3FFFu);
}

bool PcmVideoDecoder::isControlBlock(const PhysicalBlock& block)
{
    // IEC 60841 cueing word is 1100 repeated 14 times (56 bits). Split into
    // 14-bit words this alternates 0x3333 and 0x0CCC.
    return block.words[0] == 0x3333u &&
           block.words[1] == 0x0CCCu &&
           block.words[2] == 0x3333u &&
           block.words[3] == 0x0CCCu;
}

PcmVideoDecoder::Geometry PcmVideoDecoder::searchGeometry(
    const std::vector<std::uint16_t>& y,
    int width,
    int height) const
{
    Geometry best{};
    int bestCrcScore = 0;
    int bestStructureScore = -1;

    // Allow the nominal 1010 start to live partly outside the captured active
    // raster.  This is exactly the case where we may only see 010 or 10.
    for (double period = 5.00; period <= 5.26; period += 0.01)
    {
        for (double start = -22.0; start <= 30.0; start += 0.5)
        {
            Geometry candidate{start, period, true};
            int crcScore = 0;
            int structure = 0;
            int attempts = 0;

            const int usableHeight = std::min(height, kPcmUsableRasterRows);
            for (int row = 0; row < usableHeight && attempts < 24; row += 7)
            {
                const auto* line =
                    y.data() + static_cast<std::size_t>(row) *
                    static_cast<std::size_t>(width);

                bool contrastOk = false;
                const auto threshold = thresholdForGeometry(line, width, candidate, contrastOk);
                if (!contrastOk)
                {
                    continue;
                }

                ++attempts;
                structure += structureScore(
                    line,
                    width,
                    candidate,
                    threshold);

                std::array<std::uint8_t, kPayloadBits> payload{};
                if (!decodePayload(
                        line,
                        width,
                        candidate,
                        threshold,
                        payload.data()))
                {
                    continue;
                }

                if (crcValid(payload.data()))
                {
                    ++crcScore;
                }
            }

            if (crcScore > bestCrcScore ||
                (crcScore == bestCrcScore && structure > bestStructureScore))
            {
                bestCrcScore = crcScore;
                bestStructureScore = structure;
                best = candidate;
            }
        }
    }

    // Two CRC-valid lines are sufficient for a fallback lock candidate. The
    // fixed end marker only chooses between candidates with the same CRC score.
    if (bestCrcScore < 2)
    {
        best.valid = false;
    }

    return best;
}

int PcmVideoDecoder::decodeFrameLines(
    const std::vector<std::uint16_t>& y,
    int width,
    int height,
    const Geometry& geometry,
    std::int64_t frameBaseLine,
    int& testedLines,
    int& controlValidLines,
    int& controlTestedLines,
    std::array<int, 2>& validAudioByField,
    std::array<int, 2>& testedAudioByField,
    std::array<int, 2>& validControlByField,
    std::array<int, 2>& testedControlByField)
{
    int valid = 0;
    int frameCrcFails = 0;
    int frameContrastFails = 0;
    int frameDecodeFails = 0;
    testedLines = 0;
    controlValidLines = 0;
    controlTestedLines = 0;
    validAudioByField = {0, 0};
    testedAudioByField = {0, 0};
    validControlByField = {0, 0};
    testedControlByField = {0, 0};

    std::array<int, 2> controlFieldRow{-1, -1};

    if (!gSonyTrackingDump.is_open())
    {
        gSonyTrackingDump.open("pcm_sony_tracking_debug.txt", std::ios::out | std::ios::trunc);
        if (gSonyTrackingDump)
        {
            gSonyTrackingDump
                << "# OpenScope Delta65 Sony PCM transient CRC diagnostic\n"
                << "# FRAME: geometry and per-frame CRC/contrast/decode counts.\n"
                << "# NOMINAL_CRCFAIL: centre-slice failed before Delta63 phase recovery.\n"
                << "# CRCFAIL: final unrecovered failure with levels, weak bits and sweep.\n"
                << "# PHASERECOVER: nominal CRC failed but a retry phase passed full IEC CRC.\n"
                << "# ones/transitions describe the 128-bit nominal payload pattern.\n"
                << "# phase is in bit cells; 0 = current cell centre.\n";
        }
    }

    const long long debugFrame = static_cast<long long>(frameBaseLine / kPalPcmLinesPerFrame);
    if (gSonyTrackingDump)
    {
        gSonyTrackingDump << std::fixed << std::setprecision(6)
            << "FRAME_BEGIN frame=" << debugFrame
            << " sync=" << geometry.syncStart
            << " period=" << geometry.bitPeriod << '\n';
    }

    const auto debugCellLevel8 = [&](
        const std::uint16_t* line,
        std::initializer_list<int> cells) -> int
    {
        std::vector<std::uint16_t> values;
        values.reserve(cells.size());
        for (const int cell : cells)
        {
            const double x = geometry.syncStart +
                (static_cast<double>(cell) + 0.5) * geometry.bitPeriod;
            const int ix = static_cast<int>(std::lround(x));
            if (ix >= 0 && ix < width)
            {
                values.push_back(line[ix]);
            }
        }
        if (values.empty())
        {
            return -1;
        }
        auto mid = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
        std::nth_element(values.begin(), mid, values.end());
        return static_cast<int>(std::lround(static_cast<double>(*mid) / 257.0));
    };

    const auto dumpSonyLine = [&](
        const std::uint16_t* line,
        int row,
        int field,
        int fieldRow,
        int pcmLine,
        std::uint16_t threshold,
        const PhysicalBlock& block,
        const std::uint8_t* payload,
        bool control)
    {
        if (gSonyDebugLines >= kSonyDebugMaxLines)
        {
            return;
        }
        if (!gSonyDebugDump.is_open())
        {
            gSonyDebugDump.open(
                "pcm_sony16_debug.txt",
                std::ios::out | std::ios::trunc);
            if (gSonyDebugDump)
            {
                gSonyDebugDump
                    << "# OpenScope Delta59 Sony PCM raw-line diagnostic\n"
                    << "# Y levels are BT.601 8-bit equivalents sampled at cell centres.\n"
                    << "# DATA0 uses sync-low/separator cells, DATA1 sync-high cells, REF peak-white cells.\n"
                    << "# W0..W7 are the eight 14-bit transmitted words; CRC is transmitted CRC16.\n";
            }
        }
        if (!gSonyDebugDump)
        {
            return;
        }

        const int data0 = debugCellLevel8(line, {1, 3, 132});
        const int data1 = debugCellLevel8(line, {0, 2});
        const int ref = debugCellLevel8(line, {133, 134, 135, 136, 137, 138, 139});
        const int threshold8 = static_cast<int>(std::lround(static_cast<double>(threshold) / 257.0));

        gSonyDebugDump
            << (control ? "CTRL" : "AUDIO")
            << " row=" << row
            << " f=" << (field + 1)
            << " fr=" << fieldRow
            << " H=" << (pcmLine >= 0 ? pcmLine + 1 : 0)
            << " data0=" << data0
            << " data1=" << data1
            << " ref=" << ref
            << " slice=" << threshold8;

        gSonyDebugDump << std::hex << std::uppercase << std::setfill('0');
        for (int i = 0; i < 8; ++i)
        {
            gSonyDebugDump << " W" << i << "=" << std::setw(4)
                << static_cast<unsigned int>(block.words[static_cast<std::size_t>(i)] & 0x3FFFu);
        }
        gSonyDebugDump << " CRC=" << std::setw(4)
            << static_cast<unsigned int>(payloadCrcWord(payload));
        gSonyDebugDump << std::dec << std::nouppercase << std::setfill(' ') << '\n';
        gSonyDebugDump.flush();
        ++gSonyDebugLines;
    };

    const auto logCrcFailure = [&](
        const std::uint16_t* line,
        int row,
        int field,
        int fieldRow,
        int pcmLine,
        std::uint16_t threshold,
        const std::uint8_t* payload)
    {
        if (!gSonyTrackingDump)
        {
            return;
        }

        const int data0 = debugCellLevel8(line, {1, 3, 132});
        const int data1 = debugCellLevel8(line, {0, 2});
        const int ref = debugCellLevel8(line, {133, 134, 135, 136, 137, 138, 139});
        const int threshold8 = static_cast<int>(std::lround(static_cast<double>(threshold) / 257.0));

        struct WeakBit { int bit = -1; int margin = 99999; int y8 = -1; double x = 0.0; };
        std::array<WeakBit, 6> weak{};
        for (auto& w : weak) w.margin = 99999;

        for (int bit = 0; bit < kPayloadBits; ++bit)
        {
            const double x = geometry.syncStart +
                (static_cast<double>(kSyncCells + bit) + 0.5) * geometry.bitPeriod;
            const int ix = static_cast<int>(std::lround(x));
            if (ix < 0 || ix >= width) continue;
            const int y8 = static_cast<int>(std::lround(static_cast<double>(line[ix]) / 257.0));
            const int margin = std::abs(y8 - threshold8);
            WeakBit candidate{bit, margin, y8, x};
            for (std::size_t k = 0; k < weak.size(); ++k)
            {
                if (candidate.margin < weak[k].margin)
                {
                    for (std::size_t j = weak.size() - 1; j > k; --j) weak[j] = weak[j - 1];
                    weak[k] = candidate;
                    break;
                }
            }
        }

        // Re-slice this same line at nearby horizontal phases. If CRC comes
        // back at a small offset, the failure is timing/geometry rather than
        // an amplitude/slicer-threshold problem.
        bool phaseRecovered = false;
        double bestPhase = 99.0;
        int recoveredCount = 0;
        for (int step = -16; step <= 16; ++step)
        {
            const double phase = static_cast<double>(step) * 0.025; // -0.400 .. +0.400 cell
            std::array<std::uint8_t, kPayloadBits> shifted{};
            bool inRange = true;
            for (int bit = 0; bit < kPayloadBits; ++bit)
            {
                const double x = geometry.syncStart +
                    (static_cast<double>(kSyncCells + bit) + 0.5 + phase) * geometry.bitPeriod;
                const int ix = static_cast<int>(std::lround(x));
                if (ix < 0 || ix >= width) { inRange = false; break; }
                shifted[static_cast<std::size_t>(bit)] = line[ix] >= threshold ? 1u : 0u;
            }
            if (inRange && crcValid(shifted.data()))
            {
                ++recoveredCount;
                if (!phaseRecovered || std::abs(phase) < std::abs(bestPhase))
                {
                    phaseRecovered = true;
                    bestPhase = phase;
                }
            }
        }

        // Also sweep threshold at the current phase. This directly separates
        // level errors from phase errors. Units are BT.601 8-bit Y codes.
        bool thresholdRecovered = false;
        int bestThresholdDelta = 999;
        int thresholdRecoveredCount = 0;
        for (int d = -24; d <= 24; ++d)
        {
            const int t8 = threshold8 + d;
            if (t8 <= 0 || t8 >= 255) continue;
            const std::uint16_t t16 = static_cast<std::uint16_t>(t8 * 257);
            std::array<std::uint8_t, kPayloadBits> sliced{};
            bool inRange = true;
            for (int bit = 0; bit < kPayloadBits; ++bit)
            {
                const double x = geometry.syncStart +
                    (static_cast<double>(kSyncCells + bit) + 0.5) * geometry.bitPeriod;
                const int ix = static_cast<int>(std::lround(x));
                if (ix < 0 || ix >= width) { inRange = false; break; }
                sliced[static_cast<std::size_t>(bit)] = line[ix] >= t16 ? 1u : 0u;
            }
            if (inRange && crcValid(sliced.data()))
            {
                ++thresholdRecoveredCount;
                if (!thresholdRecovered || std::abs(d) < std::abs(bestThresholdDelta))
                {
                    thresholdRecovered = true;
                    bestThresholdDelta = d;
                }
            }
        }

        gSonyTrackingDump << std::fixed << std::setprecision(6)
            << "CRCFAIL frame=" << debugFrame
            << " row=" << row
            << " f=" << (field + 1)
            << " fr=" << fieldRow
            << " H=" << (pcmLine >= 0 ? pcmLine + 1 : 0)
            << " sync=" << geometry.syncStart
            << " period=" << geometry.bitPeriod
            << " data0=" << data0
            << " data1=" << data1
            << " ref=" << ref
            << " slice=" << threshold8
            << " struct=" << structureScore(line, width, geometry, threshold)
            << " phaseOK=" << (phaseRecovered ? 1 : 0)
            << " phaseBest=" << (phaseRecovered ? bestPhase : 99.0)
            << " phasePasses=" << recoveredCount
            << " threshOK=" << (thresholdRecovered ? 1 : 0)
            << " threshBestDelta=" << (thresholdRecovered ? bestThresholdDelta : 999)
            << " threshPasses=" << thresholdRecoveredCount;

        for (std::size_t k = 0; k < weak.size(); ++k)
        {
            if (weak[k].bit >= 0)
            {
                gSonyTrackingDump
                    << " weak" << k << "=" << weak[k].bit
                    << ":" << weak[k].y8
                    << ":" << weak[k].margin;
            }
        }
        gSonyTrackingDump << " crcWord=" << std::hex << std::uppercase
            << std::setw(4) << std::setfill('0') << static_cast<unsigned int>(payloadCrcWord(payload))
            << std::dec << std::nouppercase << std::setfill(' ') << '\n';
        gSonyTrackingDump.flush();
    };

    // PAL 625 capture has 575 PCM-usable raster H positions: 288 in F1 and
    // 287 in F2. A 720x576 frame buffer may still contain row 575, but that
    // row is outside the PCM-bearing 288/287 field raster and must not enter
    // slicing, CRC or line accounting.
    const int usableHeight = std::min(height, kPcmUsableRasterRows);
    for (int row = 0; row < usableHeight; ++row)
    {
        const auto* line =
            y.data() + static_cast<std::size_t>(row) *
            static_cast<std::size_t>(width);

        const int field = row & 1;
        const int fieldRow = row / 2;
        const int knownControlRow = controlFieldRow[static_cast<std::size_t>(field)];
        const bool mappedMudAudio = knownControlRow >= 0 && fieldRow > knownControlRow;

        // Once a control-H has established the MUD vertical mapping, every
        // following captured H in that field is an expected audio H. Count it
        // even if slicing fails, so a bad/missed line lowers CRC/valid-lines
        // instead of silently shrinking the denominator (e.g. 573/573).
        bool countedAsAudio = false;
        if (mappedMudAudio)
        {
            ++testedLines;
            ++testedAudioByField[static_cast<std::size_t>(field)];
            countedAsAudio = true;
        }

        bool contrastOk = false;
        const auto threshold = thresholdForGeometry(line, width, geometry, contrastOk);
        if (!contrastOk)
        {
            ++frameContrastFails;
            continue;
        }

        std::array<std::uint8_t, kPayloadBits> payload{};
        if (!decodePayload(line, width, geometry, threshold, payload.data()))
        {
            ++frameDecodeFails;
            continue;
        }

        // Delta63: the borrowed PCM-701ES exposed a slow horizontal tracking
        // wobble.  During each burst the nominal centre sample can miss one or
        // more of the late payload bits even though DATA0/DATA1 separation is
        // enormous.  The Delta62 diagnostic proved every observed CRC failure
        // could be recovered by moving the sample point only +0.025..+0.075
        // bit cell.  Do a CRC-guided per-line phase retry before declaring the
        // line bad.  CRC is the oracle: a retry is accepted only when the full
        // IEC 60841 CRC passes, so this cannot silently invent audio data.
        bool phaseRecovered = false;
        double recoveredPhase = 0.0;
        const bool nominalCrcValid = crcValid(payload.data());
        if (!nominalCrcValid)
        {
            // Delta65: characterise the exact bit pattern that made the nominal
            // centre slice fail.  Abrupt audio changes can radically alter bit
            // density without changing video timing; this tells us whether the
            // observed start/stop glitches correlate with density/transitions.
            int ones = 0;
            int transitions = 0;
            for (int bit = 0; bit < kPayloadBits; ++bit)
            {
                ones += payload[static_cast<std::size_t>(bit)] ? 1 : 0;
                if (bit > 0 &&
                    payload[static_cast<std::size_t>(bit)] !=
                    payload[static_cast<std::size_t>(bit - 1)])
                {
                    ++transitions;
                }
            }
            if (gSonyTrackingDump)
            {
                gSonyTrackingDump << std::fixed << std::setprecision(6)
                    << "NOMINAL_CRCFAIL frame=" << debugFrame
                    << " row=" << row
                    << " f=" << (field + 1)
                    << " fr=" << fieldRow
                    << " sync=" << geometry.syncStart
                    << " period=" << geometry.bitPeriod
                    << " slice=" << static_cast<int>(std::lround(static_cast<double>(threshold) / 257.0))
                    << " ones=" << ones
                    << " zeros=" << (kPayloadBits - ones)
                    << " transitions=" << transitions
                    << " crcWord=" << std::hex << std::uppercase
                    << std::setw(4) << std::setfill('0')
                    << static_cast<unsigned int>(payloadCrcWord(payload.data()))
                    << std::dec << std::nouppercase << std::setfill(' ') << '\n';
            }

            // Most measured recoveries were +0.050 cell, then +0.025.  Try the
            // positive tracking direction first to keep the recovery cheap.
            constexpr std::array<double, 8> kRetryPhase{
                +0.050, +0.025, +0.075, +0.100,
                -0.025, -0.050, -0.075, -0.100};

            for (const double phase : kRetryPhase)
            {
                std::array<std::uint8_t, kPayloadBits> shifted{};
                bool inRange = true;
                for (int bit = 0; bit < kPayloadBits; ++bit)
                {
                    const double x = geometry.syncStart +
                        (static_cast<double>(kSyncCells + bit) + 0.5 + phase) *
                            geometry.bitPeriod;
                    const int ix = static_cast<int>(std::lround(x));
                    if (ix < 0 || ix >= width)
                    {
                        inRange = false;
                        break;
                    }
                    shifted[static_cast<std::size_t>(bit)] =
                        line[ix] >= threshold ? 1u : 0u;
                }

                if (inRange && crcValid(shifted.data()))
                {
                    payload = shifted;
                    phaseRecovered = true;
                    recoveredPhase = phase;
                    break;
                }
            }
        }

        const PhysicalBlock block = payloadToWords(payload.data());

        // Recognise the control-H from its 56-bit heading BEFORE applying the
        // normal audio-line CRC accounting. A damaged control-H must never
        // appear as a bad audio line (the old behaviour caused 575/576).
        if (isControlBlock(block))
        {
            ++controlTestedLines;
            ++testedControlByField[static_cast<std::size_t>(field)];
            // The heading alone establishes the vertical MUD mapping. Even if
            // the control CRC is damaged, DATA H1 still follows this row. Only
            // the metadata bits themselves require a valid control CRC.
            controlFieldRow[static_cast<std::size_t>(field)] = fieldRow;
            if (crcValid(payload.data()))
            {
                ++controlValidLines;
                ++validControlByField[static_cast<std::size_t>(field)];
                // Control bit numbering is MSB=1 ... LSB=14. Thus bit 13 is
                // numeric bit 1 and bit 14 (pre-emphasis) is numeric bit 0.
                const std::uint16_t control = block.words[7] & 0x3FFFu;
                const bool newMode16 = (control & 0x0002u) != 0u; // Q absent => 16-bit mode
                const bool modeChanged = controlValid_ && newMode16 != controlMode16_;

                if (modeChanged)
                {
                    // A control-H is authoritative. Never feed interleave blocks
                    // from the previous coding mode into the new decoder. The
                    // control rows precede the MUD audio rows, so restarting at
                    // this frame boundary keeps the new frame internally clean.
                    physicalBlocks_.clear();
                    lastProcessedGroup_ = frameBaseLine - 1;
                    recentLeft_.clear();
                    recentRight_.clear();
                    currentAudioStereo_.clear();
                    cleanPChecks_ = 0;
                    cleanPPasses_ = 0;
                    cleanQChecks_ = 0;
                    cleanQPasses_ = 0;
                }

                controlValid_ = true;
                controlMode16_ = newMode16;
                controlPreEmphasis_ = (control & 0x0001u) == 0u; // active low

                dumpSonyLine(
                    line, row, field, fieldRow, -1, threshold,
                    block, payload.data(), true);
            }
            continue; // Control H is metadata, never audio.
        }

        if (!countedAsAudio)
        {
            ++testedLines;
            ++testedAudioByField[static_cast<std::size_t>(field)];
        }

        // Determine the logical H before CRC accounting so a failing line can
        // be identified precisely in the tracking log.
        int pcmLine = -1;
        const int ctrlRow = controlFieldRow[static_cast<std::size_t>(field)];
        if (ctrlRow >= 0 && fieldRow > ctrlRow)
        {
            pcmLine = fieldRow - ctrlRow - 1; // H1 -> zero-based 0
        }
        else
        {
            if (fieldRow >= 0 && fieldRow < kCapturedPcmLinesPerField)
            {
                pcmLine = kFirstCapturedPcmLineZeroBased + fieldRow;
            }
        }

        if (!crcValid(payload.data()))
        {
            ++frameCrcFails;
            logCrcFailure(line, row, field, fieldRow, pcmLine, threshold, payload.data());
            continue;
        }

        if (phaseRecovered && gSonyTrackingDump)
        {
            gSonyTrackingDump << std::fixed << std::setprecision(6)
                << "PHASERECOVER frame=" << debugFrame
                << " row=" << row
                << " f=" << (field + 1)
                << " fr=" << fieldRow
                << " H=" << (pcmLine >= 0 ? pcmLine + 1 : 0)
                << " phase=" << recoveredPhase
                << " slice=" << static_cast<int>(std::lround(static_cast<double>(threshold) / 257.0))
                << '\n';
        }

        ++valid;
        ++validAudioByField[static_cast<std::size_t>(field)];

        if (pcmLine < 0)
        {
            continue;
        }

        if (pcmLine < 0 || pcmLine >= kPalPcmLinesPerField)
        {
            continue;
        }

        dumpSonyLine(
            line, row, field, fieldRow, pcmLine, threshold,
            block, payload.data(), false);

        const std::int64_t physicalLine =
            frameBaseLine +
            static_cast<std::int64_t>(field) * kPalPcmLinesPerField +
            pcmLine;

        physicalBlocks_[physicalLine] = block;
    }

    if (gSonyTrackingDump)
    {
        gSonyTrackingDump << std::fixed << std::setprecision(6)
            << "FRAME_END frame=" << debugFrame
            << " sync=" << geometry.syncStart
            << " period=" << geometry.bitPeriod
            << " tested=" << testedLines
            << " valid=" << valid
            << " crcFails=" << frameCrcFails
            << " contrastFails=" << frameContrastFails
            << " decodeFails=" << frameDecodeFails
            << " f1=" << validAudioByField[0] << "/" << testedAudioByField[0]
            << " f2=" << validAudioByField[1] << "/" << testedAudioByField[1]
            << '\n';
        gSonyTrackingDump.flush();
    }

    return valid;
}

bool PcmVideoDecoder::reconstruct14BitGroup(
    std::int64_t group,
    std::array<std::uint16_t, 6>& audio,
    bool& corrected,
    bool& cleanPVerified)
{
    corrected = false;
    cleanPVerified = false;

    std::array<bool, 8> have{};
    std::array<std::uint16_t, 8> word{};
    for (int i = 0; i < 8; ++i)
    {
        const auto it = physicalBlocks_.find(
            group + static_cast<std::int64_t>(i) * kInterleaveD);
        if (it != physicalBlocks_.end())
        {
            have[static_cast<std::size_t>(i)] = true;
            word[static_cast<std::size_t>(i)] =
                static_cast<std::uint16_t>(
                    it->second.words[static_cast<std::size_t>(i)] & 0x3FFFu);
        }
    }

    std::array<int, 6> missing{};
    int missingAudio = 0;
    for (int i = 0; i < 6; ++i)
    {
        if (!have[static_cast<std::size_t>(i)])
        {
            missing[static_cast<std::size_t>(missingAudio++)] = i;
        }
    }

    // Coefficient of Wi in Q = T^6 W1 + ... + T W6.
    const auto qCoefficient = [](int index)
    {
        std::uint16_t c = 1u;
        for (int n = 0; n < 6 - index; ++n)
        {
            c = gfMulX14(c);
        }
        return c;
    };

    if (missingAudio == 1)
    {
        const int m = missing[0];
        if (have[6])
        {
            std::uint16_t recovered = word[6];
            for (int i = 0; i < 6; ++i)
            {
                if (i != m)
                {
                    recovered ^= word[static_cast<std::size_t>(i)];
                }
            }
            word[static_cast<std::size_t>(m)] =
                static_cast<std::uint16_t>(recovered & 0x3FFFu);
            have[static_cast<std::size_t>(m)] = true;
            corrected = true;
        }
        else if (have[7])
        {
            std::uint16_t remainder = word[7];
            for (int i = 0; i < 6; ++i)
            {
                if (i != m)
                {
                    remainder ^= gfMul14(
                        qCoefficient(i), word[static_cast<std::size_t>(i)]);
                }
            }
            const std::uint16_t inv = gfInv14(qCoefficient(m));
            if (inv == 0u)
            {
                return false;
            }
            word[static_cast<std::size_t>(m)] = gfMul14(inv, remainder);
            have[static_cast<std::size_t>(m)] = true;
            corrected = true;
        }
        else
        {
            return false;
        }
    }
    else if (missingAudio == 2)
    {
        // EIAJ's P+Q pair can recover two known erasures. CRC already tells us
        // exactly which physical words are missing/bad.
        if (!have[6] || !have[7])
        {
            return false;
        }

        const int a = missing[0];
        const int b = missing[1];

        std::uint16_t pRemainder = word[6]; // xa XOR xb
        std::uint16_t qRemainder = word[7]; // ca*xa XOR cb*xb
        for (int i = 0; i < 6; ++i)
        {
            if (i == a || i == b)
            {
                continue;
            }
            pRemainder ^= word[static_cast<std::size_t>(i)];
            qRemainder ^= gfMul14(
                qCoefficient(i), word[static_cast<std::size_t>(i)]);
        }

        const std::uint16_t ca = qCoefficient(a);
        const std::uint16_t cb = qCoefficient(b);
        const std::uint16_t denom = static_cast<std::uint16_t>(ca ^ cb);
        const std::uint16_t inv = gfInv14(denom);
        if (inv == 0u)
        {
            return false;
        }

        const std::uint16_t xa = gfMul14(
            inv,
            static_cast<std::uint16_t>(qRemainder ^ gfMul14(cb, pRemainder)));
        const std::uint16_t xb = static_cast<std::uint16_t>(pRemainder ^ xa);
        word[static_cast<std::size_t>(a)] = xa;
        word[static_cast<std::size_t>(b)] = xb;
        have[static_cast<std::size_t>(a)] = true;
        have[static_cast<std::size_t>(b)] = true;
        corrected = true;
    }
    else if (missingAudio > 2)
    {
        return false;
    }

    // Verify every parity equation that is actually present. This both rejects
    // false 14-bit hypotheses and keeps P verify meaningful.
    if (have[6])
    {
        const std::uint16_t expectedP = static_cast<std::uint16_t>(
            word[0] ^ word[1] ^ word[2] ^ word[3] ^ word[4] ^ word[5]);
        cleanPVerified = (expectedP == word[6]);
        if (!cleanPVerified)
        {
            return false;
        }
    }

    if (have[7])
    {
        std::array<std::uint16_t, 6> six{
            word[0], word[1], word[2], word[3], word[4], word[5]};
        ++cleanQChecks_;
        if (q14(six) != word[7])
        {
            return false;
        }
        ++cleanQPasses_;
    }
    else if (!controlValid_ || controlMode16_)
    {
        // Without a valid 14-bit control word, Q is the discriminator that
        // prevents a 16-bit LSB pack from being mistaken for EIAJ Q.
        return false;
    }

    for (int i = 0; i < 6; ++i)
    {
        audio[static_cast<std::size_t>(i)] =
            static_cast<std::uint16_t>(
                (word[static_cast<std::size_t>(i)] & 0x3FFFu) << 2);
    }

    return true;
}

bool PcmVideoDecoder::reconstruct16BitGroup(
    std::int64_t group,
    std::array<std::uint16_t, 6>& audio,
    bool& corrected,
    bool& cleanPVerified,
    bool& lsbPackMissing)
{
    corrected = false;
    cleanPVerified = false;
    lsbPackMissing = false;

    // A0, B0, A1, B1, A2, B2, P are spread over physical lines separated
    // by D=16.  The old Q slot, 7D later, carries the two LSBs for those
    // seven 16-bit values in Appendix-B 16-bit mode.
    constexpr std::array<int, 7> wordIndex{0, 1, 2, 3, 4, 5, 6};

    std::array<bool, 7> haveUpper{};
    std::array<std::uint16_t, 7> upper{};

    int missingUpper = 0;
    int missingIndex = -1;

    for (int i = 0; i < 7; ++i)
    {
        const std::int64_t physical =
            group + static_cast<std::int64_t>(i) * kInterleaveD;
        const auto it = physicalBlocks_.find(physical);
        if (it == physicalBlocks_.end())
        {
            ++missingUpper;
            missingIndex = i;
            continue;
        }

        haveUpper[static_cast<std::size_t>(i)] = true;
        upper[static_cast<std::size_t>(i)] =
            it->second.words[static_cast<std::size_t>(wordIndex[static_cast<std::size_t>(i)])];
    }

    const auto qIt = physicalBlocks_.find(group + kMaxInterleaveDelay);
    const bool haveLowPack = qIt != physicalBlocks_.end();
    const std::uint16_t lowPack = haveLowPack
        ? static_cast<std::uint16_t>(qIt->second.words[7] & 0x3FFFu)
        : 0u;
    lsbPackMissing = !haveLowPack;

    std::array<std::uint16_t, 7> value{};
    for (int i = 0; i < 7; ++i)
    {
        const int shift = 2 * (6 - i);
        const std::uint16_t low =
            static_cast<std::uint16_t>((lowPack >> shift) & 0x3u);

        if (haveUpper[static_cast<std::size_t>(i)])
        {
            value[static_cast<std::size_t>(i)] =
                static_cast<std::uint16_t>(
                    (upper[static_cast<std::size_t>(i)] << 2) | low);
        }
        else
        {
            value[static_cast<std::size_t>(i)] = low;
        }
    }

    if (missingUpper == 0)
    {
        const std::uint16_t expectedP =
            static_cast<std::uint16_t>(
                value[0] ^ value[1] ^ value[2] ^
                value[3] ^ value[4] ^ value[5]);

        // Sony PCM-F1 16-bit stores only P[15:2] in the normal P word.
        // The two low parity bits share the S/LSB word and their exact bit
        // placement is independent of the six 2-bit sample extensions.
        // Verified against a real PCM-701ES capture: P[15:2] matches 332/332
        // reconstructed groups, while treating the current S packing as a
        // seventh contiguous 2-bit value falsely rejects ~75% of clean groups.
        // Therefore P verification must use the transmitted upper 14 bits only.
        cleanPVerified =
            ((expectedP & 0xFFFCu) == (value[6] & 0xFFFCu));
        if (!cleanPVerified)
        {
            return false;
        }
    }
    else if (missingUpper == 1)
    {
        // One known erasure is exactly what the 16-bit P word can repair.
        // CRC tells us which physical line is absent/bad; P gives the missing
        // upper 14 bits.  Do not use the two low parity bits as a rejection
        // criterion until the exact Sony S-word packing is implemented.
        if (missingIndex >= 0 && missingIndex <= 5 && haveUpper[6])
        {
            std::uint16_t recovered = value[6];
            for (int i = 0; i < 6; ++i)
            {
                if (i != missingIndex)
                {
                    recovered = static_cast<std::uint16_t>(recovered ^ value[static_cast<std::size_t>(i)]);
                }
            }

            // Do not reject on the two low bits here: their exact position in
            // Sony's S word is not the same as the old contiguous-pack assumption.
            if (!haveLowPack)
            {
                recovered = static_cast<std::uint16_t>(recovered & 0xFFFCu);
            }

            value[static_cast<std::size_t>(missingIndex)] = recovered;
            corrected = true;
        }
        else if (missingIndex == 6)
        {
            std::uint16_t recoveredP = 0;
            for (int i = 0; i < 6; ++i)
            {
                recoveredP = static_cast<std::uint16_t>(recoveredP ^ value[static_cast<std::size_t>(i)]);
            }

            // Likewise, only the upper 14 parity bits are directly verified.
            if (!haveLowPack)
            {
                recoveredP = static_cast<std::uint16_t>(recoveredP & 0xFFFCu);
            }

            value[6] = recoveredP;
            corrected = true;
        }
        else
        {
            return false;
        }
    }
    else
    {
        return false;
    }

    for (int i = 0; i < 6; ++i)
    {
        std::uint16_t sample = value[static_cast<std::size_t>(i)];
        if (muteBottomTwoBits_)
        {
            sample = static_cast<std::uint16_t>(sample & 0xFFFCu);
        }
        audio[static_cast<std::size_t>(i)] = sample;
    }

    return true;
}

void PcmVideoDecoder::appendAudioGroup(
    std::int64_t group,
    const std::array<std::uint16_t, 6>& audio)
{
    const std::int64_t firstSample = group * 3;

    recentLeft_.emplace_back(firstSample + 0, signed16(audio[0]));
    recentRight_.emplace_back(firstSample + 0, signed16(audio[1]));
    recentLeft_.emplace_back(firstSample + 1, signed16(audio[2]));
    recentRight_.emplace_back(firstSample + 1, signed16(audio[3]));
    recentLeft_.emplace_back(firstSample + 2, signed16(audio[4]));
    recentRight_.emplace_back(firstSample + 2, signed16(audio[5]));

    currentAudioStereo_.push_back(signed16(audio[0]));
    currentAudioStereo_.push_back(signed16(audio[1]));
    currentAudioStereo_.push_back(signed16(audio[2]));
    currentAudioStereo_.push_back(signed16(audio[3]));
    currentAudioStereo_.push_back(signed16(audio[4]));
    currentAudioStereo_.push_back(signed16(audio[5]));

    constexpr std::size_t kRecentLimit = 8192;
    while (recentLeft_.size() > kRecentLimit)
    {
        recentLeft_.pop_front();
    }
    while (recentRight_.size() > kRecentLimit)
    {
        recentRight_.pop_front();
    }
}

void PcmVideoDecoder::reconstructAvailableGroups(
    std::int64_t newestPhysicalLine)
{
    const std::int64_t newestCompleteGroup =
        newestPhysicalLine - kMaxInterleaveDelay;

    if (lastProcessedGroup_ < 0)
    {
        lastProcessedGroup_ = -1;
    }

    for (std::int64_t group = lastProcessedGroup_ + 1;
         group <= newestCompleteGroup;
         ++group)
    {
        std::array<std::uint16_t, 6> audio{};
        bool corrected = false;
        bool cleanPVerified = false;
        bool lsbPackMissing = false;

        bool decoded = false;
        bool decoded14 = false;

        // A valid control-H is authoritative. Without one, try 14-bit first:
        // its Q equation is a very strong discriminator. If Q doesn't verify,
        // fall back to the existing PCM-F1 16-bit reconstruction.
        if (controlValid_ && !controlMode16_)
        {
            decoded = reconstruct14BitGroup(
                group, audio, corrected, cleanPVerified);
            decoded14 = decoded;
        }
        else if (controlValid_ && controlMode16_)
        {
            decoded = reconstruct16BitGroup(
                group, audio, corrected, cleanPVerified, lsbPackMissing);
        }
        else
        {
            decoded = reconstruct14BitGroup(
                group, audio, corrected, cleanPVerified);
            decoded14 = decoded;
            if (!decoded)
            {
                corrected = false;
                cleanPVerified = false;
                lsbPackMissing = false;
                decoded = reconstruct16BitGroup(
                    group, audio, corrected, cleanPVerified, lsbPackMissing);
            }
        }

        if (decoded)
        {
            ++reconstructedGroups_;
            if (corrected)
            {
                ++pCorrectedGroups_;
            }
            if (cleanPVerified)
            {
                ++cleanPChecks_;
                ++cleanPPasses_;
            }
            if (!decoded14 && lsbPackMissing)
            {
                ++lsbPackMissingGroups_;
            }
            appendAudioGroup(group, audio);
        }
        else
        {
            ++hardUncorrectableGroups_;

            bool allPresent = true;
            for (int i = 0; i < 8; ++i)
            {
                if (physicalBlocks_.find(group + i * kInterleaveD) == physicalBlocks_.end())
                {
                    allPresent = false;
                    break;
                }
            }
            if (allPresent)
            {
                ++cleanPChecks_;
            }
        }

        lastProcessedGroup_ = group;
    }
}

double PcmVideoDecoder::estimateFrequency(
    const std::deque<std::pair<std::int64_t, std::int16_t>>& samples)
{
    if (samples.size() < 64)
    {
        return 0.0;
    }

    // Measure positive-going zero-crossing periods only inside contiguous
    // reconstructed sample runs.  A known erasure creates a gap in the sample
    // indices; periods spanning such a gap must not bias the estimate.
    double periodSum = 0.0;
    int periodCount = 0;
    double lastCrossing = 0.0;
    bool haveLastCrossing = false;

    for (std::size_t i = 1; i < samples.size(); ++i)
    {
        const auto& a = samples[i - 1];
        const auto& b = samples[i];

        if (b.first != a.first + 1)
        {
            haveLastCrossing = false;
            continue;
        }

        if (a.second < 0 && b.second >= 0 && b.second != a.second)
        {
            const double fraction =
                -static_cast<double>(a.second) /
                (static_cast<double>(b.second) - static_cast<double>(a.second));
            const double crossing = static_cast<double>(a.first) + fraction;

            if (haveLastCrossing)
            {
                const double delta = crossing - lastCrossing;
                if (delta > 2.0 && delta < 2000.0)
                {
                    periodSum += delta;
                    ++periodCount;
                }
            }

            lastCrossing = crossing;
            haveLastCrossing = true;
        }
    }

    if (periodCount < 3)
    {
        return 0.0;
    }

    return 44100.0 /
        (periodSum / static_cast<double>(periodCount));
}

void PcmVideoDecoder::pruneHistory(
    std::int64_t newestPhysicalLine)
{
    const std::int64_t keepFrom = newestPhysicalLine - 4 * kPalPcmLinesPerFrame;
    auto it = physicalBlocks_.begin();
    while (it != physicalBlocks_.end() && it->first < keepFrom)
    {
        it = physicalBlocks_.erase(it);
    }
}

PcmVideoDecoder::Result PcmVideoDecoder::processLuma(
    const std::vector<std::uint16_t>& y,
    int width,
    int height,
    bool inputSignalValid,
    std::uint64_t captureGeneration,
    bool muteBottomTwoBits)
{
    Result result{};
    currentAudioStereo_.clear();
    muteBottomTwoBits_ = muteBottomTwoBits;

    if (!inputSignalValid || width <= 0 || height <= 0 ||
        y.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height))
    {
        reset();
        return result;
    }

    if (!geometry_.valid)
    {
        if (searchCooldownFrames_ > 0)
        {
            --searchCooldownFrames_;
            return result;
        }

        geometry_ = searchGeometry(y, width, height);
        if (!geometry_.valid)
        {
            searchCooldownFrames_ = 4;
            return result;
        }
    }

    if (firstCaptureGeneration_ == 0)
    {
        firstCaptureGeneration_ = captureGeneration;
    }

    const std::uint64_t relativeFrame =
        captureGeneration >= firstCaptureGeneration_
        ? captureGeneration - firstCaptureGeneration_
        : 0;

    const std::int64_t frameBaseLine =
        static_cast<std::int64_t>(relativeFrame) * kPalPcmLinesPerFrame;

    const std::uint64_t pChecksBefore = cleanPChecks_;
    const std::uint64_t pPassesBefore = cleanPPasses_;
    const std::uint64_t qChecksBefore = cleanQChecks_;
    const std::uint64_t qPassesBefore = cleanQPasses_;

    int tested = 0;
    int controlValidLines = 0;
    int controlTestedLines = 0;
    std::array<int, 2> validAudioByField{};
    std::array<int, 2> testedAudioByField{};
    std::array<int, 2> validControlByField{};
    std::array<int, 2> testedControlByField{};
    const int valid = decodeFrameLines(
        y,
        width,
        height,
        geometry_,
        frameBaseLine,
        tested,
        controlValidLines,
        controlTestedLines,
        validAudioByField,
        testedAudioByField,
        validControlByField,
        testedControlByField);

    const double percent = tested > 0
        ? 100.0 * static_cast<double>(valid) / static_cast<double>(tested)
        : 0.0;

    // Control-H is frame-local evidence. Do not retain old header metadata
    // after the control line has disappeared from the captured raster.
    if (controlValidLines == 0)
    {
        controlValid_ = false;
    }

    const bool frameLocked = valid >= 8;

    // A handful of CRC-valid lines is enough to report that PCM structure is
    // still visible, but it is not enough to keep trusting an old geometry.
    // Delta 16 widened the acquisition search substantially; a marginal
    // candidate could therefore survive forever (for example 69/556 lines),
    // preventing a clean re-acquisition after the signal became good again.
    // Treat <25% CRC-valid tested lines as degraded geometry and force a fresh
    // blind search after three consecutive frames.
    const bool geometryHealthy =
        tested > 0 && static_cast<std::int64_t>(valid) * 4 >= tested;

    if (frameLocked)
    {
        ++lockFrames_;

        reconstructAvailableGroups(
            frameBaseLine + kPalPcmLinesPerFrame - 1);
        pruneHistory(
            frameBaseLine + kPalPcmLinesPerFrame - 1);
    }

    if (geometryHealthy)
    {
        badFrames_ = 0;
    }
    else
    {
        ++badFrames_;
        if (badFrames_ >= 3)
        {
            geometry_ = {};
            lockFrames_ = 0;
            badFrames_ = 0;
            searchCooldownFrames_ = 0;
            firstCaptureGeneration_ = 0;
            lastProcessedGroup_ = -1;
            physicalBlocks_.clear();
            recentLeft_.clear();
            recentRight_.clear();
            controlValid_ = false;
            controlMode16_ = false;
            controlPreEmphasis_ = false;
        }
    }

    const std::uint64_t framePChecks = cleanPChecks_ - pChecksBefore;
    const std::uint64_t framePPasses = cleanPPasses_ - pPassesBefore;
    const double pVerifyPercent = framePChecks > 0
        ? 100.0 * static_cast<double>(framePPasses) /
            static_cast<double>(framePChecks)
        : (cleanPChecks_ > 0
            ? 100.0 * static_cast<double>(cleanPPasses_) /
                static_cast<double>(cleanPChecks_)
            : 0.0);

    result.locked = frameLocked;
    result.validLines = valid;
    result.testedLines = tested;
    result.crcPercent = percent;
    result.bitPeriodPixels = geometry_.bitPeriod;
    result.syncStartPixels = geometry_.syncStart;
    result.consecutiveLockedFrames = lockFrames_;

    const std::uint64_t frameQChecks = cleanQChecks_ - qChecksBefore;
    const std::uint64_t frameQPasses = cleanQPasses_ - qPassesBefore;
    const double frameQPercent = frameQChecks > 0
        ? 100.0 * static_cast<double>(frameQPasses) /
            static_cast<double>(frameQChecks)
        : 0.0;

    // A valid control-H remains authoritative. Without it, score each frame
    // from Q parity (strong 14-bit evidence) versus clean P with failing Q
    // (strong 16-bit evidence). The bounded score deliberately forgets old
    // history so live 14<->16 switches do not get stuck.
    if (!controlValid_ && frameQChecks >= 4)
    {
        if (frameQPercent >= 90.0)
        {
            headerlessModeScore_ = std::min(12, headerlessModeScore_ + 2);
        }
        else if (framePChecks >= 4 && pVerifyPercent >= 90.0)
        {
            headerlessModeScore_ = std::max(-12, headerlessModeScore_ - 2);
        }

        if (headerlessModeScore_ >= 4)
        {
            headerlessModeKnown_ = true;
            headerlessMode16_ = false;
        }
        else if (headerlessModeScore_ <= -4)
        {
            headerlessModeKnown_ = true;
            headerlessMode16_ = true;
        }
    }

    result.modeKnown = controlValid_ || headerlessModeKnown_;
    result.mode16Detected = controlValid_ ? controlMode16_ : headerlessMode16_;
    result.controlValid = controlValid_;
    result.preEmphasis = controlPreEmphasis_;
    result.controlValidLines = controlValidLines;
    result.controlTestedLines = controlTestedLines;
    result.field1ValidAudioLines = validAudioByField[0];
    result.field1TestedAudioLines = testedAudioByField[0];
    result.field1ValidControlLines = validControlByField[0];
    result.field1TestedControlLines = testedControlByField[0];
    result.field2ValidAudioLines = validAudioByField[1];
    result.field2TestedAudioLines = testedAudioByField[1];
    result.field2ValidControlLines = validControlByField[1];
    result.field2TestedControlLines = testedControlByField[1];
    result.reconstructedGroups = reconstructedGroups_;
    result.pCorrectedGroups = pCorrectedGroups_;
    result.lsbPackMissingGroups = lsbPackMissingGroups_;
    result.hardUncorrectableGroups = hardUncorrectableGroups_;
    result.pVerifyPercent = pVerifyPercent;
    result.leftFrequencyHz = estimateFrequency(recentLeft_);
    result.rightFrequencyHz = estimateFrequency(recentRight_);
    result.audioStereo = currentAudioStereo_;

    return result;
}
