#pragma once

#include "analysis/Analyzer.h"
#include "analysis/VectorscopeAnalyzer.h"
#include "rendering/VectorscopeGraticule.h"

#include <QImage>
#include <QRectF>
#include <QString>
#include <cstdint>
#include <QThread>

class QPainter;

struct VectorscopeRenderTimings
{
    std::uint64_t analyzerStartUs = 0;
    std::uint64_t analyzerUs = 0;
    std::uint64_t glowPersistenceStartUs = 0;
    std::uint64_t glowPersistenceUs = 0;
    std::uint64_t composeStartUs = 0;
    std::uint64_t composeUs = 0;
    std::uint64_t overlayStartUs = 0;
    std::uint64_t overlayUs = 0;

    // Glow workload instrumentation consumed by VideoEngine.
    // Keep this interface in sync with the waveform renderer stats.
    std::uint64_t glowDirtyTiles = 0;
    std::uint64_t glowTotalTiles = 0;
    std::uint64_t glowHorizontalPass1Tiles = 0;
    std::uint64_t glowVerticalPass1Tiles = 0;
    std::uint64_t glowHorizontalPass2Tiles = 0;
    std::uint64_t glowVerticalPass2Tiles = 0;
    std::uint64_t glowActiveX = 0;
    std::uint64_t glowActiveY = 0;
    std::uint64_t glowActiveWidth = 0;
    std::uint64_t glowActiveHeight = 0;
};

struct VectorscopePresentationInfo
{
    QString source = QStringLiteral("BMD");
    QString input = QStringLiteral("Composite");
    QString standard = QStringLiteral("625/50d");
    QString targets = QStringLiteral("100%");
    QString matrix = QStringLiteral("BT.601");
    QString processing = QStringLiteral("YUV 4:2:2 10 bit");
};

class VectorscopeRenderer final : public Analyzer
{
public:
    enum class Profile
    {
        Screen,
        Video
    };

    explicit VectorscopeRenderer(Profile profile);

    void analyze(const Yuv444Frame& frame) override;

    void setOutputSize(int width, int height);
    void setContentScale(double horizontalScale, double verticalScale);
    void setSelectedLine(int line);
    void setPersistence(int persistence);
    void setGlow(int glow);
    void setColorizeGamutErrors(bool enabled) noexcept;
    void setHorizontalWindow(int zoomFactor, double scrollPosition);
    void setPresentationInfo(const VectorscopePresentationInfo& info);

    void moveAnalyzerToThread(QThread* thread);

    [[nodiscard]] const QImage& image() const;
    [[nodiscard]] const VectorscopeRenderTimings& renderTimings() const noexcept;

private:
    [[nodiscard]] QRectF contentRect() const;
    [[nodiscard]] double screenOwnerWidth(const QRectF& bounds) const;
    [[nodiscard]] QRectF screenScopeRect(const QRectF& bounds) const;
    [[nodiscard]] QRectF videoScopeRect(
        const QRectF& bounds,
        QRectF* topLeftCard,
        QRectF* topRightCard,
        QRectF* bottomLeftCard,
        QRectF* bottomRightCard) const;

    void composeScreen(
        QPainter& painter,
        const QRectF& bounds,
        const QRectF& scopeRect);
    void composeVideo(
        QPainter& painter,
        const QRectF& bounds,
        const QRectF& scopeRect,
        const QRectF& topLeftCard,
        const QRectF& topRightCard,
        const QRectF& bottomLeftCard,
        const QRectF& bottomRightCard);

    Profile profile_ = Profile::Screen;

    // Double-buffered Qt transport surfaces. Render into image_ while
    // publishedImage_ remains stable for the GUI/Spout consumer.
    QImage image_{1, 1, QImage::Format_RGB32};
    QImage publishedImage_{1, 1, QImage::Format_RGB32};

    VectorscopeAnalyzer analyzer_;
    VectorscopeGraticule graticule_;
    VectorscopePresentationInfo presentation_;

    int outputWidth_ = 1;
    int outputHeight_ = 1;
    double contentScaleX_ = 1.0;
    double contentScaleY_ = 1.0;
    int selectedLine_ = -1;
    bool colorizeGamutErrors_ = true;
    int horizontalZoomFactor_ = 1;
    double horizontalScrollPosition_ = 0.0;
    VectorscopeRenderTimings renderTimings_;
};
