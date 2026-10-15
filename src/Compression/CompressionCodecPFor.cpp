#include <Common/PODArray.h>
#include <Common/SipHash.h>
#include <Compression/CompressionFactory.h>
#include <Compression/CompressionInfo.h>
#include <Compression/ICompressionCodec.h>
#include <Compression/PFor.h>
#include <Compression/registerCompressionCodecs.h>
#include <DataTypes/IDataType.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/IAST_fwd.h>
#include <base/unaligned.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <optional>
#include <span>
#include <type_traits>


namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int CANNOT_DECOMPRESS;
    extern const int ILLEGAL_CODEC_PARAMETER;
    extern const int ILLEGAL_SYNTAX_FOR_CODEC_TYPE;
    extern const int LOGICAL_ERROR;
}

/** PFor column codec: bit-packed residuals with patched exceptions (PForDelta).
  *
  * The residual of each value is computed in the element width with wrapping arithmetic and,
  * for the differencing modes, mapped to an unsigned number by zigzag encoding, so that a delta of
  * deltas close to zero (positive or negative) takes only a few bits:
  *   `none`         residual = value                                 (raw unsigned values)
  *   `delta`        residual = zigzag(value - prev_value)            (classic PForDelta)
  *   `double_delta` residual = zigzag(delta - prev_delta)            (timestamps with an almost constant stride)
  * The mode is a required parameter of the column codec and is stored in every block, so a codec
  * created from the method byte alone (the read path, `system.codecs`) has no mode: it decompresses
  * any block and cannot compress.
  * The first value is a delta from zero and, in `double_delta`, the second is a delta of deltas
  * from zero, so every compressed block is self-contained.
  *
  * The residuals are then encoded by the generic PFor block codec (`Compression/PFor.h`) in blocks
  * of 128 values: each block stores its values at the smallest bit width that keeps the block
  * short, and the few values that do not fit (a jump between series, a gap in time) are stored
  * as patched exceptions instead of widening the whole block. Unlike `DoubleDelta`, the output
  * is bit-aligned with no per-value prefixes, so it decodes with a fast fixed-width unpacker and
  * rarely benefits from a generic compression codec after it.
  *
  * Block layout (inside the standard compressed-block envelope):
  *   u8   element_size            1, 2, 4 or 8
  *   u8   mode                    0 = none, 1 = delta, 2 = double_delta
  *   raw  tail                    uncompressed_size % element_size bytes, copied verbatim
  *   PFor block stream            `PFor::encodeBlocks` over the residuals, without a header
  * The number of values is known to the decoder from the envelope, the residuals of 1- and
  * 2-byte elements are packed as `UInt32`, those of 8-byte elements as `UInt64`. The block
  * stream holds any number of values (its last block may be partial), but only whole values;
  * a compressed block is cut by bytes (e.g. a `max_compress_block_size` that is not a multiple
  * of the element size), so the leading bytes that do not form a whole element are stored as
  * is, like every other numeric codec does (`bytes_to_skip` in `Delta`, `DoubleDelta`, `T64`).
  */
class CompressionCodecPFor : public ICompressionCodec
{
public:
    enum class Mode : UInt8
    {
        None = 0,
        Delta = 1,
        DoubleDelta = 2,
    };

    CompressionCodecPFor(std::optional<Mode> mode_, UInt8 data_bytes_size_);

    uint8_t getMethodByte() const override;
    ASTPtr getCodecDescription() const override;
    void updateHash(SipHash & hash) const override;

protected:
    UInt32 doCompressData(const char * source, UInt32 source_size, char * dest) const override;
    UInt32 doDecompressData(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const override;
    UInt32 getMaxCompressedDataSize(UInt32 uncompressed_size) const override;

    bool isCompression() const override { return true; }
    bool isGenericCompression() const override { return false; }
    bool isDeltaCompression() const override { return true; }
    String getDescription() const override
    {
        return "Bit-packs zigzag-encoded deltas or deltas of deltas with patched exceptions (PForDelta); "
               "suitable for integer and time series data, usually needs no generic compression codec after it.";
    }

private:
    std::optional<Mode> mode;
    UInt8 data_bytes_size;
};


namespace
{

using Mode = CompressionCodecPFor::Mode;

constexpr UInt8 MAX_MODE = static_cast<UInt8>(Mode::DoubleDelta);
constexpr UInt8 HEADER_SIZE = 2;
constexpr auto MODE_REQUIRED_MESSAGE = "PFor codec must have a mode parameter: PFor('none'), PFor('delta') or PFor('double_delta')";

const char * modeToString(Mode mode)
{
    switch (mode)
    {
        case Mode::None: return "none";
        case Mode::Delta: return "delta";
        case Mode::DoubleDelta: return "double_delta";
    }
}

Mode parseMode(const String & name)
{
    if (name == "none")
        return Mode::None;
    if (name == "delta")
        return Mode::Delta;
    if (name == "double_delta")
        return Mode::DoubleDelta;
    throw Exception(ErrorCodes::ILLEGAL_CODEC_PARAMETER,
        "Unknown mode '{}' for codec PFor. Expected one of: 'none', 'delta', 'double_delta'", name);
}

/// The residuals of small elements are widened for packing; the PFor block codec supports UInt32 and UInt64.
template <typename ValueType>
using PackedType = std::conditional_t<sizeof(ValueType) <= sizeof(UInt32), UInt32, UInt64>;

template <typename T>
T zigzag(T value)
{
    static_assert(is_unsigned_v<T>);
    using Signed = std::make_signed_t<T>;
    return static_cast<T>((value << 1) ^ static_cast<T>(static_cast<Signed>(value) >> (8 * sizeof(T) - 1)));
}

template <typename T>
T unzigzag(T value)
{
    static_assert(is_unsigned_v<T>);
    return static_cast<T>((value >> 1) ^ static_cast<T>(T(0) - (value & 1)));
}

template <typename ValueType>
UInt32 compressDataForType(const char * source, size_t count, char * dest, Mode mode)
{
    static_assert(is_unsigned_v<ValueType>);
    using Packed = PackedType<ValueType>;

    if (count == 0)
        return 0;

    PODArray<Packed> residuals(count);
    ValueType prev_value = 0;
    ValueType prev_delta = 0;

    switch (mode)
    {
        case Mode::None:
            for (size_t i = 0; i < count; ++i)
                residuals[i] = unalignedLoadLittleEndian<ValueType>(source + i * sizeof(ValueType));
            break;
        case Mode::Delta:
            for (size_t i = 0; i < count; ++i)
            {
                const ValueType value = unalignedLoadLittleEndian<ValueType>(source + i * sizeof(ValueType));
                residuals[i] = zigzag<ValueType>(static_cast<ValueType>(value - prev_value));
                prev_value = value;
            }
            break;
        case Mode::DoubleDelta:
            for (size_t i = 0; i < count; ++i)
            {
                const ValueType value = unalignedLoadLittleEndian<ValueType>(source + i * sizeof(ValueType));
                const ValueType delta = static_cast<ValueType>(value - prev_value);
                residuals[i] = zigzag<ValueType>(static_cast<ValueType>(delta - prev_delta));
                prev_delta = delta;
                prev_value = value;
            }
            break;
    }

    return static_cast<UInt32>(PFor::encodeBlocks<Packed>(
        std::span<const Packed>(residuals.data(), count), PFor::Delta::none, reinterpret_cast<uint8_t *>(dest)));
}

/// A row of 16 bytes of packed residuals or values: 4 lanes for 1-, 2- and 4-byte elements, 2 lanes for 8-byte ones.
using Row32 = UInt32 __attribute__((vector_size(16)));
using Row64 = UInt64 __attribute__((vector_size(16)));

template <typename Packed>
using RowOf = std::conditional_t<sizeof(Packed) == sizeof(UInt32), Row32, Row64>;

/// Inclusive prefix sum over the lanes: {a, a+b} or {a, a+b, a+b+c, a+b+c+d}.
template <typename Row>
ALWAYS_INLINE Row prefixSum(Row x)
{
    if constexpr (sizeof(Row) / sizeof(x[0]) == 2)
    {
        x += __builtin_shufflevector(x, Row{}, 2, 0);
    }
    else
    {
        x += __builtin_shufflevector(x, Row{}, 4, 0, 1, 2);
        x += __builtin_shufflevector(x, Row{}, 4, 4, 0, 1);
    }
    return x;
}

template <typename Row>
ALWAYS_INLINE Row broadcastLast(Row x)
{
    if constexpr (sizeof(Row) / sizeof(x[0]) == 2)
        return __builtin_shufflevector(x, x, 1, 1);
    else
        return __builtin_shufflevector(x, x, 3, 3, 3, 3);
}

/// Reconstructs the values of one block from its residuals and stores them. The residuals are processed in rows:
/// a row is unzigzagged as a vector, the running delta and value are prefix sums within the row plus the carries
/// of the previous row, which are kept as broadcast vectors so that the serial chain stays in the vector unit
/// instead of costing ten scalar instructions per value. The sums run in the packed width and are truncated on
/// store, which gives the same low bits as the element-width arithmetic of the encoder. The carries are plain
/// locals of the caller: behind a pointer they would be reloaded after every store.
template <typename ValueType, Mode mode>
ALWAYS_INLINE void reconstructBlock(
    const PackedType<ValueType> * residuals,
    unsigned count,
    char * dest,
    RowOf<PackedType<ValueType>> & value,
    RowOf<PackedType<ValueType>> & delta)
{
    using Packed = PackedType<ValueType>;
    using Row = RowOf<Packed>;
    constexpr unsigned LANES = sizeof(Row) / sizeof(Packed);

    unsigned i = 0;
    for (; i + LANES <= count; i += LANES)
    {
        Row row;
        memcpy(&row, residuals + i, sizeof(row));

        if constexpr (mode != Mode::None)
        {
            const Row steps = (row >> 1) ^ (Row{} - (row & 1)); /// unzigzag

            if constexpr (mode == Mode::Delta)
            {
                value = prefixSum(steps) + value;
            }
            else
            {
                delta = prefixSum(steps) + delta;
                value = prefixSum(delta) + value;
            }

            row = value;
            value = broadcastLast(value);
            delta = broadcastLast(delta);
        }

        if constexpr (sizeof(ValueType) == sizeof(Packed) && std::endian::native == std::endian::little)
        {
            memcpy(dest + i * sizeof(ValueType), &row, sizeof(row));
        }
        else
        {
            for (unsigned lane = 0; lane < LANES; ++lane)
                unalignedStoreLittleEndian<ValueType>(dest + (i + lane) * sizeof(ValueType), static_cast<ValueType>(row[lane]));
        }
    }

    /// The last row of a partial block, scalar; every lane of a carry holds the same value.
    Packed last_value = value[0];
    Packed last_delta = delta[0];

    for (; i < count; ++i)
    {
        Packed current = residuals[i];
        if constexpr (mode == Mode::Delta)
        {
            last_value += unzigzag<Packed>(current);
            current = last_value;
        }
        else if constexpr (mode == Mode::DoubleDelta)
        {
            last_delta += unzigzag<Packed>(current);
            last_value += last_delta;
            current = last_value;
        }

        unalignedStoreLittleEndian<ValueType>(dest + i * sizeof(ValueType), static_cast<ValueType>(current));
    }

    value = Row{} + last_value;
    delta = Row{} + last_delta;
}

template <typename ValueType, Mode mode>
void decompressBlockStream(const char * source, UInt32 source_size, char * dest, size_t count)
{
    using Packed = PackedType<ValueType>;
    using Row = RowOf<Packed>;

    const auto * begin = reinterpret_cast<const uint8_t *>(source);
    const auto * end = begin + source_size;
    const uint8_t * pos = begin;

    /// The blocks are decoded one at a time into a small buffer that stays in L1 while the values are
    /// reconstructed from it, instead of materializing the residuals of the whole stream first.
    Packed residuals[PFor::BLOCK];
    Packed prev = 0; /// The carry of the block codec's own delta transform, unused with `Delta::none`.
    Row value = {};
    Row delta = {};

    for (size_t offset = 0; offset < count; offset += PFor::BLOCK)
    {
        /// Fail-closed: every read is bounded by `end` and any malformed block header yields 0.
        const auto block_count = static_cast<unsigned>(std::min<size_t>(PFor::BLOCK, count - offset));
        const size_t consumed = PFor::decodeBlock<Packed>(pos, block_count, PFor::Delta::none, residuals, prev, end);

        if (consumed == 0)
            throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress PFor-encoded data: the block at byte {} of {} is corrupted", pos - begin, source_size);

        pos += consumed;
        reconstructBlock<ValueType, mode>(residuals, block_count, dest + offset * sizeof(ValueType), value, delta);
    }

    /// The stream must be consumed exactly, so trailing garbage is rejected.
    if (pos != end)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress PFor-encoded data: the block stream has {} bytes but {} were decoded", source_size, pos - begin);
}

template <typename ValueType>
void decompressDataForType(const char * source, UInt32 source_size, char * dest, size_t count, Mode mode)
{
    static_assert(is_unsigned_v<ValueType>);

    switch (mode)
    {
        case Mode::None:
            decompressBlockStream<ValueType, Mode::None>(source, source_size, dest, count);
            break;
        case Mode::Delta:
            decompressBlockStream<ValueType, Mode::Delta>(source, source_size, dest, count);
            break;
        case Mode::DoubleDelta:
            decompressBlockStream<ValueType, Mode::DoubleDelta>(source, source_size, dest, count);
            break;
    }
}

UInt8 getDataBytesSize(const IDataType * column_type)
{
    if (!column_type->isValueRepresentedByNumber())
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Codec PFor is not applicable for {} because the data type is not numeric",
            column_type->getName());

    const size_t max_size = column_type->getSizeOfValueInMemory();
    if (max_size == 1 || max_size == 2 || max_size == 4 || max_size == 8)
        return static_cast<UInt8>(max_size);

    throw Exception(ErrorCodes::BAD_ARGUMENTS,
        "Codec PFor is only applicable for data types of size 1, 2, 4, 8 bytes. Given type {}",
        column_type->getName());
}

}


CompressionCodecPFor::CompressionCodecPFor(std::optional<Mode> mode_, UInt8 data_bytes_size_)
    : mode(mode_)
    , data_bytes_size(data_bytes_size_)
{
}

uint8_t CompressionCodecPFor::getMethodByte() const
{
    return static_cast<uint8_t>(CompressionMethodByte::PFor);
}

ASTPtr CompressionCodecPFor::getCodecDescription() const
{
    if (!mode)
        return makeCodecDescription("PFor");
    return makeCodecDescription("PFor", {make_intrusive<ASTLiteral>(String(modeToString(*mode)))});
}

void CompressionCodecPFor::updateHash(SipHash & hash) const
{
    getCodecDescription()->updateTreeHash(hash, /*ignore_aliases=*/ true);
    hash.update(data_bytes_size);
}

UInt32 CompressionCodecPFor::getMaxCompressedDataSize(UInt32 uncompressed_size) const
{
    const UInt32 tail_size = uncompressed_size % data_bytes_size;
    const size_t count = uncompressed_size / data_bytes_size;

    /// Same bound as `PFor::maxCompressedBytes`, taken at the element width rather than the packing width:
    /// residuals are computed in the element width, so a block never needs more than `8 * data_bytes_size`
    /// bits per value plus a 2-byte block header, whatever the width they are packed as.
    const size_t block_stream_size = data_bytes_size * count + 2 * (count / PFor::BLOCK + 1) + 16;
    return HEADER_SIZE + tail_size + static_cast<UInt32>(block_stream_size);
}

UInt32 CompressionCodecPFor::doCompressData(const char * source, UInt32 source_size, char * dest) const
{
    if (!mode)
        throw Exception(ErrorCodes::ILLEGAL_SYNTAX_FOR_CODEC_TYPE, "{}", MODE_REQUIRED_MESSAGE);

    const UInt8 tail_size = source_size % data_bytes_size;
    dest[0] = static_cast<char>(data_bytes_size);
    dest[1] = static_cast<char>(*mode);
    memcpy(&dest[HEADER_SIZE], source, tail_size);

    const UInt32 start_pos = HEADER_SIZE + tail_size;
    const size_t count = (source_size - tail_size) / data_bytes_size;
    UInt32 compressed_size = 0;

    switch (data_bytes_size)
    {
        case 1:
            compressed_size = compressDataForType<UInt8>(&source[tail_size], count, &dest[start_pos], *mode);
            break;
        case 2:
            compressed_size = compressDataForType<UInt16>(&source[tail_size], count, &dest[start_pos], *mode);
            break;
        case 4:
            compressed_size = compressDataForType<UInt32>(&source[tail_size], count, &dest[start_pos], *mode);
            break;
        case 8:
            compressed_size = compressDataForType<UInt64>(&source[tail_size], count, &dest[start_pos], *mode);
            break;
        default:
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Cannot compress with codec PFor. Invalid element size {}", UInt32{data_bytes_size});
    }

    return start_pos + compressed_size;
}

UInt32 CompressionCodecPFor::doDecompressData(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const
{
    if (source_size < HEADER_SIZE)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress PFor-encoded data. File has wrong header");

    const UInt8 element_size = source[0];
    if (element_size != 1 && element_size != 2 && element_size != 4 && element_size != 8)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress PFor-encoded data. File has wrong header: element size {}", UInt32{element_size});

    const UInt8 mode_byte = source[1];
    if (mode_byte > MAX_MODE)
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress PFor-encoded data. File has wrong header: mode {}", UInt32{mode_byte});

    const UInt8 tail_size = uncompressed_size % element_size;
    if (static_cast<UInt32>(HEADER_SIZE + tail_size) > source_size)
    {
        throw Exception(ErrorCodes::CANNOT_DECOMPRESS,
            "Cannot decompress PFor-encoded data. File has wrong header: {} bytes for a {}-byte tail",
            source_size, UInt32{tail_size});
    }

    memcpy(dest, &source[HEADER_SIZE], tail_size);

    const size_t count = (uncompressed_size - tail_size) / element_size;
    const char * block_stream = &source[HEADER_SIZE + tail_size];
    const UInt32 block_stream_size = source_size - HEADER_SIZE - tail_size;

    if (count == 0)
    {
        if (block_stream_size != 0)
            throw Exception(ErrorCodes::CANNOT_DECOMPRESS, "Cannot decompress PFor-encoded data: {} unexpected bytes after an empty block", block_stream_size);

        return uncompressed_size;
    }

    const auto stored_mode = static_cast<Mode>(mode_byte);
    switch (element_size)
    {
        case 1:
            decompressDataForType<UInt8>(block_stream, block_stream_size, &dest[tail_size], count, stored_mode);
            break;
        case 2:
            decompressDataForType<UInt16>(block_stream, block_stream_size, &dest[tail_size], count, stored_mode);
            break;
        case 4:
            decompressDataForType<UInt32>(block_stream, block_stream_size, &dest[tail_size], count, stored_mode);
            break;
        case 8:
            decompressDataForType<UInt64>(block_stream, block_stream_size, &dest[tail_size], count, stored_mode);
            break;
        default:
            /// Unreachable because of the check above.
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Cannot decompress with codec PFor. File has incorrect element size ({})", UInt32{element_size});
    }

    return uncompressed_size;
}

void registerCodecPFor(CompressionCodecFactory & factory)
{
    UInt8 method_code = static_cast<UInt8>(CompressionMethodByte::PFor);

    auto creator = [&](const ASTPtr & arguments, const IDataType * column_type) -> CompressionCodecPtr
    {
        /// The element size comes from the column type. Without a type (`clickhouse-compressor`, the lookup by
        /// method byte) the codec works on 1-byte elements, like `DoubleDelta`.
        UInt8 data_bytes_size = 1;
        if (column_type != nullptr)
            data_bytes_size = getDataBytesSize(column_type);

        /// Created from the method byte alone, e.g. to decompress a block whose header carries the mode.
        if (!arguments && !column_type)
            return std::make_shared<CompressionCodecPFor>(std::nullopt, data_bytes_size);

        if (!arguments || arguments->children.empty())
            throw Exception(ErrorCodes::ILLEGAL_SYNTAX_FOR_CODEC_TYPE, "{}", MODE_REQUIRED_MESSAGE);

        const auto & children = arguments->children;
        if (children.size() != 1)
            throw Exception(ErrorCodes::ILLEGAL_SYNTAX_FOR_CODEC_TYPE, "PFor codec must have exactly 1 parameter, given {}", children.size());

        const auto * mode_literal = children[0]->as<ASTLiteral>();
        if (!mode_literal || mode_literal->value.getType() != Field::Types::Which::String)
            throw Exception(ErrorCodes::ILLEGAL_CODEC_PARAMETER, "The argument of codec PFor must be a mode: 'none', 'delta' or 'double_delta'");

        const Mode mode = parseMode(mode_literal->value.safeGet<String>());
        return std::make_shared<CompressionCodecPFor>(mode, data_bytes_size);
    };

    factory.registerCompressionCodecWithType("PFor", method_code, creator);
}

CompressionCodecPtr getCompressionCodecPFor(UInt8 data_bytes_size)
{
    return std::make_shared<CompressionCodecPFor>(std::nullopt, data_bytes_size);
}

}
