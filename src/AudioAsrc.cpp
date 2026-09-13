#include "AudioAsrc.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
constexpr double Pi = 3.1415926535897932384626433832795;

double sinc(double x)
{
    if (std::abs(x) < 1.0e-12)
        return 1.0;

    const double pix = Pi * x;
    return std::sin(pix) / pix;
}

float clampToFloatSample(double x)
{
    return static_cast<float>(std::clamp(x, -1.0, 0.999969482421875));
}
}

void AudioAsrc::reset(double inputRate)
{
    fifo_.clear();
    inputRate_ = inputRate > 1.0 ? inputRate : 48000.0;
    sourcePos_ = HalfTaps;
    correctionPpm_ = 0.0;
    filteredError_ = 0.0;
    underruns_ = 0;

    // Pre-roll with silence so the symmetric FIR has valid history from the
    // very first real sample.
    for (int i = 0; i < HalfTaps; ++i)
    {
        fifo_.push_back(0.0f);
        fifo_.push_back(0.0f);
    }
}

void AudioAsrc::pushInterleavedFloat(const float* stereo, std::size_t frames)
{
    if (stereo == nullptr || frames == 0)
        return;

    for (std::size_t i = 0; i < frames * 2; ++i)
        fifo_.push_back(stereo[i]);

    // Hard safety cap only. Normal clock matching is done by the ASRC servo.
    const std::size_t maxFrames = static_cast<std::size_t>(inputRate_ * 0.25);
    while (fifo_.size() / 2 > maxFrames)
    {
        fifo_.pop_front();
        fifo_.pop_front();
        sourcePos_ = std::max(0.0, sourcePos_ - 1.0);
    }
}

float AudioAsrc::interpolate(int channel, double position) const
{
    const auto frameCount = static_cast<std::ptrdiff_t>(fifo_.size() / 2);
    const auto center = static_cast<std::ptrdiff_t>(std::floor(position));
    const double frac = position - static_cast<double>(center);

    // For down-conversion the reconstruction filter must also reject content
    // above the new Nyquist frequency. Keep a little transition band.
    const double rateRatio = OutputRate / inputRate_;
    const double fc = 0.5 * std::min(1.0, rateRatio) * 0.95; // cycles/input sample

    double sum = 0.0;
    double norm = 0.0;

    for (int tap = -HalfTaps + 1; tap <= HalfTaps; ++tap)
    {
        const std::ptrdiff_t index = center + tap;
        if (index < 0 || index >= frameCount)
            continue;

        const double x = static_cast<double>(tap) - frac;
        const double ideal = 2.0 * fc * sinc(2.0 * fc * x);

        // Hann window across the 32-tap finite sinc.
        const double windowPosition =
            (x + static_cast<double>(HalfTaps)) /
            (2.0 * static_cast<double>(HalfTaps));
        const double window =
            (windowPosition >= 0.0 && windowPosition <= 1.0)
                ? 0.5 - 0.5 * std::cos(2.0 * Pi * windowPosition)
                : 0.0;

        const double h = ideal * window;
        sum += static_cast<double>(fifo_[static_cast<std::size_t>(index) * 2 + channel]) * h;
        norm += h;
    }

    if (std::abs(norm) > 1.0e-12)
        sum /= norm;

    return clampToFloatSample(sum);
}

void AudioAsrc::trimConsumed()
{
    // Keep HalfTaps frames behind sourcePos_ for the symmetric FIR.
    const auto removable =
        static_cast<std::ptrdiff_t>(std::floor(sourcePos_)) - HalfTaps;

    if (removable <= 0)
        return;

    const auto framesToRemove =
        std::min<std::size_t>(
            static_cast<std::size_t>(removable),
            fifo_.size() / 2);

    for (std::size_t i = 0; i < framesToRemove; ++i)
    {
        fifo_.pop_front();
        fifo_.pop_front();
    }

    sourcePos_ -= static_cast<double>(framesToRemove);
}

std::vector<std::int16_t> AudioAsrc::takeInt16(std::size_t requestedFrames)
{
    std::vector<std::int16_t> out(requestedFrames * 2, 0);

    const auto queued = static_cast<double>(queuedInputFrames());

    // Slow elastic clock servo. Positive FIFO error means consume the input
    // clock very slightly faster. The filtering prevents audible modulation.
    const double rawError = queued - static_cast<double>(TargetFrames);
    filteredError_ = filteredError_ * 0.995 + rawError * 0.005;

    const double desiredPpm =
        std::clamp(filteredError_, -MaxCorrectionPpm, MaxCorrectionPpm);
    correctionPpm_ = correctionPpm_ * 0.995 + desiredPpm * 0.005;

    const double nominalStep = inputRate_ / OutputRate;
    const double step =
        nominalStep * (1.0 + correctionPpm_ * 1.0e-6);

    bool starved = false;

    for (std::size_t i = 0; i < requestedFrames; ++i)
    {
        const auto availableFrames = fifo_.size() / 2;
        const double latestNeeded = sourcePos_ + HalfTaps + 1;

        if (latestNeeded >= static_cast<double>(availableFrames))
        {
            starved = true;
            break;
        }

        const float l = interpolate(0, sourcePos_);
        const float r = interpolate(1, sourcePos_);

        const auto toInt16 = [](float x)
        {
            const double scaled = static_cast<double>(x) * 32768.0;
            return static_cast<std::int16_t>(
                std::clamp(
                    std::llround(scaled),
                    static_cast<long long>(std::numeric_limits<std::int16_t>::min()),
                    static_cast<long long>(std::numeric_limits<std::int16_t>::max())));
        };

        out[i * 2] = toInt16(l);
        out[i * 2 + 1] = toInt16(r);

        sourcePos_ += step;
        trimConsumed();
    }

    if (starved)
        ++underruns_;

    return out;
}
