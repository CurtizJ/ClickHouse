#pragma once

#include <Storages/MergeTree/MergeTreeIndices.h>
#include <Storages/MergeTree/IMergeTreeDataPart.h>
#include <Storages/MergeTree/MergeProjectionsIndexesTask.h>
#include <Storages/MergeTree/MergeTreeIndexText.h>
#include <Storages/MergeTree/TextIndexPositionData.h>
#include <Storages/MergeTree/MergedPartOffsets.h>
#include <Storages/MergeTree/TextIndexSegment.h>
#include <Core/SortCursor.h>
#include <Columns/ColumnString.h>
#include <Processors/ISimpleTransform.h>

#include <optional>

namespace DB
{

/// Transform that builds text indexes and periodically flushes their segments
/// into temporary storage, when amount of accumulated data reaches some threshold.
/// Used for materialization of text indexes.
class BuildTextIndexTransform final : public ISimpleTransform
{
public:
    BuildTextIndexTransform(
        SharedHeader header,
        String index_file_prefix_,
        std::vector<MergeTreeIndexPtr> indexes_,
        MutableDataPartStoragePtr temporary_storage_,
        MergeTreeWriterSettings writer_settings_,
        CompressionCodecPtr default_codec_,
        String marks_file_extension_,
        const MergeTreeSettings & storage_settings);

    String getName() const override { return "BuildTextIndexTransform"; }

    IProcessor::Status prepare() override;
    void transform(Chunk & chunk) override;

    void aggregate(const Block & block);
    void finalize();

    /// Returns all segments created by this transform for the given index and part.
    std::vector<TextIndexSegment> getSegments(const String & index_name, size_t part_idx) const;
    const std::vector<MergeTreeIndexPtr> & getIndexes() const { return indexes; }
    bool hasIndex(const String & index_name) const { return index_position_by_name.contains(index_name); }

private:
    /// Resets current index granule and flush a segment
    /// of the text index to the temporary storage.
    void writeTemporarySegment(size_t i);

    String index_file_prefix;
    std::vector<MergeTreeIndexPtr> indexes;
    std::unordered_map<String, size_t> index_position_by_name;
    MergeTreeIndexAggregators aggregators;
    MutableDataPartStoragePtr temporary_storage;
    MergeTreeWriterSettings writer_settings;
    CompressionCodecPtr default_codec;
    String marks_file_extension;

    /// Number of rows in blocks processed by the transform.
    size_t num_processed_rows = 0;
    /// Number of flushed segments for each index.
    std::vector<size_t> segment_numbers;
    /// Estimated memory retained by each index builder.
    std::vector<size_t> estimated_allocated_bytes;
    size_t max_processed_tokens;
    size_t max_allocated_bytes;
};

/// Task that merges text indexes from data parts,
/// or temporary segments of text indexes.
/// Task can recalcute row numbers in the source
/// posting to row numbers in the resulting part.
/// The mapping from old part offsets to the new part offsets is built
/// during the merge of data parts and can be optionally passed to this task.
/// Currently merges all segments in one stage
/// TODO: Implement multi-stage merge to reduce the memory usage.
class MergeTextIndexesTask : public MergeProjectionsIndexesTask
{
public:
    MergeTextIndexesTask(
        std::vector<TextIndexSegment> segments,
        MergeTreeMutableDataPartPtr new_data_part_,
        size_t num_rows_,
        MergeTreeIndexPtr index_ptr_,
        std::shared_ptr<MergedPartOffsets> merged_part_offsets_,
        const MergeTreeReaderSettings & reader_settings_,
        const MergeTreeWriterSettings & writer_settings_,
        bool sync_);

    ~MergeTextIndexesTask() noexcept override;

    bool executeStep() override;
    void cancel() noexcept override;

    MutableDataPartsVector extractTemporaryParts() override { return {}; }
    void addToChecksums(MergeTreeDataPartChecksums & checksums) override;

private:
    void finalize();
    void cancelImpl() noexcept;
    Block getHeader() const;
    void initializeQueue();

    /// Cursor over the single String sort column with statically dispatched comparisons.
    using TokenSortCursor = SpecializedSingleColumnSortCursor<ColumnString>;

    /// Returns true if the given cursor points to a new token.
    bool isNewToken(const TokenSortCursor & cursor) const;
    /// Reads the next dictionary block for the given source index.
    void readDictionaryBlock(size_t source_num);

    /// Streams the pending sources of the current token into the output posting list:
    /// k-way merges their row ids (remapped via merged_part_offsets) and encodes them
    /// either directly with the bitpacking codec or through the roaring fallback sink.
    void flushPostingList();
    /// Fast path of flushPostingList for tokens whose merged cardinality fits into raw postings:
    /// merges the values on the stack, without cursors or a roaring bitmap.
    /// Returns nullopt if a source uses a layout other than raw or embedded postings.
    std::optional<TokenPostingsInfo> tryMergeTinyTokenPostings(UInt64 total_cardinality);
    /// Common tail of flushPostingList: positions data, accumulation, and per-token state reset.
    void finalizeTokenInfo(TokenPostingsInfo token_info);
    void flushDictionaryBlock();

    std::vector<TextIndexSegment> segments;
    MergeTreeMutableDataPartPtr new_data_part;
    size_t num_rows;
    MergeTreeIndexPtr index_ptr;
    MergeTreeIndexTextParams params;

    /// If not null, posting list values must be recalculated using merged offsets.
    std::shared_ptr<MergedPartOffsets> merged_part_offsets;
    MergeTreeWriterSettings writer_settings;
    /// Whether to fsync the produced index files in `finalize` (merge/mutation `need_sync`).
    bool sync;
    size_t step_time_ms;

    std::vector<MergeTreeIndexInputStreams> input_streams;
    std::vector<std::unique_ptr<MergeTreeIndexReaderStream>> input_streams_holders;

    MergeTreeIndexOutputStreams output_streams;
    std::vector<std::unique_ptr<MergeTreeIndexWriterStream>> output_streams_holders;

    SortCursorImpls cursors;
    std::vector<DictionaryBlock> inputs;
    SortingQueue<TokenSortCursor> queue;

    /// One source's postings of the token that is currently being merged.
    /// The info is copied from the source dictionary block, because the block
    /// may be replaced by readDictionaryBlock before the token is flushed.
    struct PendingTokenPostings
    {
        size_t source_num;
        TokenPostingsInfo info;
    };

    /// Per-source postings format resolved from the source part's index header.
    struct SourcePostingsFormat
    {
        IPostingListCodec::Type codec_type;
        MergeTreeIndexVersion version;
    };

    /// Tokens accumulated for the current dictionary block.
    MutableColumnPtr output_tokens;
    /// Tokens infos accumulated for the current dictionary block.
    std::vector<TokenPostingsInfo> output_infos;
    /// Sources of the current token accumulated since the last flush.
    std::vector<PendingTokenPostings> pending_postings;
    /// Postings of the current token, used only by the roaring fallback sink of flushPostingList.
    PostingList output_postings;
    /// Positions accumulated for the current token (phrase query support).
    PODArray<RoaringishEntry> output_positions;
    /// Sparse index accumulated for the task. Flushed only once in the end of the task.
    MutableColumnPtr sparse_index_tokens;
    MutableColumnPtr sparse_index_offsets;

    /// Deserializer for the merged output part, using the destination codec resolved from the index definition.
    PostingsSerialization postings_serialization;
    /// Per-source deserializers, each using the codec read from that source part's own header.
    std::vector<PostingsSerialization> source_postings_serializations;
    /// Per-source codec type and serialization version, used to build merge cursors.
    std::vector<SourcePostingsFormat> source_formats;

    bool is_initialized = false;
};

using MergeTextIndexesTaskPtr = std::unique_ptr<MergeTextIndexesTask>;

MutableDataPartStoragePtr createTemporaryTextIndexStorage(const DiskPtr & disk, const String & part_relative_path);

std::unique_ptr<MergeTreeReaderStream> makeTextIndexInputStream(
    DataPartStoragePtr data_part_storage,
    const String & stream_name,
    const String & extension,
    const MergeTreeReaderSettings & reader_settings);

}
