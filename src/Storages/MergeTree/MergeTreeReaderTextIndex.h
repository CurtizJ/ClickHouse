#pragma once
#include <Storages/MergeTree/IMergeTreeReader.h>
#include <Storages/MergeTree/MergeTreeIndexReader.h>
#include <Storages/MergeTree/MergeTreeIndices.h>
#include <Storages/MergeTree/MergeTreeIndexText.h>
#include <Storages/MergeTree/TextIndexPositionData.h>
#include <Storages/MergeTree/TextIndexPositionCodec.h>
#include <Storages/MergeTree/TextIndexBlockedPositionsCodec.h>
#include <Storages/MergeTree/TextIndexCache.h>
#include <Storages/MergeTree/BM25State.h>
#include <Storages/MergeTree/MergeTreeIndexTextPostingListCursor.h>
#include <Interpreters/ExpressionActions.h>
#include <Processors/TopKThresholdTracker.h>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <roaring/roaring.hh>

namespace DB
{

class TextIndexAnalyzer;
class MergeTreeIndexConditionText;

using PostingsBlocksMap = absl::flat_hash_map<std::string_view, absl::btree_map<size_t, PostingListPtr>>;

/// A part of "direct read from text index" optimization.
/// This reader fills virtual columns for text search filters
/// which were replaced from the text search functions using
/// the posting lists read from the index.
///
/// E.g. `__text_index_<name>_hasToken` column created for `hasToken` function.
///
/// It also fills the BM25 score virtual columns (`__text_index_<name>_bm25_<hash>`, `Float32`) of the
/// scoring predicates of a query computing `bm25()`: the BM25 score of the predicate's tokens for the
/// rows the predicate matches and 0 elsewhere. The planner assembles `bm25()` from these columns.
///
/// For `ORDER BY bm25() DESC LIMIT n` the reader receives the top-k threshold: marks and block-aligned
/// windows whose block-max score bound stays below it are zero-filled without decoding the postings.
/// The rows are then dropped by the `__topKFilter` PREWHERE, which sees a score of exactly 0 for them.
class MergeTreeReaderTextIndex : public IMergeTreeReader
{
public:
    MergeTreeReaderTextIndex(
        const IMergeTreeReader * main_reader_,
        MergeTreeIndexWithCondition index_,
        NamesAndTypesList columns_,
        MergeTreeIndexGranulePtr index_granule_,
        BM25StatePtr bm25_score_state_,
        TopKThresholdTrackerPtr bm25_threshold_tracker_);

    size_t readRows(
        size_t from_mark,
        bool continue_reading,
        size_t max_rows_to_read,
        MutableColumns & res_columns) override;

    bool canReadIncompleteGranules() const override { return false; }
    void updateAllMarkRanges(const MarkRanges & ranges) override;

    /// Sets a pre-computed granule from the skip index reader (Path 2: use_skip_indexes_on_data_read = 1).
    /// Looks up its own index name in the map.
    void setPrecomputedGranule(const IndexGranulesMap & granules);

private:
    void setIndexGranule(MergeTreeIndexGranulePtr index_granule);
    void initializeFallbackReader(const IMergeTreeReader * main_reader);
    void createEmptyColumns(MutableColumns & columns, size_t max_rows_to_read) const;
    std::unique_ptr<MergeTreeReaderStream> makeTextIndexStream(const MergeTreeIndexSubstream & substream) const;

    /// Returns combined postings per column for the given mark, clipped to `slice_range`
    /// (the actual read window, which may be narrower than the mark on partial-mark reads).
    std::vector<PostingList> buildPostingsForMark(size_t mark, const RowsRange & slice_range, PostingList & range_posting);
    /// Returns combined posting list for a single query by taking the prebuilt
    /// postings from the analyzer and reading large postings blocks as needed.
    PostingList buildPostingsForQuery(const TextSearchQuery & query, const TextIndexAnalyzer & analyzer, const RowsRange & range, PostingList & range_posting);
    /// Reads and unions all posting list blocks for a large-posting token within the given range.
    std::vector<PostingListPtr> readPostingsBlocksForToken(std::string_view token, const TokenPostingsInfo & token_info, const RowsRange & range);
    /// Removes blocks with max value less than the given range.
    void cleanupPostingsBlocks(const RowsRange & range);
    /// Drops all cached cursors, keeping the per-column sizing.
    void resetCursors();

    std::optional<RowsRange> getRowsRangeForMark(size_t mark) const;
    MergeTreeDataPartPtr getDataPart() const;

    void readGranule();
    /// Sets per-column flags from the analyzer's verdict and collects tokens to materialize.
    void classifyVirtualColumns();
    void initializePostingStreams();
    /// Decides per virtual column whether to produce `ColumnSparse` instead of a full `ColumnUInt8`:
    /// the upper bound on the matching rows must leave at least the configured ratio of non-matching rows.
    void chooseSparseVirtualColumns();
    void fillColumn(IColumn & column, const PostingList & postings, size_t row_offset, size_t num_rows);
    void fillColumnLazy(IColumn & column, size_t column_idx, size_t row_offset, size_t num_rows, PostingList & range_posting);

    /// Fills a virtual column for an abandoned pattern query by evaluating the virtual column's
    /// default expression (the original search predicate) on the physical columns.
    /// Used when the dictionary scan was cut short and pattern tokens are incomplete.
    void fillColumnFallback(
        IColumn & column,
        const String & column_name,
        const Block & physical_block,
        size_t offset,
        size_t num_rows) const;

    PostingListCursorPtr makeLazyCursor(std::string_view token, const TokenPostingsInfo & token_info);

    /// Fills all columns for rows [row_offset, row_offset + num_rows) of `mark` from the index.
    /// `fallback_offset` is the position of `row_offset` in `fallback_block`.
    void fillRows(MutableColumns & res_columns, size_t mark, size_t row_offset, size_t num_rows, const Block & fallback_block, size_t fallback_offset);
    /// Appends `num_rows` zeros (no match, zero score) to all columns.
    void fillZeroRows(MutableColumns & res_columns, size_t num_rows) const;

    /// Fills the score column `column_idx` for rows [row_offset, row_offset + num_rows).
    void fillColumnScores(IColumn & column, size_t column_idx, size_t row_offset, size_t num_rows);

    /// Builds the scoring cursors of every score column and the pruning cursors (see `score_leaves`, `bound_cursors`).
    void initializeScoreLeaves();
    std::shared_ptr<PostingListScoringCursor> makeScoringCursor(const String & token, const TokenPostingsInfo & token_info);

    /// The top-k threshold when the reader may prune by it: the tracker is set and the threshold is positive.
    std::optional<Float64> getPruningThreshold() const;
    /// Upper bound of the assembled `bm25()` of any row of [begin, end): the sum over the pruning tokens
    /// of their block-max bound times their coefficient. Requires the doc lengths of the range to be resident.
    Float64 scoreUpperBound(size_t begin, size_t end);
    /// End of the pruning window that starts at `begin` inside the mark ending at `mark_end`: the nearest
    /// block end among the pruning cursors, where the bound can change.
    size_t nextPruningWindowEnd(size_t begin, size_t mark_end);

    /// Fills a phrase virtual column from positional data (.pos), computing matching documents
    /// via phrase intersection (cached per granule).
    void applyPostingsPhrase(IColumn & column, const TextSearchQueryPtr & search_query, size_t row_offset, size_t num_rows);
    void initializePositionsStream();

    /// Intersects the phrase tokens' postings into candidates, then decodes only the covering blocks.
    PaddedPODArray<UInt32> phraseSearchBlocked(const TextSearchQuery & search_query);
    /// One token's full posting list — the rank space the blocked position stream is addressed in.
    PostingList readAllPostingsForToken(std::string_view token, const TokenPostingsInfo & token_info);

    using TextIndexGranulePtr = std::shared_ptr<const MergeTreeIndexGranuleText>;

    MergeTreeIndexWithCondition index;
    std::shared_ptr<MergeTreeIndexConditionText> condition_text;
    std::vector<TextSearchQueryPtr> search_queries;
    TextIndexGranulePtr granule;
    PostingsBlocksMap postings_blocks;

    /// Fallback reader for the physical columns required by the fallback expressions.
    /// Used when the pattern dictionary scan is cut short.
    MergeTreeReaderPtr fallback_reader;
    /// Physical columns that fallback_reader reads (union across all fallback expressions).
    NamesAndTypesList fallback_columns_list;
    /// Per-virtual-column compiled expression of the original search predicate.
    /// Executed on the physical columns when use_fallback[i] is true.
    absl::flat_hash_map<String, ExpressionActionsPtr> fallback_expressions;
    /// Per-virtual-column flag: true if this column's query was abandoned during the scan
    /// and the predicate must be evaluated directly via fallback_expressions.
    std::vector<bool> use_fallback;
    /// Small postings stream — kept as a class member because cached lazy cursors
    /// hold a reference to it for on-demand segment reads.
    std::unique_ptr<MergeTreeReaderStream> small_postings_stream;
    /// A separate stream is created for each token to read
    /// postings blocks continuously without additional seeks.
    absl::flat_hash_map<std::string_view, std::unique_ptr<MergeTreeReaderStream>> large_postings_streams;
    /// Streams of the multi-block scoring tokens.
    /// Separate from `large_postings_streams` to avoid sharing and extra seeks.
    absl::flat_hash_map<std::string_view, std::unique_ptr<MergeTreeReaderStream>> scoring_postings_streams;

    /// Stream for position data (.pos file) used for phrase queries.
    std::unique_ptr<MergeTreeReaderStream> positions_stream;
    /// Per-reader memo of phrase results (shared via the postings cache) so repeated readRows calls skip the cache lookup.
    absl::flat_hash_map<UInt128, PaddedPODArrayPtr> phrase_search_doc_ids;

    /// Current row position used when continuing reads across multiple calls.
    size_t current_row = 0;
    size_t current_mark = 0;
    PaddedPODArray<UInt32> indices_buffer;
    TextIndexBlockedPositionsCodec::DecodeScratch blocked_positions_scratch;

    bool is_initialized = false;
    /// Virtual columns that are always true.
    std::vector<bool> is_always_true;
    /// Virtual columns produced as `ColumnSparse` because few rows are expected to match.
    /// Sized in the constructor: `createEmptyColumns` may run before the granule is analyzed.
    std::vector<bool> use_sparse;
    std::unique_ptr<MergeTreeIndexDeserializationState> deserialization_state;
    std::optional<PostingsSerialization> postings_serialization;

    /// Requested in the constructor; enabled per granule in `setIndexGranule` after checking the
    /// sparse-index header and confirming no virtual column carries pattern predicates.
    bool lazy_mode_requested = false;
    bool use_lazy_mode = false;
    float lazy_intersection_density_threshold = 0.2f;

    /// Cached lazy cursors, indexed by column position in `columns_to_read` and keyed by token.
    /// Cursors are forward-only and hold mutable segment/block position, so they must not be
    /// shared across columns. Dropped on granule reload and on backward `readRows` jumps (`from_mark < current_mark`).
    std::vector<absl::flat_hash_map<String, PostingListCursorPtr>> lazy_cursors;

    /// Per-column synthetic cursor over the analyzer-folded postings of small/embedded tokens,
    /// combined with large-posting stream cursors. Dropped on the same triggers as `lazy_cursors`.
    std::vector<PostingListCursorPtr> prebuilt_cursors;

    /// Query-global BM25 state (statistics and per-token weights); null when the query reads no scores.
    BM25StatePtr bm25_score_state;
    /// Threshold of `ORDER BY bm25() DESC LIMIT n`; null when the reader must not prune by it.
    TopKThresholdTrackerPtr bm25_threshold_tracker;

    /// Per column: true for a BM25 score column, false for a match column.
    std::vector<bool> is_score_column;
    bool has_score_columns = false;

    /// Scoring state of one score column (one scoring predicate of the query).
    struct ScoreLeaf
    {
        /// One cursor per distinct token of the predicate present in this part, sorted by ascending cardinality.
        std::vector<ScoreCursor> cursors;
        /// `hasAllTokens` uses the intersection scorer, `hasToken` / `hasAnyTokens` the union scorer.
        bool intersect = false;
        /// False when the predicate matches no row of the part (a required token is absent): the column stays 0.
        bool can_match = false;
    };

    /// A token of the pruning bound present in this part. The cursor belongs to a leaf; the bound
    /// queries do not move it.
    struct BoundCursor
    {
        PostingListScoringCursor * cursor = nullptr;
        const BM25Weight * weight = nullptr;
        UInt32 coefficient = 0;
    };

    /// Parallel to `columns_to_read`; empty for match columns.
    std::vector<ScoreLeaf> score_leaves;
    std::vector<BoundCursor> bound_cursors;
    bool score_leaves_initialized = false;
    /// The part's `.dl` doc-length cursor.
    std::shared_ptr<DocLengthsCursor> score_doc_lengths;
};

MergeTreeReaderPtr createMergeTreeReaderTextIndex(
    const IMergeTreeReader * main_reader,
    const MergeTreeIndexWithCondition & index,
    const NamesAndTypesList & columns_to_read,
    MergeTreeIndexGranulePtr index_granule,
    BM25StatePtr bm25_score_state,
    TopKThresholdTrackerPtr bm25_threshold_tracker);

}
