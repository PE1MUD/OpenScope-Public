#pragma once

#include <QImage>
#include <QObject>

class VideoSource : public QObject
{
    Q_OBJECT

public:
    explicit VideoSource(QObject* parent = nullptr);
    ~VideoSource() override = default;

    virtual void start() = 0;
    virtual void stop() = 0;

signals:
    void frameReady(const QImage& frame);
};