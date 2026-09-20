#pragma once
#include <Storages/MergeTree/MergeTreeIndexText.h>
#include <Storages/MergeTree/MergeTreeIndexConditionText.h>
#include <Storages/MergeTree/TextIndexPostingsApplier.h>
#include <absl/container/flat_hash_map.h>

#include <span>

namespace DB
{

class ColumnString;

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
        const std::vector<RowsRange> & getRanges() const { return ranges; }
        /// The same rows as a bitmap of run containers, built on the first call. Used to clip uncompressed posting lists.
        const PostingList & getBitmap();

    private:
        /// Sorted and disjoint.
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

        /// Posting list folded across materialized tokens by the query search mode (intersection for `All` and
        /// `Phrase`, union for `Any`), clipped to the readable rows. At most one representation is set:
        /// `postings_array` holds the intersection of block-compressed posting lists, a sorted array of unique row
        /// ids that never grows beyond the rarest folded token and that the reader iterates in place; `postings_bitmap`
        /// holds unions, and intersections of uncompressed posting lists, which are bitmaps on disk already.
        std::shared_ptr<PaddedPODArray<UInt32>> postings_array;
        std::optional<PostingList> postings_bitmap;

        /// Query can never match (e.g. missing token in `All` mode, empty intersection).
        bool is_failed = false;
        /// Query was discarded (low-selectivity hint, pattern bypass).
        bool is_bypassed = false;
        /// The dictionary scan stopped early, so the matched tokens are incomplete and nothing can be pruned.
        bool is_analysis_incomplete = false;
        /// Number of tokens whose posting list has already been folded.
        size_t num_read_postings = 0;
        /// Declared tokens (`query->getTokens`) that may still contribute to an `Any` query.
        size_t num_live_tokens = 0;

        void markFailed();
        void markBypassed();
        void addMissingToken(std::string_view token);
        void addTokenInfo(std::string_view token, TokenPostingsInfoPtr token_info, RowsRange token_rows_range);
        void addRowsRange(RowsRange token_rows_range);
        bool needReadPostings() const { return num_read_postings < tokens.size(); }

        /// True if the posting list of at least one token has been folded.
        bool hasPostings() const { return postings_array || postings_bitmap; }
        /// True if the folded posting list has no rows. Constant time, unlike `getPostingsCardinality` for a bitmap.
        bool hasEmptyPostings() const;
        size_t getPostingsCardinality() const;
        /// True if the folded posting list has a row in the closed range.
        bool hasPostingsInRange(const RowsRange & range) const;
        /// The folded posting list clipped to the closed range.
        PostingList getPostingsInRange(const RowsRange & range) const;
        PostingList getPostingsAsBitmap() const;
    };

    /// Plan for folding the posting list of a token into every active query that references it.
    struct PostingsApplyPlan
    {
        PostingsApplyTargets targets;
        /// Query hashes parallel to `targets.intersect`, `targets.intersect_bitmaps` and `targets.unite`.
        std::vector<UInt128> intersect_queries;
        std::vector<UInt128> intersect_bitmap_queries;
        std::vector<UInt128> unite_queries;
    };

    explicit TextIndexAnalyzer(const MergeTreeIndexConditionText & condition_text);

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

    /// Sets the codec of the posting lists of the granule. Intersections of block-compressed posting lists are
    /// folded into arrays, which lets the reading skip packed blocks; uncompressed posting lists are bitmaps
    /// already and are folded with bitmap operations. Must be called before any posting list is folded.
    void setPostingsCodecType(IPostingListCodec::Type codec_type);

    /// Returns the targets that the posting list of `token` must be folded into while it is read.
    /// The targets are empty when no active query needs the token.
    PostingsApplyPlan planApplyPostings(std::string_view token);
    /// Completes the fold after the posting list of `token` has been applied to `plan.targets`: marks the token
    /// as read, fails the `All` queries whose intersection became empty and treats the token as missing
    /// for the `Any` queries it contributed no readable row to.
    void finishApplyPostings(std::string_view token, const PostingsApplyPlan & plan);
    /// Folds an already deserialized posting list of `token` (embedded, raw or uncompressed).
    void applyPostings(std::string_view token, std::span<const UInt32> sorted_postings);
    void applyPostings(std::string_view token, const PostingList & postings);

    /// Pushes the row ranges still readable after the analysis of the primary key and prior skip indexes.
    void setReadableRows(std::vector<RowsRange> readable_ranges);
    /// Attaches a scan-discovered `token` to every pattern query whose regex matches it.
    /// Returns true if any pattern matched.
    bool addTokenToPatterns(std::string_view token);
    /// One key range per pattern, or nothing when some pattern can match tokens anywhere in the dictionary.
    std::optional<std::vector<TokenKeyRange>> getPatternTokenKeyRanges() const;
    bool canFilterTokensByLiterals() const;
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

    /// Detaches a query that has just failed from its tokens. One failed query in `All` global
    /// mode proves the whole conjunction false in this part, so it fails all the other queries too.
    void handleFailedQuery(const UInt128 & query_hash, const QueryBuilder & query_builder);

    /// Removes the query from `queries_by_token` for all affected tokens, so they stop passing `isTokenNeeded`.
    void detachQueryFromTokens(const UInt128 & query_hash, const QueryBuilder & query_builder);

    /// Fails every query and detaches them from `queries_by_token`.
    void markAllQueriesFailed();

    /// Estimates the cardinality of a query from already-read postings and `cardinality` hints for unread tokens.
    double estimateQueryCardinality(const QueryBuilder & query_builder, size_t total_rows) const;

    /* Fields built in the constructor from MergeTreeIndexConditionText. */

    TextSearchMode global_search_mode;
    /// True if the intersections are folded into sorted arrays (block-compressed posting lists), false for bitmaps.
    bool fold_intersections_into_arrays = false;
    /// One builder per parsed query, keyed by the query's stable hash.
    absl::flat_hash_map<UInt128, QueryBuilder> query_builders;
    /// Active queries that still depend on a given token.
    absl::flat_hash_map<String, QueryHashes> queries_by_token;
    /// Pattern queries grouped by their compiled regex; static for the analyzer's lifetime.
    absl::flat_hash_map<const OptimizedRegularExpression *, QueryHashes> queries_by_pattern;

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
