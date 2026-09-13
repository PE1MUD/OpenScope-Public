#include "ScopeViewport.h"

#include <QMouseEvent>
#include <QVBoxLayout>
#include <QSizePolicy>


ScopeViewport::ScopeViewport(
    QWidget* contentWidget,
    QWidget* parent)
    : QWidget(parent)
    , contentWidget_(contentWidget)
{
    setSizePolicy(
        QSizePolicy::Ignored,
        QSizePolicy::Ignored);

    setMinimumSize(
        0,
        0);

    auto* layout =
        new QVBoxLayout(this);

    layout->setContentsMargins(
        0,
        0,
        0,
        0);

    layout->setSpacing(0);

    if (contentWidget_)
    {
        contentWidget_->setParent(this);

        contentWidget_->setSizePolicy(
            QSizePolicy::Ignored,
            QSizePolicy::Ignored);

        contentWidget_->setMinimumSize(
            0,
            0);

        layout->addWidget(
            contentWidget_);
    }
}


QSize ScopeViewport::sizeHint() const
{
    return QSize(0, 0);
}

QSize ScopeViewport::minimumSizeHint() const
{
    return QSize(0, 0);
}

void ScopeViewport::focusContent()
{
    if (contentWidget_ != nullptr)
    {
        contentWidget_->setFocus(
            Qt::OtherFocusReason);
    }
}

void ScopeViewport::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        emit doubleClicked(this);
        event->accept();
        return;
    }

    QWidget::mouseDoubleClickEvent(event);
}
