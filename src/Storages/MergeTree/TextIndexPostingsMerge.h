#pragma once

#include <Storages/MergeTree/BitpackingBlockCodec.h>
#include <Storages/MergeTree/MergeTreeIndexText.h>

#include <functional>
#include <span>

namespace DB
{

class ReadBuffer;
class MergeTreeReaderStream;
class MergedPartOffsets;

/// Minimal seekable view over one source's postings (.pst) data.
/// Decouples TokenPostingsMergeCursor from MergeTreeReaderStream so tests can drive it from a plain buffer.
class IPostingsReadStream
{
public:
    virtual ~IPostingsReadStream() = default;

    /// Positions the stream at `offset` (a value from TokenPostingsInfo::offsets)
    /// and returns a buffer to read the data from.
    virtual ReadBuffer & seekTo(UInt64 offset) = 0;
};

/// Adapter over MergeTreeReaderStream used by the text index merge task.
class PostingsReaderStreamAdapter final : public IPostingsReadStream
{
public:
    explicit PostingsReaderStreamAdapter(MergeTreeReaderStream & stream_) : stream(stream_) {}
    ReadBuffer & seekTo(UInt64 offset) override;

private:
    MergeTreeReaderStream & stream;
};

/// Sequential batch decoder over one source's posting list for one token.
/// Yields the source row ids in increasing order, remapped through MergedPartOffsets when provided,
/// in batches of at most BLOCK_SIZE values. Unlike the query-time PostingListCursor it doesn't
/// require the V2 per-segment index section, so it handles all source versions uniformly.
class TokenPostingsMergeCursor
{
public:
    /// `info` must outlive the cursor. A null `merged_part_offsets` means identity mapping.
    TokenPostingsMergeCursor(
        IPostingsReadStream & stream_,
        const TokenPostingsInfo & info_,
        IPostingListCodec::Type source_codec_type,
        MergeTreeIndexVersion source_version,
        const MergedPartOffsets * merged_part_offsets_,
        size_t part_index_);

    /// The cursor holds spans and a roaring iterator into its own members, so it must not move.
    TokenPostingsMergeCursor(const TokenPostingsMergeCursor &) = delete;
    TokenPostingsMergeCursor & operator=(const TokenPostingsMergeCursor &) = delete;

    bool valid() const { return !current_batch.empty(); }
    /// Remapped row ids, strictly increasing within the batch. Empty iff the cursor is exhausted.
    std::span<const UInt32> batch() const { return current_batch; }
    /// Decodes the next batch of source row ids and remaps it. Sets an empty batch when exhausted.
    void nextBatch();

private:
    enum class Mode
    {
        Embedded,          /// Postings embedded into the dictionary block (tiny roaring bitmap).
        Raw,               /// Raw VarUInt values at info.offsets[0].
        BitpackedSegments, /// Bitpacking codec segments, one per offset, decoded block by block.
        RoaringBlocks,     /// Serialized roaring bitmaps, one per offset (codec None sources).
    };

    void fillEmbedded();
    void fillRaw();
    void fillBitpackedSegments();
    void fillRoaringBlocks();
    void remapBatch();

    IPostingsReadStream & stream;
    const TokenPostingsInfo & info;
    const MergedPartOffsets * merged_part_offsets;
    const size_t part_index;
    Mode mode;

    std::vector<UInt32> batch_buffer;
    std::span<const UInt32> current_batch;

    /// Embedded mode is read in one shot; Raw mode seeks once and then reads sequentially.
    bool started = false;
    size_t raw_values_left = 0;
    ReadBuffer * raw_data = nullptr;

    /// BitpackedSegments: bulk-read payload of the current segment and its decode state.
    size_t next_offset_idx = 0;
    size_t segment_values_left = 0;
    UInt32 prev_row_id = 0;
    std::vector<std::byte> payload;
    std::span<const std::byte> payload_span;

    /// RoaringBlocks: current deserialized block and an iterator into it.
    PostingList current_roaring;
    roaring::api::roaring_uint32_iterator_t roaring_iterator{};
    bool roaring_block_active = false;
    std::vector<char> roaring_buffer;
};

/// K-way merges strictly-increasing cursors and feeds the merged sequence to `consume` through a
/// BLOCK_SIZE staging buffer: every block passed to `consume` is full except possibly the final one,
/// so a codec sink can use the block-aligned bulk insert. The consumer may mutate the block in place.
/// Throws a LOGICAL_ERROR exception if the merged sequence is not strictly increasing.
void mergeTokenPostings(std::span<TokenPostingsMergeCursor * const> cursors, const std::function<void(std::span<UInt32>)> & consume);

}
