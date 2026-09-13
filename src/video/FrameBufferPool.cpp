#include "FrameBufferPool.h"

#include <utility>

FrameBufferPool::FrameBufferPool(int width, int height)
{
    buffers_[0].resize(width, height);
    buffers_[1].resize(width, height);
}

Yuv444Frame& FrameBufferPool::writeBuffer()
{
    return buffers_[writeIndex_];
}

const Yuv444Frame& FrameBufferPool::readBuffer() const
{
    return buffers_[readIndex_];
}

void FrameBufferPool::publish()
{
    std::swap(writeIndex_, readIndex_);
}