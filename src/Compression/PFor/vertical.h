#pragma once

// SIMD vertical bit-packing for full 128-value uint32_t blocks. Independently authored.
//
// Values are laid out in 4 interleaved 32-bit lanes: value i -> lane (i & 3), row (i >> 2),
// 32 rows per lane. The 4 lanes share one bit cursor, so a 16-byte stripe carries one
// 32-bit chunk of every lane and decode emits 4 values per vector step. The byte layout is
// exactly packedBytes(128, b) = 16*b bytes -- identical in size to the scalar horizontal
// packing, just reordered for parallel extraction. Used only for b in [1, 31]; b == 0 / 32
// and partial blocks / uint64 stay on the scalar path.
//
// GCC/Clang vector extensions lower each lane op to one SSE/NEON instruction.

#include <Compression/PFor/common.h>

#include <bit>
#include <cstring>

#if defined(__GNUC__) || defined(__clang__)
#    define PFOR_HAS_VERTICAL 1
#else
#    define PFOR_HAS_VERTICAL 0
#endif

#if PFOR_HAS_VERTICAL

namespace DB::PFor::detail
{

using v4u32 = uint32_t __attribute__((vector_size(16)));

// The packed stream is canonical little-endian (matching bitpack.h), but a vector store/load is
// native-endian, so byte-swap each 32-bit lane on big-endian targets. On little-endian this is the
// identity and folds back to a plain 16-byte move, leaving the fast path unchanged.
inline ALWAYS_INLINE v4u32 bswapLanes(v4u32 v) noexcept
{
    return v4u32{__builtin_bswap32(v[0]), __builtin_bswap32(v[1]), __builtin_bswap32(v[2]), __builtin_bswap32(v[3])};
}

inline ALWAYS_INLINE void storeStripeLE(uint8_t * p, v4u32 v) noexcept
{
    if constexpr (std::endian::native == std::endian::big)
        v = bswapLanes(v);

    std::memcpy(p, &v, 16);
}

inline ALWAYS_INLINE v4u32 loadStripeLE(const uint8_t * p) noexcept
{
    v4u32 v;
    std::memcpy(&v, p, 16);

    if constexpr (std::endian::native == std::endian::big)
        v = bswapLanes(v);

    return v;
}

inline ALWAYS_INLINE void packVertical32(const uint32_t * r, unsigned b, uint8_t * out) noexcept
{
    const uint32_t m = (1u << b) - 1u; // b in [1,31]
    const v4u32 mask = {m, m, m, m};
    v4u32 acc = {0, 0, 0, 0};
    unsigned bits = 0;
    uint8_t * p = out;

    for (unsigned row = 0; row < 32; ++row)
    {
        v4u32 v;
        std::memcpy(&v, r + 4u * row, 16);

        v &= mask;
        acc |= v << bits;
        const unsigned nb = bits + b;
        if (nb >= 32)
        {
            storeStripeLE(p, acc);
            p += 16;
            if (nb == 32)
            {
                acc = v4u32{0, 0, 0, 0};
                bits = 0;
            }
            else
            {
                acc = v >> (32 - bits); // bits > 0 here, so shift in [1,31]
                bits = nb - 32;
            }
        }
        else
        {
            bits = nb;
        }
    }
}

// Unpack with a compile-time width: the 32-row loop fully unrolls, so the per-row
// bit cursor and the refill condition fold to constants (no branches in the body),
// the same shape as generated per-width unpackers in simdcomp/TurboPFor.
template <unsigned B>
inline void unpackVertical32Fixed(const uint8_t * in, uint32_t * out) noexcept
{
    static_assert(B >= 1 && B <= 31);
    constexpr uint32_t m = (1u << B) - 1u;
    const v4u32 mask = {m, m, m, m};
    v4u32 acc = {0, 0, 0, 0};
    unsigned bits = 0;
    const uint8_t * p = in;

#pragma unroll
    for (unsigned row = 0; row < 32; ++row)
    {
        v4u32 outv;
        if (bits >= B)
        {
            outv = acc & mask;
            acc >>= B;
            bits -= B;
        }
        else
        {
            v4u32 w = loadStripeLE(p);
            p += 16;
            outv = (acc | (w << bits)) & mask; // low `bits` from acc, the rest from w
            acc = w >> (B - bits); // B - bits in [1,31]
            bits = 32 - (B - bits);
        }

        std::memcpy(out + 4u * row, &outv, 16);
    }
}

inline ALWAYS_INLINE void unpackVertical32(const uint8_t * in, unsigned b, uint32_t * out) noexcept
{
    switch (b)
    {
#define PFOR_VUNPACK_CASE(K) \
        case (K): \
            unpackVertical32Fixed<(K)>(in, out); \
            return;
        PFOR_VUNPACK_CASE(1)  PFOR_VUNPACK_CASE(2)  PFOR_VUNPACK_CASE(3)  PFOR_VUNPACK_CASE(4)
        PFOR_VUNPACK_CASE(5)  PFOR_VUNPACK_CASE(6)  PFOR_VUNPACK_CASE(7)  PFOR_VUNPACK_CASE(8)
        PFOR_VUNPACK_CASE(9)  PFOR_VUNPACK_CASE(10) PFOR_VUNPACK_CASE(11) PFOR_VUNPACK_CASE(12)
        PFOR_VUNPACK_CASE(13) PFOR_VUNPACK_CASE(14) PFOR_VUNPACK_CASE(15) PFOR_VUNPACK_CASE(16)
        PFOR_VUNPACK_CASE(17) PFOR_VUNPACK_CASE(18) PFOR_VUNPACK_CASE(19) PFOR_VUNPACK_CASE(20)
        PFOR_VUNPACK_CASE(21) PFOR_VUNPACK_CASE(22) PFOR_VUNPACK_CASE(23) PFOR_VUNPACK_CASE(24)
        PFOR_VUNPACK_CASE(25) PFOR_VUNPACK_CASE(26) PFOR_VUNPACK_CASE(27) PFOR_VUNPACK_CASE(28)
        PFOR_VUNPACK_CASE(29) PFOR_VUNPACK_CASE(30) PFOR_VUNPACK_CASE(31)
#undef PFOR_VUNPACK_CASE
        default: // callers guarantee b in [1,31]
            return;
    }
}

// SIMD delta reconstruction (inclusive prefix sum) over a contiguous uint32 residual
// array, with a running carry across blocks. `plus` is 0 for d0 and 1 for d1 (gap-1);
// it is a compile-time constant so the d0 instantiation drops the add entirely.
// Replaces the scalar prefix-sum: each 4-lane group does a 2-step in-vector scan
// (lane-wise left shifts, which lower to a single byte-shift each) plus the carry.
template <uint32_t plus>
inline ALWAYS_INLINE void deltaDecode32(uint32_t * out, unsigned cnt, uint32_t & carry) noexcept
{
    const v4u32 plusv = {plus, plus, plus, plus};
    uint32_t c = carry;
    unsigned i = 0;
    for (; i + 4 <= cnt; i += 4)
    {
        v4u32 x;
        std::memcpy(&x, out + i, 16);
        x += plusv;
        x += v4u32{0, x[0], x[1], x[2]}; // inclusive prefix sum, step 1
        x += v4u32{0, 0, x[0], x[1]};    // step 2 -> {a, a+b, a+b+c, a+b+c+d}
        x += v4u32{c, c, c, c};          // add the running carry
        std::memcpy(out + i, &x, 16);
        c = x[3];
    }
    for (; i < cnt; ++i) // tail (cnt not a multiple of 4)
    {
        c += out[i] + plus;
        out[i] = c;
    }
    carry = c;
}

// Fused single-pass unpack + delta: like unpackVertical32 but each row's 4 residuals are
// prefix-summed with the running carry and stored as final values, so there is no second
// pass over the output. Valid only for exception-free blocks (residuals == decoded base).
// plus is 0 for d0, 1 for d1.
// Compile-time-width variant of the fused unpack: same full-unroll constant folding as unpackVertical32Fixed.
template <unsigned B, uint32_t plus>
inline void unpackVertical32FusedDeltaFixed(const uint8_t * in, uint32_t * out, uint32_t & carry) noexcept
{
    static_assert(B >= 1 && B <= 31);
    constexpr uint32_t m = (1u << B) - 1u;
    const v4u32 mask = {m, m, m, m};
    constexpr v4u32 plusv = {plus, plus, plus, plus};
    v4u32 acc = {0, 0, 0, 0};
    unsigned bits = 0;
    const uint8_t * p = in;
    uint32_t c = carry;

#pragma unroll
    for (unsigned row = 0; row < 32; ++row)
    {
        v4u32 v;
        if (bits >= B)
        {
            v = acc & mask;
            acc >>= B;
            bits -= B;
        }
        else
        {
            v4u32 w = loadStripeLE(p);
            p += 16;
            v = (acc | (w << bits)) & mask;
            acc = w >> (B - bits);
            bits = 32 - (B - bits);
        }
        // The 4 lanes are consecutive values (4*row .. 4*row+3): prefix-sum + carry, fused.
        v += plusv;
        v += v4u32{0, v[0], v[1], v[2]};
        v += v4u32{0, 0, v[0], v[1]};
        v += v4u32{c, c, c, c};
        std::memcpy(out + 4u * row, &v, 16);
        c = v[3];
    }
    carry = c;
}

template <uint32_t plus>
inline ALWAYS_INLINE void unpackVertical32FusedDelta(const uint8_t * in, unsigned b, uint32_t * out, uint32_t & carry) noexcept
{
    switch (b)
    {
#define PFOR_VUNPACK_CASE(K) \
        case (K): \
            unpackVertical32FusedDeltaFixed<(K), plus>(in, out, carry); \
            return;
        PFOR_VUNPACK_CASE(1)  PFOR_VUNPACK_CASE(2)  PFOR_VUNPACK_CASE(3)  PFOR_VUNPACK_CASE(4)
        PFOR_VUNPACK_CASE(5)  PFOR_VUNPACK_CASE(6)  PFOR_VUNPACK_CASE(7)  PFOR_VUNPACK_CASE(8)
        PFOR_VUNPACK_CASE(9)  PFOR_VUNPACK_CASE(10) PFOR_VUNPACK_CASE(11) PFOR_VUNPACK_CASE(12)
        PFOR_VUNPACK_CASE(13) PFOR_VUNPACK_CASE(14) PFOR_VUNPACK_CASE(15) PFOR_VUNPACK_CASE(16)
        PFOR_VUNPACK_CASE(17) PFOR_VUNPACK_CASE(18) PFOR_VUNPACK_CASE(19) PFOR_VUNPACK_CASE(20)
        PFOR_VUNPACK_CASE(21) PFOR_VUNPACK_CASE(22) PFOR_VUNPACK_CASE(23) PFOR_VUNPACK_CASE(24)
        PFOR_VUNPACK_CASE(25) PFOR_VUNPACK_CASE(26) PFOR_VUNPACK_CASE(27) PFOR_VUNPACK_CASE(28)
        PFOR_VUNPACK_CASE(29) PFOR_VUNPACK_CASE(30) PFOR_VUNPACK_CASE(31)
#undef PFOR_VUNPACK_CASE
        default: // callers guarantee b in [1,31]
            return;
    }
}

}

#endif // PFOR_HAS_VERTICAL
