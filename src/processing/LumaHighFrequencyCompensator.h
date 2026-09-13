#pragma once

#include "video/Yuv444Frame.h"

class LumaHighFrequencyCompensator
{
public:
    void process(
        Yuv444Frame& frame,
        int gainHundredthsDb);

    // Thread-safe row-range entry point used by the capture-side F worker queue.
    // firstLine is inclusive, lastLine is exclusive.
    void processRange(
        Yuv444Frame& frame,
        int gainHundredthsDb,
        int firstLine,
        int lastLine);

private:
    void processAvx2Range(
        Yuv444Frame& frame,
        double scale,
        int firstLine,
        int lastLine);
};
