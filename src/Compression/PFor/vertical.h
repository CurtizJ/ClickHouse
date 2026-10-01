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

#include <array>
#include <bit>
#include <cstring>
#include <utility>

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

// The width is a template argument: the fully unrolled rows then shift by immediates without the
// `bits >= b` branches, whereas a runtime shift count costs an extra shuffle-port uop per shift.
template <unsigned b>
void unpackVertical32Fixed(const uint8_t * in, uint32_t * out) noexcept
{
    static_assert(b >= 1 && b <= 31);
    constexpr uint32_t m = (1u << b) - 1u;
    const v4u32 mask = {m, m, m, m};
    v4u32 acc = {0, 0, 0, 0};
    unsigned bits = 0;
    const uint8_t * p = in;
#pragma clang loop unroll(full)
    for (unsigned row = 0; row < 32; ++row)
    {
        v4u32 outv;
        if (bits >= b)
        {
            outv = acc & mask;
            acc >>= b;
            bits -= b;
        }
        else
        {
            v4u32 w = loadStripeLE(p);
            p += 16;
            outv = (acc | (w << bits)) & mask; // low `bits` from acc, the rest from w
            acc = w >> (b - bits); // b - bits in [1,31]
            bits = 32 - (b - bits);
        }
        std::memcpy(out + 4u * row, &outv, 16);
    }
}

template <unsigned... widths>
inline constexpr auto makeUnpackVertical32Table(std::integer_sequence<unsigned, widths...>) noexcept
{
    using Fn = void (*)(const uint8_t *, uint32_t *) noexcept;
    return std::array<Fn, sizeof...(widths)>{&unpackVertical32Fixed<widths + 1>...};
}

inline ALWAYS_INLINE void unpackVertical32(const uint8_t * in, unsigned b, uint32_t * out) noexcept
{
    static constexpr auto table = makeUnpackVertical32Table(std::make_integer_sequence<unsigned, 31>{});
    table[b - 1](in, out); // b in [1,31]
}

// Inclusive prefix sum of `cnt / 4` groups of 4 residuals, advancing `sum` (all lanes equal). Each group
// does a 2-step in-vector scan (lane shifts, one `vpslldq` each). As in `CompressionCodecDelta`, the running
// sum is advanced by the broadcast of the local scan, which keeps the broadcast off the loop-carried chain.
template <uint32_t plus>
inline ALWAYS_INLINE void deltaDecodeGroups32(uint32_t * out, unsigned cnt, v4u32 & sum) noexcept
{
    const v4u32 plusv = {plus, plus, plus, plus};
    for (unsigned i = 0; i + 4 <= cnt; i += 4)
    {
        v4u32 x;
        std::memcpy(&x, out + i, 16);
        x += plusv;
        x += __builtin_shufflevector(x, v4u32{}, 4, 0, 1, 2);
        x += __builtin_shufflevector(x, v4u32{}, 4, 4, 0, 1);
        const v4u32 local_total = __builtin_shufflevector(x, x, 3, 3, 3, 3);
        x += sum;
        sum += local_total;
        std::memcpy(out + i, &x, 16);
    }
}

// SIMD delta reconstruction (inclusive prefix sum) over a contiguous uint32 residual
// array, with a running carry across blocks. `plus` is 0 for d0 and 1 for d1 (gap-1).
template <uint32_t plus>
inline ALWAYS_INLINE void deltaDecode32(uint32_t * out, unsigned cnt, uint32_t & carry) noexcept
{
    v4u32 sum = {carry, carry, carry, carry};
    /// A constant trip count for full blocks lets the loop unroll; the loop control otherwise costs as much as the scan.
    if (cnt == BLOCK)
    {
        deltaDecodeGroups32<plus>(out, BLOCK, sum);
        carry = sum[0];
        return;
    }
    deltaDecodeGroups32<plus>(out, cnt, sum);
    uint32_t s = sum[0];
    for (unsigned i = cnt & ~3u; i < cnt; ++i) // tail (cnt not a multiple of 4)
    {
        s += out[i] + plus;
        out[i] = s;
    }
    carry = s;
}
}

#endif // PFOR_HAS_VERTICAL
