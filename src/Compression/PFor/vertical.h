#pragma once

// SIMD vertical bit-packing for full 128-value blocks of uint32_t or uint64_t. Independently authored.
//
// A block is laid out in LANES interleaved lanes of the element width, LANES * sizeof(T) == 16 bytes, i.e. one
// SSE/NEON vector: value i -> lane (i % LANES), row (i / LANES), so a row is LANES consecutive values and pack/unpack
// move contiguous vectors. The lanes share one bit cursor: a 16-byte stripe carries the next LANE_BITS bits of every
// lane, and decode emits LANES values per vector step. Because ROWS = 128 / LANES == LANE_BITS, every lane packs
// exactly b whole words, so a block takes exactly packedBytes(128, b) = 16*b bytes -- the size of the scalar
// horizontal packing, just reordered for parallel extraction. Used for b in [1, LANE_BITS - 1]; b == 0,
// b == LANE_BITS and partial blocks stay on the scalar path.
//
// GCC/Clang vector extensions lower each lane op to one SSE/NEON instruction.

#include <Compression/PFor/common.h>

#include <bit>
#include <cstring>
#include <type_traits>

#if defined(__GNUC__) || defined(__clang__)
#    define PFOR_HAS_VERTICAL 1
#else
#    define PFOR_HAS_VERTICAL 0
#endif

#if PFOR_HAS_VERTICAL

namespace DB::PFor::detail
{

using v4u32 = uint32_t __attribute__((vector_size(16)));
using v2u64 = uint64_t __attribute__((vector_size(16)));

/// The 16-byte vector of T lanes and the geometry of the vertical layout for T.
template <typename T>
struct Vertical
{
    static_assert(sizeof(T) == 4 || sizeof(T) == 8);
    using Vec = std::conditional_t<sizeof(T) == 4, v4u32, v2u64>;
    static constexpr unsigned LANES = 16 / sizeof(T);
    static constexpr unsigned LANE_BITS = typeBits<T>;
    static constexpr unsigned ROWS = BLOCK / LANES;
    static_assert(ROWS == LANE_BITS, "every lane must pack whole words so that a block takes exactly 16*b bytes");
};

template <typename T>
inline ALWAYS_INLINE typename Vertical<T>::Vec splat(T x) noexcept
{
    typename Vertical<T>::Vec v = {};
    for (unsigned lane = 0; lane < Vertical<T>::LANES; ++lane)
        v[lane] = x;
    return v;
}

// The packed stream is canonical little-endian (matching bitpack.h), but a vector store/load is
// native-endian, so byte-swap each lane on big-endian targets. On little-endian this is the
// identity and folds back to a plain 16-byte move, leaving the fast path unchanged.
template <typename T>
inline ALWAYS_INLINE typename Vertical<T>::Vec bswapLanes(typename Vertical<T>::Vec v) noexcept
{
    if constexpr (sizeof(T) == 4)
        return v4u32{__builtin_bswap32(v[0]), __builtin_bswap32(v[1]), __builtin_bswap32(v[2]), __builtin_bswap32(v[3])};
    else
        return v2u64{__builtin_bswap64(v[0]), __builtin_bswap64(v[1])};
}

template <typename T>
inline ALWAYS_INLINE void storeStripeLE(uint8_t * p, typename Vertical<T>::Vec v) noexcept
{
    if constexpr (std::endian::native == std::endian::big)
        v = bswapLanes<T>(v);
    std::memcpy(p, &v, 16);
}

template <typename T>
inline ALWAYS_INLINE typename Vertical<T>::Vec loadStripeLE(const uint8_t * p) noexcept
{
    typename Vertical<T>::Vec v;
    std::memcpy(&v, p, 16);
    if constexpr (std::endian::native == std::endian::big)
        v = bswapLanes<T>(v);
    return v;
}

// Pack a full block at a compile-time width B: with the row loop fully unrolled the bit cursor is a constant at
// every row, so every shift is an immediate and every refill branch folds away.
template <typename T, unsigned B>
inline ALWAYS_INLINE void packVerticalFixed(const T * r, uint8_t * out) noexcept
{
    using V = Vertical<T>;
    using Vec = typename V::Vec;
    static_assert(B >= 1 && B < V::LANE_BITS);
    const Vec mask = splat<T>(static_cast<T>((T(1) << B) - 1));
    Vec acc = {};
    unsigned bits = 0;
    uint8_t * p = out;

#pragma clang loop unroll(full)
    for (unsigned row = 0; row < V::ROWS; ++row)
    {
        Vec v;
        std::memcpy(&v, r + V::LANES * row, 16);
        v &= mask;
        acc |= v << bits;
        const unsigned nb = bits + B;

        if (nb >= V::LANE_BITS)
        {
            storeStripeLE<T>(p, acc);
            p += 16;
            if (nb == V::LANE_BITS)
            {
                acc = Vec{};
                bits = 0;
            }
            else
            {
                acc = v >> (V::LANE_BITS - bits); // bits > 0 here, so the shift is in [1, LANE_BITS - 1]
                bits = nb - V::LANE_BITS;
            }
        }
        else
        {
            bits = nb;
        }
    }
}

template <typename T, unsigned B>
inline ALWAYS_INLINE void unpackVerticalFixed(const uint8_t * in, T * out) noexcept
{
    using V = Vertical<T>;
    using Vec = typename V::Vec;
    static_assert(B >= 1 && B < V::LANE_BITS);
    const Vec mask = splat<T>(static_cast<T>((T(1) << B) - 1));
    Vec acc = {};
    unsigned bits = 0;
    const uint8_t * p = in;

#pragma clang loop unroll(full)
    for (unsigned row = 0; row < V::ROWS; ++row)
    {
        Vec outv;
        if (bits >= B)
        {
            outv = acc & mask;
            acc >>= B;
            bits -= B;
        }
        else
        {
            Vec w = loadStripeLE<T>(p);
            p += 16;
            outv = (acc | (w << bits)) & mask; // low `bits` from acc, the rest from w
            acc = w >> (B - bits); // B - bits in [1, LANE_BITS - 1]
            bits = V::LANE_BITS - (B - bits);
        }
        std::memcpy(out + V::LANES * row, &outv, 16);
    }
}

// Runtime-width front ends; b must be in [1, LANE_BITS - 1]. Widths that do not exist for T are never instantiated.
#define PFOR_VERTICAL_CASES(FN, ...) \
    switch (b) \
    { \
        PFOR_VERTICAL_CASE(FN, 1, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 2, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 3, __VA_ARGS__) \
        PFOR_VERTICAL_CASE(FN, 4, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 5, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 6, __VA_ARGS__) \
        PFOR_VERTICAL_CASE(FN, 7, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 8, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 9, __VA_ARGS__) \
        PFOR_VERTICAL_CASE(FN, 10, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 11, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 12, __VA_ARGS__) \
        PFOR_VERTICAL_CASE(FN, 13, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 14, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 15, __VA_ARGS__) \
        PFOR_VERTICAL_CASE(FN, 16, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 17, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 18, __VA_ARGS__) \
        PFOR_VERTICAL_CASE(FN, 19, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 20, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 21, __VA_ARGS__) \
        PFOR_VERTICAL_CASE(FN, 22, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 23, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 24, __VA_ARGS__) \
        PFOR_VERTICAL_CASE(FN, 25, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 26, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 27, __VA_ARGS__) \
        PFOR_VERTICAL_CASE(FN, 28, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 29, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 30, __VA_ARGS__) \
        PFOR_VERTICAL_CASE(FN, 31, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 32, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 33, __VA_ARGS__) \
        PFOR_VERTICAL_CASE(FN, 34, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 35, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 36, __VA_ARGS__) \
        PFOR_VERTICAL_CASE(FN, 37, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 38, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 39, __VA_ARGS__) \
        PFOR_VERTICAL_CASE(FN, 40, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 41, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 42, __VA_ARGS__) \
        PFOR_VERTICAL_CASE(FN, 43, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 44, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 45, __VA_ARGS__) \
        PFOR_VERTICAL_CASE(FN, 46, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 47, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 48, __VA_ARGS__) \
        PFOR_VERTICAL_CASE(FN, 49, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 50, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 51, __VA_ARGS__) \
        PFOR_VERTICAL_CASE(FN, 52, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 53, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 54, __VA_ARGS__) \
        PFOR_VERTICAL_CASE(FN, 55, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 56, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 57, __VA_ARGS__) \
        PFOR_VERTICAL_CASE(FN, 58, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 59, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 60, __VA_ARGS__) \
        PFOR_VERTICAL_CASE(FN, 61, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 62, __VA_ARGS__) PFOR_VERTICAL_CASE(FN, 63, __VA_ARGS__) \
        default: \
            return; \
    }

#define PFOR_VERTICAL_CASE(FN, K, ...) \
    case (K): \
        if constexpr ((K) < typeBits<T>) \
            FN<T, (((K) < typeBits<T>) ? (K) : 1u)>(__VA_ARGS__); \
        return;

template <typename T>
inline ALWAYS_INLINE void packVertical(const T * r, unsigned b, uint8_t * out) noexcept
{
    PFOR_VERTICAL_CASES(packVerticalFixed, r, out)
}

template <typename T>
inline ALWAYS_INLINE void unpackVertical(const uint8_t * in, unsigned b, T * out) noexcept
{
    PFOR_VERTICAL_CASES(unpackVerticalFixed, in, out)
}

#undef PFOR_VERTICAL_CASE
#undef PFOR_VERTICAL_CASES

// SIMD delta reconstruction (inclusive prefix sum) over a contiguous uint32 residual
// array, with a running carry across blocks. `plus` is 0 for d0 and 1 for d1 (gap-1).
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

// Fused single-pass unpack + delta: like unpackVertical<uint32_t> but each row's 4 residuals are
// prefix-summed with the running carry and stored as final values, so there is no second
// pass over the output. Valid only for exception-free blocks (residuals == decoded base).
// plus is 0 for d0, 1 for d1.
template <uint32_t plus>
inline ALWAYS_INLINE void unpackVertical32FusedDelta(
    const uint8_t * in, unsigned b, uint32_t * out, uint32_t & carry) noexcept
{
    const uint32_t m = (1u << b) - 1u; // b in [1,31]
    const v4u32 mask = {m, m, m, m};
    const v4u32 plusv = {plus, plus, plus, plus};
    v4u32 acc = {0, 0, 0, 0};
    unsigned bits = 0;
    const uint8_t * p = in;
    uint32_t c = carry;

    for (unsigned row = 0; row < 32; ++row)
    {
        v4u32 v;
        if (bits >= b)
        {
            v = acc & mask;
            acc >>= b;
            bits -= b;
        }
        else
        {
            v4u32 w = loadStripeLE<uint32_t>(p);
            p += 16;
            v = (acc | (w << bits)) & mask;
            acc = w >> (b - bits);
            bits = 32 - (b - bits);
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

}

#endif // PFOR_HAS_VERTICAL
