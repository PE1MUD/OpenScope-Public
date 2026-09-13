#include "output/WasapiPcmOutput.h"
#include "output/AsioPcmOutput.h"
#include "widgets/ControlWidget.h"
#include "BuildConfig.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <utility>
#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFont>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>
#include <QPaintEvent>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextBrowser>
#include <QToolButton>
#include <QVBoxLayout>
#include <deque>

class AsioBufferGraphWidget : public QWidget
{
public:
    explicit AsioBufferGraphWidget(QWidget* parent = nullptr) : QWidget(parent)
    {
        setMinimumSize(720, 320);
    }

    void append(double raw, double fast, double avg3, double target, double ppm)
    {
        raw_.push_back(raw);
        fast_.push_back(fast);
        avg3_.push_back(avg3);
        target_.push_back(target);
        ppm_.push_back(ppm);
        constexpr std::size_t Max = 600; // 60 s at the 100 ms UI update rate.
        while (raw_.size() > Max)
        {
            raw_.pop_front(); fast_.pop_front(); avg3_.pop_front(); target_.pop_front(); ppm_.pop_front();
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), palette().base());

        // Leave room for the live numeric readout and real axis labels.
        const QRectF r = QRectF(rect()).adjusted(58, 42, -18, -48);
        if (r.width() <= 1.0 || r.height() <= 1.0)
            return;

        double ymax = 10.0;
        for (double v : raw_) ymax = std::max(ymax, v * 1.12);
        for (double v : fast_) ymax = std::max(ymax, v * 1.12);
        for (double v : avg3_) ymax = std::max(ymax, v * 1.12);
        for (double v : target_) ymax = std::max(ymax, v * 1.12);
        // Round the displayed range up to a useful 5 ms boundary.
        ymax = std::max(5.0, std::ceil(ymax / 5.0) * 5.0);

        p.setPen(palette().mid().color());
        for (int i = 0; i <= 4; ++i)
        {
            const double y = r.top() + r.height() * i / 4.0;
            p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
            const double value = ymax * (1.0 - static_cast<double>(i) / 4.0);
            p.setPen(palette().text().color());
            p.drawText(QRectF(2, y - 9, 50, 18), Qt::AlignRight | Qt::AlignVCenter,
                       QStringLiteral("%1").arg(value, 0, 'f', value < 10.0 ? 1 : 0));
            p.setPen(palette().mid().color());
        }

        auto drawSeries = [&](const std::deque<double>& d, const QPen& pen)
        {
            if (d.size() < 2) return;
            p.setPen(pen);
            QPolygonF poly;
            poly.reserve(static_cast<int>(d.size()));
            for (std::size_t i = 0; i < d.size(); ++i)
            {
                const double x = r.left() + r.width() * static_cast<double>(i) /
                    static_cast<double>(std::max<std::size_t>(1, d.size() - 1));
                const double y = r.bottom() - r.height() * std::clamp(d[i] / ymax, 0.0, 1.0);
                poly << QPointF(x, y);
            }
            p.drawPolyline(poly);
        };

        drawSeries(raw_, QPen(Qt::gray, 1));
        drawSeries(fast_, QPen(Qt::blue, 2));
        drawSeries(avg3_, QPen(Qt::black, 3));
        drawSeries(target_, QPen(Qt::darkGreen, 1, Qt::DashLine));

        p.setPen(palette().text().color());
        p.drawText(QRectF(4, 2, width() - 8, 18), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("Buffer low-water (ms)"));

        if (!raw_.empty())
        {
            const QString live = QStringLiteral("raw %1 ms   fast %2 ms   3 s %3 ms   target %4 ms   nudge %5 ppm")
                .arg(raw_.back(), 0, 'f', 3)
                .arg(fast_.back(), 0, 'f', 3)
                .arg(avg3_.back(), 0, 'f', 3)
                .arg(target_.back(), 0, 'f', 3)
                .arg(ppm_.back(), 0, 'f', 0);
            p.drawText(QRectF(4, 20, width() - 8, 18), Qt::AlignLeft | Qt::AlignVCenter, live);
        }

        const double secondsVisible = raw_.size() > 1 ? static_cast<double>(raw_.size() - 1) * 0.1 : 0.0;
        p.drawText(QRectF(r.left(), r.bottom() + 5, 100, 18), Qt::AlignLeft,
                   QStringLiteral("-%1 s").arg(secondsVisible, 0, 'f', 1));
        p.drawText(QRectF(r.right() - 80, r.bottom() + 5, 80, 18), Qt::AlignRight,
                   QStringLiteral("now"));
        p.drawText(QRectF(4, height() - 22, width() - 8, 18), Qt::AlignLeft,
                   QStringLiteral("gray raw   blue fast   black 3 s avg   dashed target"));
    }

private:
    std::deque<double> raw_, fast_, avg3_, target_, ppm_;
};

namespace
{
    int chromaUiToInternal(int value)
    {
        value = std::clamp(value, 0, 100);

        if (value == 0)
        {
            return 0;
        }

        // UI 1..100 maps to the previously useful UI range 20..100.
        const double oldUiValue =
            20.0 +
            static_cast<double>(value - 1) *
                (80.0 / 99.0);

        return std::clamp(
            static_cast<int>(
                std::lround(oldUiValue * 2.0)),
            40,
            200);
    }

    int chromaInternalToUi(int intensity)
    {
        intensity = std::clamp(intensity, 0, 200);

        if (intensity == 0)
        {
            return 0;
        }

        const double oldUiValue =
            static_cast<double>(intensity) / 2.0;

        return std::clamp(
            1 +
                static_cast<int>(
                    std::lround(
                        (oldUiValue - 20.0) *
                        (99.0 / 80.0))),
            1,
            100);
    }

    class ValueSlider final : public QSlider
    {
    public:
        explicit ValueSlider(
            Qt::Orientation orientation,
            QWidget* parent = nullptr)
            : QSlider(
                orientation,
                parent)
        {
            setMinimumHeight(
                22);

            setMaximumHeight(
                22);
        }

        void setValueFormatter(
            std::function<QString(int)> formatter)
        {
            valueFormatter_ =
                std::move(formatter);

            update();
        }

        void setDoubleClickResetsToMidpoint(bool enabled)
        {
            doubleClickResetsToMidpoint_ = enabled;
            doubleClickResetValue_.reset();
        }

        void setDoubleClickResetValue(int value)
        {
            doubleClickResetValue_ = value;
            doubleClickResetsToMidpoint_ = false;
        }

    protected:
        void mousePressEvent(
            QMouseEvent* event) override
        {
            if (event->button() == Qt::LeftButton)
            {
                setSliderDown(
                    true);

                setValueFromMouse(
                    event->position());

                event->accept();
                return;
            }

            QSlider::mousePressEvent(
                event);
        }

        void mouseMoveEvent(
            QMouseEvent* event) override
        {
            if (isSliderDown() &&
                (event->buttons() & Qt::LeftButton) != 0)
            {
                setValueFromMouse(
                    event->position());

                event->accept();
                return;
            }

            QSlider::mouseMoveEvent(
                event);
        }

        void mouseReleaseEvent(
            QMouseEvent* event) override
        {
            if (event->button() == Qt::LeftButton &&
                isSliderDown())
            {
                setValueFromMouse(
                    event->position());

                setSliderDown(
                    false);

                event->accept();
                return;
            }

            QSlider::mouseReleaseEvent(
                event);
        }

        void mouseDoubleClickEvent(
            QMouseEvent* event) override
        {
            if ((doubleClickResetsToMidpoint_ ||
                    doubleClickResetValue_.has_value()) &&
                event->button() == Qt::LeftButton)
            {
                const int resetValue =
                    doubleClickResetValue_.value_or(
                        minimum() +
                        (maximum() - minimum()) / 2);

                setValue(
                    std::clamp(
                        resetValue,
                        minimum(),
                        maximum()));
                emit sliderReleased();

                event->accept();
                return;
            }

            QSlider::mouseDoubleClickEvent(event);
        }

        void paintEvent(
            QPaintEvent* event) override
        {
            Q_UNUSED(event);

            QStyleOptionSlider option;
            initStyleOption(
                &option);

            QPainter painter(
                this);

            /*
             * Draw the native groove, but not the native handle.  The value
             * bubble below is the actual handle and needs a wider safe travel
             * range than the platform handle.  Letting Qt place the small
             * native handle at the absolute end while centring our wider
             * bubble on it caused the bubble to be clipped at 0 / 100.
             */
            QStyleOptionSlider grooveOption = option;
            grooveOption.subControls =
                QStyle::SC_SliderGroove;

            style()->drawComplexControl(
                QStyle::CC_Slider,
                &grooveOption,
                &painter,
                this);

            const int valueWidth =
                valueFormatter_
                ? 58
                : 38;

            const int valueHeight =
                std::max(
                    18,
                    height() - 2);

            const int span =
                std::max(
                    width() - valueWidth,
                    1);

            const int sliderPosition =
                QStyle::sliderPositionFromValue(
                    minimum(),
                    maximum(),
                    value(),
                    span,
                    invertedAppearance());

            QRect handleRect(
                sliderPosition,
                (height() - valueHeight) / 2,
                valueWidth,
                valueHeight);

            painter.setRenderHint(
                QPainter::Antialiasing);

            painter.setPen(
                palette().color(
                    QPalette::Mid));

            painter.setBrush(
                palette().color(
                    QPalette::Button));

            painter.drawRoundedRect(
                handleRect.adjusted(
                    0,
                    1,
                    -1,
                    -1),
                3.0,
                3.0);

            QFont font =
                painter.font();

            if (font.pointSizeF() > 0.0)
            {
                font.setPointSizeF(
                    std::max(
                        7.0,
                        font.pointSizeF() - 1.0));
            }

            font.setBold(
                true);

            painter.setFont(
                font);

            painter.setPen(
                isEnabled()
                ? palette().color(
                    QPalette::ButtonText)
                : palette().color(
                    QPalette::Disabled,
                    QPalette::ButtonText));

            painter.drawText(
                handleRect,
                Qt::AlignCenter,
                valueFormatter_
                ? valueFormatter_(value())
                : QString::number(value()));
        }

    private:
        std::function<QString(int)> valueFormatter_;
        bool doubleClickResetsToMidpoint_ = false;
        std::optional<int> doubleClickResetValue_;

        void setValueFromMouse(
            const QPointF& position)
        {
            const int valueWidth =
                valueFormatter_
                ? 58
                : 38;

            const int span =
                (std::max)(
                    width() - valueWidth,
                    1);

            const int pixelPosition =
                std::clamp(
                    static_cast<int>(
                        std::lround(
                            position.x() -
                            valueWidth * 0.5)),
                    0,
                    span);

            setValue(
                QStyle::sliderValueFromPosition(
                    minimum(),
                    maximum(),
                    pixelPosition,
                    span,
                    invertedAppearance()));
        }
    };

    QWidget* createSliderRow(
        const QString& labelText,
        QSlider*& slider,
        int minimum,
        int maximum,
        int value,
        QWidget* parent,
        std::function<QString(int)> valueFormatter = {})
    {
        auto* row =
            new QWidget(parent);

        auto* layout =
            new QHBoxLayout(row);

        layout->setContentsMargins(
            0,
            0,
            0,
            0);

        layout->setSpacing(8);

        auto* label =
            new QLabel(
                labelText,
                row);

        // Keep every user-facing slider on the same horizontal ruler.
        // Longest current label ("Color carrier intensity") fits here,
        // while still leaving useful slider travel in a 380 px matrix cell.
        label->setFixedWidth(
            150);

        auto* valueSlider =
            new ValueSlider(
                Qt::Horizontal,
                row);

        valueSlider->setValueFormatter(
            std::move(valueFormatter));

        slider =
            valueSlider;

        slider->setRange(
            minimum,
            maximum);

        slider->setValue(
            value);

        layout->addWidget(
            label);

        layout->addWidget(
            slider,
            1);

        return row;
    }
}

ControlWidget::ControlWidget(
    const OpenScopeSettings& settings,
    QWidget* parent)
    : QWidget(parent)
{
    auto* outerLayout =
        new QVBoxLayout(this);

    outerLayout->setContentsMargins(
        4,
        4,
        4,
        4);

    outerLayout->setSpacing(0);

    tabs_ =
        new QTabWidget(this);

    auto* tabs =
        tabs_;

    outerLayout->addWidget(tabs_);

    // ------------------------------------------------------------
    // Display
    // ------------------------------------------------------------
    auto* displayTab =
        new QWidget(tabs);

    auto* displayLayout =
        new QVBoxLayout(displayTab);

    displayLayout->setContentsMargins(
        6,
        4,
        6,
        4);

    displayLayout->setSpacing(4);

    auto* noiseReductionCheckBox =
        new QCheckBox(
            "Enable noise reduction",
            displayTab);

    noiseReductionCheckBox->setChecked(
        settings.control
            .processing
            .noiseFilter
            .enabled);

    displayLayout->addWidget(
        noiseReductionCheckBox);

    QSlider* noiseReductionSlider = nullptr;

    QWidget* noiseReductionRow =
        createSliderRow(
            "Noise reduction intensity",
            noiseReductionSlider,
            0,
            100,
            std::clamp(
                settings.control
                    .processing
                    .noiseFilter
                    .strength,
                0,
                100),
            displayTab);

    noiseReductionSlider->setEnabled(
        noiseReductionCheckBox->isChecked());

    displayLayout->addWidget(
        noiseReductionRow);

    auto* lineSelectorVisibleCheckBox =
        new QCheckBox(
            "Line Selector Visible",
            displayTab);
    lineSelectorVisibleCheckBox->setChecked(
        settings.local.display.lineSelectorVisible);
    displayLayout->addWidget(lineSelectorVisibleCheckBox);

    auto* safetyArea90CheckBox =
        new QCheckBox(
            "Safety area 90%",
            displayTab);
    safetyArea90CheckBox->setChecked(
        settings.local.display.safetyArea90);
    displayLayout->addWidget(safetyArea90CheckBox);

    auto* textSafetyArea80CheckBox =
        new QCheckBox(
            "Text safety area 80%",
            displayTab);
    textSafetyArea80CheckBox->setChecked(
        settings.local.display.textSafetyArea80);
    displayLayout->addWidget(textSafetyArea80CheckBox);

    displayLayout->addStretch();

    connect(
        noiseReductionCheckBox,
        &QCheckBox::toggled,
        noiseReductionSlider,
        &QSlider::setEnabled);

    connect(
        noiseReductionCheckBox,
        &QCheckBox::toggled,
        this,
        &ControlWidget::noiseReductionChanged);

    connect(
        noiseReductionSlider,
        &QSlider::valueChanged,
        this,
        &ControlWidget::noiseReductionIntensityChanged);

    connect(lineSelectorVisibleCheckBox, &QCheckBox::toggled,
        this, &ControlWidget::lineSelectorVisibleChanged);
    connect(safetyArea90CheckBox, &QCheckBox::toggled,
        this, &ControlWidget::safetyArea90Changed);
    connect(textSafetyArea80CheckBox, &QCheckBox::toggled,
        this, &ControlWidget::textSafetyArea80Changed);

    tabs->addTab(
        displayTab,
        "Display");

    // ------------------------------------------------------------
    // Calibration
    // ------------------------------------------------------------
    auto* calibrationTab =
        new QWidget(tabs);

    auto* calibrationLayout =
        new QVBoxLayout(calibrationTab);

    calibrationLayout->setContentsMargins(
        6,
        4,
        6,
        4);

    calibrationLayout->setSpacing(4);

    auto* deckLinkHeading =
        new QLabel(
            "Blackmagic Level Controls",
            calibrationTab);

    QFont headingFont =
        deckLinkHeading->font();
    headingFont.setBold(true);
    deckLinkHeading->setFont(headingFont);

    calibrationLayout->addWidget(
        deckLinkHeading);

    compositeGainStatusLabel_ =
        new QLabel(
            "Waiting for DeckLink device...",
            calibrationTab);

    calibrationLayout->addWidget(
        compositeGainStatusLabel_);

    QWidget* compositeLumaGainRow =
        createSliderRow(
            "Luma gain",
            compositeLumaGainSlider_,
            0,
            0,
            0,
            calibrationTab,
            [this](int value)
            {
                if (compositeLumaGainSlider_ == nullptr)
                {
                    return QStringLiteral("1.00x");
                }

                const int minimum =
                    compositeLumaGainSlider_->minimum();

                const int maximum =
                    compositeLumaGainSlider_->maximum();

                const double gain =
                    maximum > minimum
                    ? 2.0 *
                        static_cast<double>(value - minimum) /
                        static_cast<double>(maximum - minimum)
                    : 1.0;

                return
                    QString::number(gain, 'f', 2) +
                    QStringLiteral("x");
            });

    static_cast<ValueSlider*>(
        compositeLumaGainSlider_)
        ->setDoubleClickResetsToMidpoint(true);

    compositeLumaGainSlider_->setEnabled(false);

    calibrationLayout->addWidget(
        compositeLumaGainRow);

    QWidget* compositeChromaGainRow =
        createSliderRow(
            "Chroma gain",
            compositeChromaGainSlider_,
            0,
            0,
            0,
            calibrationTab,
            [this](int value)
            {
                if (compositeChromaGainSlider_ == nullptr)
                {
                    return QStringLiteral("1.00x");
                }

                const int minimum =
                    compositeChromaGainSlider_->minimum();

                const int maximum =
                    compositeChromaGainSlider_->maximum();

                const double gain =
                    maximum > minimum
                    ? 2.0 *
                        static_cast<double>(value - minimum) /
                        static_cast<double>(maximum - minimum)
                    : 1.0;

                return
                    QString::number(gain, 'f', 2) +
                    QStringLiteral("x");
            });

    static_cast<ValueSlider*>(
        compositeChromaGainSlider_)
        ->setDoubleClickResetsToMidpoint(true);

    compositeChromaGainSlider_->setEnabled(false);

    calibrationLayout->addWidget(
        compositeChromaGainRow);

    auto* lumaCompensationHeading =
        new QLabel(
            "OpenScope luma response correction",
            calibrationTab);

    lumaCompensationHeading->setFont(headingFont);

    calibrationLayout->addSpacing(8);
    calibrationLayout->addWidget(
        lumaCompensationHeading);

    auto* lumaCompensationCheckBox =
        new QCheckBox(
            "Enable Y frequency response correction",
            calibrationTab);

    lumaCompensationCheckBox->setChecked(
        settings.control
            .processing
            .lumaCompensation
            .enabled);

    calibrationLayout->addWidget(
        lumaCompensationCheckBox);

    QSlider* lumaCompensationSlider = nullptr;

    QWidget* lumaCompensationRow =
        createSliderRow(
            "Y HF comp @ 5.8 MHz",
            lumaCompensationSlider,
            0,
            100,
            std::clamp(
                settings.control
                    .processing
                    .lumaCompensation
                    .gainHundredthsDb,
                0,
                100),
            calibrationTab,
            [](int value)
            {
                return
                    QString::number(
                        static_cast<double>(value) / 100.0,
                        'f',
                        2) +
                    " dB";
            });

    static_cast<ValueSlider*>(
        lumaCompensationSlider)
        ->setDoubleClickResetsToMidpoint(true);

    lumaCompensationSlider->setEnabled(
        lumaCompensationCheckBox->isChecked());

    calibrationLayout->addWidget(
        lumaCompensationRow);

    calibrationLayout->addStretch();

    connect(
        compositeLumaGainSlider_,
        &QSlider::valueChanged,
        this,
        &ControlWidget::compositeLumaGainChanged);

    connect(
        compositeChromaGainSlider_,
        &QSlider::valueChanged,
        this,
        &ControlWidget::compositeChromaGainChanged);

    connect(
        compositeLumaGainSlider_,
        &QSlider::sliderReleased,
        this,
        &ControlWidget::compositeGainCommitRequested);

    connect(
        compositeChromaGainSlider_,
        &QSlider::sliderReleased,
        this,
        &ControlWidget::compositeGainCommitRequested);

    connect(
        lumaCompensationCheckBox,
        &QCheckBox::toggled,
        lumaCompensationSlider,
        &QSlider::setEnabled);

    connect(
        lumaCompensationCheckBox,
        &QCheckBox::toggled,
        this,
        &ControlWidget::lumaCompensationChanged);

    connect(
        lumaCompensationSlider,
        &QSlider::valueChanged,
        this,
        &ControlWidget::lumaCompensationGainChanged);

    // Calibration is inserted immediately before Help below.

    // ------------------------------------------------------------
    // Instrument
    // ------------------------------------------------------------
    auto* instrumentTab =
        new QWidget(tabs);

    auto* instrumentLayout =
        new QVBoxLayout(instrumentTab);

    instrumentLayout->setContentsMargins(
        6,
        4,
        6,
        4);

    instrumentLayout->setSpacing(4);

    auto* lineRow =
        new QWidget(instrumentTab);

    auto* lineLayout =
        new QHBoxLayout(lineRow);

    lineLayout->setContentsMargins(
        0,
        0,
        0,
        0);

    lineLayout->setSpacing(
        8);

    lineLayout->addWidget(
        new QLabel(
            "Line selector",
            lineRow));

    lineSelector_ =
        new QSpinBox(lineRow);

    lineSelector_->setRange(
        -1,
        575);

    lineSelector_->setSpecialValueText(
        "All Lines");

    lineSelector_->setFixedHeight(
        22);

    lineSelector_->setValue(
        settings.control
            .instrument
            .lineNumber);

    lineLayout->addWidget(
        lineSelector_,
        1);

    instrumentLayout->addWidget(
        lineRow);

    auto* antiAliasingCheckBox =
        new QCheckBox(
            "Anti Aliasing",
            instrumentTab);

    antiAliasingCheckBox->setChecked(
        settings.control
            .instrument
            .waveform
            .antiAliasing);

    instrumentLayout->addSpacing(
        8);

    instrumentLayout->addWidget(
        antiAliasingCheckBox);

    auto* vintageCheckBox =
        new QCheckBox(
            "Vintage Look",
            instrumentTab);

    vintageCheckBox->setChecked(
        settings.control
            .instrument
            .waveform
            .vintageLook);

    instrumentLayout->addWidget(
        vintageCheckBox);

    auto* colorizeIllegalLuminanceCheckBox =
        new QCheckBox(
            "Colorize Illegal Luminance",
            instrumentTab);

    colorizeIllegalLuminanceCheckBox->setChecked(
        settings.control
            .instrument
            .waveform
            .colorizeIllegalLuminance);

    instrumentLayout->addWidget(
        colorizeIllegalLuminanceCheckBox);

    auto* colorizeGamutErrorsCheckBox =
        new QCheckBox(
            "Colorize Gamut Errors",
            instrumentTab);

    colorizeGamutErrorsCheckBox->setChecked(
        settings.control
            .instrument
            .vectorscope
            .colorizeGamutErrors);

    instrumentLayout->addWidget(
        colorizeGamutErrorsCheckBox);

    instrumentLayout->addSpacing(
        8);

    connect(
        antiAliasingCheckBox,
        &QCheckBox::toggled,
        this,
        &ControlWidget::antiAliasingChanged);

    connect(
        colorizeIllegalLuminanceCheckBox,
        &QCheckBox::toggled,
        this,
        &ControlWidget::colorizeIllegalLuminanceChanged);

    connect(
        colorizeGamutErrorsCheckBox,
        &QCheckBox::toggled,
        this,
        &ControlWidget::colorizeGamutErrorsChanged);

    auto* zoomRow =
        new QWidget(instrumentTab);

    auto* zoomLayout =
        new QHBoxLayout(zoomRow);

    zoomLayout->setContentsMargins(
        0,
        0,
        0,
        0);

    zoomLayout->setSpacing(
        4);

    zoomLayout->addWidget(
        new QLabel(
            "Waveform X",
            zoomRow));

    waveformZoomButtonGroup_ =
        new QButtonGroup(this);

    waveformZoomButtonGroup_->setExclusive(
        true);

    waveformZoom1Button_ =
        new QToolButton(zoomRow);

    waveformZoom5Button_ =
        new QToolButton(zoomRow);

    waveformZoom10Button_ =
        new QToolButton(zoomRow);

    waveformZoom1Button_->setText(
        "X1");

    waveformZoom5Button_->setText(
        "X5");

    waveformZoom10Button_->setText(
        "X10");

    waveformZoom1Button_->setFixedHeight(
        22);

    waveformZoom5Button_->setFixedHeight(
        22);

    waveformZoom10Button_->setFixedHeight(
        22);

    waveformZoom1Button_->setCheckable(
        true);

    waveformZoom5Button_->setCheckable(
        true);

    waveformZoom10Button_->setCheckable(
        true);

    waveformZoomButtonGroup_->addButton(
        waveformZoom1Button_,
        1);

    waveformZoomButtonGroup_->addButton(
        waveformZoom5Button_,
        5);

    waveformZoomButtonGroup_->addButton(
        waveformZoom10Button_,
        10);

    const int waveformZoom =
        settings.control
            .instrument
            .waveform
            .zoom;

    QAbstractButton* checkedZoomButton =
        waveformZoomButtonGroup_->button(
            waveformZoom);

    if (checkedZoomButton == nullptr)
    {
        checkedZoomButton =
            waveformZoom1Button_;
    }

    checkedZoomButton->setChecked(
        true);

    const bool waveformZoomEnabled =
        lineSelector_->value() >= 0;

    waveformZoom1Button_->setEnabled(
        waveformZoomEnabled);

    waveformZoom5Button_->setEnabled(
        waveformZoomEnabled);

    waveformZoom10Button_->setEnabled(
        waveformZoomEnabled);

    zoomLayout->addWidget(
        waveformZoom1Button_);

    zoomLayout->addWidget(
        waveformZoom5Button_);

    zoomLayout->addWidget(
        waveformZoom10Button_);

    zoomLayout->addStretch(
        1);

    instrumentLayout->addWidget(
        zoomRow);

    QSlider* chromaIntensitySlider = nullptr;

    QWidget* chromaIntensityRow =
        createSliderRow(
            "Color carrier intensity",
            chromaIntensitySlider,
            0,
            100,
            chromaInternalToUi(
                settings.control
                    .instrument
                    .waveform
                    .chromaRenderIntensity),
            instrumentTab);

    instrumentLayout->addWidget(
        chromaIntensityRow);

    QSlider* persistenceSlider = nullptr;

    QWidget* persistenceRow =
        createSliderRow(
            "Scopephor",
            persistenceSlider,
            0,
            100,
            std::clamp(
                settings.control
                    .instrument
                    .waveform
                    .persistenceFrames,
                0,
                200) /
                2,
            instrumentTab);

    instrumentLayout->addWidget(
        persistenceRow);

    QSlider* coreIntensitySlider = nullptr;

    QWidget* coreIntensityRow =
        createSliderRow(
            "Core intensity",
            coreIntensitySlider,
            0,
            100,
            std::clamp(
                settings.control
                    .instrument
                    .waveform
                    .coreIntensity,
                0,
                100),
            instrumentTab);

    instrumentLayout->addWidget(
        coreIntensityRow);

    QSlider* vectorscopeGlowSlider = nullptr;

    QWidget* vectorscopeGlowRow =
        createSliderRow(
            "Beam Glow",
            vectorscopeGlowSlider,
            0,
            100,
            std::clamp(
                settings.control
                    .instrument
                    .vectorscope
                    .glow,
                0,
                100),
            instrumentTab);

    instrumentLayout->addWidget(
        vectorscopeGlowRow);

    auto* defaultsRow =
        new QWidget(
            instrumentTab);

    auto* defaultsLayout =
        new QHBoxLayout(
            defaultsRow);

    defaultsLayout->setContentsMargins(
        0,
        0,
        0,
        0);

    defaultsLayout->setSpacing(
        0);

    defaultsLayout->addStretch(
        1);

    auto* defaultsButton =
        new QPushButton(
            "Defaults",
            defaultsRow);

    defaultsButton->setFixedHeight(
        22);

    defaultsButton->setToolTip(
        "Reset waveform display controls to defaults");

    defaultsLayout->addWidget(
        defaultsButton);

    instrumentLayout->addWidget(
        defaultsRow);

    instrumentLayout->addStretch();

    connect(
        lineSelector_,
        &QSpinBox::valueChanged,
        this,
        [this](int lineNumber)
        {
            const bool enabled =
                lineNumber >= 0;

            waveformZoom1Button_->setEnabled(
                enabled);

            waveformZoom5Button_->setEnabled(
                enabled);

            waveformZoom10Button_->setEnabled(
                enabled);

            if (!enabled &&
                !waveformZoom1Button_->isChecked())
            {
                waveformZoom1Button_->setChecked(
                    true);

                emit waveformZoomChanged(
                    1);
            }

            emit lineNumberChanged(
                lineNumber);
        });

    connect(
        waveformZoomButtonGroup_,
        &QButtonGroup::idClicked,
        this,
        [this](int zoomFactor)
        {
            emit waveformZoomChanged(
                zoomFactor);
        });

    connect(
        persistenceSlider,
        &QSlider::valueChanged,
        this,
        [this](int value)
        {
            const int normalizedValue =
                std::clamp(
                    value,
                    0,
                    100);

            emit waveformPersistenceChanged(
                normalizedValue *
                2);
        });

    connect(
        coreIntensitySlider,
        &QSlider::valueChanged,
        this,
        &ControlWidget::waveformCoreIntensityChanged);

    connect(
        vectorscopeGlowSlider,
        &QSlider::valueChanged,
        this,
        &ControlWidget::vectorscopeGlowChanged);

    connect(
        vintageCheckBox,
        &QCheckBox::toggled,
        this,
        &ControlWidget::vintageLookChanged);

    connect(
        chromaIntensitySlider,
        &QSlider::valueChanged,
        this,
        [this](int value)
        {
            emit chromaRenderIntensityChanged(
                chromaUiToInternal(value));
        });

    connect(
        defaultsButton,
        &QPushButton::clicked,
        this,
        [chromaIntensitySlider,
         persistenceSlider,
         coreIntensitySlider,
         vectorscopeGlowSlider,
         vintageCheckBox,
         antiAliasingCheckBox,
         colorizeIllegalLuminanceCheckBox,
         colorizeGamutErrorsCheckBox]()
        {
            constexpr int defaultValue = 50;

            chromaIntensitySlider->setValue(
                50);

            persistenceSlider->setValue(
                defaultValue);

            coreIntensitySlider->setValue(
                100);

            vectorscopeGlowSlider->setValue(
                defaultValue);

            vintageCheckBox->setChecked(
                false);

            antiAliasingCheckBox->setChecked(
                true);

            colorizeIllegalLuminanceCheckBox->setChecked(
                true);

            colorizeGamutErrorsCheckBox->setChecked(
                true);
        });

    tabs->addTab(
        instrumentTab,
        "Instrument");

    // ------------------------------------------------------------
    // View FPS
    // ------------------------------------------------------------
    auto* viewFpsTab =
        new QWidget(tabs);

    auto* viewFpsLayout =
        new QGridLayout(viewFpsTab);

    viewFpsLayout->setContentsMargins(
        6,
        4,
        6,
        4);

    viewFpsLayout->setHorizontalSpacing(12);
    viewFpsLayout->setVerticalSpacing(6);

    auto* viewHeader =
        new QLabel("View", viewFpsTab);
    auto* openScopeHeader =
        new QLabel("OpenScope FPS", viewFpsTab);
    auto* spoutHeader =
        new QLabel("Spout FPS", viewFpsTab);

    QFont fpsHeaderFont = viewHeader->font();
    fpsHeaderFont.setBold(true);
    viewHeader->setFont(fpsHeaderFont);
    openScopeHeader->setFont(fpsHeaderFont);
    spoutHeader->setFont(fpsHeaderFont);

    // Keep the headers anchored to the same edge as the values.
    // Previously the numeric values were right-aligned while their headers
    // were left-aligned inside stretchable columns, so resizing made the
    // labels and numbers visibly drift apart.
    viewHeader->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    openScopeHeader->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    spoutHeader->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    viewFpsLayout->addWidget(viewHeader, 0, 0);
    viewFpsLayout->addWidget(openScopeHeader, 0, 1);
    viewFpsLayout->addWidget(spoutHeader, 0, 2);

    videoOpenScopeFpsLabel_ = new QLabel("0.0", viewFpsTab);
    videoSpoutFpsLabel_ = new QLabel("0.0", viewFpsTab);
    waveformOpenScopeFpsLabel_ = new QLabel("0.0", viewFpsTab);
    waveformSpoutFpsLabel_ = new QLabel("0.0", viewFpsTab);
    vectorscopeOpenScopeFpsLabel_ = new QLabel("0.0", viewFpsTab);
    vectorscopeSpoutFpsLabel_ = new QLabel("0.0", viewFpsTab);

    for (QLabel* valueLabel :
         {
             videoOpenScopeFpsLabel_,
             videoSpoutFpsLabel_,
             waveformOpenScopeFpsLabel_,
             waveformSpoutFpsLabel_,
             vectorscopeOpenScopeFpsLabel_,
             vectorscopeSpoutFpsLabel_
         })
    {
        valueLabel->setAlignment(
            Qt::AlignRight | Qt::AlignVCenter);
    }

    viewFpsLayout->addWidget(new QLabel("Video", viewFpsTab), 1, 0);
    viewFpsLayout->addWidget(videoOpenScopeFpsLabel_, 1, 1);
    viewFpsLayout->addWidget(videoSpoutFpsLabel_, 1, 2);

    viewFpsLayout->addWidget(new QLabel("Waveform", viewFpsTab), 2, 0);
    viewFpsLayout->addWidget(waveformOpenScopeFpsLabel_, 2, 1);
    viewFpsLayout->addWidget(waveformSpoutFpsLabel_, 2, 2);

    viewFpsLayout->addWidget(new QLabel("Vectorscope", viewFpsTab), 3, 0);
    viewFpsLayout->addWidget(vectorscopeOpenScopeFpsLabel_, 3, 1);
    viewFpsLayout->addWidget(vectorscopeSpoutFpsLabel_, 3, 2);

    viewFpsLayout->setColumnStretch(0, 2);
    viewFpsLayout->setColumnStretch(1, 1);
    viewFpsLayout->setColumnStretch(2, 1);

    const int fpsColumnMinimum =
        (std::max)(
            openScopeHeader->sizeHint().width(),
            spoutHeader->sizeHint().width());
    viewFpsLayout->setColumnMinimumWidth(1, fpsColumnMinimum);
    viewFpsLayout->setColumnMinimumWidth(2, fpsColumnMinimum);
    viewFpsLayout->setRowStretch(4, 1);

    // ------------------------------------------------------------
    // PCM Audio
    // ------------------------------------------------------------
    auto* pcmAudioTab =
        new QWidget(tabs);

    auto* pcmAudioLayout =
        new QVBoxLayout(pcmAudioTab);

    pcmAudioLayout->setContentsMargins(
        6,
        4,
        6,
        4);

    pcmAudioLayout->setSpacing(6);

    auto* pcmEnableCheckBox =
        new QCheckBox(
            "Enable PCM decoder",
            pcmAudioTab);

    pcmEnableCheckBox->setChecked(
        settings.control.pcmAudio.enabled);

    pcmAudioLayout->addWidget(
        pcmEnableCheckBox);

    auto* pcmFormatRow = new QHBoxLayout();
    auto* pcmFormatLabel = new QLabel("Decoder format", pcmAudioTab);
    auto* pcmFormatCombo = new QComboBox(pcmAudioTab);
    pcmFormatCombo->addItem("Auto", 0);
    pcmFormatCombo->addItem("Sony PCM-F1 / EIAJ", 1);
    pcmFormatCombo->addItem("Ham PCM 2.0", 2);
    pcmFormatCombo->setCurrentIndex(0);
    pcmFormatCombo->setToolTip(
        "Auto probes the existing Sony/EIAJ decoder and the separate Ham PCM decoder until one format is stable. "
        "Manual selections bypass auto detection.");
    pcmFormatRow->addWidget(pcmFormatLabel);
    pcmFormatRow->addWidget(pcmFormatCombo, 1);
    pcmAudioLayout->addLayout(pcmFormatRow);

    pcmAudioScopeCheckBox_ =
        new QCheckBox(
            "Use vectorscope viewport as Audio Scope",
            pcmAudioTab);
    pcmAudioScopeCheckBox_->setChecked(
        settings.control.pcmAudio.audioScopeEnabled);
    pcmAudioScopeCheckBox_->setEnabled(
        settings.control.pcmAudio.enabled);
    pcmAudioScopeCheckBox_->setToolTip(
        "While PCM decoding is enabled, replace the normal chroma "
        "vectorscope viewport with a stereo Audio Scope. "
        "Uncheck to keep the normal vectorscope.");
    pcmAudioLayout->addWidget(
        pcmAudioScopeCheckBox_);

    pcmAudioScopeColorizeCheckBox_ =
        new QCheckBox(
            "Colorize Audio Scope",
            pcmAudioTab);
    pcmAudioScopeColorizeCheckBox_->setChecked(
        settings.control.pcmAudio.audioScopeColorized);
    pcmAudioScopeColorizeCheckBox_->setEnabled(
        settings.control.pcmAudio.enabled &&
        settings.control.pcmAudio.audioScopeEnabled);
    pcmAudioScopeColorizeCheckBox_->setToolTip(
        "Colour the goniometer trace by stereo geometry: left-heavy cyan, "
        "right-heavy amber, mono/correlated green and anti-phase red. "
        "Uses the same trace rasterizer and adds no blur or supersampling.");
    pcmAudioLayout->addWidget(
        pcmAudioScopeColorizeCheckBox_);

    auto* pcmOutputCheckBox =
        new QCheckBox(
            "Enable audio output",
            pcmAudioTab);
    pcmOutputCheckBox->setChecked(settings.control.pcmAudio.outputEnabled);
    pcmAudioLayout->addWidget(pcmOutputCheckBox);

    auto* pcmBackendRow = new QHBoxLayout();
    auto* pcmBackendLabel = new QLabel("Output backend", pcmAudioTab);
    auto* pcmBackendCombo = new QComboBox(pcmAudioTab);
    pcmBackendCombo->addItem("WASAPI", 0);
    pcmBackendCombo->addItem("ASIO", 1);
    pcmBackendCombo->setCurrentIndex(std::clamp(settings.control.pcmAudio.outputBackend, 0, 1));
    pcmBackendRow->addWidget(pcmBackendLabel);
    pcmBackendRow->addWidget(pcmBackendCombo, 1);
    pcmAudioLayout->addLayout(pcmBackendRow);

    auto* pcmAsioRow = new QHBoxLayout();
    auto* pcmAsioLabel = new QLabel("ASIO driver", pcmAudioTab);
    auto* pcmAsioCombo = new QComboBox(pcmAudioTab);

    // Do not let the first enumerated ASIO driver become an implicit selection.
    // Some drivers (notably ASIO4ALL) can open UI / assert while being loaded, so
    // opening a backend must only happen after an explicit user activation.
    pcmAsioCombo->addItem("Select ASIO driver...", QString());

    const auto asioDrivers = AsioPcmOutput::enumerateDrivers();
    int savedAsioIndex = 0;
    for (const auto& driver : asioDrivers)
    {
        const QString name = QString::fromStdString(driver.nameUtf8);
        pcmAsioCombo->addItem(name, name);
        if (driver.nameUtf8 == settings.control.pcmAudio.asioDriverName)
            savedAsioIndex = pcmAsioCombo->count() - 1;
    }
    pcmAsioCombo->setCurrentIndex(savedAsioIndex);
    pcmAsioCombo->setEnabled(settings.control.pcmAudio.outputBackend == 1);
    pcmAsioLabel->setEnabled(settings.control.pcmAudio.outputBackend == 1);
    pcmAsioRow->addWidget(pcmAsioLabel);
    pcmAsioRow->addWidget(pcmAsioCombo, 1);
    pcmAudioLayout->addLayout(pcmAsioRow);

    auto* pcmOutputDeviceRow =
        new QHBoxLayout();

    auto* pcmOutputDeviceLabel =
        new QLabel(
            "Audio output device",
            pcmAudioTab);

    auto* pcmOutputDeviceCombo =
        new QComboBox(
            pcmAudioTab);

    pcmOutputDeviceCombo->addItem(
        "Default Windows output",
        QString());

    const auto wasapiDevices =
        WasapiPcmOutput::enumerateDevices();

    int savedDeviceIndex = 0;
    bool savedDeviceFound =
        settings.control.pcmAudio.outputDeviceId.empty();

    for (const auto& device :
         wasapiDevices)
    {
        const QString id =
            QString::fromStdString(
                device.idUtf8);

        const QString name =
            QString::fromStdString(
                device.nameUtf8);

        pcmOutputDeviceCombo->addItem(
            name,
            id);

        if (!settings.control.pcmAudio.outputDeviceId.empty() &&
            device.idUtf8 ==
                settings.control.pcmAudio.outputDeviceId)
        {
            savedDeviceFound = true;
            savedDeviceIndex =
                pcmOutputDeviceCombo->count() - 1;
        }
    }

    if (!savedDeviceFound)
    {
        const QString savedId =
            QString::fromStdString(
                settings.control.pcmAudio.outputDeviceId);

        pcmOutputDeviceCombo->addItem(
            "Saved output device (currently unavailable)",
            savedId);

        savedDeviceIndex =
            pcmOutputDeviceCombo->count() - 1;
    }

    pcmOutputDeviceCombo->setCurrentIndex(
        savedDeviceIndex);

    pcmOutputDeviceCombo->setToolTip(
        "Select the Windows WASAPI render endpoint for decoded PCM audio. "
        "The endpoint ID is stored in the OpenScope settings.");
    pcmOutputDeviceCombo->setEnabled(settings.control.pcmAudio.outputBackend == 0);
    pcmOutputDeviceLabel->setEnabled(settings.control.pcmAudio.outputBackend == 0);

    pcmOutputDeviceRow->addWidget(
        pcmOutputDeviceLabel);

    pcmOutputDeviceRow->addWidget(
        pcmOutputDeviceCombo,
        1);

    pcmAudioLayout->addLayout(
        pcmOutputDeviceRow);

    auto* pcmBufferRow = new QHBoxLayout();
    auto* pcmBufferCaption = new QLabel("Receiver buffer", pcmAudioTab);
    auto* pcmBufferSlider = new QSlider(Qt::Horizontal, pcmAudioTab);
    auto* pcmBufferValue = new QLabel(pcmAudioTab);
    constexpr std::array<int, 7> pcmBufferSteps{0, 4, 8, 16, 32, 64, 128};
    pcmBufferSlider->setRange(0, static_cast<int>(pcmBufferSteps.size()) - 1);
    int pcmBufferIndex = 0;
    for (int i = 0; i < static_cast<int>(pcmBufferSteps.size()); ++i)
    {
        if (pcmBufferSteps[static_cast<std::size_t>(i)] == settings.control.pcmAudio.outputBufferMs)
        {
            pcmBufferIndex = i;
            break;
        }
    }
    pcmBufferSlider->setValue(pcmBufferIndex);
    pcmBufferSlider->setTickPosition(QSlider::TicksBelow);
    pcmBufferSlider->setTickInterval(1);
    pcmBufferValue->setMinimumWidth(52);
    const auto updatePcmBufferText =
        [pcmBufferValue](int ms)
        {
            pcmBufferValue->setText(ms == 0 ? QStringLiteral("Low") : QString::number(ms) + QStringLiteral(" ms"));
        };
    updatePcmBufferText(pcmBufferSteps[static_cast<std::size_t>(pcmBufferIndex)]);
    pcmBufferSlider->setToolTip(
        "Decoded-audio receiver/playout cushion. Higher values add latency but tolerate longer Windows scheduling stalls.");
    pcmBufferSlider->setEnabled(settings.control.pcmAudio.outputBackend == 0);
    pcmBufferCaption->setEnabled(settings.control.pcmAudio.outputBackend == 0);
    pcmBufferValue->setEnabled(settings.control.pcmAudio.outputBackend == 0);
    pcmBufferRow->addWidget(pcmBufferCaption);
    pcmBufferRow->addWidget(pcmBufferSlider, 1);
    pcmBufferRow->addWidget(pcmBufferValue);
    pcmAudioLayout->addLayout(pcmBufferRow);

    auto* deEmphasisRow = new QHBoxLayout();
    auto* deEmphasisLabel = new QLabel("De-emphasis", pcmAudioTab);
    auto* deEmphasisCombo = new QComboBox(pcmAudioTab);
    deEmphasisCombo->addItem("Auto", 0);
    deEmphasisCombo->addItem("Off", 1);
    deEmphasisCombo->addItem("50/15 us", 2);
    pcmDeEmphasisMode_ =
        std::clamp(settings.control.pcmAudio.deEmphasisMode, 0, 2);
    deEmphasisCombo->setCurrentIndex(pcmDeEmphasisMode_);
    deEmphasisCombo->setToolTip(
        "Auto follows a valid control-H. Without a control-H, Auto defaults to 50/15 us for 14-bit EIAJ and Off for 16-bit PCM-F1.");
    deEmphasisRow->addWidget(deEmphasisLabel);
    deEmphasisRow->addWidget(deEmphasisCombo, 1);
    pcmAudioLayout->addLayout(deEmphasisRow);

    // Keep the normal PCM tab intentionally compact. The main status line is
    // operational information; decoder diagnostics live behind Details.
    auto* pcmStatusRow = new QHBoxLayout();
    auto* pcmStatusCaption = new QLabel(QStringLiteral("STATUS"), pcmAudioTab);
    QFont pcmStatusFont = pcmStatusCaption->font();
    pcmStatusFont.setBold(true);
    pcmStatusCaption->setFont(pcmStatusFont);
    pcmLockStatusLabel_ = new QLabel(
        settings.control.pcmAudio.enabled
            ? QStringLiteral("Searching...")
            : QStringLiteral("Disabled"),
        pcmAudioTab);
    pcmLockStatusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pcmStatusRow->addWidget(pcmStatusCaption);
    pcmStatusRow->addWidget(pcmLockStatusLabel_, 1);
    pcmAudioLayout->addLayout(pcmStatusRow);

    auto* pcmDetailsButton = new QPushButton(QStringLiteral("Details..."), pcmAudioTab);
    pcmDetailsButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    pcmAudioLayout->addWidget(pcmDetailsButton, 0, Qt::AlignLeft);

    auto* pcmDetailsDialog = new QDialog(pcmAudioTab, Qt::Tool);
    pcmDetailsDialog->setWindowTitle(QStringLiteral("PCM Decoder Details"));
    pcmDetailsDialog->setModal(false);
    pcmDetailsDialog->resize(920, 520);
    pcmDetailsDialog->setMinimumWidth(880);
    auto* pcmDetailsLayout = new QVBoxLayout(pcmDetailsDialog);
    pcmDetailsLayout->setContentsMargins(12, 12, 12, 12);
    pcmDetailsLayout->setSpacing(8);

    auto* pcmGrid = new QGridLayout();
    pcmGrid->setHorizontalSpacing(12);
    pcmGrid->setVerticalSpacing(5);

    auto addPcmStatusRow =
        [pcmDetailsDialog, pcmGrid](
            int row,
            const QString& caption,
            QLabel*& valueLabel,
            const QString& initial)
        {
            auto* captionLabel = new QLabel(caption, pcmDetailsDialog);
            valueLabel = new QLabel(initial, pcmDetailsDialog);
            valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            valueLabel->setWordWrap(false);
            valueLabel->setMinimumWidth(500);
            pcmGrid->addWidget(captionLabel, row, 0, Qt::AlignTop | Qt::AlignLeft);
            pcmGrid->addWidget(valueLabel, row, 1, Qt::AlignTop | Qt::AlignLeft);
        };

    addPcmStatusRow(0, "Transport quality", pcmTransportQualityLabel_, "0.00 %");
    addPcmStatusRow(1, "PCM lines", pcmTransportLinesLabel_, "0 / 294 per field");
    addPcmStatusRow(2, "Mode", pcmModeStatusLabel_, "14/16-bit (identifying...)");
    addPcmStatusRow(3, "De-emphasis", pcmPreEmphasisStatusLabel_, "Unknown");
    addPcmStatusRow(4, "Control H", pcmControlStatusLabel_, "0 / 0");
    addPcmStatusRow(5, "CRC", pcmCrcStatusLabel_, "0.00 %");
    addPcmStatusRow(6, "Valid lines", pcmLinesStatusLabel_, "0 / 0");
    addPcmStatusRow(7, "Bit geometry", pcmGeometryStatusLabel_, "Searching...");
    addPcmStatusRow(8, "P / ECC", pcmPStatusLabel_, "Waiting for deinterleave...");
    pcmGrid->setColumnStretch(1, 1);
    pcmDetailsLayout->addLayout(pcmGrid);

    auto* asioCaption = new QLabel(QStringLiteral("ASIO Output"), pcmDetailsDialog);
    QFont asioCaptionFont = asioCaption->font();
    asioCaptionFont.setBold(true);
    asioCaption->setFont(asioCaptionFont);
    pcmDetailsLayout->addWidget(asioCaption);

    auto* asioGrid = new QGridLayout();
    asioGrid->setHorizontalSpacing(12);
    asioGrid->setVerticalSpacing(5);
    asioGrid->addWidget(new QLabel(QStringLiteral("Driver"), pcmDetailsDialog), 0, 0);
    pcmAsioDriverStatusLabel_ = new QLabel(
        pcmAsioCombo->currentIndex() > 0 ? pcmAsioCombo->currentText() : QStringLiteral("-"),
        pcmDetailsDialog);
    asioGrid->addWidget(pcmAsioDriverStatusLabel_, 0, 1);
    asioGrid->addWidget(new QLabel(QStringLiteral("Rate / quantum"), pcmDetailsDialog), 1, 0);
    pcmAsioRateStatusLabel_ = new QLabel(QStringLiteral("-"), pcmDetailsDialog);
    asioGrid->addWidget(pcmAsioRateStatusLabel_, 1, 1);
    asioGrid->addWidget(new QLabel(QStringLiteral("Buffer"), pcmDetailsDialog), 2, 0);
    pcmAsioBufferStatusLabel_ = new QLabel(QStringLiteral("-"), pcmDetailsDialog);
    asioGrid->addWidget(pcmAsioBufferStatusLabel_, 2, 1);
    asioGrid->addWidget(new QLabel(QStringLiteral("Callbacks / errors"), pcmDetailsDialog), 3, 0);
    pcmAsioCounterStatusLabel_ = new QLabel(QStringLiteral("-"), pcmDetailsDialog);
    asioGrid->addWidget(pcmAsioCounterStatusLabel_, 3, 1);
    asioGrid->setColumnStretch(1, 1);
    pcmDetailsLayout->addLayout(asioGrid);

    auto* targetRow = new QHBoxLayout();
    targetRow->addWidget(new QLabel(QStringLiteral("Target buffer"), pcmDetailsDialog));
    pcmAsioTargetSpin_ = new QSpinBox(pcmDetailsDialog);
    pcmAsioTargetSpin_->setRange(0, 100);
    pcmAsioTargetSpin_->setSuffix(QStringLiteral(" ms"));
    pcmAsioTargetSpin_->setSpecialValueText(QStringLiteral("Auto (4Q)"));
    pcmAsioTargetSpin_->setValue(0);
    pcmAsioTargetSpin_->setToolTip(QStringLiteral("0 = automatic 4 x ASIO quantum. Changes apply immediately."));
    targetRow->addWidget(pcmAsioTargetSpin_);
    auto* pcmAsioDefaultsButton = new QPushButton(QStringLiteral("Defaults (4Q)"), pcmDetailsDialog);
    targetRow->addWidget(pcmAsioDefaultsButton);
    targetRow->addStretch();
    pcmDetailsLayout->addLayout(targetRow);

    auto* asioButtonRow = new QHBoxLayout();
    auto* pcmAsioGraphButton = new QPushButton(QStringLiteral("Buffer Graph..."), pcmDetailsDialog);
    auto* pcmAsioLogButton = new QPushButton(QStringLiteral("Log: OFF"), pcmDetailsDialog);
    pcmAsioLogButton->setCheckable(true);
    asioButtonRow->addWidget(pcmAsioGraphButton);
    asioButtonRow->addWidget(pcmAsioLogButton);
    asioButtonRow->addStretch();
    pcmDetailsLayout->addLayout(asioButtonRow);

    auto* pcmAsioGraphDialog = new QDialog(pcmDetailsDialog, Qt::Tool);
    pcmAsioGraphDialog->setWindowTitle(QStringLiteral("ASIO Buffer"));
    pcmAsioGraphDialog->resize(760, 390);
    auto* graphLayout = new QVBoxLayout(pcmAsioGraphDialog);
    pcmAsioGraph_ = new AsioBufferGraphWidget(pcmAsioGraphDialog);
    graphLayout->addWidget(pcmAsioGraph_, 1);
    auto* graphClose = new QPushButton(QStringLiteral("Close"), pcmAsioGraphDialog);
    graphLayout->addWidget(graphClose, 0, Qt::AlignRight);
    connect(graphClose, &QPushButton::clicked, pcmAsioGraphDialog, &QDialog::hide);
    connect(pcmAsioGraphButton, &QPushButton::clicked, pcmAsioGraphDialog,
        [pcmAsioGraphDialog]() { pcmAsioGraphDialog->show(); pcmAsioGraphDialog->raise(); pcmAsioGraphDialog->activateWindow(); });
    connect(pcmAsioTargetSpin_, qOverload<int>(&QSpinBox::valueChanged), this,
        [this](int ms) { emit pcmAsioTargetMsChanged(static_cast<double>(ms)); });
    connect(pcmAsioDefaultsButton, &QPushButton::clicked, this,
        [this]() { if (pcmAsioTargetSpin_ != nullptr) pcmAsioTargetSpin_->setValue(0); });
    connect(pcmAsioLogButton, &QPushButton::toggled, this,
        [this, pcmAsioLogButton](bool enabled)
        {
            pcmAsioLogButton->setText(enabled ? QStringLiteral("Log: ON") : QStringLiteral("Log: OFF"));
            emit pcmAsioBufferLoggingChanged(enabled);
        });

    auto* pcmDetailsClose = new QPushButton(QStringLiteral("Close"), pcmDetailsDialog);
    pcmDetailsLayout->addWidget(pcmDetailsClose, 0, Qt::AlignRight);
    connect(pcmDetailsClose, &QPushButton::clicked, pcmDetailsDialog, &QDialog::hide);
    connect(
        pcmDetailsButton,
        &QPushButton::clicked,
        pcmDetailsDialog,
        [pcmDetailsDialog]()
        {
            pcmDetailsDialog->show();
            pcmDetailsDialog->raise();
            pcmDetailsDialog->activateWindow();
        });

    pcmAudioLayout->addStretch();

    connect(
        pcmFormatCombo,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        [this, pcmFormatCombo](int index)
        {
            pcmDecoderSelectionMode_ = pcmFormatCombo->itemData(index).toInt();
            if (pcmDecoderSelectionMode_ == 1)
                pcmResolvedHamMode_ = false;
            else if (pcmDecoderSelectionMode_ == 2)
                pcmResolvedHamMode_ = true;
            emit pcmDecoderModeChanged(pcmDecoderSelectionMode_);
        });

    connect(
        pcmEnableCheckBox,
        &QCheckBox::toggled,
        this,
        [this](bool enabled)
        {
            if (pcmAudioScopeCheckBox_ != nullptr)
                pcmAudioScopeCheckBox_->setEnabled(enabled);
            if (pcmAudioScopeColorizeCheckBox_ != nullptr)
                pcmAudioScopeColorizeCheckBox_->setEnabled(
                    enabled && pcmAudioScopeCheckBox_ != nullptr &&
                    pcmAudioScopeCheckBox_->isChecked());
            emit pcmDecoderEnabledChanged(enabled);
        });

    connect(
        pcmAudioScopeCheckBox_,
        &QCheckBox::toggled,
        this,
        [this](bool enabled)
        {
            if (pcmAudioScopeColorizeCheckBox_ != nullptr)
                pcmAudioScopeColorizeCheckBox_->setEnabled(enabled);
            emit pcmAudioScopeEnabledChanged(enabled);
        });

    connect(
        pcmAudioScopeColorizeCheckBox_,
        &QCheckBox::toggled,
        this,
        &ControlWidget::pcmAudioScopeColorizedChanged);

    connect(
        pcmOutputCheckBox,
        &QCheckBox::toggled,
        this,
        &ControlWidget::pcmAudioOutputEnabledChanged);

    connect(
        pcmBackendCombo,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        [this, pcmBackendCombo, pcmAsioCombo, pcmAsioLabel, pcmOutputDeviceCombo, pcmOutputDeviceLabel, pcmBufferSlider, pcmBufferCaption, pcmBufferValue](int index)
        {
            const int backend = pcmBackendCombo->itemData(index).toInt();
            const bool asio = backend == 1;
            pcmAsioCombo->setEnabled(asio);
            pcmAsioLabel->setEnabled(asio);
            pcmOutputDeviceCombo->setEnabled(!asio);
            pcmOutputDeviceLabel->setEnabled(!asio);
            pcmBufferSlider->setEnabled(!asio);
            pcmBufferCaption->setEnabled(!asio);
            pcmBufferValue->setEnabled(!asio);
            emit pcmAudioOutputBackendChanged(backend);
            // Deliberately do NOT emit pcmAsioDriverChanged here. Selecting the
            // ASIO backend is not the same thing as selecting/loading a driver.
        });

    connect(
        pcmAsioCombo,
        qOverload<int>(&QComboBox::activated),
        this,
        [this, pcmAsioCombo](int index)
        {
            if (index <= 0)
                return;

            const QString driverName = pcmAsioCombo->itemData(index).toString();
            if (!driverName.isEmpty())
            {
                if (pcmAsioDriverStatusLabel_ != nullptr)
                    pcmAsioDriverStatusLabel_->setText(driverName);
                emit pcmAsioDriverChanged(driverName);
            }
        });

    connect(
        pcmOutputDeviceCombo,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        [this, pcmOutputDeviceCombo](int index)
        {
            emit pcmAudioOutputDeviceChanged(
                pcmOutputDeviceCombo
                    ->itemData(index)
                    .toString());
        });

    connect(
        pcmBufferSlider,
        &QSlider::valueChanged,
        this,
        [this, pcmBufferSteps, updatePcmBufferText](int index)
        {
            const int ms = pcmBufferSteps[static_cast<std::size_t>(index)];
            updatePcmBufferText(ms);
            emit pcmAudioBufferMsChanged(ms);
        });

    connect(
        deEmphasisCombo,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        [this, deEmphasisCombo](int index)
        {
            pcmDeEmphasisMode_ =
                deEmphasisCombo->itemData(index).toInt();
            emit pcmDeEmphasisModeChanged(pcmDeEmphasisMode_);
        });

    // ------------------------------------------------------------
    // Misc
    // ------------------------------------------------------------
    auto* miscTab =
        new QWidget(tabs);

    auto* miscLayout =
        new QVBoxLayout(miscTab);

    miscLayout->setContentsMargins(
        6,
        4,
        6,
        4);

    miscLayout->setSpacing(4);

    legacyAspectRatioCheckBox_ =
        new QCheckBox(
            "Legacy Aspect Ratio",
            miscTab);

    legacyAspectRatioCheckBox_->setChecked(
        settings.local
            .display
            .aspectRatio ==
        OpenScopeSettings::AspectRatio::Ratio4x3);

    miscLayout->addWidget(
        legacyAspectRatioCheckBox_);

    auto* followWssAspectRatioCheckBox =
        new QCheckBox(
            "Follow WSS aspect ratio",
            miscTab);

    followWssAspectRatioCheckBox->setChecked(
        settings.local.display.followWssAspectRatio);

    followWssAspectRatioCheckBox->setToolTip(
        "Follow stable 625-line WSS. Full-format anamorphic switches to 16:9; letterbox modes remain 4:3.");

    miscLayout->addWidget(
        followWssAspectRatioCheckBox);

    wssStatusLabel_ =
        new QLabel(
            "WSS: not detected",
            miscTab);

    wssStatusLabel_->setWordWrap(true);
    miscLayout->addWidget(
        wssStatusLabel_);

    auto* wssTestRow =
        new QHBoxLayout();

    auto* injectWss4x3Button =
        new QPushButton(
            "Test WSS 4:3",
            miscTab);

    auto* injectWss16x9Button =
        new QPushButton(
            "Test WSS 16:9",
            miscTab);

    injectWss4x3Button->setToolTip(
        "Inject a valid 4:3 WSS burst into line 0 for 12 frames so the normal decoder can acquire it.");
    injectWss16x9Button->setToolTip(
        "Inject a valid 16:9 anamorphic WSS burst into line 0 for 12 frames so the normal decoder can acquire it.");

    wssTestRow->addWidget(injectWss4x3Button);
    wssTestRow->addWidget(injectWss16x9Button);
    wssTestRow->addStretch();
    miscLayout->addLayout(wssTestRow);

    performanceCheckBox_ =
        new QCheckBox(
            "Show Performance Floaty",
            miscTab);

    performanceCheckBox_->setChecked(
        settings.local
            .floaties
            .performanceVisible);

    miscLayout->addWidget(
        performanceCheckBox_);

    auto* preventDisplaySleepCheckBox =
        new QCheckBox(
            "Prevent display sleep / screensaver",
            miscTab);

    // Delta9 diagnostic/stability policy: keep the Windows execution-state
    // protection hard on. Minimize/full-screen transitions must not let the
    // audio timing path become background-idle work.
    preventDisplaySleepCheckBox->setChecked(true);
    preventDisplaySleepCheckBox->setEnabled(false);

    preventDisplaySleepCheckBox->setToolTip(
        "Forced ON in Delta9 while ASIO scheduling is being hardened.");

    miscLayout->addWidget(
        preventDisplaySleepCheckBox);

    if constexpr (OpenScopeBuild::kDebugBuild)
    {
        QSlider* coreWidthSlider = nullptr;

        QWidget* coreWidthRow =
            createSliderRow(
                "Core width",
                coreWidthSlider,
                5,
                30,
                std::clamp(
                    settings.control
                        .instrument
                        .waveform
                        .coreWidthTenths,
                    5,
                    30),
                miscTab,
                [](int value)
                {
                    return QString::number(
                        static_cast<double>(value) / 10.0,
                        'f',
                        1) +
                        " px";
                });

        static_cast<ValueSlider*>(coreWidthSlider)
            ->setDoubleClickResetValue(10);

        coreWidthSlider->setToolTip(
            "Waveform core width. Double-click to reset to 1.0 px.");

        miscLayout->addWidget(
            coreWidthRow);

        connect(
            coreWidthSlider,
            &QSlider::valueChanged,
            this,
            &ControlWidget::waveformCoreWidthChanged);

    }

    auto* floatiesHomeButton =
        new QPushButton(
            "Floaties 2 Home",
            miscTab);

    floatiesHomeButton->setToolTip(
        "Bring OpenScope floating windows back to visible positions on the current screen");

    miscLayout->addWidget(
        floatiesHomeButton);

    auto* spoutLabel =
        new QLabel(
            "Spout output",
            miscTab);

    miscLayout->addWidget(
        spoutLabel);

    auto* spoutVideoCheckBox =
        new QCheckBox(
            "Video",
            miscTab);

    spoutVideoCheckBox->setChecked(
        settings.local
            .spout
            .videoEnabled);

    miscLayout->addWidget(
        spoutVideoCheckBox);

    auto* spoutWaveformCheckBox =
        new QCheckBox(
            "Waveform",
            miscTab);

    spoutWaveformCheckBox->setChecked(
        settings.local
            .spout
            .waveformEnabled);

    miscLayout->addWidget(
        spoutWaveformCheckBox);

    auto* spoutVectorscopeCheckBox =
        new QCheckBox(
            "Vectorscope",
            miscTab);

    spoutVectorscopeCheckBox->setChecked(
        settings.local
            .spout
            .vectorscopeEnabled);


    miscLayout->addWidget(
        spoutVectorscopeCheckBox);

    auto* exportHighResolutionPngButton =
        new QPushButton(
            "Export high-res PNG...",
            miscTab);

    exportHighResolutionPngButton->setToolTip(
        "Choose a file name and export the next complete captured frame as a 2880 x 2304 PNG");

    miscLayout->addWidget(
        exportHighResolutionPngButton);

    auto* exportHighResolutionPngBamButton =
        new QPushButton(
            "Export high-res PNG BAM",
            miscTab);

    exportHighResolutionPngBamButton->setToolTip(
        "Immediately export the next complete captured frame to the remembered folder using the next free capture_0001.png number");

    miscLayout->addWidget(
        exportHighResolutionPngBamButton);

    if constexpr (OpenScopeBuild::kDebugBuild)
    {
        auto* waveformRawCaptureButton =
            new QPushButton(
                "Capture waveform RAW...",
                miscTab);

        waveformRawCaptureButton->setToolTip(
            "Capture 250 frames of the currently selected reconstructed 2880-sample Y line");

        miscLayout->addWidget(
            waveformRawCaptureButton);

        connect(
            waveformRawCaptureButton,
            &QPushButton::clicked,
            this,
            &ControlWidget::waveformRawCaptureRequested);
    }

    miscLayout->addStretch();

    connect(
        legacyAspectRatioCheckBox_,
        &QCheckBox::toggled,
        this,
        &ControlWidget::legacyAspectRatioChanged);

    connect(
        followWssAspectRatioCheckBox,
        &QCheckBox::toggled,
        this,
        &ControlWidget::followWssAspectRatioChanged);

    connect(
        injectWss4x3Button,
        &QPushButton::clicked,
        this,
        &ControlWidget::injectTestWss4x3Requested);

    connect(
        injectWss16x9Button,
        &QPushButton::clicked,
        this,
        &ControlWidget::injectTestWss16x9Requested);

    connect(
        performanceCheckBox_,
        &QCheckBox::toggled,
        this,
        &ControlWidget::performanceVisibilityChanged);

    connect(
        preventDisplaySleepCheckBox,
        &QCheckBox::toggled,
        this,
        &ControlWidget::preventDisplaySleepChanged);

    connect(
        floatiesHomeButton,
        &QPushButton::clicked,
        this,
        &ControlWidget::floatiesHomeRequested);

    connect(
        spoutVideoCheckBox,
        &QCheckBox::toggled,
        this,
        &ControlWidget::spoutVideoEnabledChanged);

    connect(
        spoutWaveformCheckBox,
        &QCheckBox::toggled,
        this,
        &ControlWidget::spoutWaveformEnabledChanged);

    connect(
        spoutVectorscopeCheckBox,
        &QCheckBox::toggled,
        this,
        &ControlWidget::spoutVectorscopeEnabledChanged);

    connect(
        exportHighResolutionPngButton,
        &QPushButton::clicked,
        this,
        &ControlWidget::exportHighResolutionPngRequested);

    connect(
        exportHighResolutionPngBamButton,
        &QPushButton::clicked,
        this,
        &ControlWidget::exportHighResolutionPngQuickRequested);

    tabs->addTab(
        miscTab,
        "Misc");

    tabs->addTab(
        calibrationTab,
        "Calibration");

    tabs->addTab(
        viewFpsTab,
        "View FPS");

    tabs->addTab(
        pcmAudioTab,
        "PCM Audio");

    // ------------------------------------------------------------
    // Help - only shown while the workspace is in quad view.
    // ------------------------------------------------------------
    auto* helpTab =
        new QWidget(tabs);

    auto* helpLayout =
        new QVBoxLayout(helpTab);

    helpLayout->setContentsMargins(
        6,
        4,
        6,
        4);

    auto* waveformHelp =
        new QTextBrowser(helpTab);

    waveformHelp->setOpenExternalLinks(false);
    waveformHelp->setFrameShape(QFrame::NoFrame);
    waveformHelp->setHtml(
        QStringLiteral(
            R"HTML(
<h3>Waveform</h3>
<p><b>Line selector</b><br>
Selects one video line. <b>All</b> shows all lines and disables X5/X10.</p>

<p><b>X1 / X5 / X10</b><br>
Horizontal waveform zoom only. In X5/X10, drag with the <b>right mouse button</b> to pan left/right.</p>

<p><b>Color carrier intensity</b><br>
0 disables the chroma-envelope rendering. Values 1-100 span the useful color range.</p>

<p><b>Scopephor</b><br>
Controls trace persistence.</p>

<p><b>Vintage look</b><br>
Enables the more analogue scope appearance.</p>

<h3>Waveform measurement</h3>
<p><b>D — Details</b><br>
Hold D to show &micro;s from line start and source pixel (0–719) in the blue probe.</p>

<p><b>Left mouse drag</b><br>
Manual point-to-point measurement of voltage difference and frequency.</p>

<p><b>R + left mouse drag</b><br>
Defines a reference area. The result is averaged over up to four frames.
Magenta reference level lines can be dragged vertically for manual correction.</p>

<p><b>A + left mouse drag</b><br>
Measures a sinusoidal area. Frequency is shown with amplitude in mV, or in dB when a reference exists.
Green measurement level lines can be dragged vertically and the result follows the correction.</p>

<p><b>M</b><br>
Automatic multiburst measurement. Periodic zones around 50% video level are detected,
the lowest-frequency signal becomes the reference, and the remaining valid bursts are measured.
Partial results are shown when at least four bursts are found.</p>

<p><b>C</b><br>
Clears all waveform measurements and references.</p>

<p><b>Double click</b><br>
Clears measurements and toggles the waveform viewport between quad and maximized view.</p>

<p><i>Measurements are also cleared when the selected line changes or the waveform view is resized.</i></p>
)HTML"));

    helpLayout->addWidget(
        waveformHelp);

    helpTabIndex_ =
        tabs->addTab(
            helpTab,
            "Help");

    // ------------------------------------------------------------
    // About
    // ------------------------------------------------------------
    auto* aboutTab = new QWidget(tabs);
    auto* aboutLayout = new QVBoxLayout(aboutTab);
    aboutLayout->setContentsMargins(16, 16, 16, 16);
    aboutLayout->setSpacing(12);

    aboutLogoLabel_ = new QLabel(aboutTab);
    QPixmap initialAboutLogo(
        QStringLiteral(":/branding/OpenScopeAboutLogo.png"));
    if (!initialAboutLogo.isNull())
    {
        aboutLogoLabel_->setPixmap(
            initialAboutLogo.scaled(
                128,
                128,
                Qt::KeepAspectRatio,
                Qt::SmoothTransformation));
        aboutLogoLabel_->setFixedSize(128, 128);
    }
    aboutLogoLabel_->setAlignment(Qt::AlignTop | Qt::AlignRight);
    aboutLogoLabel_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    aboutLayout->addWidget(
        aboutLogoLabel_,
        0,
        Qt::AlignTop | Qt::AlignRight);

    auto* aboutText = new QLabel(
        QStringLiteral(
            "OpenScope is a software waveform monitor, vectorscope and video toolbox. "
            "It was created because nothing is available for free that provides proper "
            "video monitoring and vectorscope functionality. Since it works on a BT.656 "
            "digital representation of analog video, some restrictions apply. "
            "There is no burst phase check, no 8fs indicator and no VITS line viewing."),
        aboutTab);
    aboutText->setWordWrap(true);
    aboutText->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    aboutLayout->addWidget(aboutText);
    aboutLayout->addStretch();

    aboutTabIndex_ =
        tabs->addTab(
            aboutTab,
            "About");

    cornerLogoLabel_ = new QLabel(this);
    cornerLogoLabel_->setAlignment(Qt::AlignCenter);
    cornerLogoLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
    cornerLogoLabel_->hide();

    connect(
        tabs_,
        &QTabWidget::currentChanged,
        this,
        [this](int)
        {
            updateBrandingLayout();
        });

    updateBrandingLayout();
}

void ControlWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateBrandingLayout();
}

void ControlWidget::updateBrandingLayout()
{
    if (aboutLogoLabel_ != nullptr)
    {
        const int availableWidth = std::max(width() - 32, 0);
        const int availableHeight = std::max(height() - 72, 0);

        if (availableWidth < 360 || availableHeight < 180)
        {
            aboutLogoLabel_->hide();
        }
        else
        {
            const int target =
                std::clamp(
                    std::min(
                        availableWidth / 4,
                        availableHeight / 3),
                    80,
                    192);

            QPixmap logoPixmap(
                QStringLiteral(":/branding/OpenScopeAboutLogo.png"));

            if (logoPixmap.isNull())
            {
                aboutLogoLabel_->hide();
            }
            else
            {
                aboutLogoLabel_->setFixedSize(target, target);
                aboutLogoLabel_->setPixmap(
                    logoPixmap.scaled(
                        target,
                        target,
                        Qt::KeepAspectRatio,
                        Qt::SmoothTransformation));
                aboutLogoLabel_->show();
            }
        }
    }

    if (cornerLogoLabel_ == nullptr ||
        tabs_ == nullptr)
    {
        return;
    }

    const int availableWidth = width();
    const int availableHeight = height();
    const bool enoughRoom =
        availableWidth >= 620 &&
        availableHeight >= 300;

    // Reserve a real right-hand strip in every normal settings page.
    // The logo therefore never floats on top of sliders, labels or buttons.
    for (int index = 0;
         index < tabs_->count();
         ++index)
    {
        if (index == aboutTabIndex_)
        {
            continue;
        }

        QWidget* page =
            tabs_->widget(index);

        if (page == nullptr ||
            page->layout() == nullptr)
        {
            continue;
        }

        QLayout* pageLayout =
            page->layout();

        if (!pageLayout->property(
                "OpenScopeOriginalRightMargin").isValid())
        {
            pageLayout->setProperty(
                "OpenScopeOriginalRightMargin",
                pageLayout->contentsMargins().right());
        }

        const int originalRightMargin =
            pageLayout->property(
                "OpenScopeOriginalRightMargin").toInt();

        const int reserveWidth =
            enoughRoom
                ? std::clamp(
                      std::min(
                          availableWidth / 7,
                          availableHeight / 4),
                      72,
                      128) + 34
                : originalRightMargin;

        QMargins margins =
            pageLayout->contentsMargins();

        margins.setRight(
            reserveWidth);

        pageLayout->setContentsMargins(
            margins);
    }

    if (tabs_->currentIndex() == aboutTabIndex_ ||
        !enoughRoom)
    {
        cornerLogoLabel_->hide();
        return;
    }

    const int target =
        std::clamp(
            std::min(
                availableWidth / 7,
                availableHeight / 4),
            72,
            128);

    QPixmap logoPixmap(
        QStringLiteral(":/branding/OpenScopeAboutLogo.png"));

    if (logoPixmap.isNull())
    {
        cornerLogoLabel_->hide();
        return;
    }

    cornerLogoLabel_->setFixedSize(
        target,
        target);

    cornerLogoLabel_->setPixmap(
        logoPixmap.scaled(
            target,
            target,
            Qt::KeepAspectRatio,
            Qt::SmoothTransformation));

    cornerLogoLabel_->move(
        availableWidth - target - 18,
        48);

    cornerLogoLabel_->show();
    cornerLogoLabel_->raise();
}

void ControlWidget::setViewFps(
    double videoOpenScopeFps,
    double videoSpoutFps,
    double waveformOpenScopeFps,
    double waveformSpoutFps,
    double vectorscopeOpenScopeFps,
    double vectorscopeSpoutFps)
{
    const auto setFps =
        [](QLabel* label, double fps)
        {
            if (label == nullptr)
            {
                return;
            }

            label->setText(
                QString::number(
                    (std::max)(0.0, fps),
                    'f',
                    1));
        };

    setFps(videoOpenScopeFpsLabel_, videoOpenScopeFps);
    setFps(videoSpoutFpsLabel_, videoSpoutFps);
    setFps(waveformOpenScopeFpsLabel_, waveformOpenScopeFps);
    setFps(waveformSpoutFpsLabel_, waveformSpoutFps);
    setFps(vectorscopeOpenScopeFpsLabel_, vectorscopeOpenScopeFps);
    setFps(vectorscopeSpoutFpsLabel_, vectorscopeSpoutFps);
}


void ControlWidget::setHelpTabVisible(
    bool visible)
{
    if (tabs_ == nullptr ||
        helpTabIndex_ < 0)
    {
        return;
    }

    if (!visible &&
        tabs_->currentIndex() == helpTabIndex_)
    {
        tabs_->setCurrentIndex(
            2);
    }

    tabs_->setTabVisible(
        helpTabIndex_,
        visible);
}


void ControlWidget::setLineNumber(
    int lineNumber)
{
    lineSelector_->setValue(
        std::clamp(
            lineNumber,
            lineSelector_->minimum(),
            lineSelector_->maximum()));
}

void ControlWidget::setWaveformZoomFactor(
    int zoomFactor)
{
    if (waveformZoomButtonGroup_ == nullptr)
    {
        return;
    }

    QAbstractButton* button =
        waveformZoomButtonGroup_->button(
            zoomFactor);

    if (button == nullptr ||
        button->isChecked())
    {
        return;
    }

    button->setChecked(
        true);

    emit waveformZoomChanged(
        zoomFactor);
}

void ControlWidget::setPerformanceVisible(
    bool visible)
{
    if (performanceCheckBox_ == nullptr)
    {
        return;
    }

    const QSignalBlocker blocker(
        performanceCheckBox_);

    performanceCheckBox_->setChecked(
        visible);
}

void ControlWidget::setAspectRatio(
    OpenScopeSettings::AspectRatio aspectRatio)
{
    if (legacyAspectRatioCheckBox_ == nullptr)
    {
        return;
    }

    const QSignalBlocker blocker(
        legacyAspectRatioCheckBox_);

    legacyAspectRatioCheckBox_->setChecked(
        aspectRatio ==
        OpenScopeSettings::AspectRatio::Ratio4x3);
}

void ControlWidget::setPcmAsioStatus(
    double sampleRate,
    long bufferFrames,
    double qMs,
    double targetMs,
    double queuedMs,
    double fastMs,
    double avg3Ms,
    double ppm,
    double callbackRate,
    std::uint64_t underruns,
    std::uint64_t overruns)
{
    if (pcmAsioRateStatusLabel_ != nullptr)
    {
        pcmAsioRateStatusLabel_->setText(
            sampleRate > 0.0
                ? QStringLiteral("%1 Hz | %2 samples / %3 ms")
                    .arg(sampleRate, 0, 'f', 0)
                    .arg(bufferFrames)
                    .arg(qMs, 0, 'f', 3)
                : QStringLiteral("Not running"));
    }
    if (pcmAsioBufferStatusLabel_ != nullptr)
    {
        pcmAsioBufferStatusLabel_->setText(
            QStringLiteral("target %1 ms | actual %2 ms | fast %3 ms | 3 s %4 ms | nudge %5 ppm")
                .arg(targetMs, 0, 'f', 3)
                .arg(queuedMs, 0, 'f', 3)
                .arg(fastMs, 0, 'f', 3)
                .arg(avg3Ms, 0, 'f', 3)
                .arg(ppm, 0, 'f', 0));
    }
    if (pcmAsioCounterStatusLabel_ != nullptr)
    {
        pcmAsioCounterStatusLabel_->setText(
            QStringLiteral("%1/s | underruns %2 | overruns %3")
                .arg(callbackRate, 0, 'f', 1)
                .arg(underruns)
                .arg(overruns));
    }
    if (pcmAsioGraph_ != nullptr && qMs > 0.0)
        pcmAsioGraph_->append(queuedMs, fastMs, avg3Ms, targetMs, ppm);
}

void ControlWidget::setPcmAudioLevels(float, float)
{
    // Programme meters live in the Audio Scope viewport.
}

void ControlWidget::setPcmDecoderStatus(
    bool enabled,
    int activeFormat,
    bool locked,
    int validLines,
    int testedLines,
    double crcPercent,
    double bitPeriodPixels,
    double syncStartPixels,
    bool modeKnown,
    bool mode16Detected,
    bool controlValid,
    bool preEmphasis,
    int controlValidLines,
    int controlTestedLines,
    int field1ValidAudioLines,
    int field1TestedAudioLines,
    int field1ValidControlLines,
    int field1TestedControlLines,
    int field2ValidAudioLines,
    int field2TestedAudioLines,
    int field2ValidControlLines,
    int field2TestedControlLines,
    std::uint64_t reconstructedGroups,
    std::uint64_t pCorrectedGroups,
    std::uint64_t lsbPackMissingGroups,
    std::uint64_t hardUncorrectableGroups,
    double pVerifyPercent,
    double leftFrequencyHz,
    double rightFrequencyHz)
{
    if (activeFormat == 1)
        pcmResolvedHamMode_ = false;
    else if (activeFormat == 2)
        pcmResolvedHamMode_ = true;
    else if (pcmDecoderSelectionMode_ == 0)
        pcmResolvedHamMode_ = false;

    if (pcmAudioScopeCheckBox_ != nullptr)
        pcmAudioScopeCheckBox_->setEnabled(enabled);
    if (pcmAudioScopeColorizeCheckBox_ != nullptr)
        pcmAudioScopeColorizeCheckBox_->setEnabled(
            enabled && pcmAudioScopeCheckBox_ != nullptr &&
            pcmAudioScopeCheckBox_->isChecked());

    if (pcmLockStatusLabel_ != nullptr)
    {
        QString summary;
        if (!enabled)
        {
            summary = QStringLiteral("Disabled");
        }
        else if (!locked)
        {
            summary = QStringLiteral("Searching...");
        }
        else
        {
            QString format;
            if (pcmResolvedHamMode_)
                format = QStringLiteral("Ham PCM 14-bit / 48 kHz");
            else if (!modeKnown)
                format = QStringLiteral("Sony PCM 14/16-bit");
            else if (mode16Detected)
                format = QStringLiteral("Sony PCM 16-bit");
            else
                format = QStringLiteral("Sony PCM 14-bit");

            summary = QStringLiteral("%1    %2 %")
                .arg(format)
                .arg(std::clamp(pVerifyPercent, 0.0, 100.0), 0, 'f', 2);
        }
        pcmLockStatusLabel_->setText(summary);
    }

    // PAL 625 capture: the PCM-usable active raster is asymmetric by field.
    // F1 has 288 H positions, F2 has 287, for 575 total. The backing video
    // buffer can be 576 rows high; its final row is not another PCM H.
    constexpr int kCapturedField1Lines = 288;
    constexpr int kCapturedField2Lines = 287;
    constexpr int kCapturedLinesPerFrame =
        kCapturedField1Lines + kCapturedField2Lines;

    // Transport quality is coverage of the complete EIAJ audio transport,
    // not capture completeness. PAL carries 294 audio H per field, so the
    // denominator is always 588 audio H per frame. Control-H is separate and
    // must not make a partially visible EIAJ transport read as 100 percent.
    constexpr int kEiajAudioLinesPerFrame = 294 * 2;
    constexpr int kHamAudioLinesPerFrame = 271 * 2;
    const int transportDenominator = pcmResolvedHamMode_
        ? kHamAudioLinesPerFrame
        : kEiajAudioLinesPerFrame;
    const double transportQualityPercent = transportDenominator > 0
        ? 100.0 * static_cast<double>(testedLines) / static_cast<double>(transportDenominator)
        : 0.0;

    if (pcmTransportQualityLabel_ != nullptr)
    {
        pcmTransportQualityLabel_->setText(
            QString::number(
                std::clamp(transportQualityPercent, 0.0, 100.0),
                'f',
                2) +
            QStringLiteral(" %") +
            (
                testedLines == 556
                    ? QStringLiteral(" (BMD max)")
                    : QString()));
    }

    if (pcmTransportLinesLabel_ != nullptr)
    {
        // Never derive a field count by dividing a frame total: if one field
        // loses one H that creates nonsense such as 286.5. Count F1/F2
        // independently so every displayed H is a real, discrete line.
        const auto fieldText = [](
            int audio, int control, int testedAudio, int testedControl)
        {
            QString text = QString::number(audio) + QStringLiteral("/") +
                QString::number(testedAudio) + QStringLiteral(" audio");
            if (testedControl > 0)
            {
                text += QStringLiteral(" + ") + QString::number(control) +
                    QStringLiteral("/") + QString::number(testedControl) +
                    QStringLiteral(" control");
            }
            return text;
        };

        pcmTransportLinesLabel_->setText(
            QStringLiteral("F1 ") +
            fieldText(field1ValidAudioLines, field1ValidControlLines,
                      field1TestedAudioLines, field1TestedControlLines) +
            QStringLiteral(" | F2 ") +
            fieldText(field2ValidAudioLines, field2ValidControlLines,
                      field2TestedAudioLines, field2TestedControlLines));
    }

    if (pcmModeStatusLabel_ != nullptr)
    {
        QString modeText;
        if (activeFormat == 0 && pcmDecoderSelectionMode_ == 0)
            modeText = QStringLiteral("Auto (identifying...)");
        else if (pcmResolvedHamMode_)
            modeText = QStringLiteral("Ham PCM 2.0, 14-bit / 48 kHz");
        else
            modeText = !modeKnown
                ? QStringLiteral("14/16-bit (identifying...)")
                : (mode16Detected
                    ? QStringLiteral("16-bit (P verified)")
                    : QStringLiteral("14-bit (P/Q verified)"));
        if (pcmDecoderSelectionMode_ == 0 && activeFormat != 0)
            modeText.prepend(QStringLiteral("Auto -> "));
        pcmModeStatusLabel_->setText(modeText);
    }

    if (pcmPreEmphasisStatusLabel_ != nullptr)
    {
        QString deEmphasisText;
        if (pcmResolvedHamMode_)
        {
            deEmphasisText = QStringLiteral("n/a (Ham PCM)");
        }
        else if (pcmDeEmphasisMode_ == 1)
        {
            deEmphasisText = QStringLiteral("OFF (forced)");
        }
        else if (pcmDeEmphasisMode_ == 2)
        {
            deEmphasisText = QStringLiteral("ON (50/15 us, forced)");
        }
        else if (controlValid)
        {
            deEmphasisText = preEmphasis
                ? QStringLiteral("ON (50/15 us, control H)")
                : QStringLiteral("OFF (control H)");
        }
        else if (modeKnown)
        {
            deEmphasisText = mode16Detected
                ? QStringLiteral("Assumed OFF (16-bit, no control H)")
                : QStringLiteral("Assumed ON (14-bit, no control H)");
        }
        else
        {
            deEmphasisText = QStringLiteral("Unknown (control line unavailable)");
        }
        pcmPreEmphasisStatusLabel_->setText(deEmphasisText);
    }

    if (pcmControlStatusLabel_ != nullptr)
    {
        pcmControlStatusLabel_->setText(
            pcmResolvedHamMode_
                ? QStringLiteral("n/a; header is RS protected per row")
                : (QString::number(controlValidLines) +
                    QStringLiteral(" / ") +
                    QString::number(controlTestedLines) +
                    QStringLiteral(" valid")));
    }

    if (pcmCrcStatusLabel_ != nullptr)
    {
        pcmCrcStatusLabel_->setText(
            QString::number(crcPercent, 'f', 2) + QStringLiteral(" %"));
    }

    if (pcmLinesStatusLabel_ != nullptr)
    {
        pcmLinesStatusLabel_->setText(
            pcmResolvedHamMode_
                ? (QString::number(validLines) + QStringLiteral(" / 542 Ham audio rows"))
                : (QString::number(validLines) +
                    QStringLiteral(" audio + ") +
                    QString::number(controlValidLines) +
                    QStringLiteral(" control / ") +
                    QString::number(kCapturedLinesPerFrame)));
    }

    if (pcmGeometryStatusLabel_ != nullptr)
    {
        pcmGeometryStatusLabel_->setText(
            bitPeriodPixels > 0.0
                ? QStringLiteral("%1 px/bit, sync x=%2")
                    .arg(bitPeriodPixels, 0, 'f', 3)
                    .arg(syncStartPixels, 0, 'f', 1)
                : QStringLiteral("Searching..."));
    }

    if (pcmPStatusLabel_ != nullptr)
    {
        pcmPStatusLabel_->setText(
            pcmResolvedHamMode_
                ? QStringLiteral("RS core %1 %, corrected rows %2, refinement CRC fallback %3")
                    .arg(pVerifyPercent, 0, 'f', 2)
                    .arg(pCorrectedGroups)
                    .arg(lsbPackMissingGroups)
                : QStringLiteral("P verify %1 %")
                    .arg(pVerifyPercent, 0, 'f', 2));
    }
}

void ControlWidget::setWssStatus(
    const QString& status)
{
    if (wssStatusLabel_ == nullptr)
    {
        return;
    }

    wssStatusLabel_->setText(
        status);
}


void ControlWidget::setCompositeInputGainState(
    bool lumaAvailable,
    bool chromaAvailable,
    int minimumHundredthsDb,
    int maximumHundredthsDb,
    int lumaHundredthsDb,
    int chromaHundredthsDb)
{
    if (compositeLumaGainSlider_ == nullptr ||
        compositeChromaGainSlider_ == nullptr ||
        compositeGainStatusLabel_ == nullptr)
    {
        return;
    }

    if (maximumHundredthsDb < minimumHundredthsDb)
    {
        std::swap(
            minimumHundredthsDb,
            maximumHundredthsDb);
    }

    {
        const QSignalBlocker lumaBlocker(
            compositeLumaGainSlider_);

        // When switching away from DeckLink (for example to the Philips
        // ROM source), keep the last hardware range and thumb position
        // visible and simply disable the control.  Collapsing the range to
        // 0..0 made both sliders jump to the far left, which falsely looked
        // like the calibration had changed.
        if (lumaAvailable)
        {
            compositeLumaGainSlider_->setRange(
                minimumHundredthsDb,
                maximumHundredthsDb);

            compositeLumaGainSlider_->setValue(
                std::clamp(
                    lumaHundredthsDb,
                    minimumHundredthsDb,
                    maximumHundredthsDb));
        }

        compositeLumaGainSlider_->setEnabled(
            lumaAvailable);
    }

    {
        const QSignalBlocker chromaBlocker(
            compositeChromaGainSlider_);

        if (chromaAvailable)
        {
            compositeChromaGainSlider_->setRange(
                minimumHundredthsDb,
                maximumHundredthsDb);

            compositeChromaGainSlider_->setValue(
                std::clamp(
                    chromaHundredthsDb,
                    minimumHundredthsDb,
                    maximumHundredthsDb));
        }

        compositeChromaGainSlider_->setEnabled(
            chromaAvailable);
    }

    if (lumaAvailable || chromaAvailable)
    {
        compositeGainStatusLabel_->setText(
            QStringLiteral(
                "DeckLink hardware control  range 0.00x to 2.00x"));
    }
    else
    {
        const bool hasRememberedDeckLinkRange =
            compositeLumaGainSlider_->maximum() >
                compositeLumaGainSlider_->minimum() ||
            compositeChromaGainSlider_->maximum() >
                compositeChromaGainSlider_->minimum();

        compositeGainStatusLabel_->setText(
            hasRememberedDeckLinkRange
            ? QStringLiteral(
                "Blackmagic composite gain disabled for current source")
            : QStringLiteral(
                "Composite gain control not available on this DeckLink device"));
    }
}

