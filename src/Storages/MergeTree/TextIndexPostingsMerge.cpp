#include <Storages/MergeTree/TextIndexPostingsMerge.h>

#include <IO/ReadHelpers.h>
#include <Storages/MergeTree/MergeTreeIndexTextPostingListCodec.h>
#include <Storages/MergeTree/MergeTreeReaderStream.h>
#include <Storages/MergeTree/MergedPartOffsets.h>

#include <array>
#include <limits>

namespace DB
{

namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int LOGICAL_ERROR;
    extern const int SUPPORT_IS_DISABLED;
}

ReadBuffer & PostingsReaderStreamAdapter::seekTo(UInt64 offset)
{
    stream.seekToMark({offset, 0});
    return *stream.getDataBuffer();
}

TokenPostingsMergeCursor::TokenPostingsMergeCursor(
    IPostingsReadStream & stream_,
    const TokenPostingsInfo & info_,
    IPostingListCodec::Type source_codec_type,
    MergeTreeIndexVersion source_version,
    const MergedPartOffsets * merged_part_offsets_,
    size_t part_index_)
    : stream(stream_)
    , info(info_)
    , merged_part_offsets(merged_part_offsets_)
    , part_index(part_index_)
{
    using enum PostingsSerialization::Flags;

    if (info.header & EmbeddedPostings)
    {
        if (info.embedded_postings.empty())
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Posting list header marks embedded postings but they are not deserialized");

        mode = Mode::Embedded;
    }
    else if (info.header & IsCompressed)
    {
        auto codec_type = source_codec_type;
        static constexpr auto required_version = static_cast<MergeTreeIndexVersion>(TextIndexHeader::Version::WithCodec);

        /// Pre-WithCodec parts don't persist the codec type, but Bitpacking was the only
        /// compression codec at the time, so an IsCompressed posting list must be Bitpacking.
        if (source_version < required_version && codec_type == IPostingListCodec::Type::None)
            codec_type = IPostingListCodec::Type::Bitpacking;

        if (codec_type == IPostingListCodec::Type::None)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "Posting list header marks compressed data but configured codec is None");

        /// Bitpacking is the only compression codec; readSegmentHeader validates the per-segment codec tag.
        mode = Mode::BitpackedSegments;
    }
    else if (info.header & RawPostings)
    {
        if (info.offsets.size() != 1)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "Raw posting list must have exactly one block, got {}", info.offsets.size());

        mode = Mode::Raw;
    }
    else
    {
        mode = Mode::RoaringBlocks;
    }

    batch_buffer.reserve(BLOCK_SIZE);
    nextBatch();
}

void TokenPostingsMergeCursor::nextBatch()
{
    current_batch = {};

    switch (mode)
    {
        case Mode::Embedded:
            fillEmbedded();
            break;
        case Mode::Raw:
            fillRaw();
            break;
        case Mode::BitpackedSegments:
            fillBitpackedSegments();
            break;
        case Mode::RoaringBlocks:
            fillRoaringBlocks();
            break;
    }

    if (!current_batch.empty())
        remapBatch();
}

void TokenPostingsMergeCursor::fillEmbedded()
{
    if (started)
        return;

    started = true;
    batch_buffer.assign(info.embedded_postings.begin(), info.embedded_postings.end());
    current_batch = std::span<const UInt32>(batch_buffer.data(), batch_buffer.size());
}

void TokenPostingsMergeCursor::fillRaw()
{
    if (!started)
    {
        started = true;
        raw_values_left = info.cardinality;
        raw_data = &stream.seekTo(info.offsets[0]);
    }

    if (raw_values_left == 0)
        return;

    size_t count = std::min<size_t>(raw_values_left, BLOCK_SIZE);
    batch_buffer.resize(count);

    for (size_t i = 0; i < count; ++i)
        readVarUInt(batch_buffer[i], *raw_data);

    raw_values_left -= count;
    current_batch = std::span<const UInt32>(batch_buffer.data(), count);
}

void TokenPostingsMergeCursor::fillBitpackedSegments()
{
    while (segment_values_left == 0)
    {
        if (next_offset_idx == info.offsets.size())
            return;

        auto & data = stream.seekTo(info.offsets[next_offset_idx]);
        ++next_offset_idx;

        auto header = PostingListCodecBitpackingImpl::readSegmentHeader(data);

        /// Bulk-read the whole segment payload; packed blocks are then decoded from memory.
        /// The trailing V2 index section (if any) is never read: each segment is reached by
        /// an explicit seek, so V1 and V2 sources are handled identically.
        payload.resize(header.payload_bytes);
        data.readStrict(reinterpret_cast<char *>(payload.data()), header.payload_bytes);
        payload_span = std::span<const std::byte>(payload.data(), payload.size());

        prev_row_id = header.first_row_id;
        segment_values_left = header.cardinality;
    }

    size_t count = std::min<size_t>(segment_values_left, BLOCK_SIZE);
    PostingListCodecBitpackingImpl::decodeBlock(payload_span, count, prev_row_id, batch_buffer);
    segment_values_left -= count;
    current_batch = std::span<const UInt32>(batch_buffer.data(), batch_buffer.size());
}

void TokenPostingsMergeCursor::fillRoaringBlocks()
{
    while (true)
    {
        if (!roaring_block_active)
        {
            if (next_offset_idx == info.offsets.size())
                return;

            auto & data = stream.seekTo(info.offsets[next_offset_idx]);
            ++next_offset_idx;

            size_t num_bytes = 0;
            readVarUInt(num_bytes, data);
            roaring_buffer.resize(num_bytes);
            data.readStrict(roaring_buffer.data(), num_bytes);

            current_roaring = PostingList::read(roaring_buffer.data());
            roaring::api::roaring_iterator_init(&current_roaring.roaring, &roaring_iterator);
            roaring_block_active = true;
        }

        batch_buffer.resize(BLOCK_SIZE);
        uint32_t count = roaring::api::roaring_uint32_iterator_read(&roaring_iterator, batch_buffer.data(), BLOCK_SIZE);

        if (count == 0)
        {
            roaring_block_active = false;
            continue;
        }

        batch_buffer.resize(count);
        current_batch = std::span<const UInt32>(batch_buffer.data(), count);
        return;
    }
}

void remapPostingRowIds(std::span<UInt32> values, const MergedPartOffsets * merged_part_offsets, size_t part_index)
{
    if (!merged_part_offsets)
        return;

    for (auto & value : values)
    {
        UInt64 new_offset = (*merged_part_offsets)[part_index, value];

        if (new_offset > std::numeric_limits<UInt32>::max())
        {
            throw Exception(ErrorCodes::SUPPORT_IS_DISABLED,
                "Cannot merge text index: remapped row id {} exceeds the maximum supported row id {}",
                new_offset, std::numeric_limits<UInt32>::max());
        }

        value = static_cast<UInt32>(new_offset);
    }
}

void TokenPostingsMergeCursor::remapBatch()
{
    remapPostingRowIds(std::span<UInt32>(batch_buffer.data(), current_batch.size()), merged_part_offsets, part_index);
}

void mergeTokenPostings(std::span<TokenPostingsMergeCursor * const> cursors, const std::function<void(std::span<UInt32>)> & consume)
{
    /// Head of one source stream: the cursor and the position of its current value in the batch.
    struct Head
    {
        TokenPostingsMergeCursor * cursor;
        size_t pos;
    };

    std::vector<Head> heads;
    heads.reserve(cursors.size());

    for (auto * cursor : cursors)
    {
        if (cursor->valid())
            heads.push_back({cursor, 0});
    }

    std::array<UInt32, BLOCK_SIZE> staging;
    size_t staged = 0;
    /// Last emitted value; -1 means nothing was emitted yet.
    Int64 prev = -1;

    auto flush_staging = [&]
    {
        consume(std::span<UInt32>(staging.data(), staged));
        staged = 0;
    };

    auto emit = [&](UInt32 value)
    {
        if (static_cast<Int64>(value) <= prev)
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "Merged text index posting lists are not strictly increasing: row id {} goes after {}", value, prev);

        prev = static_cast<Int64>(value);
        staging[staged] = value;

        if (++staged == BLOCK_SIZE)
            flush_staging();
    };

    while (!heads.empty())
    {
        /// Linear min-scan: the head with the minimum current value leads;
        /// also track the minimum among the other heads for the fast path.
        size_t leader = 0;
        UInt32 leader_value = heads[0].cursor->batch()[heads[0].pos];
        UInt64 min_others = std::numeric_limits<UInt64>::max();

        for (size_t i = 1; i < heads.size(); ++i)
        {
            UInt32 value = heads[i].cursor->batch()[heads[i].pos];
            if (value < leader_value)
            {
                min_others = std::min(min_others, static_cast<UInt64>(leader_value));
                leader = i;
                leader_value = value;
            }
            else
            {
                min_others = std::min(min_others, static_cast<UInt64>(value));
            }
        }

        auto & head = heads[leader];
        auto batch = head.cursor->batch();

        if (static_cast<UInt64>(batch.back()) < min_others)
        {
            /// Concatenation fast path: the whole remaining batch of the leader precedes
            /// every other stream, so it can be staged without further comparisons.
            for (size_t i = head.pos; i < batch.size(); ++i)
                emit(batch[i]);
            head.pos = batch.size();
        }
        else
        {
            emit(leader_value);
            ++head.pos;
        }

        if (head.pos == batch.size())
        {
            head.cursor->nextBatch();
            head.pos = 0;
            if (!head.cursor->valid())
                heads.erase(heads.begin() + leader);
        }
    }

    if (staged > 0)
        flush_staging();
}

}
