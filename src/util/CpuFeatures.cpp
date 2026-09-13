#include "CpuFeatures.h"

#include <intrin.h>

bool CpuFeatures::supportsAvx2Fma() noexcept
{
    int registers[4]{};

    __cpuid(
        registers,
        0);

    const int maximumLeaf =
        registers[0];

    if (maximumLeaf < 7)
    {
        return false;
    }

    __cpuid(
        registers,
        1);

    constexpr int kFmaBit =
        1 << 12;

    constexpr int kOsXsaveBit =
        1 << 27;

    constexpr int kAvxBit =
        1 << 28;

    const int ecx =
        registers[2];

    if ((ecx & kFmaBit) == 0 ||
        (ecx & kOsXsaveBit) == 0 ||
        (ecx & kAvxBit) == 0)
    {
        return false;
    }

    constexpr unsigned long long kXmmState =
        1ull << 1;

    constexpr unsigned long long kYmmState =
        1ull << 2;

    const unsigned long long xcr0 =
        _xgetbv(0);

    if ((xcr0 & (kXmmState | kYmmState)) !=
        (kXmmState | kYmmState))
    {
        return false;
    }

    __cpuidex(
        registers,
        7,
        0);

    constexpr int kAvx2Bit =
        1 << 5;

    const int ebx =
        registers[1];

    return (ebx & kAvx2Bit) != 0;
}