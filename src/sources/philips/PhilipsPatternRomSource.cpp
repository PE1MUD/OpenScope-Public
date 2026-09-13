#include "sources/philips/PhilipsPatternRomSource.h"

#include "VideoEngine.h"

#include <algorithm>
#include <chrono>

PhilipsPatternRomSource::PhilipsPatternRomSource(
    VideoEngine* videoEngine)
    : videoEngine_(videoEngine)
{
}

PhilipsPatternRomSource::~PhilipsPatternRomSource()
{
    stop();
}

bool PhilipsPatternRomSource::load(
    const QString& iniFileName,
    QString* errorMessage)
{
    stop();

    loaded_.store(
        false,
        std::memory_order_release);

    if (!decoder_.loadIni(
            iniFileName,
            errorMessage))
    {
        return false;
    }

    if (!decoder_.decodeFrame(
            frame0_,
            0,
            errorMessage))
    {
        return false;
    }

    if (decoder_.alternateFrames())
    {
        if (!decoder_.decodeFrame(
                frame1_,
                1,
                errorMessage))
        {
            return false;
        }
    }
    else
    {
        frame1_ =
            frame0_;
    }

    loaded_.store(
        true,
        std::memory_order_release);

    return true;
}

void PhilipsPatternRomSource::start()
{
    if (!loaded_.load(
            std::memory_order_acquire) ||
        running_.exchange(
            true,
            std::memory_order_acq_rel))
    {
        return;
    }

    thread_ =
        std::jthread(
            [this](std::stop_token stopToken)
            {
                run(stopToken);
            });
}

void PhilipsPatternRomSource::stop()
{
    running_.store(
        false,
        std::memory_order_release);

    if (thread_.joinable())
    {
        thread_.request_stop();
        thread_.join();
    }
}

bool PhilipsPatternRomSource::isRunning() const
{
    return running_.load(
        std::memory_order_acquire);
}

QString PhilipsPatternRomSource::setName() const
{
    return decoder_.setName();
}

QString PhilipsPatternRomSource::shortName() const
{
    return decoder_.shortName();
}

QString PhilipsPatternRomSource::iniFileName() const
{
    return decoder_.iniFileName();
}

double PhilipsPatternRomSource::lumaSampleRateHz() const
{
    return decoder_.lumaSampleRateHz();
}

void PhilipsPatternRomSource::run(
    std::stop_token stopToken)
{
    using Clock =
        std::chrono::steady_clock;

    constexpr auto framePeriod =
        std::chrono::milliseconds(40);

    auto nextFrame =
        Clock::now();

    std::uint64_t frameNumber = 0;

    while (!stopToken.stop_requested() &&
        running_.load(
            std::memory_order_acquire))
    {
        const Yuv444Frame& sourceFrame =
            (frameNumber & 1u) == 0u
            ? frame0_
            : frame1_;

        if (videoEngine_ != nullptr)
        {
            if (Yuv444Frame* destination =
                    videoEngine_->tryAcquireWriteFrame())
            {
                destination->width =
                    sourceFrame.width;

                destination->height =
                    sourceFrame.height;

                destination->sampleClockHz =
                    sourceFrame.sampleClockHz;

                destination->y =
                    sourceFrame.y;

                destination->u =
                    sourceFrame.u;

                destination->v =
                    sourceFrame.v;

                videoEngine_->submitWriteFrame();
            }
        }

        ++frameNumber;

        nextFrame +=
            framePeriod;

        const auto now =
            Clock::now();

        if (nextFrame <= now)
        {
            // Do not burst several frames after a scheduling delay.
            nextFrame =
                now + framePeriod;
        }

        std::this_thread::sleep_until(
            nextFrame);
    }

    running_.store(
        false,
        std::memory_order_release);
}
