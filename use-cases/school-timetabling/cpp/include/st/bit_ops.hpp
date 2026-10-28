#pragma once

// Portable bit counting for the day/period masks (32-bit unsigned).
//
// GCC/Clang: the same __builtin_* calls as before, so codegen is unchanged.
// MSVC has no __builtin_*: _BitScanForward/_BitScanReverse for the scans and a
// branch-free SWAR popcount (the POPCNT instruction is not guaranteed on every
// x64 CPU, and C++17 has no <bit>).
//
// Like the builtins, ctz and clz are undefined for 0; callers check first.

#include <cstdint>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

namespace st::bits
{

inline int popcount(std::uint32_t x)
{
#if defined(_MSC_VER) && !defined(__clang__)
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    x = (x + (x >> 4)) & 0x0F0F0F0Fu;
    return static_cast<int>((x * 0x01010101u) >> 24);
#else
    return __builtin_popcount(x);
#endif
}

/// Count trailing zeros; x must be non-zero.
inline int ctz(std::uint32_t x)
{
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long index;
    _BitScanForward(&index, x);
    return static_cast<int>(index);
#else
    return __builtin_ctz(x);
#endif
}

/// Count leading zeros; x must be non-zero.
inline int clz(std::uint32_t x)
{
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long index;
    _BitScanReverse(&index, x);
    return 31 - static_cast<int>(index);
#else
    return __builtin_clz(x);
#endif
}

} // namespace st::bits
