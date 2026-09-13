#pragma once

#include <QObject>
#include <QImage>
#include <QString>

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

class SpoutOutput final : public QObject
{
    Q_OBJECT

public:
    explicit SpoutOutput(
        QString senderName,
        QObject* parent = nullptr);

    ~SpoutOutput() override;

    QString senderName() const;
    bool isActive() const noexcept;

    // Current CPU/QImage path. This is intentionally kept behind this
    // class so the renderer side can later move to D3D11 without changing
    // the rest of OpenScope.
signals:
    void submissionTiming(
        std::uint64_t queueDelayUs,
        std::uint64_t sendUs,
        std::uint64_t intervalUs);

public slots:
    void submitImage(
        const QImage& image);

    void submitTimedImage(
        const QImage& image,
        qint64 dispatchTimestampUs);
    void setInputSignalValid(bool valid);
    void setEnabled(bool enabled);
    void stop();

public:
    // Future zero-copy entry point. The texture must belong to the same
    // D3D11 device as this SpoutOutput instance.
    bool submitTexture(ID3D11Texture2D* texture);

    ID3D11Device* device() const noexcept;
    ID3D11DeviceContext* context() const noexcept;

private:
    class Private;
    Private* d_ = nullptr;
};
