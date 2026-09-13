#include "YSpectrumWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QKeySequence>
#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPushButton>
#include <QResizeEvent>
#include <QShortcut>
#include <QString>

#include <algorithm>
#include <array>
#include <cmath>

namespace
{
constexpr double kPi = 3.1415926535897932384626433832795;

struct ReferenceNoisePoint
{
    double frequencyHz;
    double db;
};

// Empirical Intensity Pro 4K analogue-input noise-floor reference.
// Digitized from a clean, flat-field TPG20 capture on the better of the
// tested cards (AVG 16, full active line).  This is deliberately a visual
// guide only; it is not used by the SNR calculation.
constexpr std::array<ReferenceNoisePoint, 67> kBlackmagicReferenceNoise =
{{
    { 0.1e6, -72.8 }, { 0.2e6, -72.6 }, { 0.3e6, -72.7 },
    { 0.4e6, -72.7 }, { 0.5e6, -72.7 }, { 0.6e6, -72.5 },
    { 0.7e6, -72.9 }, { 0.8e6, -72.9 }, { 0.9e6, -73.1 },
    { 1.0e6, -73.1 }, { 1.1e6, -73.5 }, { 1.2e6, -73.6 },
    { 1.3e6, -73.6 }, { 1.4e6, -73.7 }, { 1.5e6, -74.1 },
    { 1.6e6, -74.1 }, { 1.7e6, -74.2 }, { 1.8e6, -74.4 },
    { 1.9e6, -74.4 }, { 2.0e6, -74.4 }, { 2.1e6, -74.6 },
    { 2.2e6, -74.8 }, { 2.3e6, -74.8 }, { 2.4e6, -74.9 },
    { 2.5e6, -74.9 }, { 2.6e6, -75.0 }, { 2.7e6, -75.0 },
    { 2.8e6, -75.0 }, { 2.9e6, -75.2 }, { 3.0e6, -75.3 },
    { 3.1e6, -75.1 }, { 3.2e6, -74.9 }, { 3.3e6, -75.0 },
    { 3.4e6, -75.0 }, { 3.5e6, -74.7 }, { 3.6e6, -74.7 },
    { 3.7e6, -74.8 }, { 3.8e6, -74.7 }, { 3.9e6, -74.5 },
    { 4.0e6, -74.8 }, { 4.1e6, -74.7 }, { 4.2e6, -74.9 },
    { 4.3e6, -74.9 }, { 4.4e6, -74.9 }, { 4.5e6, -74.7 },
    { 4.6e6, -74.5 }, { 4.7e6, -74.3 }, { 4.8e6, -74.0 },
    { 4.9e6, -73.8 }, { 5.0e6, -73.7 },
    { 5.1e6, -73.5 }, { 5.2e6, -73.4 }, { 5.3e6, -73.2 },
    { 5.4e6, -72.9 }, { 5.5e6, -72.6 }, { 5.6e6, -72.8 },
    { 5.7e6, -72.6 }, { 5.8e6, -72.4 }, { 5.9e6, -72.2 },
    { 6.0e6, -72.5 }, { 6.1e6, -72.6 }, { 6.2e6, -72.9 },
    { 6.3e6, -73.8 }, { 6.4e6, -75.0 }, { 6.5e6, -76.2 },
    { 6.6e6, -78.0 }, { 6.75e6, -80.0 }
}};

// Empirical flat-field reference for the same card.  Derived from the
// continuous 575-lines/frame flat-field measurement.  The small coherent
// ~350 kHz spur is intentionally not included: this curve represents the
// underlying broadband card noise floor, not card-specific discrete spurs.
constexpr std::array<ReferenceNoisePoint, 28> kBlackmagicFlatFieldReferenceNoise =
{{
    { 0.10e6, -72.6 }, { 0.20e6, -73.0 }, { 0.30e6, -73.2 },
    { 0.40e6, -73.5 }, { 0.50e6, -73.8 }, { 0.75e6, -74.4 },
    { 1.00e6, -75.0 }, { 1.25e6, -75.5 }, { 1.50e6, -75.9 },
    { 1.75e6, -76.3 }, { 2.00e6, -76.6 }, { 2.25e6, -76.9 },
    { 2.50e6, -77.1 }, { 2.75e6, -77.3 }, { 3.00e6, -77.4 },
    { 3.25e6, -77.4 }, { 3.50e6, -77.3 }, { 3.75e6, -77.2 },
    { 4.00e6, -77.1 }, { 4.25e6, -76.9 }, { 4.50e6, -76.7 },
    { 4.75e6, -76.5 }, { 5.00e6, -76.1 }, { 5.25e6, -75.6 },
    { 5.50e6, -75.0 }, { 5.75e6, -74.5 }, { 6.00e6, -74.1 },
    { 6.75e6, -73.8 }
}};

QString amplitudeText(double voltsPeak)
{
    if (voltsPeak >= 1.0)
    {
        return QStringLiteral("%1 Vpk").arg(voltsPeak, 0, 'f', 3);
    }

    if (voltsPeak >= 0.001)
    {
        return QStringLiteral("%1 mVpk").arg(voltsPeak * 1000.0, 0, 'f', 2);
    }

    return QStringLiteral("%1 uVpk").arg(voltsPeak * 1'000'000.0, 0, 'f', 1);
}

QString rmsText(double voltsRms)
{
    if (voltsRms >= 0.001)
    {
        return QStringLiteral("%1 mVrms").arg(voltsRms * 1000.0, 0, 'f', 3);
    }

    return QStringLiteral("%1 uVrms").arg(voltsRms * 1'000'000.0, 0, 'f', 1);
}

// CCIR Recommendation 567 / ITU-T J.61 unified video-noise weighting.
// A(f) is the insertion loss in dB:
//
//   A = 10 log10(
//       (1 + [ (1 + 1/a) * omega * tau ]^2) /
//       (1 + [ (1/a)     * omega * tau ]^2))
//
// with tau = 245 ns and a = 4.5.  Because the spectrum integration below
// sums power, return the corresponding linear POWER weighting 10^(-A/10).
double unifiedVideoNoisePowerWeight(double frequencyHz)
{
    constexpr double tauSeconds = 245.0e-9;
    constexpr double a = 4.5;

    const double omegaTau =
        2.0 * kPi * frequencyHz * tauSeconds;

    const double numeratorTerm =
        (1.0 + 1.0 / a) * omegaTau;
    const double denominatorTerm =
        (1.0 / a) * omegaTau;

    const double insertionLossDb =
        10.0 * std::log10(
            (1.0 + numeratorTerm * numeratorTerm) /
            (1.0 + denominatorTerm * denominatorTerm));

    return std::pow(10.0, -insertionLossDb / 10.0);
}
}

YSpectrumWindow::YSpectrumWindow(QWidget* parent)
    : QWidget(parent)
    , sourceCombo_(new QComboBox(this))
    , averageCombo_(new QComboBox(this))
    , snrSpurNotchCheckBox_(new QCheckBox(
          QStringLiteral("SNR spur notch"),
          this))
    , maxHoldButton_(new QPushButton(QStringLiteral("Max Hold"), this))
    , clearButton_(new QPushButton(QStringLiteral("Clear"), this))
    , measurePeaksButton_(new QPushButton(QStringLiteral("Measure Peaks"), this))
    , flatFieldButton_(new QPushButton(QStringLiteral("Flat Field"), this))
{
    setWindowTitle(QStringLiteral("OpenScope - Y Spectrum (experimental)"));
    setWindowFlag(Qt::Window, true);
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(1600, 900);
    setMinimumSize(900, 520);
    setMouseTracking(true);

    sourceCombo_->addItem(QStringLiteral("Full active line"));
    sourceCombo_->addItem(QStringLiteral("Safety area (80%)"));
    sourceCombo_->addItem(QStringLiteral("Visible portion"));
    sourceCombo_->setCurrentIndex(0);

    averageCombo_->addItem(QStringLiteral("AVG 1"), 1);
    averageCombo_->addItem(QStringLiteral("AVG 4"), 4);
    averageCombo_->addItem(QStringLiteral("AVG 16"), 16);
    averageCombo_->addItem(QStringLiteral("AVG 64"), 64);
    averageCombo_->setCurrentIndex(2);

    maxHoldButton_->setCheckable(true);
    flatFieldButton_->setCheckable(true);

    // Deliberately default OFF: excluding a coherent card spur makes the SNR
    // figure more optimistic and must therefore be an explicit user choice.
    snrSpurNotchCheckBox_->setChecked(false);
    snrSpurNotchCheckBox_->setStyleSheet(
        QStringLiteral(
            "QCheckBox { color: rgb(220, 225, 228); spacing: 6px; }"
            "QCheckBox::indicator {"
            "  width: 16px;"
            "  height: 16px;"
            "  border: 1px solid rgb(145, 155, 160);"
            "  background: rgb(20, 24, 26);"
            "}"
            "QCheckBox::indicator:checked {"
            "  border: 1px solid rgb(220, 225, 228);"
            "  background: rgb(87, 255, 118);"
            "}"));
    snrSpurNotchCheckBox_->setToolTip(
        QStringLiteral(
            "Auto-track strongest spur from 300-400 kHz; "
            "exclude a small symmetric bin window from SNR only. "
            "Spectrum remains unchanged."));

    updateControlGeometry();

    connect(
        sourceCombo_,
        &QComboBox::currentIndexChanged,
        this,
        [this](int)
        {
            resetAverage();
            clearMaxHold();
            clearPeakMeasurement();
            rebuildSpectrum();
            update();
        });

    connect(
        averageCombo_,
        &QComboBox::currentIndexChanged,
        this,
        [this](int)
        {
            resetAverage();
            clearPeakMeasurement();
            rebuildSpectrum();
            update();
        });

    connect(
        snrSpurNotchCheckBox_,
        &QCheckBox::toggled,
        this,
        [this](bool)
        {
            if (flatFieldButton_->isChecked())
            {
                rebuildFlatFieldSpectrum();
            }
            else
            {
                rebuildSpectrum();
            }

            update();
        });

    connect(
        maxHoldButton_,
        &QPushButton::toggled,
        this,
        [this](bool enabled)
        {
            if (enabled)
            {
                clearMaxHold();
            }
            update();
        });

    connect(
        clearButton_,
        &QPushButton::clicked,
        this,
        [this]()
        {
            resetAverage();
            clearMaxHold();
            clearPeakMeasurement();

            if (flatFieldButton_->isChecked())
            {
                emit flatFieldCaptureRequested();
            }
            else
            {
                rebuildSpectrum();
            }

            update();
        });

    connect(
        measurePeaksButton_,
        &QPushButton::clicked,
        this,
        [this]()
        {
            measurePeaks();
            update();
        });

    connect(
        flatFieldButton_,
        &QPushButton::toggled,
        this,
        [this](bool enabled)
        {
            sourceCombo_->setEnabled(!enabled);
            // Flat-field mode already averages all usable lines in each frame.
            // Keep the temporal AVG control enabled so consecutive flat-field
            // frame spectra can be averaged as well.
            averageCombo_->setEnabled(true);

            resetAverage();
            clearMaxHold();
            clearPeakMeasurement();

            if (enabled)
            {
                emit flatFieldCaptureRequested();
            }
            else
            {
                rebuildSpectrum();
            }

            update();
        });

    auto* escapeShortcut =
        new QShortcut(QKeySequence(Qt::Key_Escape), this);
    escapeShortcut->setContext(Qt::WidgetWithChildrenShortcut);

    connect(
        escapeShortcut,
        &QShortcut::activated,
        this,
        &QWidget::hide);
}

void YSpectrumWindow::setSamples(
    const QVector<float>& fullLine,
    const QVector<float>& visiblePart,
    int selectedLine,
    int zoomFactor,
    bool inputSignalValid)
{
    fullLine_ = fullLine;
    visiblePart_ = visiblePart;

    safetyArea_.clear();
    if (!fullLine_.isEmpty())
    {
        const int margin =
            static_cast<int>(fullLine_.size()) / 10;

        const int safetySampleCount =
            static_cast<int>(fullLine_.size()) - 2 * margin;

        if (safetySampleCount > 0)
        {
            safetyArea_ = fullLine_.mid(
                margin,
                safetySampleCount);
        }
    }

    const bool lineChanged =
        selectedLine_ != selectedLine;

    const bool zoomChanged =
        zoomFactor_ != zoomFactor;

    const bool inputValidityChanged =
        inputSignalValid_ != inputSignalValid;

    inputSignalValid_ =
        inputSignalValid;

    if (lineChanged || zoomChanged || inputValidityChanged)
    {
        resetAverage();
        clearPeakMeasurement();
    }

    if (lineChanged || inputValidityChanged)
    {
        clearMaxHold();
    }

    selectedLine_ = selectedLine;
    zoomFactor_ = zoomFactor;

    if (isVisible())
    {
        if (!flatFieldButton_->isChecked())
        {
            rebuildSpectrum();
        }
        update();
    }
}

void YSpectrumWindow::setFlatFieldSamples(
    const QVector<float>& samples,
    int lineLength,
    int lineCount,
    bool inputSignalValid)
{
    if (!flatFieldButton_->isChecked())
    {
        return;
    }

    flatFieldSamples_ = samples;
    flatFieldLineLength_ = lineLength;
    flatFieldLineCount_ = lineCount;
    flatFieldInputSignalValid_ = inputSignalValid;
    inputSignalValid_ = inputSignalValid;

    rebuildFlatFieldSpectrum();
    update();

    // Flat-field acquisition is continuous while the mode is enabled.
    // Request the next frame only after this one has been consumed, so there
    // can be at most one outstanding request.
    if (flatFieldButton_->isChecked())
    {
        emit flatFieldCaptureRequested();
    }
}


void YSpectrumWindow::setSnrMeasurementEnabled(
    bool enabled,
    const QString& disabledReason)
{
    if (snrMeasurementEnabled_ == enabled &&
        snrDisabledReason_ == disabledReason)
    {
        return;
    }

    snrMeasurementEnabled_ = enabled;
    snrDisabledReason_ = enabled
        ? QString()
        : disabledReason;

    if (flatFieldButton_->isChecked())
    {
        rebuildFlatFieldSpectrum();
    }
    else
    {
        rebuildSpectrum();
    }
    update();
}

void YSpectrumWindow::resetAverage()
{
    averagedAmplitudePower_.clear();
    averagedNoisePower_.clear();
    averageValid_ = false;
    averageLine_ = -2;
    averageZoomFactor_ = -1;
    averageSourceIndex_ = -1;
}

void YSpectrumWindow::clearMaxHold()
{
    maxHoldAmplitudePower_.clear();
}

void YSpectrumWindow::clearPeakMeasurement()
{
    measuredPeaks_.clear();
}

void YSpectrumWindow::measurePeaks()
{
    measuredPeaks_.clear();

    if (spectrum_.size() < 3)
    {
        return;
    }

    struct Candidate
    {
        int bin = 0;
        double db = kMinimumDb;
    };

    QVector<Candidate> candidates;

    for (int bin = 1; bin < spectrum_.size() - 1; ++bin)
    {
        const SpectrumPoint& previous = spectrum_[bin - 1];
        const SpectrumPoint& point = spectrum_[bin];
        const SpectrumPoint& next = spectrum_[bin + 1];

        if (point.frequencyHz < kMinimumSnrFrequencyHz ||
            point.frequencyHz > kMaximumDisplayFrequencyHz)
        {
            continue;
        }

        if (point.db > previous.db &&
            point.db >= next.db)
        {
            candidates.append({ bin, point.db });
        }
    }

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const Candidate& a, const Candidate& b)
        {
            return a.db > b.db;
        });

    const QVector<float>& source =
        sourceCombo_->currentIndex() == 0
        ? fullLine_
        : visiblePart_;

    const double trueResolutionHz =
        source.isEmpty()
        ? kReconstructedSampleClockHz / static_cast<double>(kFftSize)
        : kReconstructedSampleClockHz / static_cast<double>(source.size());

    const double minimumPeakSpacingHz =
        3.0 * trueResolutionHz;

    for (const Candidate& candidate : candidates)
    {
        const SpectrumPoint& point = spectrum_[candidate.bin];

        bool tooClose = false;
        for (const MeasuredPeak& accepted : measuredPeaks_)
        {
            if (std::abs(accepted.frequencyHz - point.frequencyHz) <
                minimumPeakSpacingHz)
            {
                tooClose = true;
                break;
            }
        }

        if (tooClose)
        {
            continue;
        }

        measuredPeaks_.append(
            {
                point.frequencyHz,
                point.amplitudeVoltsPeak,
                point.db
            });

        if (measuredPeaks_.size() >= kMaximumMeasuredPeaks)
        {
            break;
        }
    }

    // Detection selects the strongest peaks first, but presentation is easier to
    // read when marker numbers increase from left to right across the spectrum.
    std::sort(
        measuredPeaks_.begin(),
        measuredPeaks_.end(),
        [](const MeasuredPeak& a, const MeasuredPeak& b)
        {
            return a.frequencyHz < b.frequencyHz;
        });
}

void YSpectrumWindow::updateControlGeometry()
{
    sourceCombo_->setGeometry(16, 12, 190, 28);
    averageCombo_->setGeometry(216, 12, 92, 28);
    maxHoldButton_->setGeometry(318, 12, 92, 28);
    clearButton_->setGeometry(420, 12, 72, 28);
    measurePeaksButton_->setGeometry(502, 12, 122, 28);
    flatFieldButton_->setGeometry(634, 12, 100, 28);
    snrSpurNotchCheckBox_->setGeometry(744, 12, 150, 28);
}

void YSpectrumWindow::resizeEvent(QResizeEvent* event)
{
    updateControlGeometry();
    QWidget::resizeEvent(event);
}

int YSpectrumWindow::averageDepth() const
{
    return averageCombo_->currentData().toInt();
}

void YSpectrumWindow::rebuildSpectrum()
{
    const int sourceIndex = sourceCombo_->currentIndex();

    const QVector<float>* sourcePtr = &fullLine_;
    if (sourceIndex == 1)
    {
        sourcePtr = &safetyArea_;
    }
    else if (sourceIndex == 2)
    {
        sourcePtr = &visiblePart_;
    }

    const QVector<float>& source = *sourcePtr;

    spectrum_.clear();
    snrValid_ = false;
    weightedSnrValid_ = false;
    flatRegion_ = false;
    flatnessSpanVolts_ = 0.0;
    noiseRmsVolts_ = 0.0;
    weightedNoiseRmsVolts_ = 0.0;
    snrDb_ = 0.0;
    weightedSnrDb_ = 0.0;

    if (source.size() < 8)
    {
        return;
    }

    const int sampleCount = std::min<int>(source.size(), kFftSize);

    double mean = 0.0;
    for (int i = 0; i < sampleCount; ++i)
    {
        mean += static_cast<double>(source[i]);
    }
    mean /= static_cast<double>(sampleCount);

    // SNR is only meaningful on a genuinely flat luminance region.
    // Use a robust central 90% span so a few isolated noisy samples do not
    // invalidate an otherwise flat line, while obvious picture content does.
    QVector<double> sortedSamples;
    sortedSamples.reserve(sampleCount);

    for (int i = 0; i < sampleCount; ++i)
    {
        sortedSamples.append(static_cast<double>(source[i]));
    }

    std::sort(sortedSamples.begin(), sortedSamples.end());

    const int lowIndex = std::clamp(
        static_cast<int>(std::floor(0.05 * static_cast<double>(sampleCount - 1))),
        0,
        sampleCount - 1);
    const int highIndex = std::clamp(
        static_cast<int>(std::ceil(0.95 * static_cast<double>(sampleCount - 1))),
        0,
        sampleCount - 1);

    flatnessSpanVolts_ =
        sortedSamples[highIndex] - sortedSamples[lowIndex];
    flatRegion_ =
        flatnessSpanVolts_ <= kMaximumFlatnessSpanVolts;

    QVector<double> real(kFftSize, 0.0);
    QVector<double> imag(kFftSize, 0.0);

    double windowSum = 0.0;
    double windowPowerSum = 0.0;

    for (int i = 0; i < sampleCount; ++i)
    {
        const double window =
            sampleCount > 1
            ? 0.5 - 0.5 * std::cos(
                2.0 * kPi * static_cast<double>(i) /
                static_cast<double>(sampleCount - 1))
            : 1.0;

        real[i] =
            (static_cast<double>(source[i]) - mean) * window;

        windowSum += window;
        windowPowerSum += window * window;
    }

    if (windowSum <= 0.0 || windowPowerSum <= 0.0)
    {
        return;
    }

    fftInPlace(real, imag);

    const double binWidthHz =
        kReconstructedSampleClockHz /
        static_cast<double>(kFftSize);

    const int maximumBin = std::min(
        kFftSize / 2,
        static_cast<int>(
            std::floor(kMaximumDisplayFrequencyHz / binWidthHz)));

    QVector<double> instantaneousAmplitudePower(maximumBin + 1, 0.0);
    QVector<double> instantaneousNoisePower(maximumBin + 1, 0.0);

    for (int bin = 0; bin <= maximumBin; ++bin)
    {
        const double magnitudeSquared =
            real[bin] * real[bin] +
            imag[bin] * imag[bin];

        const double magnitude = std::sqrt(magnitudeSquared);

        const double amplitude =
            bin == 0
            ? magnitude / windowSum
            : 2.0 * magnitude / windowSum;

        instantaneousAmplitudePower[bin] = amplitude * amplitude;

        const double oneSidedFactor =
            (bin == 0 || bin == kFftSize / 2)
            ? 1.0
            : 2.0;

        instantaneousNoisePower[bin] =
            oneSidedFactor * magnitudeSquared /
            (static_cast<double>(kFftSize) * windowPowerSum);
    }

    const bool averageIdentityChanged =
        !averageValid_ ||
        averageLine_ != selectedLine_ ||
        averageZoomFactor_ != zoomFactor_ ||
        averageSourceIndex_ != sourceIndex ||
        averagedAmplitudePower_.size() != instantaneousAmplitudePower.size();

    if (averageIdentityChanged)
    {
        averagedAmplitudePower_ = instantaneousAmplitudePower;
        averagedNoisePower_ = instantaneousNoisePower;
        averageValid_ = true;
        averageLine_ = selectedLine_;
        averageZoomFactor_ = zoomFactor_;
        averageSourceIndex_ = sourceIndex;
    }
    else
    {
        const double alpha = 1.0 / static_cast<double>(averageDepth());

        for (qsizetype i = 0; i < averagedAmplitudePower_.size(); ++i)
        {
            averagedAmplitudePower_[i] +=
                alpha *
                (instantaneousAmplitudePower[i] - averagedAmplitudePower_[i]);

            averagedNoisePower_[i] +=
                alpha *
                (instantaneousNoisePower[i] - averagedNoisePower_[i]);
        }
    }

    if (maxHoldButton_->isChecked())
    {
        if (maxHoldAmplitudePower_.size() != instantaneousAmplitudePower.size())
        {
            maxHoldAmplitudePower_ = instantaneousAmplitudePower;
        }
        else
        {
            for (qsizetype i = 0; i < maxHoldAmplitudePower_.size(); ++i)
            {
                maxHoldAmplitudePower_[i] = std::max(
                    maxHoldAmplitudePower_[i],
                    instantaneousAmplitudePower[i]);
            }
        }
    }

    spectrum_.reserve(maximumBin + 1);

    double integratedNoisePower = 0.0;
    double integratedWeightedNoisePower = 0.0;

    // Track the strongest narrow coherent component in the card-spur region.
    // This does not alter the displayed spectrum.  It is only used by the
    // optional SNR integration notch below.
    trackedSnrSpurFrequencyHz_ = 0.0;
    trackedSnrSpurBin_ = -1;

    if (snrSpurNotchCheckBox_->isChecked())
    {
        double strongestPower = -1.0;

        for (int bin = 1; bin <= maximumBin; ++bin)
        {
            const double frequencyHz =
                static_cast<double>(bin) * binWidthHz;

            if (frequencyHz < kSnrSpurSearchMinimumHz ||
                frequencyHz > kSnrSpurSearchMaximumHz)
            {
                continue;
            }

            const double power = averagedAmplitudePower_[bin];

            if (power > strongestPower)
            {
                strongestPower = power;
                trackedSnrSpurFrequencyHz_ = frequencyHz;
                trackedSnrSpurBin_ = bin;
            }
        }
    }

    for (int bin = 0; bin <= maximumBin; ++bin)
    {
        const double frequencyHz =
            static_cast<double>(bin) * binWidthHz;

        const double amplitude =
            std::sqrt(std::max(0.0, averagedAmplitudePower_[bin]));

        // OpenScope video level convention: 700 mV = 0 dB.
        // This is intentionally not dBV; dBV would use 1 Vrms as reference.
        const double db =
            amplitude > 1.0e-12
            ? 20.0 * std::log10(amplitude / kReferenceVolts)
            : kMinimumDb;

        SpectrumPoint point;
        point.frequencyHz = frequencyHz;
        point.amplitudeVoltsPeak = amplitude;
        point.db = db;
        point.noisePowerVoltsSquared = averagedNoisePower_[bin];
        spectrum_.append(point);

        const bool insideSnrSpurNotch =
            snrSpurNotchCheckBox_->isChecked() &&
            trackedSnrSpurBin_ >= 0 &&
            std::abs(bin - trackedSnrSpurBin_) <=
                kSnrSpurNotchHalfWidthBins;

        if (frequencyHz >= kMinimumSnrFrequencyHz &&
            frequencyHz <= kMaximumSnrFrequencyHz &&
            !insideSnrSpurNotch)
        {
            const double binNoisePower =
                averagedNoisePower_[bin];

            integratedNoisePower += binNoisePower;
            integratedWeightedNoisePower +=
                binNoisePower *
                unifiedVideoNoisePowerWeight(frequencyHz);
        }
    }

    if (integratedNoisePower > 0.0)
    {
        noiseRmsVolts_ = std::sqrt(integratedNoisePower);
        snrDb_ = 20.0 * std::log10(
            kReferenceVolts / noiseRmsVolts_);
        snrValid_ =
            snrMeasurementEnabled_ &&
            inputSignalValid_ &&
            flatRegion_ &&
            noiseRmsVolts_ >= kMinimumMeaningfulNoiseRmsVolts &&
            std::isfinite(snrDb_);
    }

    if (integratedWeightedNoisePower > 0.0)
    {
        weightedNoiseRmsVolts_ =
            std::sqrt(integratedWeightedNoisePower);
        weightedSnrDb_ = 20.0 * std::log10(
            kReferenceVolts / weightedNoiseRmsVolts_);
        weightedSnrValid_ =
            snrValid_ &&
            std::isfinite(weightedSnrDb_);
    }
}

void YSpectrumWindow::rebuildFlatFieldSpectrum()
{
    spectrum_.clear();
    snrValid_ = false;
    weightedSnrValid_ = false;
    flatRegion_ = false;
    flatnessSpanVolts_ = 0.0;
    noiseRmsVolts_ = 0.0;
    weightedNoiseRmsVolts_ = 0.0;
    snrDb_ = 0.0;
    weightedSnrDb_ = 0.0;

    if (flatFieldLineLength_ < 8 ||
        flatFieldLineCount_ < 1 ||
        flatFieldSamples_.size() <
            static_cast<qsizetype>(flatFieldLineLength_) *
            static_cast<qsizetype>(flatFieldLineCount_))
    {
        return;
    }

    const int sampleCount = std::min(
        flatFieldLineLength_,
        kFlatFieldFftSize);

    // Flat-field mode intentionally averages POWER spectra across all usable
    // lines.  Averaging the samples first would cancel random noise and would
    // therefore produce a falsely optimistic SNR figure.
    const double binWidthHz =
        kNativeSampleClockHz /
        static_cast<double>(kFlatFieldFftSize);

    const int maximumBin = std::min(
        kFlatFieldFftSize / 2,
        static_cast<int>(
            std::floor(kMaximumDisplayFrequencyHz / binWidthHz)));

    QVector<double> accumulatedAmplitudePower(
        maximumBin + 1,
        0.0);
    QVector<double> accumulatedNoisePower(
        maximumBin + 1,
        0.0);

    QVector<double> real(kFlatFieldFftSize, 0.0);
    QVector<double> imag(kFlatFieldFftSize, 0.0);

    // Validate that this really is a flat field.  A central 90 % span over
    // all supplied lines is robust against a few isolated noisy samples.
    QVector<double> sortedSamples;
    sortedSamples.reserve(flatFieldSamples_.size());
    for (float sample : flatFieldSamples_)
    {
        sortedSamples.append(static_cast<double>(sample));
    }
    std::sort(sortedSamples.begin(), sortedSamples.end());

    const int totalSampleCount = sortedSamples.size();
    const int lowIndex = std::clamp(
        static_cast<int>(std::floor(
            0.05 * static_cast<double>(totalSampleCount - 1))),
        0,
        totalSampleCount - 1);
    const int highIndex = std::clamp(
        static_cast<int>(std::ceil(
            0.95 * static_cast<double>(totalSampleCount - 1))),
        0,
        totalSampleCount - 1);

    flatnessSpanVolts_ =
        sortedSamples[highIndex] - sortedSamples[lowIndex];
    flatRegion_ =
        flatnessSpanVolts_ <= kMaximumFlatnessSpanVolts;

    int processedLines = 0;

    for (int line = 0; line < flatFieldLineCount_; ++line)
    {
        const qsizetype lineOffset =
            static_cast<qsizetype>(line) *
            static_cast<qsizetype>(flatFieldLineLength_);

        double mean = 0.0;
        for (int i = 0; i < sampleCount; ++i)
        {
            mean += static_cast<double>(
                flatFieldSamples_[lineOffset + i]);
        }
        mean /= static_cast<double>(sampleCount);

        std::fill(real.begin(), real.end(), 0.0);
        std::fill(imag.begin(), imag.end(), 0.0);

        double windowSum = 0.0;
        double windowPowerSum = 0.0;

        for (int i = 0; i < sampleCount; ++i)
        {
            const double window =
                sampleCount > 1
                ? 0.5 - 0.5 * std::cos(
                    2.0 * kPi * static_cast<double>(i) /
                    static_cast<double>(sampleCount - 1))
                : 1.0;

            real[i] =
                (static_cast<double>(
                    flatFieldSamples_[lineOffset + i]) - mean) *
                window;

            windowSum += window;
            windowPowerSum += window * window;
        }

        if (windowSum <= 0.0 || windowPowerSum <= 0.0)
        {
            continue;
        }

        fftInPlace(real, imag);

        for (int bin = 0; bin <= maximumBin; ++bin)
        {
            const double magnitudeSquared =
                real[bin] * real[bin] +
                imag[bin] * imag[bin];

            const double magnitude =
                std::sqrt(magnitudeSquared);

            const double amplitude =
                bin == 0
                ? magnitude / windowSum
                : 2.0 * magnitude / windowSum;

            accumulatedAmplitudePower[bin] +=
                amplitude * amplitude;

            const double oneSidedFactor =
                (bin == 0 || bin == kFlatFieldFftSize / 2)
                ? 1.0
                : 2.0;

            accumulatedNoisePower[bin] +=
                oneSidedFactor * magnitudeSquared /
                (static_cast<double>(kFlatFieldFftSize) *
                    windowPowerSum);
        }

        ++processedLines;
    }

    if (processedLines <= 0)
    {
        return;
    }

    const double inverseLineCount =
        1.0 / static_cast<double>(processedLines);

    // One flat-field frame produces one power-spectrum estimate by averaging
    // the spectra of all usable lines.  The normal AVG selector then performs
    // an additional temporal average across consecutive flat-field frames.
    QVector<double> instantaneousAmplitudePower(maximumBin + 1, 0.0);
    QVector<double> instantaneousNoisePower(maximumBin + 1, 0.0);

    for (int bin = 0; bin <= maximumBin; ++bin)
    {
        instantaneousAmplitudePower[bin] =
            accumulatedAmplitudePower[bin] * inverseLineCount;
        instantaneousNoisePower[bin] =
            accumulatedNoisePower[bin] * inverseLineCount;
    }

    const bool averageIdentityChanged =
        !averageValid_ ||
        averageSourceIndex_ != -100 ||
        averagedAmplitudePower_.size() != instantaneousAmplitudePower.size();

    if (averageIdentityChanged)
    {
        averagedAmplitudePower_ = instantaneousAmplitudePower;
        averagedNoisePower_ = instantaneousNoisePower;
        averageValid_ = true;
        averageLine_ = -1;
        averageZoomFactor_ = -1;
        averageSourceIndex_ = -100;
    }
    else
    {
        const double alpha =
            1.0 / static_cast<double>(averageDepth());

        for (qsizetype i = 0; i < averagedAmplitudePower_.size(); ++i)
        {
            averagedAmplitudePower_[i] +=
                alpha *
                (instantaneousAmplitudePower[i] - averagedAmplitudePower_[i]);

            averagedNoisePower_[i] +=
                alpha *
                (instantaneousNoisePower[i] - averagedNoisePower_[i]);
        }
    }

    if (maxHoldButton_->isChecked())
    {
        if (maxHoldAmplitudePower_.size() != instantaneousAmplitudePower.size())
        {
            maxHoldAmplitudePower_ = instantaneousAmplitudePower;
        }
        else
        {
            for (qsizetype i = 0; i < maxHoldAmplitudePower_.size(); ++i)
            {
                maxHoldAmplitudePower_[i] = std::max(
                    maxHoldAmplitudePower_[i],
                    instantaneousAmplitudePower[i]);
            }
        }
    }

    spectrum_.reserve(maximumBin + 1);

    double integratedNoisePower = 0.0;
    double integratedWeightedNoisePower = 0.0;

    // Track the strongest narrow coherent component in the card-spur region.
    // This does not alter the displayed spectrum.  It is only used by the
    // optional SNR integration notch below.
    trackedSnrSpurFrequencyHz_ = 0.0;
    trackedSnrSpurBin_ = -1;

    if (snrSpurNotchCheckBox_->isChecked())
    {
        double strongestPower = -1.0;

        for (int bin = 1; bin <= maximumBin; ++bin)
        {
            const double frequencyHz =
                static_cast<double>(bin) * binWidthHz;

            if (frequencyHz < kSnrSpurSearchMinimumHz ||
                frequencyHz > kSnrSpurSearchMaximumHz)
            {
                continue;
            }

            const double power = averagedAmplitudePower_[bin];

            if (power > strongestPower)
            {
                strongestPower = power;
                trackedSnrSpurFrequencyHz_ = frequencyHz;
                trackedSnrSpurBin_ = bin;
            }
        }
    }

    for (int bin = 0; bin <= maximumBin; ++bin)
    {
        const double frequencyHz =
            static_cast<double>(bin) * binWidthHz;

        const double amplitude =
            std::sqrt(std::max(
                0.0,
                averagedAmplitudePower_[bin]));

        const double db =
            amplitude > 1.0e-12
            ? 20.0 * std::log10(amplitude / kReferenceVolts)
            : kMinimumDb;

        SpectrumPoint point;
        point.frequencyHz = frequencyHz;
        point.amplitudeVoltsPeak = amplitude;
        point.db = db;
        point.noisePowerVoltsSquared =
            averagedNoisePower_[bin];
        spectrum_.append(point);

        const bool insideSnrSpurNotch =
            snrSpurNotchCheckBox_->isChecked() &&
            trackedSnrSpurBin_ >= 0 &&
            std::abs(bin - trackedSnrSpurBin_) <=
                kSnrSpurNotchHalfWidthBins;

        if (frequencyHz >= kMinimumSnrFrequencyHz &&
            frequencyHz <= kMaximumSnrFrequencyHz &&
            !insideSnrSpurNotch)
        {
            const double binNoisePower =
                averagedNoisePower_[bin];

            integratedNoisePower += binNoisePower;
            integratedWeightedNoisePower +=
                binNoisePower *
                unifiedVideoNoisePowerWeight(frequencyHz);
        }
    }

    if (integratedNoisePower > 0.0)
    {
        noiseRmsVolts_ = std::sqrt(integratedNoisePower);
        snrDb_ = 20.0 * std::log10(
            kReferenceVolts / noiseRmsVolts_);
        snrValid_ =
            snrMeasurementEnabled_ &&
            flatFieldInputSignalValid_ &&
            flatRegion_ &&
            noiseRmsVolts_ >= kMinimumMeaningfulNoiseRmsVolts &&
            std::isfinite(snrDb_);
    }

    if (integratedWeightedNoisePower > 0.0)
    {
        weightedNoiseRmsVolts_ =
            std::sqrt(integratedWeightedNoisePower);
        weightedSnrDb_ = 20.0 * std::log10(
            kReferenceVolts / weightedNoiseRmsVolts_);
        weightedSnrValid_ =
            snrValid_ &&
            std::isfinite(weightedSnrDb_);
    }
}

void YSpectrumWindow::fftInPlace(
    QVector<double>& real,
    QVector<double>& imag) const
{
    const int n = real.size();

    for (int i = 1, j = 0; i < n; ++i)
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
        {
            j ^= bit;
        }
        j ^= bit;

        if (i < j)
        {
            std::swap(real[i], real[j]);
            std::swap(imag[i], imag[j]);
        }
    }

    for (int length = 2; length <= n; length <<= 1)
    {
        const double angle = -2.0 * kPi / static_cast<double>(length);
        const double wLengthReal = std::cos(angle);
        const double wLengthImag = std::sin(angle);

        for (int i = 0; i < n; i += length)
        {
            double wReal = 1.0;
            double wImag = 0.0;

            for (int j = 0; j < length / 2; ++j)
            {
                const int even = i + j;
                const int odd = even + length / 2;

                const double oddReal =
                    real[odd] * wReal - imag[odd] * wImag;
                const double oddImag =
                    real[odd] * wImag + imag[odd] * wReal;

                const double evenReal = real[even];
                const double evenImag = imag[even];

                real[even] = evenReal + oddReal;
                imag[even] = evenImag + oddImag;
                real[odd] = evenReal - oddReal;
                imag[odd] = evenImag - oddImag;

                const double nextWReal =
                    wReal * wLengthReal - wImag * wLengthImag;
                const double nextWImag =
                    wReal * wLengthImag + wImag * wLengthReal;

                wReal = nextWReal;
                wImag = nextWImag;
            }
        }
    }
}

int YSpectrumWindow::nearestBinForFrequency(double frequencyHz) const
{
    if (spectrum_.isEmpty())
    {
        return -1;
    }

    const double binWidthHz =
        kReconstructedSampleClockHz /
        static_cast<double>(kFftSize);

    return std::clamp(
        static_cast<int>(std::llround(frequencyHz / binWidthHz)),
        0,
        static_cast<int>(spectrum_.size()) - 1);
}

double YSpectrumWindow::frequencyForX(double x) const
{
    const double left = 84.0;
    const double right = static_cast<double>(width()) - 28.0;
    const double normalized = std::clamp(
        (x - left) / std::max(1.0, right - left),
        0.0,
        1.0);

    return normalized * kMaximumDisplayFrequencyHz;
}

double YSpectrumWindow::xForFrequency(double frequencyHz) const
{
    const double left = 84.0;
    const double right = static_cast<double>(width()) - 28.0;
    return left +
        std::clamp(
            frequencyHz / kMaximumDisplayFrequencyHz,
            0.0,
            1.0) *
        (right - left);
}

double YSpectrumWindow::yForDb(double db) const
{
    const double top = 76.0;
    const double bottom = static_cast<double>(height()) - 132.0;
    const double normalized = std::clamp(
        (kMaximumDb - db) /
            (kMaximumDb - kMinimumDb),
        0.0,
        1.0);
    return top + normalized * (bottom - top);
}

void YSpectrumWindow::setMarkerFromX(int markerIndex, double x)
{
    markerFrequencyHz_[markerIndex] = frequencyForX(x);
    update();
}

void YSpectrumWindow::mousePressEvent(QMouseEvent* event)
{
    const double frequency = frequencyForX(event->position().x());
    const double distanceA = std::abs(frequency - markerFrequencyHz_[0]);
    const double distanceB = std::abs(frequency - markerFrequencyHz_[1]);

    activeMarker_ = distanceA <= distanceB ? 0 : 1;
    setMarkerFromX(activeMarker_, event->position().x());
    event->accept();
}

void YSpectrumWindow::mouseMoveEvent(QMouseEvent* event)
{
    if (event->buttons() & Qt::LeftButton)
    {
        setMarkerFromX(activeMarker_, event->position().x());
        event->accept();
        return;
    }

    QWidget::mouseMoveEvent(event);
}

void YSpectrumWindow::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(9, 12, 14));
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF plotRect(
        84.0,
        76.0,
        std::max(1, width() - 112),
        std::max(1, height() - 182));

    QPen gridPen(QColor(55, 62, 66));
    gridPen.setWidthF(1.0);

    const QPen axisTextPen(QColor(145, 155, 160));

    for (int mhz = 0; mhz <= 7; ++mhz)
    {
        const double x = xForFrequency(
            static_cast<double>(mhz) * 1'000'000.0);

        painter.setPen(gridPen);
        painter.drawLine(QPointF(x, plotRect.top()), QPointF(x, plotRect.bottom()));

        painter.setPen(axisTextPen);
        painter.drawText(
            QRectF(x - 30.0, plotRect.bottom() + 6.0, 60.0, 22.0),
            Qt::AlignHCenter | Qt::AlignTop,
            QString::number(mhz));
    }

    for (int db = 0; db >= static_cast<int>(kMinimumDb); db -= 20)
    {
        const double y = yForDb(static_cast<double>(db));

        painter.setPen(gridPen);
        painter.drawLine(QPointF(plotRect.left(), y), QPointF(plotRect.right(), y));

        painter.setPen(axisTextPen);
        painter.drawText(
            QRectF(10.0, y - 10.0, 64.0, 20.0),
            Qt::AlignRight | Qt::AlignVCenter,
            QStringLiteral("%1 dB").arg(db));
    }

    // Keep the plot axes visually distinct from the darker grid.
    QPen axisLinePen(QColor(145, 155, 160));
    axisLinePen.setWidthF(1.2);
    painter.setPen(axisLinePen);
    painter.drawLine(plotRect.bottomLeft(), plotRect.topLeft());
    painter.drawLine(plotRect.bottomLeft(), plotRect.bottomRight());

    // The reconstructed stream runs at 54 MHz, but its original video
    // information came from 13.5 MHz samples.  Mark that original Nyquist
    // limit explicitly: reconstruction can make the roll-off visible, but it
    // cannot create new source information above 6.75 MHz.
    const double nyquistX =
        xForFrequency(kOriginalNyquistFrequencyHz);

    QPen nyquistPen(QColor(125, 148, 158));
    nyquistPen.setWidthF(1.2);
    nyquistPen.setStyle(Qt::DashLine);
    painter.setPen(nyquistPen);
    painter.drawLine(
        QPointF(nyquistX, plotRect.top()),
        QPointF(nyquistX, plotRect.bottom()));

    painter.setPen(QColor(175, 194, 201));
    painter.drawText(
        QRectF(nyquistX - 132.0, plotRect.top() + 4.0, 124.0, 20.0),
        Qt::AlignRight | Qt::AlignVCenter,
        QStringLiteral("Nyquist 6.75 MHz"));

    const QVector<float>& source =
        sourceCombo_->currentIndex() == 0
        ? fullLine_
        : (sourceCombo_->currentIndex() == 1 ? safetyArea_ : visiblePart_);

    const bool flatFieldMode =
        flatFieldButton_->isChecked() &&
        flatFieldLineLength_ > 0 &&
        flatFieldLineCount_ > 0;

    const double trueResolutionHz = flatFieldMode
        ? kNativeSampleClockHz / static_cast<double>(flatFieldLineLength_)
        : (source.isEmpty()
            ? 0.0
            : kReconstructedSampleClockHz /
                static_cast<double>(source.size()));

    painter.setPen(QColor(220, 225, 228));

    const QString statusText = flatFieldMode
        ? QStringLiteral("Flat field   %1 lines/frame   AVG %2 frames   last line skipped   N=%3/line   resolution ~%4 kHz   FFT=1024   Hann   DC removed")
            .arg(flatFieldLineCount_)
            .arg(averageDepth())
            .arg(flatFieldLineLength_)
            .arg(trueResolutionHz / 1000.0, 0, 'f', 1)
        : QStringLiteral("Line %1   Zoom x%2   N=%3   resolution ~%4 kHz   FFT=4096   Hann   DC removed")
            .arg(selectedLine_)
            .arg(zoomFactor_)
            .arg(source.size())
            .arg(trueResolutionHz / 1000.0, 0, 'f', 1);

    painter.drawText(
        QRectF(904.0, 12.0, std::max(1, width() - 920), 28.0),
        Qt::AlignLeft | Qt::AlignVCenter,
        statusText);



    // Empirical Blackmagic Intensity Pro 4K reference noise floor.
    // In Flat Field mode, flip to the corresponding many-line reference.
    // Both are visual comparison aids only and never affect SNR calculations.
    {
        QPen referencePen(QColor(145, 150, 154, 145));
        referencePen.setWidthF(1.2);
        referencePen.setStyle(Qt::DashLine);
        painter.setPen(referencePen);

        const bool useFlatFieldReference = flatFieldMode;

        auto drawReference =
            [&](const auto& reference)
            {
                for (std::size_t i = 1; i < reference.size(); ++i)
                {
                    const ReferenceNoisePoint& previous = reference[i - 1];
                    const ReferenceNoisePoint& current = reference[i];

                    painter.drawLine(
                        QPointF(
                            xForFrequency(previous.frequencyHz),
                            yForDb(previous.db)),
                        QPointF(
                            xForFrequency(current.frequencyHz),
                            yForDb(current.db)));
                }
            };

        if (useFlatFieldReference)
        {
            drawReference(kBlackmagicFlatFieldReferenceNoise);
        }
        else
        {
            drawReference(kBlackmagicReferenceNoise);
        }

        painter.setPen(QColor(165, 170, 174));
        painter.drawText(
            QRectF(
                xForFrequency(3.80e6),
                yForDb(useFlatFieldReference ? -73.8 : -72.8) - 18.0,
                xForFrequency(6.65e6) - xForFrequency(3.80e6),
                18.0),
            Qt::AlignRight | Qt::AlignVCenter,
            useFlatFieldReference
                ? QStringLiteral("BMD IP4K flat-field reference noise")
                : QStringLiteral("BMD IP4K reference noise"));
    }


    if (spectrum_.size() > 1)
    {
        // Make the SNR integration band visible in the trace itself:
        //   0.10 .. 5.00 MHz  = active SNR band (green)
        //   outside that band = displayed for context only (grey)
        // The colour split is presentation-only; the SNR calculation above
        // already integrates exactly kMinimumSnrFrequencyHz..kMaximumSnrFrequencyHz.
        QPen activeTracePen(QColor(87, 255, 118));
        activeTracePen.setWidthF(1.4);

        QPen contextTracePen(QColor(126, 136, 141));
        contextTracePen.setWidthF(1.4);

        const bool showSnrSpurNotch =
            snrSpurNotchCheckBox_->isChecked() &&
            trackedSnrSpurBin_ >= 0 &&
            trackedSnrSpurBin_ < spectrum_.size();

        const int leftExcludedBin =
            std::max(0, trackedSnrSpurBin_ - kSnrSpurNotchHalfWidthBins);
        const int rightExcludedBin =
            std::min(
                static_cast<int>(spectrum_.size()) - 1,
                trackedSnrSpurBin_ + kSnrSpurNotchHalfWidthBins);

        for (qsizetype i = 1; i < spectrum_.size(); ++i)
        {
            const SpectrumPoint& previous = spectrum_[i - 1];
            const SpectrumPoint& current = spectrum_[i];

            const double segmentFrequencyHz =
                0.5 * (previous.frequencyHz + current.frequencyHz);

            const bool inSnrBand =
                segmentFrequencyHz >= kMinimumSnrFrequencyHz &&
                segmentFrequencyHz <= kMaximumSnrFrequencyHz;

            const bool inNotch =
                showSnrSpurNotch &&
                static_cast<int>(i - 1) <= rightExcludedBin &&
                static_cast<int>(i) >= leftExcludedBin;

            // Keep the genuine measured spectrum visible inside the notch.
            // Grey means: real card response, deliberately excluded from SNR.
            painter.setPen(
                inNotch
                    ? contextTracePen
                    : (inSnrBand ? activeTracePen : contextTracePen));

            painter.drawLine(
                QPointF(
                    xForFrequency(previous.frequencyHz),
                    yForDb(previous.db)),
                QPointF(
                    xForFrequency(current.frequencyHz),
                    yForDb(current.db)));
        }

        // Draw the corrected SNR response as a simple bridge between the first
        // clean bins left and right of the excluded spur region.
        if (showSnrSpurNotch)
        {
            const int leftAnchorBin =
                std::max(0, leftExcludedBin - 1);
            const int rightAnchorBin =
                std::min(
                    static_cast<int>(spectrum_.size()) - 1,
                    rightExcludedBin + 1);

            if (rightAnchorBin > leftAnchorBin)
            {
                const SpectrumPoint& leftPoint = spectrum_[leftAnchorBin];
                const SpectrumPoint& rightPoint = spectrum_[rightAnchorBin];

                painter.setPen(activeTracePen);
                painter.drawLine(
                    QPointF(
                        xForFrequency(leftPoint.frequencyHz),
                        yForDb(leftPoint.db)),
                    QPointF(
                        xForFrequency(rightPoint.frequencyHz),
                        yForDb(rightPoint.db)));
            }
        }
    }

    if (maxHoldButton_->isChecked() &&
        maxHoldAmplitudePower_.size() == spectrum_.size())
    {
        const double binWidthHz =
            kReconstructedSampleClockHz /
            static_cast<double>(kFftSize);

        QPen activeHoldPen(QColor(255, 190, 70, 185));
        activeHoldPen.setWidthF(1.1);

        QPen contextHoldPen(QColor(126, 136, 141, 155));
        contextHoldPen.setWidthF(1.1);

        for (qsizetype bin = 1; bin < maxHoldAmplitudePower_.size(); ++bin)
        {
            const auto pointForBin =
                [&](qsizetype index)
                {
                    const double amplitude =
                        std::sqrt(
                            std::max(
                                0.0,
                                maxHoldAmplitudePower_[index]));

                    const double db =
                        amplitude > 1.0e-12
                        ? 20.0 * std::log10(amplitude / kReferenceVolts)
                        : kMinimumDb;

                    return QPointF(
                        xForFrequency(
                            static_cast<double>(index) * binWidthHz),
                        yForDb(db));
                };

            const double segmentFrequencyHz =
                (static_cast<double>(bin) - 0.5) * binWidthHz;

            const bool inSnrBand =
                segmentFrequencyHz >= kMinimumSnrFrequencyHz &&
                segmentFrequencyHz <= kMaximumSnrFrequencyHz;

            painter.setPen(inSnrBand ? activeHoldPen : contextHoldPen);
            painter.drawLine(
                pointForBin(bin - 1),
                pointForBin(bin));
        }
    }

    const QColor markerColors[2] = {
        QColor(255, 210, 70),
        QColor(90, 190, 255)
    };

    QString markerTexts[2];
    double markerDb[2] = { kMinimumDb, kMinimumDb };
    double markerActualHz[2] = { 0.0, 0.0 };

    for (int marker = 0; marker < 2; ++marker)
    {
        const int bin = nearestBinForFrequency(markerFrequencyHz_[marker]);
        const double x = xForFrequency(markerFrequencyHz_[marker]);

        QPen markerPen(markerColors[marker]);
        markerPen.setWidthF(marker == activeMarker_ ? 2.0 : 1.2);
        painter.setPen(markerPen);
        painter.drawLine(QPointF(x, plotRect.top()), QPointF(x, plotRect.bottom()));

        if (bin >= 0)
        {
            const SpectrumPoint& point = spectrum_[bin];
            painter.drawEllipse(
                QPointF(xForFrequency(point.frequencyHz), yForDb(point.db)),
                4.0,
                4.0);

            markerDb[marker] = point.db;
            markerActualHz[marker] = point.frequencyHz;

            markerTexts[marker] =
                QStringLiteral("%1: %2 MHz   %3 dB   %4")
                    .arg(marker == 0 ? QStringLiteral("A") : QStringLiteral("B"))
                    .arg(point.frequencyHz / 1'000'000.0, 0, 'f', 4)
                    .arg(point.db, 0, 'f', 1)
                    .arg(amplitudeText(point.amplitudeVoltsPeak));
        }
    }

    painter.setPen(markerColors[0]);
    painter.drawText(
        QRectF(plotRect.left(), 44.0, plotRect.width() * 0.5, 24.0),
        Qt::AlignLeft | Qt::AlignVCenter,
        markerTexts[0]);

    painter.setPen(markerColors[1]);
    painter.drawText(
        QRectF(plotRect.left() + plotRect.width() * 0.5, 44.0, plotRect.width() * 0.5, 24.0),
        Qt::AlignRight | Qt::AlignVCenter,
        markerTexts[1]);

    // Instrument-style SNR card.  The medium-grey panel separates the
    // measurement from the trace while keeping the white readout highly
    // legible. Never show a plausible-looking number when the selected data
    // is not a valid analogue SNR measurement source.
    const QFont normalFont = painter.font();

    const QRectF snrCardRect(
        plotRect.left() + 12.0,
        plotRect.top() + 10.0,
        530.0,
        118.0);

    painter.setPen(QPen(QColor(135, 142, 146), 1.0));
    painter.setBrush(QColor(68, 74, 78, 235));
    painter.drawRoundedRect(snrCardRect, 9.0, 9.0);

    const QString snrText = weightedSnrValid_
        ? QStringLiteral("SNR %1 dB Weighted").arg(weightedSnrDb_, 0, 'f', 1)
        : QStringLiteral("SNR -- dB Weighted");

    const QRectF snrTextRect =
        snrCardRect.adjusted(14.0, 2.0, -12.0, -66.0);

    QFont snrFont = normalFont;
    snrFont.setBold(true);
    snrFont.setPointSizeF(26.0);

    // Keep the full "Weighted" label visible.  The numeric value can make
    // the headline a few pixels wider, so shrink only when actually needed.
    QFontMetricsF snrMetrics(snrFont);
    if (snrMetrics.horizontalAdvance(snrText) > snrTextRect.width())
    {
        const qreal fitScale =
            snrTextRect.width() / snrMetrics.horizontalAdvance(snrText);
        snrFont.setPointSizeF(
            std::max<qreal>(20.0, snrFont.pointSizeF() * fitScale));
    }

    painter.setFont(snrFont);
    painter.setPen(QColor(248, 250, 250));
    painter.drawText(
        snrTextRect,
        Qt::AlignLeft | Qt::AlignVCenter,
        snrText);

    QFont snrDetailFont = normalFont;
    snrDetailFont.setBold(false);
    snrDetailFont.setPointSizeF(9.0);
    painter.setFont(snrDetailFont);
    painter.setPen(QColor(224, 228, 230));

    QString snrDetail;
    if (!snrMeasurementEnabled_)
    {
        snrDetail = snrDisabledReason_.isEmpty()
            ? QStringLiteral("SNR DISABLED FOR THIS SOURCE")
            : snrDisabledReason_;
    }
    else if (!inputSignalValid_)
    {
        snrDetail =
            QStringLiteral("NO INPUT SOURCE   DeckLink status   SNR suppressed");
    }
    else if (!flatRegion_)
    {
        snrDetail =
            QStringLiteral("NOT FLAT   span %1 mV   SNR suppressed")
                .arg(flatnessSpanVolts_ * 1000.0, 0, 'f', 1);
    }
    else if (noiseRmsVolts_ < kMinimumMeaningfulNoiseRmsVolts)
    {
        snrDetail =
            QStringLiteral("NO MEASURABLE ANALOGUE NOISE   SNR suppressed");
    }
    else
    {
        snrDetail = snrValid_
            ? QStringLiteral("Unweighted %1 dB   CCIR 567 / J.61   flat span %2 mV")
                .arg(snrDb_, 0, 'f', 1)
                .arg(flatnessSpanVolts_ * 1000.0, 0, 'f', 1)
            : QStringLiteral("0.10-5.00 MHz   Ref 700 mV   flat span %1 mV")
                .arg(flatnessSpanVolts_ * 1000.0, 0, 'f', 1);
    }

    painter.drawText(
        snrCardRect.adjusted(16.0, 55.0, -12.0, -32.0),
        Qt::AlignLeft | Qt::AlignVCenter,
        snrDetail);

    painter.setPen(QColor(184, 190, 193));
    painter.drawText(
        snrCardRect.adjusted(16.0, 82.0, -12.0, -7.0),
        Qt::AlignLeft | Qt::AlignVCenter,
        QStringLiteral("Blackmagic card SNR ~58 dB UNWTD"));

    painter.setBrush(Qt::NoBrush);
    painter.setFont(normalFont);

    // Peak measurement is intentionally snapshot-based. The live spectrum keeps
    // moving, while the measured top-five list remains readable until the next
    // Measure Peaks action or an explicit/automatic clear. Peak numbering is in
    // ascending frequency order, matching the markers from left to right.
    if (!measuredPeaks_.isEmpty())
    {
        const double tableWidth = 380.0;
        const double rowHeight = 20.0;
        const double tableHeight = 28.0 +
            rowHeight * static_cast<double>(measuredPeaks_.size());
        const QRectF tableRect(
            plotRect.right() - tableWidth - 12.0,
            plotRect.top() + 12.0,
            tableWidth,
            tableHeight);

        painter.fillRect(tableRect, QColor(5, 8, 10, 205));
        painter.setPen(QColor(95, 105, 110));
        painter.drawRect(tableRect);

        const double left = tableRect.left() + 10.0;
        const double usableWidth = tableRect.width() - 20.0;
        const double rankWidth = 34.0;
        const double frequencyWidth = 92.0;
        const double levelWidth = 76.0;
        const double amplitudeWidth =
            usableWidth - rankWidth - frequencyWidth - levelWidth;

        const QRectF rankHeader(
            left, tableRect.top() + 4.0, rankWidth, 20.0);
        const QRectF frequencyHeader(
            rankHeader.right(), tableRect.top() + 4.0, frequencyWidth, 20.0);
        const QRectF levelHeader(
            frequencyHeader.right(), tableRect.top() + 4.0, levelWidth, 20.0);
        const QRectF amplitudeHeader(
            levelHeader.right(), tableRect.top() + 4.0, amplitudeWidth, 20.0);

        painter.setPen(QColor(225, 225, 225));
        painter.drawText(
            rankHeader,
            Qt::AlignLeft | Qt::AlignVCenter,
            QStringLiteral("#"));
        painter.drawText(
            frequencyHeader,
            Qt::AlignRight | Qt::AlignVCenter,
            QStringLiteral("MHz"));
        painter.drawText(
            levelHeader,
            Qt::AlignRight | Qt::AlignVCenter,
            QStringLiteral("dB"));
        painter.drawText(
            amplitudeHeader,
            Qt::AlignRight | Qt::AlignVCenter,
            QStringLiteral("Amplitude"));

        for (int i = 0; i < measuredPeaks_.size(); ++i)
        {
            const MeasuredPeak& peak = measuredPeaks_[i];
            const double y = tableRect.top() + 26.0 +
                static_cast<double>(i) * rowHeight;

            const QRectF rankCell(left, y, rankWidth, rowHeight);
            const QRectF frequencyCell(
                rankCell.right(), y, frequencyWidth, rowHeight);
            const QRectF levelCell(
                frequencyCell.right(), y, levelWidth, rowHeight);
            const QRectF amplitudeCell(
                levelCell.right(), y, amplitudeWidth, rowHeight);

            painter.setPen(QColor(205, 210, 212));
            painter.drawText(
                rankCell,
                Qt::AlignLeft | Qt::AlignVCenter,
                QString::number(i + 1));
            painter.drawText(
                frequencyCell,
                Qt::AlignRight | Qt::AlignVCenter,
                QString::number(
                    peak.frequencyHz / 1'000'000.0,
                    'f',
                    4));
            painter.drawText(
                levelCell,
                Qt::AlignRight | Qt::AlignVCenter,
                QString::number(peak.db, 'f', 1));
            painter.drawText(
                amplitudeCell,
                Qt::AlignRight | Qt::AlignVCenter,
                amplitudeText(peak.amplitudeVoltsPeak));

            const QPointF peakPosition(
                xForFrequency(peak.frequencyHz),
                yForDb(peak.db));

            painter.setPen(QColor(255, 245, 160));
            painter.setBrush(QColor(9, 12, 14));
            painter.drawEllipse(peakPosition, 4.0, 4.0);
            painter.drawText(
                QRectF(
                    peakPosition.x() - 10.0,
                    peakPosition.y() - 24.0,
                    20.0,
                    18.0),
                Qt::AlignHCenter | Qt::AlignVCenter,
                QString::number(i + 1));
            painter.setBrush(Qt::NoBrush);
        }
    }

    const double row1Y = plotRect.bottom() + 34.0;
    const double row2Y = plotRect.bottom() + 60.0;

    painter.setPen(QColor(210, 210, 210));
    painter.drawText(
        QRectF(plotRect.left(), row1Y, plotRect.width() * 0.58, 22.0),
        Qt::AlignLeft | Qt::AlignVCenter,
        measuredPeaks_.isEmpty()
        ? QStringLiteral("Peaks: press Measure Peaks for a stable top-5 snapshot")
        : QStringLiteral("Peaks: measured snapshot (%1 found)").arg(measuredPeaks_.size()));

    painter.setPen(QColor(180, 180, 180));
    painter.drawText(
        QRectF(plotRect.left(), row2Y, plotRect.width() * 0.55, 22.0),
        Qt::AlignLeft | Qt::AlignVCenter,
        QStringLiteral("A-B: df=%1 MHz   dB=%2 dB")
            .arg(std::abs(markerActualHz[1] - markerActualHz[0]) / 1'000'000.0, 0, 'f', 4)
            .arg(markerDb[1] - markerDb[0], 0, 'f', 1));

    painter.setPen(QColor(225, 225, 225));
    painter.drawText(
        QRectF(plotRect.left() + plotRect.width() * 0.58, row1Y, plotRect.width() * 0.42, 22.0),
        Qt::AlignRight | Qt::AlignVCenter,
        noiseRmsVolts_ > 0.0
        ? (snrMeasurementEnabled_
            ? (weightedNoiseRmsVolts_ > 0.0
                ? QStringLiteral("Noise flat: %1   weighted: %2")
                    .arg(rmsText(noiseRmsVolts_))
                    .arg(rmsText(weightedNoiseRmsVolts_))
                : QStringLiteral("Noise: %1").arg(rmsText(noiseRmsVolts_)))
            : QStringLiteral("Integrated AC: %1").arg(rmsText(noiseRmsVolts_)))
        : QStringLiteral("Noise: ---"));

    painter.setPen(QColor(170, 175, 178));
    painter.drawText(
        QRectF(plotRect.left() + plotRect.width() * 0.55, row2Y, plotRect.width() * 0.45, 22.0),
        Qt::AlignRight | Qt::AlignVCenter,
        !snrMeasurementEnabled_
        ? QStringLiteral("SNR N/A: digital ROM source")
        : (!inputSignalValid_
            ? QStringLiteral("SNR invalid: DeckLink reports no input source")
            : (!flatRegion_
                ? QStringLiteral("SNR invalid: selected line/ROI is not flat")
                : (noiseRmsVolts_ < kMinimumMeaningfulNoiseRmsVolts
                    ? QStringLiteral("SNR invalid: no measurable analogue noise")
                    : QStringLiteral("SNR flat + CCIR 567 weighted   BW 0.10-5.00 MHz   Ref 700 mV   AVG %1")
                        .arg(averageDepth())))));

    painter.setPen(QColor(190, 195, 198));
    painter.drawText(
        QRectF(plotRect.left(), plotRect.bottom() + 7.0, plotRect.width(), 22.0),
        Qt::AlignHCenter | Qt::AlignTop,
        QStringLiteral("Frequency (MHz)"));
}
