#pragma once

#include "settings/OpenScopeSettings.h"

#include <QImage>
#include <QSize>
#include <QTimer>
#include <QWidget>

class QFocusEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QWheelEvent;

class VideoWidget : public QWidget
{
    Q_OBJECT

public:
    explicit VideoWidget(QWidget* parent = nullptr);

    void setImage(const QImage& image);
    void setInputSignalValid(bool valid);

    void setAspectRatio(
        OpenScopeSettings::AspectRatio aspectRatio);

    void setAntiAliasing(bool enabled);
    void setSafetyAreas(bool safetyArea90, bool textSafetyArea80);
    void refreshOutputSize();

signals:
    void outputSizeChanged(
        int width,
        int height);

    void imageClicked(
        double normalizedX,
        double normalizedY);

    void leftInteractionStarted();
    void doubleClickRestoreRequested();

    void rightClicked();

    void zoomInRequested();
    void zoomOutRequested();
    void lineUpRequested();
    void lineDownRequested();
    void panLeftRequested();
    void panRightRequested();
    void multiburstRequested();
    void spectrumRequested();

protected:
    void focusOutEvent(
        QFocusEvent* event) override;

    void keyPressEvent(
        QKeyEvent* event) override;

    void keyReleaseEvent(
        QKeyEvent* event) override;

    const QImage& image() const;

    QSize fitAspectSize(
        int availableWidth,
        int availableHeight) const;

    void emitOutputSize();

    void mousePressEvent(
        QMouseEvent* event) override;

    void mouseMoveEvent(
        QMouseEvent* event) override;

    void mouseDoubleClickEvent(
        QMouseEvent* event) override;

    void paintEvent(
        QPaintEvent* event) override;

    void resizeEvent(
        QResizeEvent* event) override;

    void wheelEvent(
        QWheelEvent* event) override;

private:
    bool emitImagePosition(
        const QPointF& position);

    void emitHeldArrowRequests();
    void stopArrowRepeatIfIdle();

    QImage image_;
    bool inputSignalValid_ = true;

    QTimer arrowRepeatTimer_;

    bool upHeld_ = false;
    bool downHeld_ = false;
    bool leftHeld_ = false;
    bool rightHeld_ = false;

    int wheelVerticalRemainder_ = 0;
    int wheelHorizontalRemainder_ = 0;

    OpenScopeSettings::AspectRatio aspectRatio_ =
        OpenScopeSettings::AspectRatio::Ratio16x9;

    bool antiAliasing_ = true;
    bool safetyArea90_ = false;
    bool textSafetyArea80_ = false;
};
