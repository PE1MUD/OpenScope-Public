#pragma once

#include "Video\Yuv444Frame.h"

class Analyzer
{
public:
    virtual ~Analyzer() = default;

    virtual void analyze(const Yuv444Frame& frame) = 0;
};