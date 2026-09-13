#include "Pcm16VideoEncoder.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <QImage>
#include <QPainter>
#include <QFont>

void Pcm16VideoEncoder::reset()
{
    history_.clear();

    Group silence{};
    for (int i = 0; i < 112; ++i)
        history_.push_back(silence);

    firstGroup_ = -112;
    frame_ = 0;
    preEmphasisStateEnabled_ = false;
    preX1_ = {};
    preY1_ = {};
}

Pcm16VideoEncoder::Group Pcm16VideoEncoder::makeGroup(const std::int16_t* s) const
{
    Group g{};
    for (int i = 0; i < 6; ++i)
        g[i] = static_cast<std::uint16_t>(s[i]);
    g[6] = static_cast<std::uint16_t>(g[0]^g[1]^g[2]^g[3]^g[4]^g[5]);
    return g;
}

Pcm16VideoEncoder::Group Pcm16VideoEncoder::groupAt(std::int64_t g) const
{
    if (g < firstGroup_ || g >= firstGroup_ + static_cast<std::int64_t>(history_.size()))
        return {};
    return history_[static_cast<std::size_t>(g-firstGroup_)];
}

// GF(2) companion operation for the EIAJ 14-bit Q code.
// Generator/primitive polynomial: x^14 + x^8 + 1.
std::uint16_t Pcm16VideoEncoder::gfMulX14(std::uint16_t v)
{
    v &= 0x3FFFu;
    const bool carry = (v & 0x2000u) != 0;
    v = static_cast<std::uint16_t>((v << 1) & 0x3FFFu);
    if (carry)
        v ^= 0x0101u; // x^8 + 1
    return v;
}

std::uint16_t Pcm16VideoEncoder::q14(const Group& g)
{
    // EIAJ: Q = T^6 W1 + T^5 W2 + ... + T W6 (mod 2).
    std::uint16_t q = 0;
    for (int i = 0; i < 6; ++i)
    {
        std::uint16_t v = static_cast<std::uint16_t>((g[i] >> 2) & 0x3FFFu);
        for (int power = 6 - i; power > 0; --power)
            v = gfMulX14(v);
        q ^= v;
    }
    return static_cast<std::uint16_t>(q & 0x3FFFu);
}

std::array<std::uint16_t,8> Pcm16VideoEncoder::physicalWords16(std::int64_t g) const
{
    const auto g0=groupAt(g),g1=groupAt(g-16),g2=groupAt(g-32),g3=groupAt(g-48),
               g4=groupAt(g-64),g5=groupAt(g-80),gp=groupAt(g-96),gq=groupAt(g-112);

    std::uint16_t lsbPack = 0;
    for (int i = 0; i < 7; ++i)
        lsbPack = static_cast<std::uint16_t>((lsbPack << 2) | (gq[i] & 3u));

    return {
        static_cast<std::uint16_t>(g0[0]>>2),
        static_cast<std::uint16_t>(g1[1]>>2),
        static_cast<std::uint16_t>(g2[2]>>2),
        static_cast<std::uint16_t>(g3[3]>>2),
        static_cast<std::uint16_t>(g4[4]>>2),
        static_cast<std::uint16_t>(g5[5]>>2),
        static_cast<std::uint16_t>(gp[6]>>2),
        static_cast<std::uint16_t>(lsbPack & 0x3FFFu)
    };
}

std::array<std::uint16_t,8> Pcm16VideoEncoder::physicalWords14(std::int64_t g) const
{
    const auto g0=groupAt(g),g1=groupAt(g-16),g2=groupAt(g-32),g3=groupAt(g-48),
               g4=groupAt(g-64),g5=groupAt(g-80),gp=groupAt(g-96),gq=groupAt(g-112);

    const auto upper14 = [](std::uint16_t v) {
        return static_cast<std::uint16_t>((v >> 2) & 0x3FFFu);
    };

    const std::uint16_t p14 = static_cast<std::uint16_t>(
        upper14(gp[0]) ^ upper14(gp[1]) ^ upper14(gp[2]) ^
        upper14(gp[3]) ^ upper14(gp[4]) ^ upper14(gp[5]));

    return {
        upper14(g0[0]), upper14(g1[1]), upper14(g2[2]), upper14(g3[3]),
        upper14(g4[4]), upper14(g5[5]), p14, q14(gq)
    };
}

std::array<std::uint16_t,8> Pcm16VideoEncoder::controlWords(bool mode16, bool preEmphasis) const
{
    // 56-bit heading = 1100 repeated 14 times. Because 14-bit word
    // boundaries are not multiples of four, the words alternate 0x3333/0x0CCC.
    // Content ID and address are zero for this test encoder.
    // CT bits 1..10 = 0, 11 copy inhibit absent = 0, 12 P present = 0,
    // 13 Q present=0 / absent=1, 14 pre-emphasis present=0 / absent=1.
    const std::uint16_t control = static_cast<std::uint16_t>(
        (mode16 ? 0x0002u : 0x0000u) |
        (preEmphasis ? 0x0000u : 0x0001u));

    return {0x3333u,0x0CCCu,0x3333u,0x0CCCu,0u,0u,0u,control};
}

std::uint16_t Pcm16VideoEncoder::crc(const std::array<std::uint16_t,8>& w)
{
    std::array<int,128> b{};
    int k=0;
    for(auto v:w)
        for(int i=13;i>=0;--i)
            b[k++]=(v>>i)&1;
    for(int i=0;i<16;++i)
        b[i]^=1;
    for(int i=0;i<112;++i)
        if(b[i])
        {
            b[i]^=1;
            b[i+4]^=1;
            b[i+11]^=1;
            b[i+16]^=1;
        }
    std::uint16_t r=0;
    for(int i=112;i<128;++i)
        r=static_cast<std::uint16_t>((r<<1)|b[i]);
    return r;
}

void Pcm16VideoEncoder::shapeLuma(std::vector<float>& y, double bandwidthMHz)
{
    if (y.empty())
        return;

    // Restore the original 0.2.4 pulse shaper: two forward/reverse one-pole
    // pairs. It is symmetric (zero phase), keeps bit centres in place and
    // produced the cleaner eye/plateaus seen in the early encoder tests.
    constexpr double SampleRateMHz = 13.5;
    constexpr double Pi = 3.14159265358979323846;
    const double fc = std::clamp(bandwidthMHz, 1.0, 6.0);
    const double a = std::exp(-2.0 * Pi * fc / SampleRateMHz);

    auto forward = [&](bool reverse)
    {
        if (!reverse)
        {
            double state = y.front();
            for (std::size_t x = 1; x < y.size(); ++x)
            {
                state = (1.0-a)*static_cast<double>(y[x]) + a*state;
                y[x] = static_cast<float>(state);
            }
        }
        else
        {
            double state = y.back();
            for (std::size_t x = y.size()-1; x-- > 0; )
            {
                state = (1.0-a)*static_cast<double>(y[x]) + a*state;
                y[x] = static_cast<float>(state);
            }
        }
    };

    forward(false);
    forward(true);
    forward(false);
    forward(true);
}

void Pcm16VideoEncoder::applyPreEmphasis(std::vector<std::int16_t>& stereo, bool enabled)
{
    if (!enabled)
    {
        if (preEmphasisStateEnabled_)
        {
            preX1_ = {};
            preY1_ = {};
        }
        preEmphasisStateEnabled_ = false;
        return;
    }

    if (!preEmphasisStateEnabled_)
    {
        preX1_ = {};
        preY1_ = {};
        preEmphasisStateEnabled_ = true;
    }

    // Standard PCM adaptor 50/15 us pre-emphasis, bilinear transformed at
    // 44.1 kHz. H(s) = (1 + s*50us) / (1 + s*15us).
    constexpr double Fs = 44100.0;
    constexpr double T1 = 50.0e-6;
    constexpr double T2 = 15.0e-6;
    constexpr double K = 2.0 * Fs;
    constexpr double a0 = 1.0 + K*T2;
    constexpr double b0 = (1.0 + K*T1) / a0;
    constexpr double b1 = (1.0 - K*T1) / a0;
    constexpr double a1 = (1.0 - K*T2) / a0;

    for (std::size_t i = 0; i + 1 < stereo.size(); i += 2)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            const double x = static_cast<double>(stereo[i+static_cast<std::size_t>(ch)]) / 32768.0;
            const double y = b0*x + b1*preX1_[ch] - a1*preY1_[ch];
            preX1_[ch] = x;
            preY1_[ch] = y;

            const long long q = std::llround(std::clamp(y, -1.0, 32767.0/32768.0) * 32768.0);
            stereo[i+static_cast<std::size_t>(ch)] = static_cast<std::int16_t>(
                std::clamp(q,
                    static_cast<long long>(std::numeric_limits<std::int16_t>::min()),
                    static_cast<long long>(std::numeric_limits<std::int16_t>::max())));
        }
    }
}

void Pcm16VideoEncoder::renderLine(std::uint8_t* row, const std::array<std::uint16_t,8>& w) const
{
    std::array<int,140> cells{};
    cells[0]=1; cells[1]=0; cells[2]=1; cells[3]=0;
    int k=4;
    for(auto v:w)
        for(int i=13;i>=0;--i)
            cells[k++]=(v>>i)&1;

    const auto c=crc(w);
    for(int i=15;i>=0;--i)
        cells[k++]=(c>>i)&1;
    cells[k++]=0;
    for(int i=0;i<7;++i)
        cells[k++]=1;

    std::array<float, Width> original{};
    for(int x=0;x<Width;++x)
        original[static_cast<std::size_t>(x)] = cells[static_cast<std::size_t>(std::min(139,x*140/720))] ? 220.0f : 32.0f;

    const int offset=std::clamp(horizontalOffsetPixels_.load(),-24,24);
    constexpr int Guard=64;
    constexpr float BlackLevel=32.0f;
    std::vector<float> extended(static_cast<std::size_t>(Width+2*Guard),BlackLevel);

    for(int x=0;x<Width;++x)
    {
        const int destination=Guard+x+offset;
        if(destination>=0 && destination<static_cast<int>(extended.size()))
            extended[static_cast<std::size_t>(destination)]=original[static_cast<std::size_t>(x)];
    }

    if(pulseShapingEnabled_.load())
        shapeLuma(extended,videoBandwidthMHz_.load());

    for(int x=0;x<Width;x+=2)
    {
        const auto sampleAt=[&](int activeX)
        {
            const auto value=extended[static_cast<std::size_t>(Guard+activeX)];
            return static_cast<std::uint8_t>(std::clamp(std::lround(value),0L,255L));
        };
        row[2*x]=128; row[2*x+1]=sampleAt(x);
        row[2*x+2]=128; row[2*x+3]=sampleAt(x+1);
    }
}

void Pcm16VideoEncoder::fillBlackLine(std::uint8_t* row)
{
    for(int x=0;x<Width;x+=2)
    {
        row[2*x]=128; row[2*x+1]=16;
        row[2*x+2]=128; row[2*x+3]=16;
    }
}


void Pcm16VideoEncoder::setHamText(const std::string& text)
{
    std::lock_guard lock(hamTextMutex_);
    hamText_ = text.substr(0, 10);
}

std::string Pcm16VideoEncoder::hamText() const
{
    std::lock_guard lock(hamTextMutex_);
    return hamText_;
}

std::uint8_t Pcm16VideoEncoder::hamGfMul(std::uint8_t a, std::uint8_t b)
{
    std::uint16_t aa = a, bb = b, r = 0;
    while (bb != 0)
    {
        if (bb & 1u) r ^= aa;
        bb >>= 1;
        aa <<= 1;
        if (aa & 0x100u) aa ^= 0x11Du;
    }
    return static_cast<std::uint8_t>(r);
}

std::array<std::uint8_t,4> Pcm16VideoEncoder::hamRsParity(const std::array<std::uint8_t,9>& message)
{
    constexpr std::array<std::uint8_t,5> g{1,15,54,120,64};
    std::array<std::uint8_t,4> rem{};
    for (const auto d : message)
    {
        const auto fb = static_cast<std::uint8_t>(d ^ rem[0]);
        rem = {rem[1], rem[2], rem[3], 0};
        if (fb != 0)
            for (int j=0; j<4; ++j)
                rem[static_cast<std::size_t>(j)] ^= hamGfMul(g[static_cast<std::size_t>(j+1)], fb);
    }
    return rem;
}

std::uint8_t Pcm16VideoEncoder::hamCrc8(const std::uint8_t* data, std::size_t size)
{
    std::uint8_t c=0;
    for (std::size_t i=0;i<size;++i)
    {
        c ^= data[i];
        for (int b=0;b<8;++b)
            c = (c & 0x80u) ? static_cast<std::uint8_t>((c << 1) ^ 0x07u)
                            : static_cast<std::uint8_t>(c << 1);
    }
    return c;
}

int Pcm16VideoEncoder::hamTextIndex(char ch)
{
    static constexpr char alphabet[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,-/:;!?'()+=&@#*%<>\"_[]{}|";
    const char u = (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 'a' + 'A') : ch;
    for (int i=0; alphabet[i] != '\0'; ++i)
        if (alphabet[i] == u) return i;
    return 0;
}

std::array<std::uint8_t,20> Pcm16VideoEncoder::makeHamPayload(
    const std::int16_t* stereoPairs, int n, bool marker, std::uint8_t textBits) const
{
    constexpr std::array<std::uint8_t,14> key{0x87,0x23,0x46,0xDC,0xB0,0xDD,0xEE,0xF8,0xFD,0xC3,0x5C,0xBF,0x5C,0x53};
    std::array<std::uint8_t,20> out{};
    out[0] = static_cast<std::uint8_t>(0xC0u | (marker ? 0x20u : 0u) | ((textBits & 0x03u) << 3) | (n & 0x07));

    std::array<std::uint8_t,8> core{};
    std::array<std::uint8_t,8> refinement{};
    for (int pair=0;pair<4;++pair)
    {
        for (int ch=0;ch<2;++ch)
        {
            const int slot=pair*2+ch;
            std::int16_t src=0;
            if (pair<n && stereoPairs!=nullptr) src=stereoPairs[pair*2+ch];
            const std::uint16_t s14=static_cast<std::uint16_t>((static_cast<std::int32_t>(src) >> 2) & 0x3FFF);
            core[static_cast<std::size_t>(slot)] = static_cast<std::uint8_t>((s14 >> 6) & 0xFFu);
            refinement[static_cast<std::size_t>(slot)] = static_cast<std::uint8_t>(s14 & 0x3Fu);
            out[1+slot] = static_cast<std::uint8_t>(core[static_cast<std::size_t>(slot)] ^ key[static_cast<std::size_t>(slot)]);
        }
    }

    std::uint64_t packed=0;
    for (int i=0;i<8;++i) packed=(packed<<6) | refinement[static_cast<std::size_t>(i)];
    std::array<std::uint8_t,6> e{};
    for (int j=0;j<6;++j)
    {
        e[static_cast<std::size_t>(j)] = static_cast<std::uint8_t>((packed >> (40-8*j)) & 0xFFu);
        e[static_cast<std::size_t>(j)] ^= key[static_cast<std::size_t>(8+j)];
    }

    std::array<std::uint8_t,9> msg{};
    for (int i=0;i<9;++i) msg[static_cast<std::size_t>(i)] = out[static_cast<std::size_t>(i)];
    const auto parity=hamRsParity(msg);
    for (int i=0;i<4;++i) out[9+i]=parity[static_cast<std::size_t>(i)];
    for (int j=0;j<6;++j) out[13+j]=e[static_cast<std::size_t>(j)];
    out[19]=hamCrc8(e.data(),e.size());
    return out;
}

void Pcm16VideoEncoder::renderHamLine(std::uint8_t* row, int frameRow, const std::array<std::uint8_t,20>& payload) const
{
    std::array<int,180> bits{};
    // Direct 576-row mapping used by the OpenScope/Intensity Pro 4K loopback:
    // frame rows 0/1 have one run-in polarity, 2/3 the opposite, then repeat.
    // This is exactly ((frameRow >> 1) & 1) from the Ham PCM v3 line-code spec.
    const bool invert = ((frameRow >> 1) & 1) != 0;
    for (int i=0;i<8;++i) bits[static_cast<std::size_t>(i)] = ((i & 1) == 0) ^ invert;
    for (int i=8;i<12;++i) bits[static_cast<std::size_t>(i)] = 0;
    constexpr std::uint8_t sync=0x2E;
    for (int i=0;i<8;++i) bits[static_cast<std::size_t>(12+i)] = (sync >> (7-i)) & 1u;
    int k=20;
    for (const auto byte:payload)
        for (int b=7;b>=0;--b) bits[static_cast<std::size_t>(k++)]=(byte>>b)&1u;

    // Experimental robust Ham PCM raster: 3 BT.601 pixels per bit.
    // 180 * 3 = 540 active code pixels, centred in the 720-pixel row so
    // neither the run-in nor CRC touches the capture-window edges.
    constexpr int HamPixelsPerBit = 3;
    constexpr int HamCodePixels = 180 * HamPixelsPerBit;
    constexpr int HamLeftMargin = (Width - HamCodePixels) / 2; // 90 px
    constexpr int Guard=64;
    constexpr float Black=16.0f, White=235.0f;
    std::vector<float> extended(static_cast<std::size_t>(Width+2*Guard),Black);
    const int offset=std::clamp(horizontalOffsetPixels_.load(),-24,24);
    for (int x=0;x<HamCodePixels;++x)
    {
        const int destination=Guard+HamLeftMargin+x+offset;
        if (destination>=0 && destination<static_cast<int>(extended.size()))
            extended[static_cast<std::size_t>(destination)] =
                bits[static_cast<std::size_t>(x/HamPixelsPerBit)] ? White : Black;
    }
    if (pulseShapingEnabled_.load()) shapeLuma(extended,videoBandwidthMHz_.load());
    for (int x=0;x<Width;x+=2)
    {
        const auto y0=static_cast<std::uint8_t>(std::clamp(std::lround(extended[static_cast<std::size_t>(Guard+x)]),0L,255L));
        const auto y1=static_cast<std::uint8_t>(std::clamp(std::lround(extended[static_cast<std::size_t>(Guard+x+1)]),0L,255L));
        row[2*x]=128; row[2*x+1]=y0; row[2*x+2]=128; row[2*x+3]=y1;
    }
}

void Pcm16VideoEncoder::renderHamCaptionBand(std::vector<std::uint8_t>& out, const std::string& text) const
{
    QImage image(Width, 32, QImage::Format_Grayscale8);
    image.fill(16);
    QPainter painter(&image);
    painter.setPen(QColor(235,235,235));
    QFont font(QStringLiteral("Arial"));
    font.setPixelSize(22);
    font.setBold(true);
    painter.setFont(font);
    painter.drawText(QRect(0,0,Width,32), Qt::AlignCenter, QString::fromStdString(text));
    painter.end();

    for (int y=0;y<32;++y)
    {
        auto* row=out.data()+(260+y)*Width*2;
        const auto* src=image.constScanLine(y);
        for (int x=0;x<Width;x+=2)
        {
            row[2*x]=128; row[2*x+1]=src[x];
            row[2*x+2]=128; row[2*x+3]=src[x+1];
        }
    }
}

void Pcm16VideoEncoder::encodeHamFrame(const std::vector<std::int16_t>& stereo, std::vector<std::uint8_t>& out)
{
    // Build the native Ham raster first, then apply the same full-frame offset
    // semantics as Sony/EIAJ.  Offset 0 is direct 1:1 row mapping.
    std::vector<std::uint8_t> logical(Width*Height*2,0);
    for (int r=0;r<Height;++r) fillBlackLine(logical.data()+r*Width*2);

    std::string text;
    { std::lock_guard lock(hamTextMutex_); text=hamText_; }
    text.resize(10,' ');
    renderHamCaptionBand(logical, text);

    std::uint64_t text60=0;
    for (char ch:text) text60=(text60<<6) | static_cast<std::uint64_t>(hamTextIndex(ch)&0x3F);

    // OpenScope/Intensity Pro 4K loopback is measured 1:1 in active-row numbering:
    // transmitted frame row 260 is captured as row 260.  Therefore Ham PCM starts
    // directly at frame row 0; there is no hidden one-line capture shift here.
    // Each field has 288 active rows, 16 of which are the caption band, leaving
    // 272 structured PCM rows.  128*3 + 144*4 = 960 stereo pairs/field, exactly
    // 48 kHz locked to the 50 Hz field clock.
    constexpr int FieldRows = Height/2;             // 288
    constexpr int AudioRowsPerField = 272;
    constexpr int PairsPerField = HamSamplesPerFrame/2; // 960
    constexpr int FourPairRowsPerField = PairsPerField - AudioRowsPerField*3; // 144
    static_assert(FourPairRowsPerField == 144);

    int samplePair=0;

    auto emitStructured=[&](int frameRow,int n)
    {
        const int cyclePos=hamTextRow_%30;
        const bool marker=(cyclePos==0);
        const int shift=58-2*cyclePos;
        const auto textBits=static_cast<std::uint8_t>((text60>>shift)&0x03u);
        const std::int16_t* ptr=(n>0 && samplePair*2<static_cast<int>(stereo.size())) ? stereo.data()+samplePair*2 : nullptr;
        const auto payload=makeHamPayload(ptr,n,marker,textBits);
        renderHamLine(logical.data()+frameRow*Width*2, frameRow, payload);
        samplePair += n;
        ++hamTextRow_;
    };

    // Generator/audio order remains field 1 first, then field 2.  Raster placement
    // itself is direct frame-row placement (0..575); only the 3/4-pair scheduler is
    // reset per field.  The protected text stream continues across structured rows.
    for (int fld=0;fld<2;++fld)
    {
        int extraAccumulator=0;
        int fieldPairs=0;

        for (int fieldRow=0;fieldRow<FieldRows;++fieldRow)
        {
            const int frameRow=fieldRow*2+fld;

            if (frameRow>=260 && frameRow<=291)
                continue; // visible caption band: deliberately no run-in

            int n=3;
            extraAccumulator += FourPairRowsPerField;
            if (extraAccumulator>=AudioRowsPerField)
            {
                ++n;
                extraAccumulator-=AudioRowsPerField;
            }

            const int remaining=PairsPerField-fieldPairs;
            n=std::clamp(n,0,std::min(4,remaining));
            emitStructured(frameRow,n);
            fieldPairs += n;
        }

        // The field scheduler is intentionally exact: never let one field borrow
        // samples from the other, because Ham PCM is locked to 960 pairs/field.
        if (fieldPairs != PairsPerField)
        {
            // Defensive only; constants above are chosen so this cannot fire.
            // Leave the transport structurally valid rather than crossing fields.
        }
    }

    // Apply the exact same vertical offset convention as the Sony PCM path.
    // A positive full-frame offset skips logical rows at the top of each field;
    // the vacated rows at the bottom are black. Odd offsets are split between
    // the two fields in the same way as Sony/EIAJ.
    out.assign(Width*Height*2,0);
    for (int r=0;r<Height;++r) fillBlackLine(out.data()+r*Width*2);

    const int frameOffset=std::clamp(pcmFrameOffset_.load(),0,12);
    for (int fld=0;fld<2;++fld)
    {
        const int fieldSkip=(frameOffset + (fld==0 ? 1 : 0))/2;
        for (int outFieldRow=0; outFieldRow<FieldRows; ++outFieldRow)
        {
            const int logicalFieldRow=outFieldRow + fieldSkip;
            if (logicalFieldRow>=FieldRows)
                continue;

            const int dstFrameRow=outFieldRow*2+fld;
            const int srcFrameRow=logicalFieldRow*2+fld;
            std::memcpy(out.data()+dstFrameRow*Width*2,
                        logical.data()+srcFrameRow*Width*2,
                        Width*2);
        }
    }
}

void Pcm16VideoEncoder::encodeFrame(const std::vector<std::int16_t>& stereoIn,std::vector<std::uint8_t>& out)
{
    if (isHamPcmMode())
    {
        encodeHamFrame(stereoIn, out);
        ++frame_;
        return;
    }
    out.assign(Width*Height*2,0);
    const auto base=static_cast<std::int64_t>(frame_)*588;

    // Snapshot format state before touching audio. This single snapshot is
    // used for pre-emphasis, control-H and all PCM data lines in this frame.
    const bool mode16=is16BitMode();
    const bool framePreEmphasis=preEmphasisEnabled_.load();

    std::vector<std::int16_t> stereo=stereoIn;
    applyPreEmphasis(stereo, framePreEmphasis);

    for(int g=0;g<588;++g)
    {
        std::int16_t six[6]{};
        for(int j=0;j<3;++j)
        {
            const auto si=(g*3+j)*2;
            if(si+1<static_cast<int>(stereo.size()))
            {
                six[2*j]=stereo[static_cast<std::size_t>(si)];
                six[2*j+1]=stereo[static_cast<std::size_t>(si+1)];
            }
        }
        history_.push_back(makeGroup(six));
    }

    if(history_.size()>2048)
    {
        const auto drop=history_.size()-1024;
        history_.erase(history_.begin(),history_.begin()+static_cast<std::ptrdiff_t>(drop));
        firstGroup_+=static_cast<std::int64_t>(drop);
    }

    // Vertical mapping is expressed as a full-frame offset, not as a per-field
    // logical PCM line number.  Sony/BMD mapping is +12 frame lines = +6 lines
    // per field, so logical PCM lines 7..294 exactly fill each 288-line field.
    const int frameOffset=std::clamp(pcmFrameOffset_.load(),0,12);
    const bool mudControlVisible=(frameOffset==0);

    for(int fld=0;fld<2;++fld)
    {
        // Preserve a meaningful intermediate slider: an odd full-frame offset
        // is split across the two interlaced fields, with field 0 taking the
        // extra line.  The two field skips always add up to frameOffset.
        const int fieldSkip=(frameOffset + (fld==0 ? 1 : 0))/2;

        for(int r=0;r<288;++r)
        {
            auto* row=out.data()+((2*r+fld)*Width*2);

            if(mudControlVisible && r==0)
            {
                renderLine(row,controlWords(mode16, framePreEmphasis));
                continue;
            }

            const int audioRow = r - (mudControlVisible ? 1 : 0);
            const int logicalLine = 1 + fieldSkip + audioRow; // 1..294

            if(audioRow>=0 && logicalLine>=1 && logicalLine<=294)
            {
                const auto physical=base+fld*294+(logicalLine-1);
                renderLine(row,mode16 ? physicalWords16(physical) : physicalWords14(physical));
            }
            else
            {
                fillBlackLine(row);
            }
        }
    }

    ++frame_;
}
