#pragma once

#include <Storages/MergeTree/IPostingListCodec.h>
#include <Common/PODArray_fwd.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace DB
{

/// Per-block payload codec for the segmented posting-list framework (see SegmentedPostingListCodec).
///
/// Encodes / decodes ONE block (1..BLOCK_SIZE row ids) including any codec-specific framing. Row ids are stored as
/// deltas from the previous row id; the codec computes and restores them, so it can fuse that with (un)packing.
/// The surrounding segment / Index Section layout is identical across codecs; only the per-block payload differs:
///   - Bitpacking: [1 byte bits][bitpacked deltas]
///   - PFor:  [PFor block of deltas]
class IPostingListBlockCodec
{
public:
    /// Number of row ids in a full block.
    static constexpr size_t BLOCK_SIZE = 128;

    virtual ~IPostingListBlockCodec() = default;

    /// Append one encoded block of `row_ids` (1..BLOCK_SIZE increasing values) to `out`, delta-coded starting
    /// from `prev_row_id`, the row id preceding the block. Returns the number of bytes appended.
    virtual size_t encodeBlock(std::span<const uint32_t> row_ids, uint32_t prev_row_id, PODArray<char> & out) = 0;

    /// Decode one block of `count` (1..BLOCK_SIZE) row ids from `in` into `out` (which must hold at least `count`
    /// slots), restoring them from deltas starting at `prev_row_id`, and advancing `in` past the consumed bytes.
    /// Returns the number of bytes consumed.
    virtual size_t decodeBlock(std::span<const std::byte> & in, size_t count, uint32_t prev_row_id, std::span<uint32_t> out) = 0;

    /// Upper bound on the encoded size of one block (1..BLOCK_SIZE delta values), in bytes.
    virtual size_t maxBlockBytes() const = 0;

    /// The codec type recorded in each segment header.
    virtual IPostingListCodec::Type type() const = 0;
};

/// Creates the per-block payload codec for `type`. Throws for `None` (it has no blocks).
std::unique_ptr<IPostingListBlockCodec> createPostingListBlockCodec(IPostingListCodec::Type type);

}
