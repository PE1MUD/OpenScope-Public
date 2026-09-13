#include "widgets/VideoWidget.h"
#include "ui/ViewportOverlay.h"

#include <QColor>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

VideoWidget::VideoWidget(QWidget* parent)
    : QWidget(parent)
    , image_(720, 576, QImage::Format_RGB32)
{
    setFocusPolicy(Qt::StrongFocus);

    arrowRepeatTimer_.setSingleShot(false);
    arrowRepeatTimer_.setInterval(180);

    connect(
        &arrowRepeatTimer_,
        &QTimer::timeout,
        this,
        [this]()
        {
            // First repeat waits a little, subsequent repeats are smooth.
            if (arrowRepeatTimer_.interval() != 30)
            {
                arrowRepeatTimer_.setInterval(30);
            }

            emitHeldArrowRequests();
        });
}

void VideoWidget::setImage(const QImage& image)
{
    image_ = image;
    update();
}

void VideoWidget::setInputSignalValid(bool valid)
{
    if (inputSignalValid_ == valid)
    {
        return;
    }

    inputSignalValid_ = valid;
    update();
}

void VideoWidget::setAspectRatio(
    OpenScopeSettings::AspectRatio aspectRatio)
{
    if (aspectRatio_ == aspectRatio)
    {
        return;
    }

    aspectRatio_ = aspectRatio;

    emitOutputSize();
    update();
}

void VideoWidget::refreshOutputSize()
{
    emitOutputSize();
    update();
}


void VideoWidget::setAntiAliasing(bool enabled)
{
    if (antiAliasing_ == enabled)
    {
        return;
    }

    antiAliasing_ = enabled;
    update();
}

void VideoWidget::setSafetyAreas(
    bool safetyArea90,
    bool textSafetyArea80)
{
    if (safetyArea90_ == safetyArea90 &&
        textSafetyArea80_ == textSafetyArea80)
    {
        return;
    }

    safetyArea90_ = safetyArea90;
    textSafetyArea80_ = textSafetyArea80;
    update();
}

const QImage& VideoWidget::image() const
{
    return image_;
}

QSize VideoWidget::fitAspectSize(
    int availableWidth,
    int availableHeight) const
{
    availableWidth =
        (std::max)(availableWidth, 1);

    availableHeight =
        (std::max)(availableHeight, 1);

    const double aspectRatio =
        OpenScopeSettings::aspectRatioValue(
            aspectRatio_);

    int outputWidth =
        availableWidth;

    int outputHeight =
        static_cast<int>(
            std::lround(
                static_cast<double>(outputWidth) /
                aspectRatio));

    if (outputHeight > availableHeight)
    {
        outputHeight =
            availableHeight;

        outputWidth =
            static_cast<int>(
                std::lround(
                    static_cast<double>(outputHeight) *
                    aspectRatio));
    }

    return QSize(
        (std::max)(outputWidth, 1),
        (std::max)(outputHeight, 1));
}

void VideoWidget::emitOutputSize()
{
    const QSize outputSize =
        fitAspectSize(
            width(),
            height());

    emit outputSizeChanged(
        outputSize.width(),
        outputSize.height());
}

bool VideoWidget::emitImagePosition(
    const QPointF& position)
{
    if (image_.isNull())
    {
        return false;
    }

    const QSize outputSize =
        fitAspectSize(
            width(),
            height());

    const QRect imageRect(
        (width() - outputSize.width()) / 2,
        (height() - outputSize.height()) / 2,
        outputSize.width(),
        outputSize.height());

    if (!imageRect.contains(
            position.toPoint()))
    {
        return false;
    }

    const double normalizedX =
        std::clamp(
            (position.x() -
                static_cast<double>(imageRect.left())) /
                static_cast<double>(
                    (std::max)(imageRect.width() - 1, 1)),
            0.0,
            1.0);

    const double normalizedY =
        std::clamp(
            (position.y() -
                static_cast<double>(imageRect.top())) /
                static_cast<double>(
                    (std::max)(imageRect.height() - 1, 1)),
            0.0,
            1.0);

    emit imageClicked(
        normalizedX,
        normalizedY);

    return true;
}


void VideoWidget::focusOutEvent(
    QFocusEvent* event)
{
    upHeld_ = false;
    downHeld_ = false;
    leftHeld_ = false;
    rightHeld_ = false;

    arrowRepeatTimer_.stop();
    arrowRepeatTimer_.setInterval(180);

    QWidget::focusOutEvent(event);
}

void VideoWidget::emitHeldArrowRequests()
{
    // Opposite directions cancel each other. Orthogonal directions
    // are intentionally emitted together, so e.g. Up+Right works.
    if (upHeld_ != downHeld_)
    {
        if (upHeld_)
        {
            emit lineUpRequested();
        }
        else
        {
            emit lineDownRequested();
        }
    }

    if (leftHeld_ != rightHeld_)
    {
        if (leftHeld_)
        {
            emit panLeftRequested();
        }
        else
        {
            emit panRightRequested();
        }
    }
}

void VideoWidget::stopArrowRepeatIfIdle()
{
    if (!upHeld_ &&
        !downHeld_ &&
        !leftHeld_ &&
        !rightHeld_)
    {
        arrowRepeatTimer_.stop();
        arrowRepeatTimer_.setInterval(180);
    }
}

void VideoWidget::keyPressEvent(
    QKeyEvent* event)
{
    switch (event->key())
    {
    case Qt::Key_Plus:
        if (!event->isAutoRepeat())
        {
            emit zoomInRequested();
        }
        event->accept();
        return;

    case Qt::Key_Minus:
        if (!event->isAutoRepeat())
        {
            emit zoomOutRequested();
        }
        event->accept();
        return;

    case Qt::Key_M:
        if (!event->isAutoRepeat())
        {
            emit multiburstRequested();
        }
        event->accept();
        return;

    case Qt::Key_F:
        if (!event->isAutoRepeat())
        {
            emit spectrumRequested();
        }
        event->accept();
        return;

    case Qt::Key_Up:
        if (!event->isAutoRepeat() && !upHeld_)
        {
            upHeld_ = true;
            emit lineUpRequested();

            if (!arrowRepeatTimer_.isActive())
            {
                arrowRepeatTimer_.setInterval(180);
                arrowRepeatTimer_.start();
            }
        }
        event->accept();
        return;

    case Qt::Key_Down:
        if (!event->isAutoRepeat() && !downHeld_)
        {
            downHeld_ = true;
            emit lineDownRequested();

            if (!arrowRepeatTimer_.isActive())
            {
                arrowRepeatTimer_.setInterval(180);
                arrowRepeatTimer_.start();
            }
        }
        event->accept();
        return;

    case Qt::Key_Left:
        if (!event->isAutoRepeat() && !leftHeld_)
        {
            leftHeld_ = true;
            emit panLeftRequested();

            if (!arrowRepeatTimer_.isActive())
            {
                arrowRepeatTimer_.setInterval(180);
                arrowRepeatTimer_.start();
            }
        }
        event->accept();
        return;

    case Qt::Key_Right:
        if (!event->isAutoRepeat() && !rightHeld_)
        {
            rightHeld_ = true;
            emit panRightRequested();

            if (!arrowRepeatTimer_.isActive())
            {
                arrowRepeatTimer_.setInterval(180);
                arrowRepeatTimer_.start();
            }
        }
        event->accept();
        return;

    default:
        break;
    }

    QWidget::keyPressEvent(event);
}

void VideoWidget::keyReleaseEvent(
    QKeyEvent* event)
{
    if (event->isAutoRepeat())
    {
        event->accept();
        return;
    }

    switch (event->key())
    {
    case Qt::Key_Up:
        upHeld_ = false;
        break;

    case Qt::Key_Down:
        downHeld_ = false;
        break;

    case Qt::Key_Left:
        leftHeld_ = false;
        break;

    case Qt::Key_Right:
        rightHeld_ = false;
        break;

    default:
        QWidget::keyReleaseEvent(event);
        return;
    }

    stopArrowRepeatIfIdle();
    event->accept();
}

void VideoWidget::mousePressEvent(
    QMouseEvent* event)
{
    setFocus(Qt::MouseFocusReason);

    if (event->button() == Qt::LeftButton)
    {
        const QSize outputSize =
            fitAspectSize(
                width(),
                height());

        const QRect imageRect(
            (width() - outputSize.width()) / 2,
            (height() - outputSize.height()) / 2,
            outputSize.width(),
            outputSize.height());

        if (imageRect.contains(
                event->position().toPoint()))
        {
            emit leftInteractionStarted();

            if (emitImagePosition(
                    event->position()))
            {
                event->accept();
                return;
            }
        }
    }

    if (event->button() == Qt::RightButton)
    {
        const QSize outputSize =
            fitAspectSize(
                width(),
                height());

        const QRect imageRect(
            (width() - outputSize.width()) / 2,
            (height() - outputSize.height()) / 2,
            outputSize.width(),
            outputSize.height());

        if (imageRect.contains(
                event->position().toPoint()))
        {
            emit rightClicked();
            event->accept();
            return;
        }
    }

    QWidget::mousePressEvent(event);
}

void VideoWidget::mouseMoveEvent(
    QMouseEvent* event)
{
    if ((event->buttons() & Qt::LeftButton) != 0 &&
        emitImagePosition(
            event->position()))
    {
        event->accept();
        return;
    }

    QWidget::mouseMoveEvent(event);
}

void VideoWidget::mouseDoubleClickEvent(
    QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        // The first click of a Qt double-click has already travelled
        // through mousePressEvent() and therefore may have changed the
        // selected waveform line / X position. Restore that pre-click
        // state before the parent maximizes the viewport.
        emit doubleClickRestoreRequested();
    }

    // Let the containing ScopeViewport handle maximize/restore.
    event->ignore();
}

void VideoWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter painter(this);

    painter.fillRect(
        rect(),
        Qt::black);

    if (!inputSignalValid_)
    {
        ViewportOverlay::drawNoVideo(
            painter,
            QRectF(rect()));
        return;
    }

    if (image_.isNull())
    {
        return;
    }

    const QSize outputSize =
        fitAspectSize(
            width(),
            height());

    const int x =
        (width() - outputSize.width()) / 2;

    const int y =
        (height() - outputSize.height()) / 2;

    painter.setRenderHint(
        QPainter::SmoothPixmapTransform,
        antiAliasing_);

    const QRect imageRect(
        x,
        y,
        outputSize.width(),
        outputSize.height());

    painter.drawImage(
        imageRect,
        image_);

    const auto drawSafetyFrame =
        [&painter, &imageRect](double fraction)
        {
            const double w = imageRect.width() * fraction;
            const double h = imageRect.height() * fraction;
            const QRectF frame(
                imageRect.center().x() - w * 0.5,
                imageRect.center().y() - h * 0.5,
                w,
                h);

            // Match the selected-line highlight: invert the displayed
            // video under the 2 px guide instead of drawing a fixed
            // white/grey overlay. With an opaque white source,
            // Difference gives 255 - destination for every channel.
            painter.save();
            painter.setCompositionMode(
                QPainter::CompositionMode_Difference);

            QPen pen(Qt::white);
            pen.setWidth(2);
            pen.setCosmetic(true);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(frame);
            painter.restore();
        };

    if (safetyArea90_)
    {
        drawSafetyFrame(0.90);
    }

    if (textSafetyArea80_)
    {
        drawSafetyFrame(0.80);
    }
}


void VideoWidget::wheelEvent(
    QWheelEvent* event)
{
    setFocus(Qt::MouseFocusReason);

    QPoint delta = event->angleDelta();
    int threshold = 120;

    if (delta.isNull())
    {
        delta = event->pixelDelta();
        threshold = 40;
    }

    if (event->inverted())
    {
        delta = -delta;
    }

    wheelVerticalRemainder_ += delta.y();
    wheelHorizontalRemainder_ += delta.x();

    while (wheelVerticalRemainder_ >= threshold)
    {
        wheelVerticalRemainder_ -= threshold;
        emit lineUpRequested();
    }

    while (wheelVerticalRemainder_ <= -threshold)
    {
        wheelVerticalRemainder_ += threshold;
        emit lineDownRequested();
    }

    while (wheelHorizontalRemainder_ >= threshold)
    {
        wheelHorizontalRemainder_ -= threshold;
        emit panLeftRequested();
    }

    while (wheelHorizontalRemainder_ <= -threshold)
    {
        wheelHorizontalRemainder_ += threshold;
        emit panRightRequested();
    }

    if (!delta.isNull())
    {
        event->accept();
        return;
    }

    QWidget::wheelEvent(event);
}

void VideoWidget::resizeEvent(
    QResizeEvent* event)
{
    QWidget::resizeEvent(event);

    emitOutputSize();
}
