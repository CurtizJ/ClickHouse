#include <Storages/MergeTree/PostingListSegment.h>
#include <Storages/MergeTree/MergeTreeIndexText.h>
#include <Storages/MergeTree/MergeTreeReaderStream.h>
#include <Storages/MergeTree/BitpackingBlockCodec.h>
#include <Storages/MergeTree/PostingListBlockCodec.h>
#include <Storages/MergeTree/TextIndexCache.h>
#include <Formats/MarkInCompressedFile.h>
#include <IO/ReadHelpers.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}

namespace
{

/// Narrow an on-disk UInt64 field to UInt32, throwing CORRUPTED_DATA if the value exceeds the representable range.
UInt32 requireUInt32(UInt64 value, std::string_view field_name)
{
    if (value > std::numeric_limits<UInt32>::max())
    {
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "Corrupted data in posting list segment: {} value {} exceeds UInt32 max",
            field_name, value);
    }

    return static_cast<UInt32>(value);
}

}

UInt32 PostingListSegment::getBlockFirstRowId(size_t block_idx) const
{
    chassert(block_idx < block_count);

    if (block_idx == 0)
        return first_row_id;

    UInt32 prev_last_row_id = block_last_row_ids[block_idx - 1];
    if (prev_last_row_id == std::numeric_limits<UInt32>::max())
    {
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "Corrupted data in posting list segment: previous block_last_row_id is UInt32::max "
            "at block {}, computing the first row id of the block would overflow", block_idx);
    }

    return prev_last_row_id + 1;
}

size_t PostingListSegment::getBlockSize(size_t block_idx) const
{
    chassert(block_idx < block_count);

    if (block_idx + 1 == block_count && tail_size > 0)
        return tail_size;

    return BLOCK_SIZE;
}

std::span<const std::byte> PostingListSegment::getBlockData(size_t block_idx) const
{
    chassert(block_idx < block_count);

    size_t payload_offset = static_cast<size_t>(block_offsets[block_idx]);
    if (payload_offset >= payload_buffer.size())
    {
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "Corrupted data: block offset {} is out of payload bounds {}",
            payload_offset, payload_buffer.size());
    }

    size_t next_offset = (block_idx + 1 < block_count)
        ? static_cast<size_t>(block_offsets[block_idx + 1])
        : payload_buffer.size();

    std::span<const std::byte> block_data(
        reinterpret_cast<const std::byte *>(payload_buffer.data() + payload_offset),
        next_offset - payload_offset);

    if (block_data.empty())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "Corrupted data: empty block at index {}", block_idx);

    return block_data;
}

PostingListSegment readPostingListSegment(MergeTreeReaderStream & stream, const TokenPostingsInfo & info, size_t segment_idx)
{
    chassert(segment_idx < info.offsets.size());
    PostingListSegment segment;

    UInt64 segment_file_offset = info.offsets[segment_idx];

    /// Seek to segment start and read the header.
    stream.seekToMark({segment_file_offset, 0});
    auto * data_buffer = stream.getDataBuffer();

    UInt64 codec_type = 0;
    readVarUInt(codec_type, *data_buffer);

    if (!isValidPostingListBlockCodecType(codec_type))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "Corrupted data in posting list segment: unknown posting list block codec type {}", codec_type);

    segment.codec_type = static_cast<IPostingListCodec::Type>(codec_type);

    UInt64 payload_bytes = 0;
    readVarUInt(payload_bytes, *data_buffer);
    UInt64 seg_cardinality = 0;
    readVarUInt(seg_cardinality, *data_buffer);
    UInt64 first_row_id = 0;
    readVarUInt(first_row_id, *data_buffer);

    segment.doc_count = requireUInt32(seg_cardinality, "seg_cardinality");
    segment.first_row_id = requireUInt32(first_row_id, "first_row_id");

    const auto & segment_range = info.ranges[segment_idx];

    if (segment_range.begin > segment_range.end)
    {
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "Corrupted data in posting list segment: segment row range has begin {} > end {} for segment {}",
            segment_range.begin, segment_range.end, segment_idx);
    }

    const UInt64 range_span = static_cast<UInt64>(segment_range.end) - static_cast<UInt64>(segment_range.begin) + 1;

    if (segment.doc_count > range_span)
    {
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "Corrupted data in posting list segment: segment cardinality {} exceeds segment row range span {} for segment [{}, {}]",
            segment.doc_count, range_span, segment_range.begin, segment_range.end);
    }

    /// The per-block codec owns the codec-specific per-block worst-case size, so `payload_bytes`
    /// can be bounded against corrupted metadata without naming a concrete codec.
    auto block_codec = createPostingListBlockCodec(segment.codec_type);

    /// Cap `payload_bytes` before resizing so corrupted metadata can't force a huge allocation.
    const UInt64 max_blocks_count = (static_cast<UInt64>(segment.doc_count) + BLOCK_SIZE - 1) / BLOCK_SIZE;
    const UInt64 per_block_cap = block_codec->maxBlockBytes();
    const UInt64 max_payload_bytes = max_blocks_count * per_block_cap;

    if (payload_bytes > max_payload_bytes)
    {
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "Corrupted data in posting list segment: payload_bytes {} exceeds upper bound {} "
            "for segment with {} documents",
            payload_bytes, max_payload_bytes, segment.doc_count);
    }

    /// Bulk-read the entire payload into memory.
    segment.payload_buffer.resize(payload_bytes);
    data_buffer->readStrict(reinterpret_cast<char *>(segment.payload_buffer.data()), payload_bytes);

    if (!(info.header & PostingsSerialization::Flags::HasBlockIndex))
    {
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "Corrupted data in posting list segment: per-segment block index is missing "
            "(HasBlockIndex flag not set in posting list header)");
    }

    /// Index Section follows immediately after the payload in the .pst stream.
    UInt64 num_blocks = 0;
    readVarUInt(num_blocks, *data_buffer);

    if (num_blocks == 0)
    {
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "Posting list number of blocks is 0 for segment with {} documents",
            segment.doc_count);
    }

    /// Full blocks of BLOCK_SIZE row ids followed by at most one shorter tail block.
    if (num_blocks != max_blocks_count)
    {
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "Corrupted data in posting list segment: number of blocks {} does not match {} expected for segment with {} documents",
            num_blocks, max_blocks_count, segment.doc_count);
    }

    segment.block_last_row_ids.resize(num_blocks);
    segment.block_offsets.resize(num_blocks);

    for (size_t i = 0; i < num_blocks; ++i)
    {
        UInt64 v = 0;
        readVarUInt(v, *data_buffer);
        segment.block_last_row_ids[i] = requireUInt32(v, "block_last_row_id");

        if (i > 0 && segment.block_last_row_ids[i] <= segment.block_last_row_ids[i - 1])
        {
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "Corrupted data in posting list segment: block_last_row_ids not strictly "
                "monotonic at block {}: previous = {}, current = {}",
                i, segment.block_last_row_ids[i - 1], segment.block_last_row_ids[i]);
        }
    }

    for (size_t i = 0; i < num_blocks; ++i)
    {
        UInt64 v = 0;
        readVarUInt(v, *data_buffer);
        segment.block_offsets[i] = v;

        if (segment.block_offsets[i] >= payload_bytes)
        {
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "Corrupted data in posting list segment: block_offsets[{}] = {} is outside payload of {} bytes",
                i, segment.block_offsets[i], payload_bytes);
        }

        if (i > 0 && segment.block_offsets[i] <= segment.block_offsets[i - 1])
        {
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "Corrupted data in posting list segment: block_offsets not strictly monotonic at block {}: previous = {}, current = {}",
                i, segment.block_offsets[i - 1], segment.block_offsets[i]);
        }
    }

    segment.block_count = num_blocks;
    segment.tail_size = segment.doc_count % BLOCK_SIZE;
    return segment;
}

PostingListSegmentPtr getPostingListSegment(
    MergeTreeReaderStream & stream,
    const TokenPostingsInfo & info,
    size_t segment_idx,
    TextIndexPostingsCache * postings_cache,
    const String & index_id_for_cache,
    ProfileEvents::Event read_event)
{
    auto read_segment = [&]
    {
        ProfileEvents::increment(read_event);
        return std::make_shared<PostingListSegment>(readPostingListSegment(stream, info, segment_idx));
    };

    if (!postings_cache)
        return read_segment();

    UInt64 segment_file_offset = info.offsets[segment_idx];
    auto key = TextIndexPostingsCache::hash(index_id_for_cache, segment_file_offset, static_cast<UInt8>(TextIndexPostingsCacheKind::Segment));

    auto cell = postings_cache->getOrSet(key, [&]
    {
        return std::make_shared<TextIndexPostingsCacheCell>(read_segment());
    });

    return std::get<PostingListSegmentPtr>(cell->value);
}

}
