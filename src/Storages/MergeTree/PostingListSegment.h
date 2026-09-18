#pragma once
#include <base/types.h>
#include <Common/PODArray.h>
#include <Common/ProfileEvents.h>
#include <Storages/MergeTree/IPostingListCodec.h>
#include <cstdint>
#include <memory>
#include <span>

namespace DB
{

struct TokenPostingsInfo;
class MergeTreeReaderStream;
class TextIndexPostingsCache;

/// Immutable, decoded metadata of one segment of a compressed (bitpacked) posting list.
/// Per-task cursors hold non-owning views, so it is parsed once and safe to share without synchronization.
struct PostingListSegment
{
    /// Bulk-loaded compressed payload of the segment: bytes [header_end, index_section_start).
    PaddedPODArray<uint8_t> payload_buffer;
    /// Per-packed-block index (parallel arrays), enabling O(log N) advance within the segment.
    /// Last row_id of packed block j
    PaddedPODArray<UInt32> block_last_row_ids;
    /// Byte offset of packed block j within payload_buffer
    PaddedPODArray<UInt64> block_offsets;

    /// Total doc count in this segment.
    UInt32 doc_count = 0;
    /// First row_id of the segment (delta base for the first block).
    UInt32 first_row_id = 0;
    /// Total packed blocks, including the (possibly shorter) tail block.
    size_t block_count = 0;
    /// Element count of the tail block (< BLOCK_SIZE), 0 if the segment is block-aligned.
    size_t tail_size = 0;
    /// Block codec used to compress this segment's packed blocks.
    IPostingListCodec::Type codec_type = IPostingListCodec::Type::Bitpacking;

    size_t bytesAllocated() const
    {
        return sizeof(*this)
            + payload_buffer.allocated_bytes()
            + block_last_row_ids.allocated_bytes()
            + block_offsets.allocated_bytes();
    }

    /// Closed row range of the packed block: the first row id of a block follows the last row id of the previous one.
    UInt32 getBlockFirstRowId(size_t block_idx) const;
    UInt32 getBlockLastRowId(size_t block_idx) const { return block_last_row_ids[block_idx]; }
    /// Number of row ids in the packed block: BLOCK_SIZE for all blocks but a shorter tail block.
    size_t getBlockSize(size_t block_idx) const;
    /// Compressed bytes of the packed block inside `payload_buffer`.
    std::span<const std::byte> getBlockData(size_t block_idx) const;
};

using PostingListSegmentPtr = std::shared_ptr<const PostingListSegment>;
/// A flattened, sorted array of posting list row ids.
using FlatPostingsPtr = std::shared_ptr<const PaddedPODArray<UInt32>>;

/// Reads and parses the `segment_idx`-th segment of a compressed posting list from `stream`: the segment
/// header, the bulk-loaded payload and the Index Section with the per-block last row ids and offsets.
/// The posting list must be written with a block index (`PostingsSerialization::Flags::HasBlockIndex`).
PostingListSegment readPostingListSegment(MergeTreeReaderStream & stream, const TokenPostingsInfo & info, size_t segment_idx);

/// The same, memoized in `postings_cache` (keyed by `index_id_for_cache` + segment byte offset) when the cache is set,
/// so a segment is parsed once and shared by every reader of the index. `read_event` is incremented on each read from the stream.
PostingListSegmentPtr getPostingListSegment(
    MergeTreeReaderStream & stream,
    const TokenPostingsInfo & info,
    size_t segment_idx,
    TextIndexPostingsCache * postings_cache,
    const String & index_id_for_cache,
    ProfileEvents::Event read_event);

}
