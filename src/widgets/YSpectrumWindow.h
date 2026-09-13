#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QMouseEvent;
class QPaintEvent;
class QPushButton;
class QResizeEvent;

class YSpectrumWindow final : public QWidget
{
    Q_OBJECT

public:
    explicit YSpectrumWindow(QWidget* parent = nullptr);

    void setSamples(
        const QVector<float>& fullLine,
        const QVector<float>& visiblePart,
        int selectedLine,
        int zoomFactor,
        bool inputSignalValid);

    void setSnrMeasurementEnabled(
        bool enabled,
        const QString& disabledReason = QString());

    void setFlatFieldSamples(
        const QVector<float>& samples,
        int lineLength,
        int lineCount,
        bool inputSignalValid);

signals:
    void flatFieldCaptureRequested();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    struct SpectrumPoint
    {
        double frequencyHz = 0.0;
        double amplitudeVoltsPeak = 0.0;
        double db = -160.0;
        double noisePowerVoltsSquared = 0.0;
    };

    struct MeasuredPeak
    {
        double frequencyHz = 0.0;
        double amplitudeVoltsPeak = 0.0;
        double db = -160.0;
    };

    void rebuildSpectrum();
    void rebuildFlatFieldSpectrum();
    void resetAverage();
    void clearMaxHold();
    void clearPeakMeasurement();
    void measurePeaks();
    void updateControlGeometry();
    void fftInPlace(QVector<double>& real, QVector<double>& imag) const;
    int nearestBinForFrequency(double frequencyHz) const;
    double frequencyForX(double x) const;
    double xForFrequency(double frequencyHz) const;
    double yForDb(double db) const;
    void setMarkerFromX(int markerIndex, double x);
    int averageDepth() const;

    QComboBox* sourceCombo_ = nullptr;
    QComboBox* averageCombo_ = nullptr;
    QCheckBox* snrSpurNotchCheckBox_ = nullptr;
    QPushButton* maxHoldButton_ = nullptr;
    QPushButton* clearButton_ = nullptr;
    QPushButton* measurePeaksButton_ = nullptr;
    QPushButton* flatFieldButton_ = nullptr;

    QVector<float> fullLine_;
    QVector<float> safetyArea_;
    QVector<float> visiblePart_;
    QVector<SpectrumPoint> spectrum_;
    QVector<double> averagedAmplitudePower_;
    QVector<double> averagedNoisePower_;
    QVector<double> maxHoldAmplitudePower_;
    QVector<MeasuredPeak> measuredPeaks_;
    QVector<float> flatFieldSamples_;
    int flatFieldLineLength_ = 0;
    int flatFieldLineCount_ = 0;
    bool flatFieldInputSignalValid_ = true;

    int selectedLine_ = -1;
    int zoomFactor_ = 1;
    int averageLine_ = -2;
    int averageZoomFactor_ = -1;
    int averageSourceIndex_ = -1;
    bool averageValid_ = false;

    double markerFrequencyHz_[2] = { 1'000'000.0, 4'433'618.75 };
    int activeMarker_ = 0;

    double noiseRmsVolts_ = 0.0;
    double weightedNoiseRmsVolts_ = 0.0;
    double snrDb_ = 0.0;
    double weightedSnrDb_ = 0.0;
    bool snrValid_ = false;
    bool weightedSnrValid_ = false;
    bool snrMeasurementEnabled_ = true;
    QString snrDisabledReason_;
    bool flatRegion_ = false;
    double flatnessSpanVolts_ = 0.0;
    double trackedSnrSpurFrequencyHz_ = 0.0;
    int trackedSnrSpurBin_ = -1;
    bool inputSignalValid_ = true;

    static constexpr int kFftSize = 4096;
    static constexpr int kFlatFieldFftSize = 1024;
    static constexpr double kNativeSampleClockHz = 13'500'000.0;
    static constexpr double kReconstructedSampleClockHz =
        kNativeSampleClockHz * 4.0;
    static constexpr double kMinimumSnrFrequencyHz = 100'000.0;
    static constexpr double kMaximumSnrFrequencyHz = 5'000'000.0;
    static constexpr double kSnrSpurSearchMinimumHz = 300'000.0;
    static constexpr double kSnrSpurSearchMaximumHz = 400'000.0;
    static constexpr int kSnrSpurNotchHalfWidthBins = 3;
    static constexpr double kOriginalNyquistFrequencyHz =
        kNativeSampleClockHz * 0.5;
    static constexpr int kMaximumMeasuredPeaks = 5;
    static constexpr double kMaximumDisplayFrequencyHz = 7'000'000.0;
    static constexpr double kReferenceVolts = 0.700;
    static constexpr double kMinimumDb = -90.0;
    static constexpr double kMaximumFlatnessSpanVolts = 0.050;
    static constexpr double kMinimumMeaningfulNoiseRmsVolts = 0.000050;
    static constexpr double kMaximumDb = 0.0;
};
