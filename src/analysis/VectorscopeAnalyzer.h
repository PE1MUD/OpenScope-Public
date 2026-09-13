#pragma once

#include "video/Yuv444Frame.h"
#include <vector>
#include <cstdint>
#include <QImage>
#include <QObject>

struct VectorscopeAnalyzerTimings
{
    std::uint64_t setupUs = 0;
    std::uint64_t statisticsUs = 0;
    std::uint64_t traceUs = 0;
    std::uint64_t glowUs = 0;
    std::uint64_t persistenceUs = 0;
    std::uint32_t glowDirtyTiles = 0;
    std::uint32_t glowTotalTiles = 0;
    std::uint32_t glowHorizontalPass1Tiles = 0;
    std::uint32_t glowVerticalPass1Tiles = 0;
    std::uint32_t glowHorizontalPass2Tiles = 0;
    std::uint32_t glowVerticalPass2Tiles = 0;
    std::int32_t glowActiveX = 0;
    std::int32_t glowActiveY = 0;
    std::int32_t glowActiveWidth = 0;
    std::int32_t glowActiveHeight = 0;
};

class VectorscopeAnalyzer : public QObject
{
    Q_OBJECT
public:
    VectorscopeAnalyzer();
    void setSelectedLine(int line);

    void setOutputSize(
        int width,
        int height);
    void setScale(double scale);
    void setGeometryScale(
        double horizontalScale,
        double verticalScale);
    void setPersistence(int persistence);
    void setGlow(int glow);

    void setHorizontalWindow(
        int zoomFactor,
        double scrollPosition);
    void analyze(const Yuv444Frame& frame);

    const QImage& image() const;
    [[nodiscard]] const VectorscopeAnalyzerTimings& renderTimings() const noexcept;

private:
    static constexpr int kAllLinesWidth = 360;
    static constexpr int kAllLinesHeight = 384;
    void renderSingleLine(const Yuv444Frame& frame);
    void renderAllLines(const Yuv444Frame& frame);
    void applyPersistence();
    void applyGlowPostProcess();
    std::uint32_t accumulateLineSegment(
        double x0,
        double y0,
        double x1,
        double y1,
        std::uint32_t energy);
    void accumulateLineSegmentInteger(
        int x0,
        int y0,
        int x1,
        int y1,
        std::uint32_t energy,
        int width,
        int height,
        std::vector<std::uint32_t>& density);
    std::vector<std::uint32_t> density_;
    QImage image_;
    QImage persistenceImage_;
    QImage allLinesImage_;
    std::vector<std::uint32_t> allLinesDensity_;
    int selectedLine_ = -1;
    double scale_ = 1.0;
    double geometryScaleX_ = 1.0;
    double geometryScaleY_ = 1.0;
    int persistence_ = 0;
    int glow_ = 50;
    int horizontalZoomFactor_ = 1;
    double horizontalScrollPosition_ = 0.0;
    VectorscopeAnalyzerTimings renderTimings_;
};