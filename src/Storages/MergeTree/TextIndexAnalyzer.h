#pragma once
#include <Storages/MergeTree/MergeTreeIndexText.h>
#include <Storages/MergeTree/MergeTreeIndexConditionText.h>
#include <Storages/MergeTree/PostingListSegment.h>
#include <absl/container/flat_hash_map.h>
#include <absl/container/node_hash_map.h>
#include <mutex>

namespace DB
{

class ColumnString;
class IPostingListBlockCodec;

/// The rows of the posting list of one token, as read: a bitmap or a sorted array. The other form is built on first use,
/// once for all queries that fold the token.
class TokenPostings
{
public:
    /// `bitmap` must outlive this object.
    explicit TokenPostings(const PostingList & bitmap_) : bitmap(&bitmap_) {}
    explicit TokenPostings(PaddedPODArrayPtr array_) : array(std::move(array_)) {}

    bool isEmpty() const { return array ? array->empty() : bitmap->isEmpty(); }
    bool hasArray() const { return array != nullptr; }
    const PostingList & getBitmap();
    const PaddedPODArrayPtr & getArray();

private:
    const PostingList * bitmap = nullptr;
    std::optional<PostingList> bitmap_of_array;
    PaddedPODArrayPtr array;
};

/// Rows folded across the posting lists of the tokens of a query: by union for `Any` queries, by intersection for
/// `All` and `Phrase` queries. The rows of block-compressed posting lists are kept as sorted arrays, which they are
/// decoded into, so no bitmap is built: a compressed posting list is intersected with an array without decoding the
/// blocks with none of its rows, and a union is kept as its largest array and the merged other arrays, so the rows of
/// the largest one are not copied unless a single array is requested.
/// Otherwise the rows are kept as a bitmap, which is also how an uncompressed posting list is stored.
class FoldedPostings
{
public:
    FoldedPostings() = default;
    /// The rows of the first posting list of a query, as a sorted array if `as_array`.
    FoldedPostings(TokenPostings & postings, bool as_array);

    /// The accessors require `finalize` to be called after the last `unite`.
    size_t cardinality() const;
    bool isEmpty() const { return cardinality() == 0; }
    /// True if some row is in the closed range.
    bool hasRowsInRange(UInt32 begin, UInt32 end) const;
    /// The rows in the closed range.
    PostingList getBitmapInRange(UInt32 begin, UInt32 end) const;
    PostingList getBitmap() const;
    /// The rows as one sorted array, shared by all readers of the granule: the array itself, or the array built once
    /// from the bitmap or from the arrays of a union. It is clipped to the readable rows of the query, so it must never
    /// go to the server-wide postings cache.
    PaddedPODArrayPtr getArray() const;
    /// The rows as sorted arrays whose union they are, without building one array (see `getArray`).
    std::vector<PaddedPODArrayPtr> getArrays() const;

    /// Adds the rows of `postings`. The sorted arrays are merged at once by `finalize`.
    void unite(TokenPostings & postings);
    /// Keeps the rows that `postings` contains.
    void intersect(TokenPostings & postings);
    /// Keeps the rows that the segment of a compressed posting list contains. Only the blocks that may contain
    /// one of the rows are decoded. Requires a sorted array.
    void intersect(const PostingListSegment & segment, IPostingListBlockCodec & block_codec);
    /// Merges the sorted arrays added by `unite`, except the largest one.
    void finalize();

    bool isArray() const { return array != nullptr || !arrays_to_unite.empty(); }

private:
    bool isUnion() const { return !arrays_to_unite.empty(); }

    /// The sorted array of rows, or nullptr if the rows are in `bitmap` or in `arrays_to_unite`.
    PaddedPODArrayPtr array;
    /// The sorted arrays of a union: all of them before `finalize`, the largest one and the union of the others after it.
    std::vector<PaddedPODArrayPtr> arrays_to_unite;
    bool is_finalized = false;
    size_t union_cardinality = 0;
    PostingList bitmap;

    /// The single array of `bitmap` or `arrays_to_unite`, built on request.
    mutable std::once_flag single_array_once;
    mutable PaddedPODArrayPtr single_array;
};

/// Drives text-index analysis during a granule's dictionary scan: folds per-query
/// token postings and row ranges, then bypasses queries that have failed or are no
/// longer worth evaluating (low-selectivity hints, pattern bypass).
class TextIndexAnalyzer
{
public:
    /// Half-open range of dictionary token keys. An empty `end` reaches the end of the dictionary.
    /// Equal bounds are the single key `begin`, not an empty range.
    struct TokenKeyRange
    {
        String begin;
        String end;
    };

    struct ReadableRows
    {
    public:
        explicit ReadableRows(std::vector<RowsRange> ranges_);
        std::optional<RowsRange> clipRowsRange(const RowsRange & rows_range) const;
        PostingList clipPostings(const PostingList & postings);
        /// Removes the rows that are not readable from the sorted array.
        void clipRows(PaddedPODArray<UInt32> & rows) const;
        size_t getSizeInBytes() const;

    private:
        std::vector<RowsRange> ranges;
        PostingList ranges_bitmap;
    };

    /// Per-query mutable analysis state. Updated as the dictionary scan delivers
    /// token info, missing-token notifications, and materialized posting lists.
    struct QueryBuilder
    {
        /// Original parsed search query (tokens + patterns + search mode).
        TextSearchQueryPtr query;
        /// Tokens this query has observed so far (declared + pattern-discovered).
        TokenToPostingsInfosMap tokens;
        /// Row range folded across observed tokens by the query search mode (intersect for `All`, union for `Any`).
        std::optional<RowsRange> rows_range;
        /// Rows folded across materialized tokens by the query search mode.
        std::optional<FoldedPostings> postings;
        /// Union of the approximated posting lists of tokens (see `TextIndexAnalyzer::addApproximatePostings`): a superset
        /// of their rows that is exact at the granularity of marks. It serves the index analysis only: such tokens
        /// stay unread, so the readers read their posting lists themselves.
        std::optional<PostingList> approximate_postings;

        /// Query can never match (e.g. missing token in `All` mode, empty intersection).
        bool is_failed = false;
        /// Query was discarded (low-selectivity hint, pattern bypass).
        bool is_bypassed = false;
        /// The dictionary scan stopped early, so the matched tokens are incomplete and nothing can be pruned.
        bool is_analysis_incomplete = false;
        /// Number of tokens whose posting list has already been folded into `postings`.
        size_t num_read_postings = 0;
        /// Number of tokens whose posting list has been folded into `approximate_postings`.
        size_t num_approximate_postings = 0;
        /// Fold the rows into sorted arrays (posting lists of a block codec, and no patterns), see `FoldedPostings`.
        bool fold_into_arrays = false;
        /// Declared tokens (`query->getTokens`) that may still contribute to an `Any` query.
        size_t num_live_tokens = 0;

        void markFailed();
        void markBypassed();
        void addMissingToken(std::string_view token);
        /// `embedded_postings` are the clipped embedded postings, or nullptr if there are none.
        void addTokenInfo(std::string_view token, TokenPostingsInfoPtr token_info, RowsRange token_rows_range, TokenPostings * embedded_postings);
        void addRowsRange(RowsRange token_rows_range);
        void addPostings(TokenPostings & token_postings);
        void addApproximatePostings(const PostingList & token_postings);
        void intersectPostings(const PostingListSegment & segment, IPostingListBlockCodec & block_codec);
        /// True if some token is not folded into `postings`, so the readers must read its posting list.
        bool needReadPostings() const { return num_read_postings < tokens.size(); }
        /// True if every token is folded into `postings` or `approximate_postings`, so they answer the index analysis.
        bool hasAllPostings() const { return num_read_postings + num_approximate_postings >= tokens.size(); }
        /// True if the query can take a posting list approximated at the granularity of marks without losing precision
        /// of the index analysis: a union of such approximations is exact at that granularity too, an intersection is not.
        bool canApproximatePostings() const;

    };

    /// `postings_codec_type` is the codec of the posting lists of the granule: the rows of block-compressed
    /// posting lists are folded into sorted arrays, see `FoldedPostings`.
    TextIndexAnalyzer(const MergeTreeIndexConditionText & condition_text, IPostingListCodec::Type postings_codec_type);

    bool alwaysFalse() const { return always_false; }
    const TokenToPostingsInfosMap & getAllTokenInfos() const { return all_token_infos; }
    const absl::flat_hash_set<String> & getMissingTokens() const { return missing_tokens; }
    const QueryBuilder & getQueryBuilder(const TextSearchQuery & query) const;

    /// True if at least one active query still depends on this token.
    bool isTokenNeeded(std::string_view token) const;
    /// True if this token's posting list has already been added (embdded or read from disk).
    bool hasReadPostings(std::string_view token) const;

    void addMissingToken(std::string_view token);
    void addTokenInfo(std::string_view token, TokenPostingsInfoPtr token_info);
    void addPostings(std::string_view token, const PostingList & postings);
    /// `rows` are the sorted rows of the posting list of `token`.
    void addPostings(std::string_view token, PaddedPODArray<UInt32> && rows);
    /// True if every active query referencing `token` can take its posting list approximated at the granularity of marks.
    bool canApproximatePostings(std::string_view token) const;
    /// Adds a superset of the rows of the posting list of `token` that is exact at the granularity of marks: a range of marks
    /// has a row of the posting list if and only if it has a row of `postings`. The token stays unread for the readers.
    void addApproximatePostings(std::string_view token, const PostingList & postings);
    /// Must be called after the last posting list is added, before the folded postings are read.
    void finalize();
    /// True if every active query referencing `token` has folded an intersection into a sorted array, so the posting
    /// list of `token` can be intersected with these arrays (`intersectPostings`) instead of being read in full.
    bool canIntersectPostings(std::string_view token) const;
    /// Intersects the arrays folded by the queries referencing `token` with the segment of its posting list.
    void intersectPostings(std::string_view token, const PostingListSegment & segment, IPostingListBlockCodec & block_codec);

    /// Pushes the row ranges still readable after the analysis of the primary key and prior skip indexes.
    void setReadableRows(std::vector<RowsRange> readable_ranges);
    /// The row ranges pushed by `setReadableRows`, nullptr if every row is readable.
    const ReadableRows * getReadableRows() const { return readable_rows ? &*readable_rows : nullptr; }
    /// Attaches a scan-discovered `token` to every pattern query whose regex matches it.
    /// Returns true if any pattern matched.
    bool addTokenToPatterns(std::string_view token);
    /// One key range per pattern, or nothing when some pattern can match tokens anywhere in the dictionary.
    std::optional<std::vector<TokenKeyRange>> getPatternTokenKeyRanges() const;
    bool canFilterTokensByLiterals() const;
    bool canIntersectDictionary() const { return !pattern_cursors.empty(); }
    /// Like `TextIndexDictionaryDFA::Cursor::next`, but for the union of all pattern DFAs: `token` matches if any
    /// DFA accepts it, and `lower_bound` is the smallest lower bound of all DFAs.
    TextIndexDictionaryDFA::Cursor::Result nextPatternToken(std::string_view token, String & lower_bound);
    /// Appends, ascending, the tokens `addTokenToPatterns` accepts, running it only on those holding a pattern's literal.
    void matchTokensByLiterals(const ColumnString & tokens, PaddedPODArray<UInt8> & candidate_marks, std::vector<size_t> & matched_indices);
    /// Marks all pattern queries as bypassed (e.g. dictionary scan budget exhausted).
    void bypassPatternQueries();

    /// Discards `Hint`-mode queries whose estimated cardinality (read postings + `cardinality`
    /// estimates for unread multi-block tokens) exceeds `selectivity_threshold * total_rows`.
    void analyzeCardinalitiesAndBypassHints(double selectivity_threshold, size_t total_rows);

private:
    using QueryHashes = absl::flat_hash_set<UInt128>;

    /// Applies `operation` to every active query that references `token`,
    /// then cleans up `queries_by_token` for any query that just failed.
    template <typename Operation>
    void processTokenOperation(std::string_view token, Operation && operation);

    static void markPatternCandidateTokens(
        const OptimizedRegularExpression & pattern, const ColumnString & tokens, PaddedPODArray<UInt8> & candidate_marks);

    /// Folds the read (and clipped) posting list of `token` into the queries referencing it.
    void foldPostings(std::string_view token, TokenPostings & postings);

    /// Removes the query from `queries_by_token` for all affected tokens, so they stop passing `isTokenNeeded`.
    void detachQueryFromTokens(const UInt128 & query_hash, const QueryBuilder & query_builder);

    /// Fails every query and detaches them from `queries_by_token`.
    void markAllQueriesFailed();

    /// Estimates the cardinality of a query from already-read postings and `cardinality` hints for unread tokens.
    double estimateQueryCardinality(const QueryBuilder & query_builder, size_t total_rows) const;

    /* Fields built in the constructor from MergeTreeIndexConditionText. */

    TextSearchMode global_search_mode;
    /// True if the rows are folded into sorted arrays (block-compressed posting lists).
    bool fold_into_arrays = false;
    /// One builder per parsed query, keyed by the query's stable hash. Node map because `QueryBuilder` is not movable.
    absl::node_hash_map<UInt128, QueryBuilder> query_builders;
    /// Active queries that still depend on a given token.
    absl::flat_hash_map<String, QueryHashes> queries_by_token;
    /// Pattern queries grouped by their compiled regex; static for the analyzer's lifetime.
    absl::flat_hash_map<const OptimizedRegularExpression *, QueryHashes> queries_by_pattern;
    std::vector<TextIndexDictionaryDFA::Cursor> pattern_cursors;

    /* Fields updated dynamically during text index analysis. */

    bool always_false = false;
    /// Dictionary entries observed during the scan, keyed by token.
    TokenToPostingsInfosMap all_token_infos;
    /// Tokens looked up and not present in the dictionary.
    absl::flat_hash_set<String> missing_tokens;
    /// Tokens whose posting list has been added (embedded or read from disk).
    absl::flat_hash_set<String> tokens_with_postings;
    /// Row ranges still readable after the analysis of the primary key and prior skip indexes.
    std::optional<ReadableRows> readable_rows;
};

}
