#include <Storages/MergeTree/TextIndexPostingsApplier.h>
#include <Storages/MergeTree/BitpackingBlockCodec.h>
#include <Storages/MergeTree/PostingListBlockCodec.h>
#include <Common/ProfileEvents.h>

#include <algorithm>

namespace ProfileEvents
{
    extern const Event TextIndexAnalyzePostingsBlocksDecoded;
    extern const Event TextIndexAnalyzePostingsBlocksSkipped;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}

namespace
{

/// The first readable range that ends at or after `value`, starting the search from `from`.
size_t findReadableRange(const std::vector<RowsRange> & ranges, size_t from, size_t value)
{
    auto it = std::lower_bound(
        ranges.begin() + from, ranges.end(), value,
        [](const RowsRange & range, size_t v) { return range.end < v; });

    return static_cast<size_t>(it - ranges.begin());
}

/// True if the sorted array `rows` has a value in the closed range [begin, end], searching from `from`.
bool hasRowInRange(const PaddedPODArray<UInt32> & rows, size_t from, size_t begin, size_t end)
{
    auto it = std::lower_bound(rows.begin() + from, rows.end(), begin);
    return it != rows.end() && *it <= end;
}

}

bool PostingsApplyTargets::needRange(size_t begin, size_t end) const
{
    bool has_readable_rows_targets = !unite.empty()
        || std::ranges::any_of(intersect, [](const auto & target) { return !target.initialized; });

    if (has_readable_rows_targets)
    {
        if (!readable_ranges)
            return true;

        size_t range_idx = findReadableRange(*readable_ranges, 0, begin);
        if (range_idx < readable_ranges->size() && (*readable_ranges)[range_idx].begin <= end)
            return true;
    }

    for (const auto & target : intersect)
    {
        if (target.initialized && hasRowInRange(*target.rows, 0, begin, end))
            return true;
    }

    return false;
}

PostingsApplier::PostingsApplier(PostingsApplyTargets & targets_)
    : targets(targets_)
    , intersect_states(targets.intersect.size())
{
    has_readable_rows_targets = !targets.unite.empty()
        || std::ranges::any_of(targets.intersect, [](const auto & target) { return !target.initialized; });
}

bool PostingsApplier::needBlock(UInt32 begin, UInt32 end)
{
    if (has_readable_rows_targets)
    {
        if (!targets.readable_ranges)
            return true;

        /// Blocks ascend, so the readable ranges that end before the block are never needed again.
        const auto & ranges = *targets.readable_ranges;
        readable_range_idx = findReadableRange(ranges, readable_range_idx, begin);

        if (readable_range_idx < ranges.size() && ranges[readable_range_idx].begin <= end)
            return true;
    }

    for (size_t i = 0; i < targets.intersect.size(); ++i)
    {
        const auto & target = targets.intersect[i];
        if (target.initialized && hasRowInRange(*target.rows, intersect_states[i].read_pos, begin, end))
            return true;
    }

    return false;
}

bool PostingsApplier::exhausted() const
{
    if (has_readable_rows_targets)
    {
        if (!targets.readable_ranges || readable_range_idx < targets.readable_ranges->size())
            return false;
    }

    for (size_t i = 0; i < targets.intersect.size(); ++i)
    {
        const auto & target = targets.intersect[i];
        if (target.initialized && intersect_states[i].read_pos < target.rows->size())
            return false;
    }

    return true;
}

void PostingsApplier::applySegment(const PostingListSegment & segment, IPostingListBlockCodec & block_codec)
{
    chassert(block_codec.type() == segment.codec_type);
    decode_buffer.resize(BLOCK_SIZE);

    /// A dense token appends thousands of blocks to a fresh intersection; grow it once instead of doubling along the way.
    for (const auto & target : targets.intersect)
    {
        if (!target.initialized)
            target.rows->reserve(target.rows->size() + segment.doc_count);
    }

    for (size_t block_idx = 0; block_idx < segment.block_count; ++block_idx)
    {
        if (exhausted())
        {
            num_blocks_skipped += segment.block_count - block_idx;
            break;
        }

        UInt32 block_first_row_id = segment.getBlockFirstRowId(block_idx);
        UInt32 block_last_row_id = segment.getBlockLastRowId(block_idx);

        if (!needBlock(block_first_row_id, block_last_row_id))
        {
            ++num_blocks_skipped;
            continue;
        }

        const size_t count = segment.getBlockSize(block_idx);
        std::span<const std::byte> block_data = segment.getBlockData(block_idx);

        /// The block stores gaps from the last row id of the previous block (the first row id of the segment for the first block).
        const UInt32 base_row_id = block_idx == 0 ? segment.first_row_id : segment.block_last_row_ids[block_idx - 1];

        /// The block span comes from the Index Section offsets and must be consumed in full (`decodeBlock` advances it).
        const size_t expected_bytes = block_data.size();
        const size_t consumed_bytes = block_codec.decodeBlock(block_data, count, std::span(decode_buffer.data(), count), base_row_id);

        if (consumed_bytes != expected_bytes)
        {
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "Corrupted data in posting list segment: block {} consumed {} bytes but its Index Section span is {} bytes",
                block_idx, consumed_bytes, expected_bytes);
        }

        ++num_blocks_decoded;
        applyBlock(decode_buffer.data(), count);
    }
}

void PostingsApplier::applyRows(std::span<const UInt32> sorted_rows)
{
    if (!sorted_rows.empty())
        applyBlock(sorted_rows.data(), sorted_rows.size());
}

void PostingsApplier::applyBitmap(const PostingList & postings)
{
    /// Nothing to clip and nothing to intersect: fold the bitmap as a whole.
    if (targets.intersect.empty() && !targets.readable_ranges)
    {
        size_t cardinality = postings.cardinality();
        for (auto & target : targets.unite)
        {
            *target.postings |= postings;
            target.num_applied += cardinality;
        }
        return;
    }

    PaddedPODArray<UInt32> rows(postings.cardinality());
    postings.toUint32Array(rows.data());
    applyRows(rows);
}

size_t PostingsApplier::clipToReadableRanges(const UInt32 * values, size_t count)
{
    const auto & ranges = *targets.readable_ranges;

    if (clip_buffer.size() < count)
        clip_buffer.resize(count);

    size_t num_kept = 0;
    for (size_t i = 0; i < count; ++i)
    {
        UInt32 value = values[i];

        while (readable_range_idx < ranges.size() && ranges[readable_range_idx].end < value)
            ++readable_range_idx;

        if (readable_range_idx == ranges.size())
            break;

        if (value >= ranges[readable_range_idx].begin)
            clip_buffer[num_kept++] = value;
    }

    return num_kept;
}

void PostingsApplier::applyBlock(const UInt32 * values, size_t count)
{
    if (has_readable_rows_targets)
    {
        const UInt32 * readable_values = values;
        size_t readable_count = count;

        if (targets.readable_ranges)
        {
            readable_count = clipToReadableRanges(values, count);
            readable_values = clip_buffer.data();
        }

        if (readable_count > 0)
        {
            if (!targets.unite.empty())
            {
                union_postings.addMany(readable_count, readable_values);
                num_union_applied += readable_count;
            }

            for (auto & target : targets.intersect)
            {
                if (!target.initialized)
                    target.rows->insert(readable_values, readable_values + readable_count);
            }
        }
    }

    /// The rows of an initialized intersect target are inside the readable ranges already, so the block is
    /// merged as a whole: the rows the block contains are compacted to the front, the rows it does not are dropped.
    for (size_t i = 0; i < targets.intersect.size(); ++i)
    {
        const auto & target = targets.intersect[i];
        if (!target.initialized)
            continue;

        auto & rows = *target.rows;
        auto & state = intersect_states[i];
        const size_t size = rows.size();

        size_t read_pos = state.read_pos;
        size_t write_pos = state.write_pos;
        size_t value_pos = 0;

        while (read_pos < size && value_pos < count)
        {
            UInt32 row = rows[read_pos];
            UInt32 value = values[value_pos];

            if (row < value)
            {
                ++read_pos;
            }
            else if (row > value)
            {
                ++value_pos;
            }
            else
            {
                rows[write_pos++] = row;
                ++read_pos;
                ++value_pos;
            }
        }

        state.read_pos = read_pos;
        state.write_pos = write_pos;
    }
}

void PostingsApplier::finish()
{
    for (size_t i = 0; i < targets.intersect.size(); ++i)
    {
        auto & target = targets.intersect[i];

        /// The rows after the last applied block are not in the posting list either.
        if (target.initialized)
            target.rows->resize(intersect_states[i].write_pos);

        target.num_applied = target.rows->size();
    }

    for (size_t i = 0; i < targets.unite.size(); ++i)
    {
        auto & target = targets.unite[i];
        target.num_applied += num_union_applied;

        if (num_union_applied == 0)
            continue;

        /// The last target takes the bitmap over when it has nothing folded yet.
        if (i + 1 == targets.unite.size() && target.postings->isEmpty())
            *target.postings = std::move(union_postings);
        else
            *target.postings |= union_postings;
    }

    if (num_blocks_decoded)
        ProfileEvents::increment(ProfileEvents::TextIndexAnalyzePostingsBlocksDecoded, num_blocks_decoded);
    if (num_blocks_skipped)
        ProfileEvents::increment(ProfileEvents::TextIndexAnalyzePostingsBlocksSkipped, num_blocks_skipped);
}

}
