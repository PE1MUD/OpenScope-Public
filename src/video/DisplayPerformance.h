#pragma once

#include <cstdint>

struct DisplayPerformance
{
    std::uint64_t allocationUs = 0;
    std::uint64_t setupUs = 0;
    std::uint64_t interpolationUs = 0;
    std::uint64_t colorConversionUs = 0;
    std::uint64_t outputUs = 0;
    std::uint64_t composeUs = 0;
};