#pragma once
#include <base/types.h>
#include <Common/PODArray.h>
#include <Common/ProfileEvents.h>
#include <Storages/MergeTree/IPostingListCodec.h>
#include <cstdint>
#include <memory>

namespace DB
{

class IPostingListBlockCodec;
class MergeTreeReaderStream;
class TextIndexPostingsCache;
struct TokenPostingsInfo;

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
    /// Element count of the tail block (< `IPostingListBlockCodec::BLOCK_SIZE`), 0 if the segment is block-aligned.
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

    /// Reads and parses the `segment_idx`-th segment of the posting list from `stream`:
    /// the header, the bulk-loaded payload and the Index Section, checked against the token metadata.
    static PostingListSegment read(MergeTreeReaderStream & stream, const TokenPostingsInfo & info, size_t segment_idx);

    /// The lowest row id the packed block may hold: a block starts after the last row id of the previous block.
    UInt32 getBlockFirstRowId(size_t block_idx) const { return block_idx == 0 ? first_row_id : block_last_row_ids[block_idx - 1] + 1; }

    /// Decodes the row ids of the packed block into `out`, which must have room for `IPostingListBlockCodec::BLOCK_SIZE` values.
    /// `block_codec` must have the type `codec_type`. Returns the number of decoded row ids.
    size_t decodeBlock(size_t block_idx, IPostingListBlockCodec & block_codec, uint32_t * out) const;
};

using PostingListSegmentPtr = std::shared_ptr<const PostingListSegment>;

/// Returns the `segment_idx`-th segment of the posting list, memoized in `postings_cache` (keyed by `index_id_for_cache`
/// and the segment offset) when the cache is set. `read_event` is incremented every time the segment is read from `stream`.
PostingListSegmentPtr getPostingListSegment(
    MergeTreeReaderStream & stream,
    const TokenPostingsInfo & info,
    size_t segment_idx,
    TextIndexPostingsCache * postings_cache,
    const String & index_id_for_cache,
    ProfileEvents::Event read_event);

}
