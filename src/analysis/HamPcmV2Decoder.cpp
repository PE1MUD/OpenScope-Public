#include "HamPcmV2Decoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace
{
constexpr std::array<std::uint8_t, 14> kKey{
    0x87, 0x23, 0x46, 0xDC, 0xB0, 0xDD, 0xEE,
    0xF8, 0xFD, 0xC3, 0x5C, 0xBF, 0x5C, 0x53};
constexpr double kNominalPeriod = 696.0 / 180.0;
constexpr double kNominalStart = 12.0;

std::uint8_t gfMul(std::uint8_t a, std::uint8_t b)
{
    unsigned int aa = a, bb = b, r = 0;
    while (bb != 0)
    {
        if ((bb & 1u) != 0u) r ^= aa;
        bb >>= 1u;
        aa <<= 1u;
        if ((aa & 0x100u) != 0u) aa ^= 0x11Du;
    }
    return static_cast<std::uint8_t>(r & 0xFFu);
}

std::uint8_t gfPow(std::uint8_t a, int e)
{
    std::uint8_t r = 1;
    while (e > 0)
    {
        if ((e & 1) != 0) r = gfMul(r, a);
        e >>= 1;
        if (e != 0) a = gfMul(a, a);
    }
    return r;
}

std::uint8_t gfInv(std::uint8_t a)
{
    return a == 0 ? 0 : gfPow(a, 254);
}

std::array<std::uint8_t, 4> syndromes(const std::array<std::uint8_t, 13>& cw)
{
    std::array<std::uint8_t, 4> s{};
    for (int i = 0; i < 4; ++i)
    {
        const std::uint8_t x = gfPow(2, i);
        std::uint8_t acc = 0;
        for (const auto b : cw)
            acc = static_cast<std::uint8_t>(gfMul(acc, x) ^ b);
        s[static_cast<std::size_t>(i)] = acc;
    }
    return s;
}

bool allZero(const std::array<std::uint8_t, 4>& s)
{
    return std::all_of(s.begin(), s.end(), [](std::uint8_t v) { return v == 0; });
}

bool rsCorrect(std::array<std::uint8_t, 13>& cw, int& correctedBytes)
{
    correctedBytes = 0;
    const auto syn = syndromes(cw);
    if (allZero(syn)) return true;

    for (int p = 0; p < 13; ++p)
    {
        const std::uint8_t x = gfPow(2, 12 - p);
        const std::uint8_t y = syn[0];
        std::uint8_t xp = 1;
        bool ok = true;
        for (int i = 0; i < 4; ++i)
        {
            if (gfMul(y, xp) != syn[static_cast<std::size_t>(i)]) { ok = false; break; }
            xp = gfMul(xp, x);
        }
        if (ok)
        {
            cw[static_cast<std::size_t>(p)] ^= y;
            if (allZero(syndromes(cw))) { correctedBytes = 1; return true; }
            cw[static_cast<std::size_t>(p)] ^= y;
        }
    }

    for (int p = 0; p < 12; ++p)
    {
        const std::uint8_t x1 = gfPow(2, 12 - p);
        for (int q = p + 1; q < 13; ++q)
        {
            const std::uint8_t x2 = gfPow(2, 12 - q);
            const std::uint8_t den = static_cast<std::uint8_t>(x1 ^ x2);
            if (den == 0) continue;
            const std::uint8_t y1 = gfMul(
                static_cast<std::uint8_t>(syn[1] ^ gfMul(syn[0], x2)), gfInv(den));
            const std::uint8_t y2 = static_cast<std::uint8_t>(syn[0] ^ y1);
            std::uint8_t x1p = 1, x2p = 1;
            bool ok = true;
            for (int i = 0; i < 4; ++i)
            {
                const auto expect = static_cast<std::uint8_t>(gfMul(y1, x1p) ^ gfMul(y2, x2p));
                if (expect != syn[static_cast<std::size_t>(i)]) { ok = false; break; }
                x1p = gfMul(x1p, x1);
                x2p = gfMul(x2p, x2);
            }
            if (!ok) continue;
            cw[static_cast<std::size_t>(p)] ^= y1;
            cw[static_cast<std::size_t>(q)] ^= y2;
            if (allZero(syndromes(cw))) { correctedBytes = 2; return true; }
            cw[static_cast<std::size_t>(p)] ^= y1;
            cw[static_cast<std::size_t>(q)] ^= y2;
        }
    }
    return false;
}

std::uint8_t crc8(const std::uint8_t* data, int count)
{
    std::uint8_t c = 0;
    for (int i = 0; i < count; ++i)
    {
        c ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            c = (c & 0x80u) != 0u
                ? static_cast<std::uint8_t>((static_cast<unsigned int>(c) << 1u) ^ 0x07u)
                : static_cast<std::uint8_t>(static_cast<unsigned int>(c) << 1u);
    }
    return c;
}

struct Row
{
    bool coreGood = false;
    bool refinementGood = false;
    int correctedBytes = 0;
    int n = 0;
    bool textMarker = false;
    int textBits = 0;
    double period = kNominalPeriod;
    double start = kNominalStart;
    std::array<std::int16_t, 8> samples{};
};


constexpr std::array<int, 180> makeNominalCenter2()
{
    std::array<int, 180> out{};
    for (int cell = 0; cell < 180; ++cell)
    {
        const int x0 = (696 * cell + 90) / 180;
        const int x1 = (696 * (cell + 1) + 90) / 180;
        out[static_cast<std::size_t>(cell)] = x0 + x1 - 1;
    }
    return out;
}

constexpr auto kNominalCenter2 = makeNominalCenter2();

struct PreparedGeometry
{
    double start = kNominalStart;
    double period = kNominalPeriod;
    std::array<int, 180> x{};
};

PreparedGeometry prepareGeometry(double start, double period)
{
    PreparedGeometry g{};
    g.start = start;
    g.period = period;
    const double factor = 0.5 * period / kNominalPeriod;
    for (int cell = 0; cell < 180; ++cell)
        g.x[static_cast<std::size_t>(cell)] = static_cast<int>(
            std::lround(start + static_cast<double>(kNominalCenter2[static_cast<std::size_t>(cell)]) * factor));
    return g;
}

bool evaluatePrepared(
    const std::uint16_t* line,
    int width,
    const PreparedGeometry& g,
    std::uint16_t& threshold,
    int& alt,
    int& guardErrors,
    int& syncErrors)
{
    std::array<std::uint16_t, 8> run{};
    std::uint16_t lo = std::numeric_limits<std::uint16_t>::max();
    std::uint16_t hi = 0;
    for (int i = 0; i < 8; ++i)
    {
        const int x = g.x[static_cast<std::size_t>(i)];
        if (x < 0 || x >= width) return false;
        run[static_cast<std::size_t>(i)] = line[x];
        lo = std::min(lo, line[x]);
        hi = std::max(hi, line[x]);
    }
    if (static_cast<int>(hi) - static_cast<int>(lo) < 40 * 257) return false;

    threshold = static_cast<std::uint16_t>(
        (static_cast<unsigned int>(lo) + static_cast<unsigned int>(hi)) / 2u);
    int a = 0, b = 0;
    for (int i = 0; i < 8; ++i)
    {
        const bool bit = run[static_cast<std::size_t>(i)] >= threshold;
        a += bit == ((i & 1) == 0);
        b += bit == ((i & 1) != 0);
    }
    alt = std::max(a, b);
    if (alt < 7) return false;

    guardErrors = 0;
    for (int i = 8; i < 12; ++i)
    {
        const int x = g.x[static_cast<std::size_t>(i)];
        if (x < 0 || x >= width) return false;
        guardErrors += line[x] >= threshold ? 1 : 0;
    }

    constexpr std::uint8_t sync = 0x2E;
    syncErrors = 0;
    for (int i = 0; i < 8; ++i)
    {
        const int x = g.x[static_cast<std::size_t>(12 + i)];
        if (x < 0 || x >= width) return false;
        const bool expect = ((sync >> (7 - i)) & 1u) != 0u;
        const bool got = line[x] >= threshold;
        syncErrors += got == expect ? 0 : 1;
    }
    return guardErrors <= 1 && syncErrors <= 1;
}

Row decodePrepared(const std::uint16_t* line, int width, const PreparedGeometry& g)
{
    Row out{};
    if (line == nullptr || width < 700) return out;

    std::uint16_t threshold = 0;
    int alt = 0, guardErrors = 0, syncErrors = 0;
    if (!evaluatePrepared(line, width, g, threshold, alt, guardErrors, syncErrors)) return out;

    out.start = g.start;
    out.period = g.period;
    std::array<std::uint8_t, 20> bytes{};
    for (int byte = 0; byte < 20; ++byte)
    {
        std::uint8_t v = 0;
        for (int bit = 0; bit < 8; ++bit)
        {
            const int cell = 20 + byte * 8 + bit;
            const int x = g.x[static_cast<std::size_t>(cell)];
            if (x < 0 || x >= width) return Row{};
            v = static_cast<std::uint8_t>((v << 1u) | (line[x] >= threshold ? 1u : 0u));
        }
        bytes[static_cast<std::size_t>(byte)] = v;
    }

    std::array<std::uint8_t, 13> core{};
    std::copy_n(bytes.begin(), 13, core.begin());
    int corrected = 0;
    if (!rsCorrect(core, corrected)) return out;
    const std::uint8_t h = core[0];
    const int n = h & 0x07;
    if ((h & 0xC0u) != 0xC0u || n > 4) return out;

    out.coreGood = true;
    out.correctedBytes = corrected;
    out.n = n;
    out.textMarker = (h & 0x20u) != 0u;
    out.textBits = (h >> 3u) & 0x03u;

    std::array<std::uint8_t, 8> corePlain{};
    for (int i = 0; i < 8; ++i)
        corePlain[static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>(core[static_cast<std::size_t>(i + 1)] ^ kKey[static_cast<std::size_t>(i)]);

    std::array<std::uint8_t, 6> refinement{};
    for (int i = 0; i < 6; ++i)
        refinement[static_cast<std::size_t>(i)] = bytes[static_cast<std::size_t>(13 + i)];
    out.refinementGood = crc8(refinement.data(), 6) == bytes[19];
    if (out.refinementGood)
    {
        for (int i = 0; i < 6; ++i)
            refinement[static_cast<std::size_t>(i)] ^= kKey[static_cast<std::size_t>(8 + i)];
    }
    else refinement.fill(0);

    std::array<std::uint8_t, 8> low6{};
    int bi = 0;
    for (int slot = 0; slot < 8; ++slot)
    {
        std::uint8_t v = 0;
        for (int bit = 0; bit < 6; ++bit, ++bi)
        {
            const int byte = bi / 8;
            const int shift = 7 - (bi % 8);
            v = static_cast<std::uint8_t>((v << 1u) |
                ((refinement[static_cast<std::size_t>(byte)] >> shift) & 1u));
        }
        low6[static_cast<std::size_t>(slot)] = v;
    }

    for (int slot = 0; slot < 8; ++slot)
    {
        const std::uint16_t u14 = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(corePlain[static_cast<std::size_t>(slot)]) << 6u) |
            low6[static_cast<std::size_t>(slot)]);
        int value = static_cast<int>(u14);
        if ((u14 & 0x2000u) != 0u) value -= 0x4000;
        out.samples[static_cast<std::size_t>(slot)] = static_cast<std::int16_t>(value << 2);
    }
    return out;
}

Row decodeRowSearch(
    const std::uint16_t* line,
    int width,
    double periodMin,
    double periodMax,
    double periodStepSize,
    double startMin,
    double startMax,
    double startStepSize)
{
    Row out{};
    if (line == nullptr || width < 700) return out;

    double bestMetric = -1.0e9;
    PreparedGeometry best{};
    bool haveBest = false;

    for (double period = periodMin; period <= periodMax + 1.0e-9; period += periodStepSize)
    {
        for (double start = startMin; start <= startMax + 1.0e-9; start += startStepSize)
        {
            const auto g = prepareGeometry(start, period);
            std::uint16_t threshold = 0;
            int alt = 0, guardErrors = 0, syncErrors = 0;
            if (!evaluatePrepared(line, width, g, threshold, alt, guardErrors, syncErrors)) continue;

            const double metric = alt * 4.0 - guardErrors * 3.0 - syncErrors * 5.0
                - std::abs(period - kNominalPeriod) * 12.0
                - std::abs(start - kNominalStart) * 0.08;
            if (metric > bestMetric)
            {
                bestMetric = metric;
                best = g;
                haveBest = true;
            }
        }
    }
    return haveBest ? decodePrepared(line, width, best) : out;
}

Row decodeRowAcquire(const std::uint16_t* line, int width)
{
    Row coarse = decodeRowSearch(
        line, width,
        kNominalPeriod - 0.40, kNominalPeriod + 0.40, 0.05,
        -4.0, 32.0, 1.0);
    if (!coarse.coreGood) return coarse;

    Row fine = decodeRowSearch(
        line, width,
        coarse.period - 0.06, coarse.period + 0.06, 0.01,
        coarse.start - 1.0, coarse.start + 1.0, 0.25);
    return fine.coreGood ? fine : coarse;
}

Row decodeRowRecover(const std::uint16_t* line, int width, double startHint, double periodHint)
{
    // Only a failed exact lock pays for a small neighbourhood search.
    return decodeRowSearch(
        line, width,
        periodHint - 0.03, periodHint + 0.03, 0.01,
        startHint - 0.75, startHint + 0.75, 0.25);
}

bool expectedAudioRow(int row)
{
    if (row < 0 || row > 575) return false;
    if (row == 574 || row == 575) return false;
    return row < 260 || row > 291;
}
}

bool HamPcmV2Decoder::probeLuma(
    const std::vector<std::uint16_t>& y,
    int width,
    int height,
    bool inputSignalValid) const
{
    if (!inputSignalValid || width < 720 || height < 576 ||
        y.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height))
        return false;

    // Two widely separated normal audio rows are enough: decodeRowAcquire()
    // validates run-in/guard/sync and the RS-protected core. Requiring both
    // makes accidental detection on Sony/video vanishingly unlikely, while
    // costing only two row acquisitions instead of a full 542-row search.
    constexpr std::array<int, 2> kProbeRows{96, 384};
    for (const int row : kProbeRows)
    {
        const auto* line = y.data() +
            static_cast<std::size_t>(row) * static_cast<std::size_t>(width);
        if (!decodeRowAcquire(line, width).coreGood)
            return false;
    }
    return true;
}

void HamPcmV2Decoder::reset()
{
    textCollecting_ = false;
    textRows_ = 0;
    textBits_ = 0;
    text_.clear();
    geometryHintValid_ = false;
    geometryStartHint_ = kNominalStart;
    geometryPeriodHint_ = kNominalPeriod;
}

HamPcmV2Decoder::Result HamPcmV2Decoder::processLuma(
    const std::vector<std::uint16_t>& y,
    int width,
    int height,
    bool inputSignalValid)
{
    Result result{};
    if (!inputSignalValid || width != 720 || height < 576 ||
        y.size() < static_cast<std::size_t>(width * height))
        return result;

    struct Entry { Row row; int field = 0; };
    std::vector<Entry> ordered;
    ordered.reserve(542);
    std::array<int, 2> validByField{};
    std::array<int, 2> testedByField{};
    std::array<int, 2> pairsByField{};
    int valid = 0, refinementGood = 0, refinementTested = 0, correctedRows = 0;
    double periodSum = 0.0, startSum = 0.0;
    bool geometryLocked = geometryHintValid_;
    double trackStart = geometryHintValid_ ? geometryStartHint_ : kNominalStart;
    double trackPeriod = geometryHintValid_ ? geometryPeriodHint_ : kNominalPeriod;
    PreparedGeometry trackGeometry = prepareGeometry(trackStart, trackPeriod);

    for (int field = 0; field < 2; ++field)
    {
        for (int row = field; row <= 575; row += 2)
        {
            if (!expectedAudioRow(row)) continue;
            ++testedByField[static_cast<std::size_t>(field)];
            const auto* line = y.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(width);
            Row decoded = geometryLocked
                ? decodePrepared(line, width, trackGeometry)
                : decodeRowAcquire(line, width);
            if (!decoded.coreGood && geometryLocked)
                decoded = decodeRowRecover(line, width, trackStart, trackPeriod);
            if (!decoded.coreGood && geometryLocked)
                decoded = decodeRowAcquire(line, width);
            if (decoded.coreGood)
            {
                geometryLocked = true;
                if (decoded.start != trackStart || decoded.period != trackPeriod)
                {
                    trackStart = decoded.start;
                    trackPeriod = decoded.period;
                    trackGeometry = prepareGeometry(trackStart, trackPeriod);
                }
            }
            ordered.push_back({decoded, field});
            if (!decoded.coreGood) continue;
            ++valid; ++validByField[static_cast<std::size_t>(field)];
            correctedRows += decoded.correctedBytes > 0 ? 1 : 0;
            ++refinementTested;
            refinementGood += decoded.refinementGood ? 1 : 0;
            periodSum += decoded.period; startSum += decoded.start;
        }
    }

    if (valid < 32)
    {
        geometryHintValid_ = false;
        return result;
    }

    // Carry the proven geometry into the next frame. Normal steady-state frames
    // then need no acquisition search at all.
    geometryHintValid_ = geometryLocked;
    geometryStartHint_ = trackStart;
    geometryPeriodHint_ = trackPeriod;

    constexpr char alphabet[] =
        " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,-/:;!?'()+=&@#*%<>\"_[]{}|";

    for (const auto& e : ordered)
    {
        const auto& row = e.row;
        if (!row.coreGood)
        {
            textCollecting_ = false; textRows_ = 0; textBits_ = 0;
            continue;
        }

        if (row.textMarker)
        {
            textCollecting_ = true; textRows_ = 0; textBits_ = 0;
        }
        if (textCollecting_)
        {
            textBits_ = (textBits_ << 2u) | static_cast<std::uint64_t>(row.textBits & 0x03);
            ++textRows_;
            if (textRows_ == 30)
            {
                std::string decoded;
                decoded.reserve(10);
                for (int ch = 0; ch < 10; ++ch)
                {
                    const int shift = 54 - ch * 6;
                    decoded.push_back(alphabet[(textBits_ >> shift) & 0x3Fu]);
                }
                while (!decoded.empty() && decoded.back() == ' ') decoded.pop_back();
                text_ = decoded;
                textCollecting_ = false; textRows_ = 0; textBits_ = 0;
            }
        }

        pairsByField[static_cast<std::size_t>(e.field)] += row.n;
        for (int pair = 0; pair < row.n; ++pair)
        {
            result.audioStereo.push_back(row.samples[static_cast<std::size_t>(pair * 2)]);
            result.audioStereo.push_back(row.samples[static_cast<std::size_t>(pair * 2 + 1)]);
        }
    }

    result.locked = true;
    result.validLines = valid;
    result.testedLines = testedByField[0] + testedByField[1];
    result.crcPercent = refinementTested > 0
        ? 100.0 * static_cast<double>(refinementGood) / refinementTested : 0.0;
    result.bitPeriodPixels = valid > 0 ? periodSum / valid : kNominalPeriod;
    result.syncStartPixels = valid > 0 ? startSum / valid : kNominalStart;
    result.field1ValidAudioLines = validByField[0];
    result.field1TestedAudioLines = testedByField[0];
    result.field2ValidAudioLines = validByField[1];
    result.field2TestedAudioLines = testedByField[1];
    result.field1AudioPairs = pairsByField[0];
    result.field2AudioPairs = pairsByField[1];
    result.correctedRows = static_cast<std::uint64_t>(correctedRows);
    result.refinementFallbackRows = static_cast<std::uint64_t>(refinementTested - refinementGood);
    result.hardUncorrectableRows = static_cast<std::uint64_t>(result.testedLines - valid);
    result.text = text_;
    return result;
}
