#pragma once

#include "Yuv444Frame.h"

class FrameBufferPool
{
public:
    explicit FrameBufferPool(int width, int height);

    Yuv444Frame& writeBuffer();
    const Yuv444Frame& readBuffer() const;

    void publish();

private:
    Yuv444Frame buffers_[2];
    int writeIndex_ = 0;
    int readIndex_ = 1;
};