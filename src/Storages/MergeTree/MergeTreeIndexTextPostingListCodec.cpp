#include <Storages/MergeTree/MergeTreeIndexTextPostingListCodec.h>
#include <Storages/MergeTree/MergeTreeIndexText.h>
#include <Storages/MergeTree/BitpackingBlockCodec.h>
#include <Common/ProfileEvents.h>

#include <roaring/roaring.hh>

namespace ProfileEvents
{
    extern const Event TextIndexPostingsBlocksSkipped;
}

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int LOGICAL_ERROR;
}

/// Normalize the requested block size to a multiple of BLOCK_SIZE.
/// We encode/decode posting lists in fixed-size blocks, and the SIMD bit-packing
/// implementation expects block-aligned sizes for efficient processing.
PostingListCodecBitpackingImpl::PostingListCodecBitpackingImpl(size_t postings_list_block_size)
    : max_rowids_in_segment((postings_list_block_size + BLOCK_SIZE - 1) & ~(BLOCK_SIZE - 1))
{
    compressed_data.reserve(BLOCK_SIZE);
    current_segment.reserve(BLOCK_SIZE);
}

void PostingListCodecBitpackingImpl::insert(uint32_t row_id)
{
    if (row_ids_in_current_segment == 0)
    {
        segment_descriptors.emplace_back();
        segment_descriptors.back().row_id_begin = row_id;
        segment_descriptors.back().compressed_data_offset = compressed_data.size();
        segment_block_metas.emplace_back();

        prev_row_id = row_id;
        current_segment.emplace_back(row_id - prev_row_id);
        ++row_ids_in_current_segment;
        ++total_row_ids;
        return;
    }

    current_segment.emplace_back(row_id - prev_row_id);
    prev_row_id = row_id;
    ++row_ids_in_current_segment;
    ++total_row_ids;

    if (current_segment.size() == BLOCK_SIZE)
    {
        encodeBlock(current_segment);
        current_segment.clear();
    }

    if (row_ids_in_current_segment == max_rowids_in_segment)
        flushCurrentSegment();
}

void PostingListCodecBitpackingImpl::insert(std::span<uint32_t> row_ids)
{
    chassert(row_ids.size() == BLOCK_SIZE && row_ids_in_current_segment % BLOCK_SIZE == 0);

    if (row_ids_in_current_segment == 0)
    {
        segment_descriptors.emplace_back();
        segment_descriptors.back().row_id_begin = row_ids.front();
        segment_descriptors.back().compressed_data_offset = compressed_data.size();
        segment_block_metas.emplace_back();

        prev_row_id = row_ids.front();
    }
    row_ids_in_current_segment += BLOCK_SIZE;
    total_row_ids += BLOCK_SIZE;

    auto last_row = row_ids.back();
    std::adjacent_difference(row_ids.begin(), row_ids.end(), row_ids.begin());
    row_ids[0] -= prev_row_id;
    prev_row_id = last_row;

    encodeBlock(row_ids);

    if (row_ids_in_current_segment == max_rowids_in_segment)
        flushCurrentSegment();
}

void PostingListCodecBitpackingImpl::decode(ReadBuffer & in, PostingList & postings, const RowsRange * clip_range)
{
    Header header;
    header.read(in);

    prev_row_id = header.first_row_id;

    const size_t num_blocks = header.cardinality / BLOCK_SIZE;
    const size_t tail_size = header.cardinality % BLOCK_SIZE;

    current_segment.reserve(BLOCK_SIZE);
    if (header.payload_bytes > (compressed_data.capacity() - compressed_data.size()))
        compressed_data.reserve(compressed_data.size() + header.payload_bytes);
    compressed_data.resize(header.payload_bytes);

    in.readStrict(compressed_data.data(), header.payload_bytes);

    std::span<const std::byte> compressed_data_span(reinterpret_cast<const std::byte*>(compressed_data.data()), compressed_data.size());

    if (clip_range)
    {
        decodeClipped(in, header, *clip_range, compressed_data_span, postings);
        return;
    }

    for (size_t i = 0; i < num_blocks; i++)
    {
        decodeBlock(compressed_data_span, BLOCK_SIZE, prev_row_id, current_segment);
        postings.addMany(current_segment.size(), current_segment.data());
    }
    if (tail_size)
    {
        decodeBlock(compressed_data_span, tail_size, prev_row_id, current_segment);
        postings.addMany(current_segment.size(), current_segment.data());
    }
}

void PostingListCodecBitpackingImpl::decodeClipped(
    ReadBuffer & in, const Header & header, const RowsRange & clip_range, std::span<const std::byte> payload, PostingList & postings)
{
    const size_t tail_size = header.cardinality % BLOCK_SIZE;
    const size_t num_blocks = header.cardinality / BLOCK_SIZE + (tail_size != 0);

    /// The Index Section follows the segment payload (see serializeTo):
    /// [number of blocks][last row id per block...][relative offset per block...], all VarUInt.
    UInt64 num_blocks_in_index = 0;
    readVarUInt(num_blocks_in_index, in);

    if (num_blocks_in_index != num_blocks)
    {
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "Corrupted data: expected {} blocks in the index section of a segment with {} postings, but got {}",
            num_blocks, header.cardinality, num_blocks_in_index);
    }

    std::vector<uint32_t> block_last_row_ids(num_blocks);
    for (size_t i = 0; i < num_blocks; ++i)
    {
        UInt64 v = 0;
        readVarUInt(v, in);
        block_last_row_ids[i] = static_cast<uint32_t>(v);

        if (i > 0 && block_last_row_ids[i] <= block_last_row_ids[i - 1])
        {
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "Corrupted data: block last row ids are not strictly monotonic at block {}: previous = {}, current = {}",
                i, block_last_row_ids[i - 1], block_last_row_ids[i]);
        }
    }

    std::vector<uint64_t> block_offsets(num_blocks);
    for (size_t i = 0; i < num_blocks; ++i)
    {
        UInt64 v = 0;
        readVarUInt(v, in);
        block_offsets[i] = v;

        if (block_offsets[i] >= payload.size() || (i > 0 && block_offsets[i] <= block_offsets[i - 1]))
        {
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "Corrupted data: invalid block offset {} at block {} (payload size {})",
                block_offsets[i], i, payload.size());
        }
    }

    const uint32_t clip_begin = static_cast<uint32_t>(std::min<size_t>(clip_range.begin, std::numeric_limits<uint32_t>::max()));
    const uint32_t clip_end = static_cast<uint32_t>(std::min<size_t>(clip_range.end, std::numeric_limits<uint32_t>::max()));

    /// The first block that may contain a row id within the clip range.
    const size_t first_block = std::lower_bound(block_last_row_ids.begin(), block_last_row_ids.end(), clip_begin) - block_last_row_ids.begin();
    size_t decoded_blocks = 0;

    if (first_block != num_blocks)
    {
        prev_row_id = first_block == 0 ? header.first_row_id : block_last_row_ids[first_block - 1];
        payload = payload.subspan(block_offsets[first_block]);

        /// Blocks intersecting the clip range form a contiguous run, decode them sequentially.
        for (size_t i = first_block; i < num_blocks; ++i)
        {
            /// The lowest possible first row id of the block.
            const uint32_t block_begin = i == 0 ? header.first_row_id : block_last_row_ids[i - 1] + 1;
            if (block_begin > clip_end)
                break;

            const size_t count = (tail_size != 0 && i == num_blocks - 1) ? tail_size : BLOCK_SIZE;
            decodeBlock(payload, count, prev_row_id, current_segment);
            ++decoded_blocks;

            /// Trim row ids outside the clip range from the boundary blocks.
            const uint32_t * decoded_begin = current_segment.data();
            const uint32_t * decoded_end = decoded_begin + current_segment.size();

            if (i == first_block)
                decoded_begin = std::lower_bound(decoded_begin, decoded_end, clip_begin);
            if (block_last_row_ids[i] > clip_end)
                decoded_end = std::upper_bound(decoded_begin, decoded_end, clip_end);

            postings.addMany(decoded_end - decoded_begin, decoded_begin);
        }
    }

    ProfileEvents::increment(ProfileEvents::TextIndexPostingsBlocksSkipped, num_blocks - decoded_blocks);
}

void PostingListCodecBitpackingImpl::serializeTo(WriteBuffer & out, TokenPostingsInfo & info) const
{
    info.offsets.reserve(segment_descriptors.size());
    info.ranges.reserve(segment_descriptors.size());

    for (size_t seg_idx = 0; seg_idx < segment_descriptors.size(); ++seg_idx)
    {
        const auto & descriptor = segment_descriptors[seg_idx];
        info.offsets.emplace_back(out.count());
        info.ranges.emplace_back(descriptor.row_id_begin, descriptor.row_id_end);

        Header header(descriptor.compressed_data_size, descriptor.cardinality, descriptor.row_id_begin);
        header.write(out);
        out.write(compressed_data.data() + descriptor.compressed_data_offset, descriptor.compressed_data_size);

        /// Index Section: append per-block metadata after segment payload.
        /// This allows PostingListCursor to binary-search for blocks without
        /// decoding the entire segment. The cursor reads header + payload + Index Section
        /// sequentially, so no offset storage is needed in the dictionary.
        const auto & block_metas = segment_block_metas[seg_idx].metas;
        writeVarUInt(block_metas.size(), out);

        for (const auto & meta : block_metas)
            writeVarUInt(meta.last_row_id, out);

        for (const auto & meta : block_metas)
            writeVarUInt(meta.relative_offset, out);
    }
}

namespace
{
void writeByte(uint8_t x, std::span<char> & out)
{
    out[0] = static_cast<char>(x);
    out = out.subspan(1);
}

uint8_t readByte(std::span<const std::byte> & in)
{
    auto v = static_cast<uint8_t>(in[0]);
    in = in.subspan(1);
    return v;
}
}

void PostingListCodecBitpackingImpl::encodeBlock(std::span<uint32_t> segment)
{
    auto & segment_descriptor = segment_descriptors.back();
    segment_descriptor.cardinality += segment.size();
    segment_descriptor.row_id_end = prev_row_id;

    /// Record packed block metadata for V2 Index Section.
    /// relative_offset is relative to the segment's compressed_data_offset.
    auto & block_metas = segment_block_metas.back().metas;
    block_metas.push_back(
    {
        prev_row_id,
        compressed_data.size() - segment_descriptor.compressed_data_offset
    });

    auto [needed_bytes_without_header, max_bits] = BitpackingBlockCodec::calculateNeededBytesAndMaxBits(segment);
    size_t remaining_memory = compressed_data.capacity() - compressed_data.size();
    size_t needed_bytes_with_header = needed_bytes_without_header + 1;
    if (remaining_memory < needed_bytes_with_header)
    {
        size_t min_need = needed_bytes_with_header - remaining_memory;
        compressed_data.reserve(compressed_data.size() + 2 * min_need);
    }
    /// Block Layout: [1byte(max_bits)][payload]
    size_t offset = compressed_data.size();
    compressed_data.resize(compressed_data.size() + needed_bytes_with_header);
    std::span<char> compressed_data_span(compressed_data.data() + offset, needed_bytes_with_header);
    writeByte(static_cast<uint8_t>(max_bits), compressed_data_span);
    auto used_memory = BitpackingBlockCodec::encode(segment, max_bits, compressed_data_span);

    if (used_memory != needed_bytes_without_header || !compressed_data_span.empty())
    {
        throw Exception(ErrorCodes::LOGICAL_ERROR,
        "Bitpacking encode size mismatch: expected {} bytes payload with {} bits encoding for {} integers, "
        "but actually used {} bytes with {} bytes remaining in buffer",
        needed_bytes_without_header,
        max_bits,
        segment.size(),
        used_memory,
        compressed_data_span.size());
    }

    segment_descriptor.compressed_data_size = compressed_data.size() - segment_descriptor.compressed_data_offset;
}

void PostingListCodecBitpackingImpl::decodeBlock(
        std::span<const std::byte> & in, size_t count, uint32_t & prev_row_id, std::vector<uint32_t> & current_segment)
{
    chassert(count <= BLOCK_SIZE);
    if (in.empty())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "Corrupted data: expected at least {} bytes, but got {}", 1, in.size());

    uint8_t bits = readByte(in);
    if (bits > 32)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "Corrupted data: expected bits <= 32, but got {}", bits);
    current_segment.resize(count);

    size_t required_size = BitpackingBlockCodec::bitpackingCompressedBytes(count, bits);
    if (in.size() < required_size)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "Corrupted data: expected data size {}, but got {}", required_size, in.size());

    /// Decode postings to buffer named temp.
    std::span<uint32_t> current_span(current_segment.data(), current_segment.size());
    size_t consumed_size = BitpackingBlockCodec::decode(in, count, bits, current_span);
    if (required_size != consumed_size)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
        "Bitpacking decode size mismatch: expected to consume {} bytes for {} integers with {} bits, but actually consumed {} bytes",
        required_size,
        count,
        bits,
        consumed_size);

    /// Restore the original array from the decompressed delta values.
    std::inclusive_scan(current_segment.begin(), current_segment.end(), current_segment.begin(), std::plus<uint32_t>{}, prev_row_id);
    prev_row_id = current_segment.empty() ? prev_row_id : current_segment.back();
}

void PostingListCodecBitpacking::decode(ReadBuffer & in, PostingList & postings, const RowsRange * clip_range) const
{
    PostingListCodecBitpackingImpl impl;
    impl.decode(in, postings, clip_range);
}

void PostingListCodecBitpacking::encode(
        const PostingList & postings, size_t max_rowids_in_segment, TokenPostingsInfo & info, WriteBuffer & out) const
{
    PostingListCodecBitpackingImpl impl(max_rowids_in_segment);
    std::vector<uint32_t> rowids;
    rowids.resize(postings.cardinality());
    postings.toUint32Array(rowids.data());

    std::span<uint32_t> rowids_view(rowids.data(), rowids.size());
    while (rowids_view.size() >= BLOCK_SIZE)
    {
        auto front = rowids_view.first(BLOCK_SIZE);
        impl.insert(front);
        rowids_view = rowids_view.subspan(BLOCK_SIZE);
    }

    if (!rowids_view.empty())
    {
        for (auto rowid: rowids_view)
            impl.insert(rowid);
    }
    impl.encode(out, info);
}
}

