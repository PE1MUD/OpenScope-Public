#include "output/SpoutOutput.h"
#include "ui/ViewportOverlay.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <d3d11.h>
#include <wrl/client.h>

#if __has_include(<SpoutDX.h>)
#include <SpoutDX.h>
#elif __has_include(<SpoutDX/SpoutDX.h>)
#include <SpoutDX/SpoutDX.h>
#else
#error "SpoutDX.h not found. Install vcpkg port spout2[dx]."
#endif

#include <QByteArray>
#include <QDebug>
#include <QElapsedTimer>
#include <QPainter>

#include <chrono>

#include <utility>

using Microsoft::WRL::ComPtr;

class SpoutOutput::Private
{
public:
    explicit Private(QString name)
        : senderName(std::move(name))
    {
    }

    bool ensureStarted()
    {
        if (!enabled)
        {
            return false;
        }

        if (active)
        {
            return true;
        }

        if (!sender.OpenDirectX11())
        {
            if (!openFailureReported)
            {
                qWarning()
                    << "Spout: OpenDirectX11 failed for"
                    << senderName;
                openFailureReported = true;
            }
            return false;
        }

        device = sender.GetDX11Device();
        context = sender.GetDX11Context();

        if (device == nullptr || context == nullptr)
        {
            qWarning()
                << "Spout: no D3D11 device/context for"
                << senderName;
            sender.CloseDirectX11();
            device = nullptr;
            context = nullptr;
            return false;
        }

        const QByteArray utf8Name = senderName.toUtf8();
        sender.SetSenderName(utf8Name.constData());

        active = true;
        openFailureReported = false;
        return true;
    }

    void releaseTexture()
    {
        uploadTexture.Reset();
        textureWidth = 0;
        textureHeight = 0;
    }

    bool ensureUploadTexture(int width, int height)
    {
        if (uploadTexture &&
            textureWidth == width &&
            textureHeight == height)
        {
            return true;
        }

        releaseTexture();

        if (device == nullptr || width <= 0 || height <= 0)
        {
            return false;
        }

        D3D11_TEXTURE2D_DESC description{};
        description.Width = static_cast<UINT>(width);
        description.Height = static_cast<UINT>(height);
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags =
            D3D11_BIND_SHADER_RESOURCE |
            D3D11_BIND_RENDER_TARGET;

        const HRESULT result =
            device->CreateTexture2D(
                &description,
                nullptr,
                uploadTexture.GetAddressOf());

        if (FAILED(result))
        {
            qWarning()
                << "Spout: CreateTexture2D failed for"
                << senderName
                << Qt::hex
                << static_cast<qulonglong>(result);
            return false;
        }

        textureWidth = width;
        textureHeight = height;
        return true;
    }

    void stop()
    {
        releaseTexture();

        if (active)
        {
            sender.ReleaseSender();
        }

        sender.CloseDirectX11();

        device = nullptr;
        context = nullptr;
        active = false;
        lastSubmitTimestampUs = 0;
    }

    QString senderName;
    spoutDX sender;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ComPtr<ID3D11Texture2D> uploadTexture;

    int textureWidth = 0;
    int textureHeight = 0;

    bool enabled = false;
    bool active = false;
    bool openFailureReported = false;
    qint64 lastSubmitTimestampUs = 0;
    bool inputSignalValid = true;
};

SpoutOutput::SpoutOutput(
    QString senderName,
    QObject* parent)
    : QObject(parent)
    , d_(new Private(std::move(senderName)))
{
}

SpoutOutput::~SpoutOutput()
{
    stop();
    delete d_;
    d_ = nullptr;
}

QString SpoutOutput::senderName() const
{
    return d_->senderName;
}

bool SpoutOutput::isActive() const noexcept
{
    return d_->active;
}

void SpoutOutput::submitImage(
    const QImage& image)
{
    submitTimedImage(
        image,
        0);
}

void SpoutOutput::submitTimedImage(
    const QImage& image,
    qint64 dispatchTimestampUs)
{
    using Clock = std::chrono::steady_clock;

    const qint64 startTimestampUs =
        static_cast<qint64>(
            std::chrono::duration_cast<
                std::chrono::microseconds>(
                    Clock::now().time_since_epoch())
            .count());

    const std::uint64_t queueDelayUs =
        dispatchTimestampUs > 0 &&
        startTimestampUs > dispatchTimestampUs
        ? static_cast<std::uint64_t>(
            startTimestampUs - dispatchTimestampUs)
        : 0u;

    QElapsedTimer sendTimer;
    sendTimer.start();

    // A queued frame can still arrive after the UI has disabled a
    // Spout output. Never let such a stale frame reopen the sender.
    if (!d_->enabled ||
        image.isNull())
    {
        return;
    }

    if (!d_->ensureStarted())
    {
        return;
    }

    QImage source = image;

    if (!d_->inputSignalValid)
    {
        source = image.convertToFormat(QImage::Format_RGB32);
        QPainter painter(&source);
        painter.fillRect(source.rect(), Qt::black);
        ViewportOverlay::drawNoVideo(
            painter,
            QRectF(source.rect()),
            true);
    }

    // QImage RGB32/ARGB32 is BGRA byte order on little-endian Windows,
    // matching DXGI_FORMAT_B8G8R8A8_UNORM exactly.
    if (source.format() != QImage::Format_RGB32 &&
        source.format() != QImage::Format_ARGB32 &&
        source.format() != QImage::Format_ARGB32_Premultiplied)
    {
        source = source.convertToFormat(QImage::Format_ARGB32);
    }

    if (!d_->ensureUploadTexture(
            source.width(),
            source.height()))
    {
        return;
    }

    d_->context->UpdateSubresource(
        d_->uploadTexture.Get(),
        0,
        nullptr,
        source.constBits(),
        static_cast<UINT>(source.bytesPerLine()),
        0);

    d_->sender.SendTexture(d_->uploadTexture.Get());

    const std::uint64_t sendUs =
        static_cast<std::uint64_t>(
            sendTimer.nsecsElapsed() / 1000);

    const qint64 completeTimestampUs =
        static_cast<qint64>(
            std::chrono::duration_cast<
                std::chrono::microseconds>(
                    Clock::now().time_since_epoch())
            .count());

    const std::uint64_t intervalUs =
        d_->lastSubmitTimestampUs > 0 &&
        completeTimestampUs > d_->lastSubmitTimestampUs
        ? static_cast<std::uint64_t>(
            completeTimestampUs -
            d_->lastSubmitTimestampUs)
        : 0u;

    d_->lastSubmitTimestampUs =
        completeTimestampUs;

    emit submissionTiming(
        queueDelayUs,
        sendUs,
        intervalUs);
}

void SpoutOutput::setInputSignalValid(bool valid)
{
    d_->inputSignalValid = valid;
}

void SpoutOutput::setEnabled(bool enabled)
{
    if (d_ == nullptr)
    {
        return;
    }

    d_->enabled = enabled;

    if (!enabled)
    {
        d_->stop();
    }
}

bool SpoutOutput::submitTexture(ID3D11Texture2D* texture)
{
    if (!d_->enabled ||
        texture == nullptr)
    {
        return false;
    }

    if (!d_->ensureStarted())
    {
        return false;
    }

    // This is the future zero-copy path. Renderer-owned textures must be
    // created on the same D3D11 device returned by device().
    return d_->sender.SendTexture(texture);
}

void SpoutOutput::stop()
{
    if (d_ != nullptr)
    {
        d_->stop();
    }
}

ID3D11Device* SpoutOutput::device() const noexcept
{
    return d_->device;
}

ID3D11DeviceContext* SpoutOutput::context() const noexcept
{
    return d_->context;
}
