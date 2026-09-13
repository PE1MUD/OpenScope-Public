#pragma once

#include "sources/philips/PhilipsPatternRomDecoder.h"

#include <QString>

#include <atomic>
#include <thread>

class VideoEngine;

class PhilipsPatternRomSource
{
public:
    explicit PhilipsPatternRomSource(
        VideoEngine* videoEngine);

    ~PhilipsPatternRomSource();

    bool load(
        const QString& iniFileName,
        QString* errorMessage = nullptr);

    void start();
    void stop();

    bool isRunning() const;
    QString setName() const;
    QString shortName() const;
    QString iniFileName() const;
    double lumaSampleRateHz() const;

private:
    void run(
        std::stop_token stopToken);

    VideoEngine* videoEngine_ = nullptr;
    PhilipsPatternRomDecoder decoder_;
    Yuv444Frame frame0_;
    Yuv444Frame frame1_;
    std::jthread thread_;
    std::atomic_bool loaded_{ false };
    std::atomic_bool running_{ false };
};
