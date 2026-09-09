#include <Columns/ColumnsCommon.h>
#include <Columns/ColumnSparse.h>
#include <IO/ReadHelpers.h>
#include <Storages/MergeTree/MergeTreeIndexText.h>
#include <Storages/MergeTree/TextIndexAnalyzer.h>
#include <Storages/MergeTree/IPostingListCodec.h>
#include <Storages/MergeTree/MergeTreeReaderTextIndex.h>
#include <Storages/MergeTree/TextIndexPositionCodec.h>
#include <Storages/MergeTree/TextIndexPhraseSearch.h>
#include <Storages/MergeTree/MergeTreeIndexTextPostingListCursor.h>
#include <Storages/MergeTree/LoadedMergeTreeDataPartInfoForReader.h>
#include <Storages/MergeTree/MergeTreeIndexConditionText.h>
#include <Storages/MergeTree/BM25State.h>
#include <Storages/MergeTree/TextIndexUtils.h>
#include <Interpreters/Context.h>
#include <Interpreters/ExpressionActions.h>
#include <Interpreters/inplaceBlockConversions.h>
#include <Common/logger_useful.h>
#include <Common/Stopwatch.h>
#include <Columns/ColumnsNumber.h>
#include <Storages/MergeTree/TextIndexCache.h>
#include <Core/Settings.h>

#include <algorithm>
#include <span>

namespace ProfileEvents
{
    extern const Event TextIndexReaderTotalMicroseconds;
    extern const Event TextIndexPositionsDecodeMicroseconds;
    extern const Event TextIndexPhraseMatchMicroseconds;
    extern const Event TextIndexPositionsBlocksRead;
    extern const Event TextIndexPositionsBlocksTotal;
    extern const Event TextIndexPositionsBytesRead;
    extern const Event TextIndexPhraseCandidates;
    extern const Event TextIndexPhraseSearches;
    extern const Event TextIndexPhraseFallbacks;
    extern const Event TextScoreMarksPruned;
    extern const Event TextScoreWindowsPruned;
    extern const Event TextIndexSparseVirtualColumns;
}

namespace DB
{

namespace Setting
{
    extern const SettingsTextIndexPostingListApplyMode text_index_posting_list_apply_mode;
    extern const SettingsFloat text_index_lazy_intersection_density_threshold;
    extern const SettingsFloat text_index_hint_max_selectivity;
}

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int CORRUPTED_DATA;
    extern const int LOGICAL_ERROR;
    extern const int NOT_IMPLEMENTED;
}

namespace
{

/// Upper bound on the number of rows of the part a token query can match: an intersection (`All`, `Phrase`)
/// is bounded by its smallest posting list, a union (`Any`) by the sum of its posting lists. Tokens whose
/// postings the analyzer has already folded into `query_builder.postings` are counted through that folded list.
size_t estimateMaxMatchingRows(
    const TextIndexAnalyzer & analyzer,
    const TextIndexAnalyzer::QueryBuilder & query_builder,
    const TextSearchQuery & query)
{
    const bool is_union = query.getSearchMode() == TextSearchMode::Any;

    std::optional<size_t> result;
    if (query_builder.postings)
        result = query_builder.postings->cardinality();

    for (const auto & [token, token_info] : query_builder.tokens)
    {
        if (analyzer.hasReadPostings(token))
            continue;

        const size_t cardinality = token_info->cardinality;
        if (!result)
            result = cardinality;
        else if (is_union)
            *result += cardinality;
        else
            *result = std::min(*result, cardinality);
    }

    return result.value_or(0);
}

/// Completes an append of `num_rows` rows to a sparse virtual column after `num_matches` offsets
/// were pushed for it: adds the ones they refer to and grows the column.
void finishSparseRows(ColumnSparse & column, size_t num_matches, size_t num_rows)
{
    auto & values = assert_cast<ColumnUInt8 &>(column.getValuesColumn()).getData();
    values.resize_fill(values.size() + num_matches, 1);
    /// `insertManyDefaults` only grows the size of a sparse column.
    column.insertManyDefaults(num_rows);
}

/// Appends `num_rows` rows to the sparse column with ones at the sorted absolute row numbers `matches`,
/// which must lie in [row_offset, row_offset + num_rows).
void appendSparseRows(ColumnSparse & column, std::span<const UInt32> matches, size_t row_offset, size_t num_rows)
{
    const size_t old_size = column.size();
    auto & offsets = column.getOffsetsData();
    offsets.reserve(offsets.size() + matches.size());

    for (UInt32 row : matches)
    {
        const size_t relative_row_number = row - row_offset;
        chassert(relative_row_number < num_rows);
        offsets.push_back(old_size + relative_row_number);
    }

    finishSparseRows(column, matches.size(), num_rows);
}

/// Appends `num_rows` rows to a full or sparse virtual column with ones at the sorted absolute
/// row numbers `matches`, which must lie in [row_offset, row_offset + num_rows).
void appendMatchingRows(IColumn & column, std::span<const UInt32> matches, size_t row_offset, size_t num_rows)
{
    if (auto * sparse_column = typeid_cast<ColumnSparse *>(&column))
    {
        appendSparseRows(*sparse_column, matches, row_offset, num_rows);
        return;
    }

    auto & column_data = assert_cast<ColumnUInt8 &>(column).getData();
    const size_t old_size = column_data.size();
    column_data.resize_fill(old_size + num_rows, 0);

    for (UInt32 row : matches)
    {
        const size_t relative_row_number = row - row_offset;
        chassert(relative_row_number < num_rows);
        column_data[old_size + relative_row_number] = 1;
    }
}

}

MergeTreeReaderTextIndex::MergeTreeReaderTextIndex(
    const IMergeTreeReader * main_reader_,
    MergeTreeIndexWithCondition index_,
    NamesAndTypesList columns_,
    MergeTreeIndexGranulePtr index_granule_,
    BM25StatePtr bm25_score_state_,
    TopKThresholdTrackerPtr bm25_threshold_tracker_)
    : IMergeTreeReader(
        main_reader_->data_part_info_for_read,
        columns_,
        /*virtual_fields=*/ {},
        main_reader_->storage_snapshot,
        main_reader_->storage_settings,
        Context::getGlobalContextInstance()->getIndexUncompressedCache().get(),
        Context::getGlobalContextInstance()->getIndexMarkCache().get(),
        main_reader_->all_mark_ranges,
        main_reader_->settings)
    , index(std::move(index_))
    , condition_text(std::dynamic_pointer_cast<MergeTreeIndexConditionText>(index.condition_template->generateUnsubstituted()))
    , bm25_score_state(std::move(bm25_score_state_))
    , bm25_threshold_tracker(std::move(bm25_threshold_tracker_))
{
    search_queries.reserve(columns_.size());
    is_score_column.reserve(columns_.size());

    for (const auto & column : columns_)
    {
        if (!column.name.starts_with(TEXT_INDEX_VIRTUAL_COLUMN_PREFIX))
        {
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "Column {} with type {} should not be filled by text index reader",
                column.name, column.type->getName());
        }

        /// A `Float32` virtual column is the BM25 score of its search query, a `UInt8` one is its match.
        WhichDataType which(column.type);
        if (which.isFloat32())
        {
            if (!bm25_score_state)
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Column '{}' is read by the text index reader, but the BM25 query state is not set", column.name);
        }
        else if (!which.isUInt8())
        {
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "Column {} with type {} should not be filled by text index reader",
                column.name, column.type->getName());
        }

        is_score_column.push_back(which.isFloat32());
        has_score_columns |= which.isFloat32();
        search_queries.push_back(condition_text->getSearchQueryForVirtualColumn(column.name));
    }

    lazy_cursors.resize(columns_.size());
    prebuilt_cursors.resize(columns_.size());
    score_leaves.resize(columns_.size());
    use_sparse.resize(columns_.size(), false);

    auto data_part = getDataPart();
    auto index_format = index.index->getDeserializedFormat(*data_part, index.index->getFileName());
    chassert(index_format);

    MergeTreeIndexDeserializationState state
    {
        .version = index_format.version,
        .condition = condition_text.get(),
        .part_info = *data_part_info_for_read,
        .index = *index.index,
        .readable_ranges = nullptr,
        .text_index_read_postings = true,
    };

    deserialization_state = std::make_unique<MergeTreeIndexDeserializationState>(std::move(state));

    /// Lazy mode is requested per query; actual support is determined from the on-disk sparse-index header.
    const auto & ctx_settings = condition_text->getContext()->getSettingsRef();
    const auto apply_mode = ctx_settings[Setting::text_index_posting_list_apply_mode].value;

    lazy_mode_requested = (apply_mode == TextIndexPostingListApplyMode::LAZY);
    lazy_intersection_density_threshold = ctx_settings[Setting::text_index_lazy_intersection_density_threshold].value;

    if (!std::isfinite(lazy_intersection_density_threshold) || lazy_intersection_density_threshold < 0.0f || lazy_intersection_density_threshold > 1.0f)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Setting text_index_lazy_intersection_density_threshold must be a value in [0.0, 1.0], got {}", lazy_intersection_density_threshold);

    if (index_granule_)
        setIndexGranule(std::move(index_granule_));

    initializeFallbackReader(main_reader_);
}

void MergeTreeReaderTextIndex::setIndexGranule(MergeTreeIndexGranulePtr index_granule)
{
    chassert(index_granule);
    granule = std::dynamic_pointer_cast<const MergeTreeIndexGranuleText>(index_granule);

    if (!granule)
    {
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected granule type for the text index '{}'", index.index->index.name);
    }

    if (bm25_score_state && !granule->isScoringEnabled())
    {
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "Granule of the text index '{}' was deserialized without term frequencies, but the query computes BM25 scores",
            index.index->index.name);
    }

    /// Phrase search results are cached per granule; drop them when the granule changes.
    phrase_search_doc_ids.clear();
    /// Scoring cursors reference the previous granule's token infos; rebuild them on the next fill.
    score_leaves.assign(score_leaves.size(), {});
    bound_cursors.clear();
    score_leaves_initialized = false;
    auto postings_codec = PostingListCodecFactory::createPostingListCodec(granule->getPostingsCodecType());

    /// Lazy mode requires the per-segment block-index section (from `V1_WithCodec` onward) and
    /// pure-token queries — pattern predicates take the eager materialize path.
    use_lazy_mode = lazy_mode_requested
        && postings_codec->getType() != IPostingListCodec::Type::None
        && granule->getSerializationVersion() >= MergeTreeTextIndexSerializationVersion::V1_WithCodec
        && !condition_text->hasSearchPatterns();

    postings_serialization = PostingsSerialization(std::move(postings_codec), granule->getSerializationVersion());
}

void MergeTreeReaderTextIndex::initializeFallbackReader(const IMergeTreeReader * main_reader)
{
    /// Check if any virtual column may need a fallback path:
    /// - Pattern queries (LIKE): fallback when dictionary scan is abandoned.
    /// - Phrase queries (hasPhrase with Exact mode): fallback when estimated cardinality is too high
    ///   and reading position data would be slower than evaluating directly.
    /// Only exact direct read needs it: a hint keeps the original predicate, so it can just be always true.
    auto needs_fallback_for_query = [](const auto & search_query)
    {
        if (!search_query || search_query->getDirectReadMode() != TextIndexDirectReadMode::Exact)
            return false;

        return !search_query->getPatterns().empty() || search_query->getSearchMode() == TextSearchMode::Phrase;
    };

    if (std::ranges::none_of(search_queries, needs_fallback_for_query))
        return;

    /// Build a fallback evaluation path. Compile each virtual column's default expression
    /// (the original search predicate) and determine the required physical columns from it.
    /// Used when:
    /// - The dictionary scan is cut short (LIKE pattern queries).
    /// - Phrase search cardinality is too high (cheaper to evaluate hasPhrase on physical data).
    auto context_copy = createContextForDefaultExpressions();
    auto combined_columns = buildCombinedColumnsForDefaultExpressions();

    /// Build a header block containing all physical columns (column type only, no data).
    /// evaluateMissingDefaults passes this to createExpressionsAnalyzer, which creates
    /// a StorageDummy from it — StorageDummy requires at least one column, so the header
    /// must be non-empty.
    Block physical_header;
    for (const auto & phys_col : storage_snapshot->metadata->getColumns().getAllPhysical())
        physical_header.insert({phys_col.type->createColumn(), phys_col.type, phys_col.name});

    NameSet fallback_columns_set;
    for (size_t i = 0; i < columns_to_read.size(); ++i)
    {
        const auto & column = columns_to_read[i];
        const auto & search_query = search_queries[i];
        if (!needs_fallback_for_query(search_query))
            continue;

        /// Compile the virtual column's default expression (the original search predicate).
        /// We pass a header with all physical columns so that createExpressionsAnalyzer
        /// can build a non-empty StorageDummy (it requires at least one column).
        NamesAndTypesList need_col{{column.name, column.type}};
        auto dag = DB::evaluateMissingDefaults(physical_header, need_col, combined_columns, context_copy);
        if (!dag)
            continue;

        dag->addMaterializingOutputActions(/*materialize_sparse=*/ false);
        auto actions = std::make_shared<ExpressionActions>(
            std::move(*dag), ExpressionActionsSettings(context_copy->getSettingsRef()));

        /// Collect the physical columns this expression requires.
        for (const auto & req : actions->getRequiredColumnsWithTypes())
        {
            if (fallback_columns_set.insert(req.name).second)
                fallback_columns_list.push_back(req);
        }

        fallback_expressions.emplace(column.name, std::move(actions));
    }

    if (!fallback_columns_list.empty())
    {
        fallback_reader = createMergeTreeReader(
            main_reader->data_part_info_for_read,
            fallback_columns_list,
            main_reader->storage_snapshot,
            main_reader->storage_settings,
            main_reader->all_mark_ranges,
            /*virtual_fields=*/{},
            main_reader->uncompressed_cache,
            main_reader->mark_cache,
            /*deserialization_prefixes_cache=*/nullptr,
            main_reader->settings,
            /*avg_value_size_hints=*/{},
            /*profile_callback=*/{});
    }
}

void MergeTreeReaderTextIndex::updateAllMarkRanges(const MarkRanges & ranges)
{
    IMergeTreeReader::updateAllMarkRanges(ranges);

    if (fallback_reader)
    {
        fallback_reader->updateAllMarkRanges(ranges);
    }

    if (!ranges.empty())
    {
        const auto & index_granularity = data_part_info_for_read->getIndexGranularity();
        size_t row_begin = index_granularity.getMarkStartingRow(ranges.front().begin);
        size_t row_end = index_granularity.getMarkStartingRow(ranges.back().end);

        if (row_begin != row_end)
            cleanupPostingsBlocks(RowsRange(row_begin, row_end - 1));
    }
}

MergeTreeDataPartPtr MergeTreeReaderTextIndex::getDataPart() const
{
    const auto * loaded_data_part = typeid_cast<const LoadedMergeTreeDataPartInfoForReader *>(data_part_info_for_read.get());
    if (!loaded_data_part)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Reading text index is supported only for loaded data parts");

    return loaded_data_part->getDataPart();
}

void MergeTreeReaderTextIndex::readGranule()
{
    auto substreams = index.index->getSubstreams();
    auto data_part = getDataPart();

    LOG_TRACE(getLogger("MergeTreeReaderTextIndex"), "Reading text index granule for data part '{}'", data_part->getDataPartStorage().getFullPath());

    auto sparse_index_stream = makeTextIndexStream(substreams[0]);
    auto dictionary_stream = makeTextIndexStream(substreams[1]);
    small_postings_stream = makeTextIndexStream(substreams[2]);

    sparse_index_stream->seekToStart();
    resetCursors();

    MergeTreeIndexInputStreams streams;
    streams[MergeTreeIndexSubstream::Type::Regular] = sparse_index_stream.get();
    streams[MergeTreeIndexSubstream::Type::TextIndexDictionary] = dictionary_stream.get();
    streams[MergeTreeIndexSubstream::Type::TextIndexPostings] = small_postings_stream.get();

    auto granule_ptr = index.index->createIndexGranule();
    granule_ptr->deserializeBinaryWithMultipleStreams(streams, *deserialization_state);
    setIndexGranule(std::move(granule_ptr));
}

void MergeTreeReaderTextIndex::classifyVirtualColumns()
{
    is_always_true.resize(columns_to_read.size(), false);
    use_fallback.resize(columns_to_read.size(), false);

    const auto & analyzer = granule->getAnalyzer();

    for (size_t i = 0; i < columns_to_read.size(); ++i)
    {
        const auto & column = columns_to_read[i];
        const auto & search_query = search_queries[i];

        /// The score columns are filled from the scoring cursors.
        if (is_score_column[i])
            continue;

        const auto & query_builder = analyzer.getQueryBuilder(*search_query);

        if (search_query->getTokens().empty() && search_query->getPatterns().empty())
        {
            /// Token and phrase searches with no search tokens never match (row-level returns 0, e.g. when a
            /// postprocessor maps every needle token to empty). Encode this as an explicit no-match so direct
            /// read agrees with the row-scan path; otherwise an always-true virtual column would wrongly keep
            /// all rows once granule pruning cannot mask it (e.g. under OR).
            if (search_query->getFunctionName() == "hasAnyTokens" || search_query->getFunctionName() == "hasAllTokens"
                || search_query->getSearchMode() == TextSearchMode::Phrase)
                continue;

            /// Always return true for empty needles.
            is_always_true[i] = true;
        }
        else if (query_builder.is_failed)
        {
            /// Query is definitely false (e.g. a required token in All mode is missing).
            continue;
        }
        else if (query_builder.is_bypassed)
        {
            if (search_query->getDirectReadMode() == TextIndexDirectReadMode::Hint)
            {
                is_always_true[i] = true;
            }
            else
            {
                if (!fallback_reader || !fallback_expressions.contains(column.name))
                {
                    throw Exception(ErrorCodes::LOGICAL_ERROR,
                        "The fallback reader or expression for pattern virtual column '{}' is not initialized", column.name);
                }

                use_fallback[i] = true;
            }
        }
        else if (
            search_query->getSearchMode() == TextSearchMode::Phrase
            && search_query->getDirectReadMode() == TextIndexDirectReadMode::Exact
            && fallback_reader && fallback_expressions.contains(column.name))
        {
            /// For phrase queries with positions, check selectivity before reading positional data.
            /// Reading large position lists for common phrases is slower than evaluating `hasPhrase`
            /// on physical data via the fallback path. Estimate the phrase cardinality as the
            /// intersection of its tokens (a safe upper bound) from the analyzer's per-token cardinalities.
            const auto & all_token_infos = analyzer.getAllTokenInfos();
            const auto & settings = condition_text->getContext()->getSettingsRef();
            const double selectivity_threshold = static_cast<double>(settings[Setting::text_index_hint_max_selectivity]);
            /// Cardinalities (granule) and num_rows_in_part (part) share scale - a text index has whole-part granularity.
            const size_t num_rows_in_part = data_part_info_for_read->getRowCount();

            const bool all_tokens_present = ((num_rows_in_part > 0) && std::ranges::all_of(search_query->getTokens(),
                    [&](const auto & token) { return all_token_infos.find(token) != all_token_infos.end(); }));

            if (all_tokens_present)
            {
                double log_cardinality = 0.0;
                for (const auto & token : search_query->getTokens())
                    log_cardinality += std::log(static_cast<double>(all_token_infos.find(token)->second->cardinality));

                log_cardinality -= static_cast<double>(search_query->getTokens().size() - 1) * std::log(static_cast<double>(num_rows_in_part));
                if (std::exp(log_cardinality) > static_cast<double>(num_rows_in_part) * selectivity_threshold)
                {
                    use_fallback[i] = true;
                    ProfileEvents::increment(ProfileEvents::TextIndexPhraseFallbacks);
                }
            }
        }
    }
}

void MergeTreeReaderTextIndex::chooseSparseVirtualColumns()
{
    const auto & analyzer = granule->getAnalyzer();
    /// Cardinalities (granule) and the row count (part) share scale - a text index has whole-part granularity.
    const size_t num_rows_in_part = data_part_info_for_read->getRowCount();
    const double ratio_of_defaults = static_cast<double>(settings.text_index_ratio_of_defaults_for_sparse_columns);
    /// A threshold of 1.0 or above disables sparse columns: no bound is below zero.
    const double max_matching_rows_for_sparse = (1.0 - ratio_of_defaults) * static_cast<double>(num_rows_in_part);

    for (size_t i = 0; i < columns_to_read.size(); ++i)
    {
        /// Always-true columns are all ones; the fallback evaluates an arbitrary expression into a full column.
        if (is_always_true[i] || use_fallback[i])
            continue;

        const auto & search_query = search_queries[i];
        const auto & query_builder = analyzer.getQueryBuilder(*search_query);

        /// Queries that never match give an all-zero column; the rest are bounded by their posting lists.
        /// A score column shares the search query of its filter, and BM25 scores only the rows that
        /// match it, so the same bound applies to the rows with a non-zero score.
        const bool never_matches = query_builder.is_failed || (search_query->getTokens().empty() && search_query->getPatterns().empty());
        const double max_matching_rows = never_matches ? 0.0 : static_cast<double>(estimateMaxMatchingRows(analyzer, query_builder, *search_query));

        use_sparse[i] = max_matching_rows < max_matching_rows_for_sparse;
        if (use_sparse[i])
            ProfileEvents::increment(ProfileEvents::TextIndexSparseVirtualColumns);
    }
}

void MergeTreeReaderTextIndex::initializePostingStreams()
{
    const auto & analyzer = granule->getAnalyzer();
    const auto & token_infos = analyzer.getAllTokenInfos();

    auto data_part = getDataPart();
    auto substream = index.index->getSubstreams()[2];

    for (const auto & [token, token_info] : token_infos)
    {
        if (analyzer.isTokenNeeded(token) && !analyzer.hasReadPostings(token))
            large_postings_streams.emplace(token, makeTextIndexStream(substream));
    }
}

PostingListCursorPtr MergeTreeReaderTextIndex::makeLazyCursor(std::string_view token, const TokenPostingsInfo & token_info)
{
    if (!(token_info.header & PostingsSerialization::Flags::IsCompressed))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected token for lazy mode: {}. Multi-block postings must be compressed", token);

    auto * postings_cache = condition_text->postingsCache().get();
    const auto & index_id_for_cache = granule->getIndexIdForCaches();

    auto stream_it = large_postings_streams.find(token);
    if (stream_it != large_postings_streams.end())
        return std::make_shared<PostingListCursor>(*stream_it->second, token_info, postings_cache, index_id_for_cache);

    if (!small_postings_stream)
        small_postings_stream = makeTextIndexStream(index.index->getSubstreams()[2]);

    return std::make_shared<PostingListCursor>(*small_postings_stream, token_info, postings_cache, index_id_for_cache);
}

std::shared_ptr<PostingListScoringCursor> MergeTreeReaderTextIndex::makeScoringCursor(const String & token, const TokenPostingsInfo & token_info)
{
    if (token_info.header & PostingsSerialization::Flags::EmbeddedPostings)
    {
        if (token_info.embedded_postings.empty())
            return nullptr;

        auto scoring_postings = std::make_shared<ScoringPostings>();
        scoring_postings->row_ids.insert(token_info.embedded_postings.begin(), token_info.embedded_postings.end());

        /// Embedded postings without stored term frequencies imply `tf == 1` for every row.
        if (token_info.embedded_term_frequencies.empty())
            scoring_postings->term_frequencies.resize_fill(scoring_postings->row_ids.size(), 1u);
        else
            scoring_postings->term_frequencies.insert(token_info.embedded_term_frequencies.begin(), token_info.embedded_term_frequencies.end());

        scoring_postings->calculateMaxTermFrequency();
        return std::make_shared<PostingListScoringCursor>(std::move(scoring_postings), score_doc_lengths.get());
    }

    if (token_info.offsets.size() == 1)
    {
        auto scoring_postings = granule->getScoringPostings(token_info.offsets[0]);

        if (!scoring_postings)
        {
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "Postings of the scoring token '{}' were not decoded during the analysis of the text index '{}'",
                token, index.index->index.name);
        }

        if (scoring_postings->row_ids.empty())
            return nullptr;

        return std::make_shared<PostingListScoringCursor>(std::move(scoring_postings), score_doc_lengths.get());
    }

    if (!(token_info.header & PostingsSerialization::Flags::IsCompressed))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected scoring token '{}': multi-block postings must be compressed", token);

    if (!postings_serialization.has_value())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Postings serialization is not set");

    auto [stream_it, inserted] = scoring_postings_streams.try_emplace(token);
    if (inserted)
        stream_it->second = makeTextIndexStream(index.index->getSubstreams()[2]);

    return std::make_shared<PostingListScoringCursor>(
        *stream_it->second, token_info, score_doc_lengths.get(), condition_text->postingsCache().get(), granule->getIndexIdForCaches());
}

void MergeTreeReaderTextIndex::initializeScoreLeaves()
{
    score_leaves_initialized = true;
    score_leaves.assign(columns_to_read.size(), {});
    bound_cursors.clear();

    chassert(granule && bm25_score_state);
    const auto & analyzer = granule->getAnalyzer();

    /// The whole filter is false in this part: no row of the part is observable, so all scores stay zero.
    if (analyzer.alwaysFalse())
        return;

    const auto & scoring_stats = granule->getScoringStats();

    /// The pool pre-pass has already rejected parts without scoring data; keep a defensive check.
    if (granule->getSerializationVersion() < MergeTreeTextIndexSerializationVersion::V3_WithScoring || !scoring_stats.hasSegmentedDocLengths())
    {
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "Cannot compute bm25(): the text index '{}' in part '{}' was written without BM25 scoring data. "
            "Recreate the index with `enable_scoring = 1` and run `ALTER TABLE ... MATERIALIZE INDEX {}`",
            index.index->index.name, getDataPart()->name, index.index->index.name);
    }

    if (!score_doc_lengths)
    {
        auto substreams = index.index->getSubstreams();

        auto doc_lengths_substream = std::ranges::find_if(substreams,[](const auto & substream)
        {
            return substream.type == MergeTreeIndexSubstream::Type::TextIndexDocLengths;
        });

        if (doc_lengths_substream == substreams.end())
        {
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "Text index '{}' has no doc-lengths substream to compute bm25() from",
                index.index->index.name);
        }

        /// The decoded `.dl` segments go to the postings cache, so all the readers of the query
        /// share them instead of each reading the same segments of the stream.
        score_doc_lengths = std::make_shared<DocLengthsCursor>(
            makeTextIndexStream(*doc_lengths_substream),
            scoring_stats,
            condition_text->postingsCache().get(),
            granule->getIndexIdForCaches());
    }

    const auto & token_infos = analyzer.getAllTokenInfos();

    absl::flat_hash_map<std::string_view, const BM25ScoringToken *> scoring_tokens;
    for (const auto & scoring_token : bm25_score_state->tokens)
        scoring_tokens.emplace(scoring_token.token, &scoring_token);

    absl::flat_hash_set<std::string_view> bound_tokens;

    for (size_t i = 0; i < columns_to_read.size(); ++i)
    {
        if (!is_score_column[i])
            continue;

        const auto & search_query = *search_queries[i];
        auto & leaf = score_leaves[i];
        leaf.intersect = search_query.getSearchMode() == TextSearchMode::All;
        leaf.can_match = true;

        /// The tokens are sorted; a repeated token must contribute once.
        const String * previous_token = nullptr;
        std::vector<const BM25ScoringToken *> leaf_scoring_tokens;

        for (const auto & token : search_query.getTokens())
        {
            if (previous_token && *previous_token == token)
                continue;
            previous_token = &token;

            auto scoring_token_it = scoring_tokens.find(token);
            if (scoring_token_it == scoring_tokens.end())
            {
                throw Exception(ErrorCodes::LOGICAL_ERROR,
                    "Token '{}' of the scoring predicate {} has no BM25 weight", token, columns_to_read[i].name);
            }

            auto info_it = token_infos.find(token);
            std::shared_ptr<PostingListScoringCursor> cursor;
            if (info_it != token_infos.end() && info_it->second)
                cursor = makeScoringCursor(token, *info_it->second);

            /// The token is absent from this part: the predicate never matches when it requires the token,
            /// otherwise the token contributes 0 to every row.
            if (!cursor)
            {
                if (leaf.intersect)
                {
                    leaf.can_match = false;
                    break;
                }

                continue;
            }

            leaf.cursors.push_back(ScoreCursor
            {
                .cursor = std::move(cursor),
                .weight = &scoring_token_it->second->weight,
                .cardinality = info_it->second->cardinality,
            });

            leaf_scoring_tokens.push_back(scoring_token_it->second);
        }

        if (!leaf.can_match)
        {
            leaf.cursors.clear();
            continue;
        }

        /// The pruning bound is over distinct tokens, and a token contributes only through the predicates
        /// that can match in this part, so a cursor of such a predicate answers the bound queries for it.
        for (size_t j = 0; j < leaf.cursors.size(); ++j)
        {
            const auto * scoring_token = leaf_scoring_tokens[j];
            if (scoring_token->pruning_coefficient == 0 || !bound_tokens.insert(scoring_token->token).second)
                continue;

            bound_cursors.push_back(BoundCursor
            {
                .cursor = leaf.cursors[j].cursor.get(),
                .weight = &scoring_token->weight,
                .coefficient = scoring_token->pruning_coefficient,
            });
        }

        /// The conjunction scorer leads with the sparsest cursor.
        std::ranges::sort(leaf.cursors, {}, &ScoreCursor::cardinality);
    }
}

void MergeTreeReaderTextIndex::fillColumnScores(IColumn & column, size_t column_idx, size_t row_offset, size_t num_rows)
{
    if (auto * column_sparse = typeid_cast<ColumnSparse *>(&column))
    {
        fillColumnScoresSparse(*column_sparse, column_idx, row_offset, num_rows);
        return;
    }

    auto & column_data = assert_cast<ColumnFloat32 &>(column).getData();
    size_t old_size = column_data.size();
    column_data.resize_fill(old_size + num_rows);

    if (!score_leaves_initialized)
        initializeScoreLeaves();

    auto & leaf = score_leaves[column_idx];
    if (!leaf.can_match || leaf.cursors.empty())
        return;

    requireRowOffsetRepresentable(row_offset);
    Float32 * data = column_data.data() + old_size;
    score_doc_lengths->ensureRange(row_offset, num_rows);

    if (leaf.intersect)
        scoreCursorsIntersection(data, leaf.cursors, row_offset, num_rows);
    else
        scoreCursorsUnion(data, leaf.cursors, row_offset, num_rows);
}

void MergeTreeReaderTextIndex::fillColumnScoresSparse(ColumnSparse & column, size_t column_idx, size_t row_offset, size_t num_rows)
{
    /// The offsets the scorers emit are absolute positions in the column, so they continue
    /// from the rows appended by the previous windows of this read.
    const size_t old_size = column.size();

    if (!score_leaves_initialized)
        initializeScoreLeaves();

    auto & leaf = score_leaves[column_idx];
    if (!leaf.can_match || leaf.cursors.empty())
    {
        column.insertManyDefaults(num_rows);
        return;
    }

    requireRowOffsetRepresentable(row_offset);
    score_doc_lengths->ensureRange(row_offset, num_rows);

    auto & offsets_data = column.getOffsetsData();
    auto & scores_data = assert_cast<ColumnFloat32 &>(column.getValuesColumn()).getData();

    if (leaf.intersect)
        scoreCursorsIntersectionSparse(offsets_data, scores_data, old_size, leaf.cursors, row_offset, num_rows);
    else
        scoreCursorsUnionSparse(offsets_data, scores_data, old_size, leaf.cursors, row_offset, num_rows);

    /// `insertManyDefaults` only grows the size of a sparse column, extending it over
    /// the rows appended above (both scored and not).
    column.insertManyDefaults(num_rows);
}

std::optional<Float64> MergeTreeReaderTextIndex::getPruningThreshold() const
{
    /// Pruning zero-fills the match columns too, which is correct only when the same reader zero-fills
    /// the score columns the `__topKFilter` PREWHERE drops the rows by.
    if (!has_score_columns || !bm25_threshold_tracker || !bm25_threshold_tracker->isSet())
        return std::nullopt;

    /// The sort key is the `Float32` assembled score, published as a `Float64` field.
    Field value = bm25_threshold_tracker->getValue();
    if (value.getType() != Field::Types::Float64)
        return std::nullopt;

    Float64 threshold = value.safeGet<Float64>();
    if (!(threshold > 0.0))
        return std::nullopt;

    return threshold;
}

Float64 MergeTreeReaderTextIndex::scoreUpperBound(size_t begin, size_t end)
{
    Float64 result = 0.0;
    for (auto & bound : bound_cursors)
        result += static_cast<Float64>(bound.coefficient) * static_cast<Float64>(bound.cursor->upperBound(begin, end, *bound.weight));

    return result;
}

size_t MergeTreeReaderTextIndex::nextPruningWindowEnd(size_t begin, size_t mark_end)
{
    size_t window_end = mark_end;
    for (auto & bound : bound_cursors)
    {
        if (auto block_end = bound.cursor->nextBlockEnd(begin); block_end && *block_end < window_end)
            window_end = *block_end;
    }

    chassert(window_end > begin);
    return window_end;
}

void MergeTreeReaderTextIndex::initializePositionsStream()
{
    const auto & data_part = getDataPart();
    auto index_format = index.index->getDeserializedFormat(*data_part, index.index->getFileName());

    const auto positions_substream = std::ranges::find_if(
        index_format.substreams,
        [](const auto & substream) { return substream.type == MergeTreeIndexSubstream::Type::TextIndexPositions; });

    if (positions_substream == index_format.substreams.end())
        return;

    positions_stream = makeTextIndexInputStream(
        data_part->getDataPartStoragePtr(),
        index.index->getFileName() + positions_substream->suffix,
        positions_substream->extension,
        MergeTreeIndexReader::patchSettings(settings, positions_substream->type));

    positions_stream->seekToStart();
}

size_t MergeTreeReaderTextIndex::readRows(
    size_t from_mark,
    bool continue_reading,
    size_t max_rows_to_read,
    MutableColumns & res_columns)
{
    ProfileEventTimeIncrement<Microseconds> watch(ProfileEvents::TextIndexReaderTotalMicroseconds);
    const auto & index_granularity = data_part_info_for_read->getIndexGranularity();

    size_t from_row = 0;
    if (continue_reading)
    {
        from_mark = current_mark;
        from_row = current_row;
    }
    else
    {
        /// Backward jump invalidates the per-token cursor cache: cached cursors are
        /// forward-only (their `linearOr` / `linearAnd` / `advance` walk segments from
        /// `current_segment_idx` onward), so they cannot serve an earlier row.
        if (from_mark < current_mark)
            resetCursors();

        from_row = index_granularity.getMarkStartingRow(from_mark);
    }

    size_t total_rows = data_part_info_for_read->getRowCount();
    if (from_row < total_rows)
        max_rows_to_read = std::min(max_rows_to_read, total_rows - from_row);
    else
        max_rows_to_read = 0;

    if (res_columns.empty())
    {
        ++current_mark;
        current_row += max_rows_to_read;
        return max_rows_to_read;
    }

    size_t read_rows = 0;
    size_t total_marks = data_part_info_for_read->getIndexGranularity().getMarksCountWithoutFinal();

    if (!is_initialized && max_rows_to_read > 0)
    {
        /// Granule may be not set in the distributed index analysis.
        /// TODO: implement distributed index analysis for text index.
        if (!granule)
            readGranule();

        is_initialized = true;
        classifyVirtualColumns();
        chooseSparseVirtualColumns();
        initializePostingStreams();
        initializePositionsStream();
    }

    /// The columns are created after the analysis, which decides between full and sparse columns.
    createEmptyColumns(res_columns, max_rows_to_read);

    const bool any_use_fallback = !use_fallback.empty() && std::ranges::any_of(use_fallback, [](bool b) { return b; });

    /// If any column needs the fallback evaluation, read the physical columns upfront.
    /// We pass the same mark/continue_reading/offset arguments so the fallback reader stays
    /// in sync with the text-index reader across multiple readRows calls.
    Block fallback_block;
    if (any_use_fallback && fallback_reader && max_rows_to_read > 0)
    {
        MutableColumns fallback_cols(fallback_columns_list.size());
        fallback_reader->readRows(from_mark, continue_reading, max_rows_to_read, fallback_cols);
        size_t col_idx = 0;
        for (const auto & col_name_type : fallback_columns_list)
            fallback_block.insert({std::move(fallback_cols[col_idx++]), col_name_type.type, col_name_type.name});
    }

    /// The bound of a window is compared with the threshold after this relative slack, so the rounding of
    /// the `Float32` scores and bounds can never prune a row whose exact score reaches the threshold.
    static constexpr Float64 bound_slack = 1.0 + 1.0 / (1 << 20);
    const size_t first_row_of_call = from_row;

    while (read_rows < max_rows_to_read && from_mark < total_marks)
    {
        /// When the number of rows in a part is smaller than `index_granularity`,
        /// `MergeTreeReaderTextIndex` must ensure that the virtual column it reads
        /// contains no more data rows than actually exist in the part
        size_t rows_to_read = std::min(index_granularity.getMarkRows(from_mark), max_rows_to_read - read_rows);
        const size_t mark_end = from_row + rows_to_read;

        if (auto threshold = getPruningThreshold())
        {
            if (!score_leaves_initialized)
                initializeScoreLeaves();

            if (score_doc_lengths)
                score_doc_lengths->ensureRange(from_row, rows_to_read);

            /// A mark whose score bound stays below the threshold holds no row of the top-k: its rows are
            /// zero-filled and dropped by the `__topKFilter` PREWHERE without decoding the postings.
            if (scoreUpperBound(from_row, mark_end) * bound_slack < *threshold)
            {
                fillZeroRows(res_columns, rows_to_read);
                ProfileEvents::increment(ProfileEvents::TextScoreMarksPruned);
            }
            else
            {
                /// Inside the mark, the bound changes only at the block boundaries of the pruning tokens.
                size_t window_begin = from_row;
                while (window_begin < mark_end)
                {
                    size_t window_end = nextPruningWindowEnd(window_begin, mark_end);

                    if (scoreUpperBound(window_begin, window_end) * bound_slack < *threshold)
                    {
                        fillZeroRows(res_columns, window_end - window_begin);
                        ProfileEvents::increment(ProfileEvents::TextScoreWindowsPruned);
                    }
                    else
                    {
                        fillRows(res_columns, from_mark, window_begin, window_end - window_begin, fallback_block, window_begin - first_row_of_call);
                    }

                    window_begin = window_end;
                }
            }
        }
        else
        {
            fillRows(res_columns, from_mark, from_row, rows_to_read, fallback_block, from_row - first_row_of_call);
        }

        ++from_mark;
        from_row += rows_to_read;
        read_rows += rows_to_read;
    }

    /// Remove blocks that are no longer needed.
    if (auto rows_range = getRowsRangeForMark(from_mark - 1))
        cleanupPostingsBlocks(*rows_range);

    current_mark = from_mark;
    current_row = from_row;
    return read_rows;
}

void MergeTreeReaderTextIndex::createEmptyColumns(MutableColumns & columns, size_t max_rows_to_read) const
{
    for (size_t i = 0; i < columns.size(); ++i)
    {
        if (columns[i] != nullptr)
            continue;

        if (use_sparse[i])
        {
            /// A sparse column stores only the matching rows, so there is nothing worth reserving.
            /// The nested column follows the virtual column's own type: `UInt8` for a filter, `Float32` for a score.
            columns[i] = ColumnSparse::create(columns_to_read[i].type->createColumn());
        }
        else
        {
            auto column = columns_to_read[i].type->createColumn(*serializations[i]);
            column->reserve(max_rows_to_read);
            columns[i] = std::move(column);
        }
    }
}

void MergeTreeReaderTextIndex::fillRows(MutableColumns & res_columns, size_t mark, size_t row_offset, size_t num_rows, const Block & fallback_block, size_t fallback_offset)
{
    /// In lazy mode skip per-mark Roaring Bitmap materialization — cursors decode on demand.
    PostingList range_posting;
    std::vector<PostingList> mark_postings;

    if (!use_lazy_mode)
        mark_postings = buildPostingsForMark(mark, RowsRange(row_offset, row_offset + num_rows - 1), range_posting);

    for (size_t i = 0; i < res_columns.size(); ++i)
    {
        auto & column_mutable = *res_columns[i];
        const auto & search_query = search_queries[i];

        if (is_score_column[i])
        {
            fillColumnScores(column_mutable, i, row_offset, num_rows);
        }
        else if (is_always_true[i])
        {
            auto & column_data = assert_cast<ColumnUInt8 &>(column_mutable).getData();
            column_data.resize_fill(column_mutable.size() + num_rows, 1);
        }
        else if (use_fallback[i] && !fallback_block.empty())
        {
            fillColumnFallback(
                column_mutable,
                columns_to_read[i].name,
                fallback_block,
                fallback_offset,
                num_rows);
        }
        else if (search_query->getSearchMode() == TextSearchMode::Phrase)
        {
            /// Phrase queries are resolved from positional data (.pos), not per-mark posting lists.
            applyPostingsPhrase(column_mutable, search_query, row_offset, num_rows);
        }
        else if (use_lazy_mode)
        {
            fillColumnLazy(column_mutable, i, row_offset, num_rows, range_posting);
        }
        else
        {
            fillColumn(column_mutable, mark_postings[i], row_offset, num_rows);
        }
    }
}

void MergeTreeReaderTextIndex::fillZeroRows(MutableColumns & res_columns, size_t num_rows) const
{
    for (size_t i = 0; i < res_columns.size(); ++i)
    {
        /// Zero for a score column, no match for a filter column, no offsets for a sparse one:
        /// in every representation the pruned rows are the default value.
        res_columns[i]->insertManyDefaults(num_rows);
    }
}

std::unique_ptr<MergeTreeReaderStream> MergeTreeReaderTextIndex::makeTextIndexStream(const MergeTreeIndexSubstream & substream) const
{
    auto data_part = getDataPart();

    return makeTextIndexInputStream(
        data_part->getDataPartStoragePtr(),
        index.index->getFileName() + substream.suffix,
        substream.extension,
        MergeTreeIndexReader::patchSettings(settings, substream.type));
}

std::optional<RowsRange> MergeTreeReaderTextIndex::getRowsRangeForMark(size_t mark) const
{
    const auto & index_granularity = data_part_info_for_read->getIndexGranularity();
    size_t row_begin = index_granularity.getMarkStartingRow(mark);
    size_t row_end = index_granularity.getMarkStartingRow(mark + 1);

    if (row_begin == row_end)
        return {};

    return RowsRange(row_begin, row_end - 1);
}

std::vector<PostingList> MergeTreeReaderTextIndex::buildPostingsForMark(size_t mark, const RowsRange & slice_range, PostingList & range_posting)
{
    std::vector<PostingList> result(columns_to_read.size());
    auto mark_range = getRowsRangeForMark(mark);

    if (!mark_range.has_value())
        return result;

    /// Clip to `slice_range`, not the full mark, so postings stay in bounds on partial-mark
    /// reads (`max_rows_to_read` stops inside the mark).
    auto effective_range = mark_range->intersectWith(slice_range);
    if (!effective_range.has_value())
        return result;

    const auto & analyzer = granule->getAnalyzer();
    range_posting.addRangeClosed(static_cast<UInt32>(effective_range->begin), static_cast<UInt32>(effective_range->end));

    for (size_t i = 0; i < columns_to_read.size(); ++i)
    {
        if (is_always_true[i] || use_fallback[i] || is_score_column[i])
            continue;

        const auto & search_query = search_queries[i];
        if (search_query->getTokens().empty() && search_query->getPatterns().empty())
            continue;

        /// Phrase queries are resolved from positional data (.pos) in applyPostingsPhrase,
        /// not from per-mark posting lists.
        if (search_query->getSearchMode() == TextSearchMode::Phrase)
            continue;

        result[i] = buildPostingsForQuery(*search_query, analyzer, *effective_range, range_posting);
    }

    return result;
}

PostingList MergeTreeReaderTextIndex::buildPostingsForQuery(
    const TextSearchQuery & query,
    const TextIndexAnalyzer & analyzer,
    const RowsRange & range,
    PostingList & range_posting)
{
    const auto & query_builder = analyzer.getQueryBuilder(query);
    if (query_builder.is_failed)
        return {};

    std::optional<PostingList> result;
    if (query_builder.postings)
        result = *query_builder.postings & range_posting;

    if (!query_builder.needReadPostings())
        return result.value_or(PostingList{});

    for (const auto & [token, token_info] : query_builder.tokens)
    {
        if (!large_postings_streams.contains(token))
            continue;

        auto read_blocks = readPostingsBlocksForToken(token, *token_info, range);
        if (read_blocks.empty())
        {
            if (query.getSearchMode() == TextSearchMode::All)
                return {};
            else
                continue;
        }

        PostingList large_postings = (*read_blocks.front() & range_posting);
        for (size_t i = 1; i < read_blocks.size(); ++i)
            large_postings |= (*read_blocks[i] & range_posting);

        if (!result)
            result = std::move(large_postings);
        else if (query.getSearchMode() == TextSearchMode::All)
            *result &= large_postings;
        else if (query.getSearchMode() == TextSearchMode::Any)
            *result |= large_postings;

        if (query.getSearchMode() == TextSearchMode::All && result && result->isEmpty())
            return {};
    }

    return result.value_or(PostingList{});
}

std::vector<PostingListPtr> MergeTreeReaderTextIndex::readPostingsBlocksForToken(std::string_view token, const TokenPostingsInfo & token_info, const RowsRange & range)
{
    if (!postings_serialization.has_value())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Postings serialization is not set");

    auto blocks_to_read = token_info.getBlocksToRead(range);

    if (blocks_to_read.empty())
        return {};

    std::vector<PostingListPtr> result;
    for (const auto & block_idx : blocks_to_read)
    {
        auto * postings_stream = large_postings_streams.at(token).get();
        auto [it, inserted] = postings_blocks[token].try_emplace(block_idx);

        if (inserted)
        {
            it->second = MergeTreeIndexGranuleText::readPostingsBlock(
                *postings_stream,
                *deserialization_state,
                token_info,
                block_idx,
                postings_serialization.value(),
                granule->getIndexIdForCaches(),
                /*with_scoring=*/ false).postings;
        }

        result.push_back(it->second);
    }

    return result;
}

void MergeTreeReaderTextIndex::resetCursors()
{
    lazy_cursors.assign(lazy_cursors.size(), {});
    prebuilt_cursors.assign(prebuilt_cursors.size(), {});
    score_leaves.assign(score_leaves.size(), {});
    bound_cursors.clear();
    score_leaves_initialized = false;
}

void MergeTreeReaderTextIndex::cleanupPostingsBlocks(const RowsRange & range)
{
    if (!granule)
        return;

    const auto & analyzer = granule->getAnalyzer();
    const auto & token_infos = analyzer.getAllTokenInfos();

    for (const auto & [token, token_info] : token_infos)
    {
        auto it = postings_blocks.find(token);
        if (it == postings_blocks.end())
            continue;

        for (size_t i = 0; i < token_info->ranges.size(); ++i)
        {
            if (!token_info->ranges[i].intersects(range))
                it->second.erase(i);
        }
    }
}

void MergeTreeReaderTextIndex::fillColumn(IColumn & column, const PostingList & postings, size_t row_offset, size_t num_rows)
{
    size_t cardinality = postings.cardinality();
    if (cardinality == 0)
    {
        column.insertManyDefaults(num_rows);
        return;
    }

    indices_buffer.resize(cardinality);
    postings.toUint32Array(indices_buffer.data());
    appendMatchingRows(column, std::span<const UInt32>(indices_buffer.data(), cardinality), row_offset, num_rows);
}

void MergeTreeReaderTextIndex::fillColumnLazy(IColumn & column, size_t column_idx, size_t row_offset, size_t num_rows, PostingList & range_posting)
{
    const auto & search_query = search_queries[column_idx];
    chassert(search_query->getPatterns().empty());

    if (search_query->getTokens().empty())
    {
        /// hasAnyTokens / hasAllTokens whose needle tokens were all dropped (e.g. by a postprocessor): no
        /// match, so fill zeros for every row read, matching fillColumn and the row-scan path.
        column.insertManyDefaults(num_rows);
        return;
    }

    const auto & analyzer = granule->getAnalyzer();
    const auto & query_builder = analyzer.getQueryBuilder(*search_query);

    if (query_builder.is_failed)
    {
        column.insertManyDefaults(num_rows);
        return;
    }

    std::vector<PostingListCursorPtr> cursors;
    cursors.reserve(query_builder.tokens.size());

    if (query_builder.needReadPostings())
    {
        auto & column_cursors = lazy_cursors[column_idx];

        for (const auto & [token, token_info] : query_builder.tokens)
        {
            if (analyzer.hasReadPostings(token))
                continue;

            auto [it, inserted] = column_cursors.try_emplace(token);

            if (inserted)
                it->second = makeLazyCursor(token, *token_info);

            cursors.push_back(it->second);
        }
    }

    if (query_builder.postings)
    {
        /// Check the per-column cache first: the prebuilt cursor is built once and reused across marks.
        auto & prebuilt_cursor = prebuilt_cursors[column_idx];

        if (prebuilt_cursor)
        {
            cursors.push_back(prebuilt_cursor);
        }
        else if (!query_builder.postings->isEmpty())
        {
            /// If there are no cursors for large postings, fill the column directly from the postings.
            if (cursors.empty())
            {
                if (range_posting.isEmpty())
                {
                    requireRowOffsetRepresentable(row_offset);
                    auto range_end = static_cast<UInt32>(std::min<size_t>(row_offset + num_rows - 1, std::numeric_limits<UInt32>::max()));
                    range_posting.addRangeClosed(static_cast<UInt32>(row_offset), range_end);
                }

                PostingList clipped = *query_builder.postings & range_posting;
                fillColumn(column, clipped, row_offset, num_rows);
                return;
            }

            /// Convert postings to a sorted array and build a cursor from it.
            auto key = TextIndexPostingsCache::hash(granule->getIndexIdForCaches(), columns_to_read[column_idx].name, static_cast<UInt8>(TextIndexPostingsCacheKind::Flat));

            auto cell = condition_text->postingsCache()->getOrSet(key, [&]
            {
                auto flat = std::make_shared<PaddedPODArray<UInt32>>(query_builder.postings->cardinality());
                query_builder.postings->toUint32Array(flat->data());
                return std::make_shared<TextIndexPostingsCacheCell>(std::move(flat));
            });

            prebuilt_cursor = std::make_shared<PostingListCursor>(std::get<PaddedPODArrayPtr>(cell->value));
            cursors.push_back(prebuilt_cursor);
        }
    }

    if (cursors.empty())
    {
        column.insertManyDefaults(num_rows);
        return;
    }

    const auto search_mode = search_query->getSearchMode();
    if (search_mode != TextSearchMode::Any && search_mode != TextSearchMode::All)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Invalid search mode: {}", search_mode);

    if (auto * sparse_column = typeid_cast<ColumnSparse *>(&column))
    {
        /// The kernels append the offsets of the matching rows directly.
        auto & offsets = sparse_column->getOffsetsData();
        const size_t old_offsets_size = offsets.size();

        if (search_mode == TextSearchMode::Any)
            lazyUnionPostingListsSparse(offsets, sparse_column->size(), cursors, row_offset, num_rows);
        else
            lazyIntersectPostingListsSparse(offsets, sparse_column->size(), cursors, row_offset, num_rows);

        finishSparseRows(*sparse_column, offsets.size() - old_offsets_size, num_rows);
        return;
    }

    auto & column_data = assert_cast<ColumnUInt8 &>(column).getData();
    size_t old_size = column_data.size();
    column_data.resize_fill(old_size + num_rows, 0);
    UInt8 * out = column_data.data() + old_size;

    if (search_mode == TextSearchMode::Any)
        lazyUnionPostingLists(out, cursors, row_offset, num_rows);
    else
        lazyIntersectPostingLists(out, cursors, row_offset, num_rows, lazy_intersection_density_threshold);
}

PostingList MergeTreeReaderTextIndex::readAllPostingsForToken(std::string_view token, const TokenPostingsInfo & token_info)
{
    if (token_info.header & PostingsSerialization::Flags::EmbeddedPostings)
    {
        /// Embedded postings are stored as a flat sorted array in the dictionary.
        PostingList result;
        result.addMany(token_info.embedded_postings.size(), token_info.embedded_postings.data());
        return result;
    }

    if (!postings_serialization.has_value())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Postings serialization is not set");

    const size_t num_rows_in_part = data_part_info_for_read->getRowCount();
    const RowsRange full_range(0, num_rows_in_part ? num_rows_in_part - 1 : 0);
    const auto blocks_to_read = token_info.getBlocksToRead(full_range);

    PostingList result;
    for (const auto & block_idx : blocks_to_read)
    {
        MergeTreeReaderStream * postings_stream = nullptr;
        if (auto stream_it = large_postings_streams.find(token); stream_it != large_postings_streams.end())
        {
            postings_stream = stream_it->second.get();
        }
        else
        {
            if (!small_postings_stream)
                small_postings_stream = makeTextIndexStream(index.index->getSubstreams()[2]);
            postings_stream = small_postings_stream.get();
        }

        auto [it, inserted] = postings_blocks[token].try_emplace(block_idx);
        if (inserted)
        {
            it->second = MergeTreeIndexGranuleText::readPostingsBlock(
                *postings_stream,
                *deserialization_state,
                token_info,
                block_idx,
                postings_serialization.value(),
                granule->getIndexIdForCaches(),
                /*with_scoring=*/ false).postings;
        }

        result |= *it->second;
    }

    return result;
}

PaddedPODArray<UInt32> MergeTreeReaderTextIndex::phraseSearchBlocked(const TextSearchQuery & search_query)
{
    const auto & all_token_infos = granule->getAnalyzer().getAllTokenInfos();
    const auto & phrase_tokens = search_query.getPhraseTokens();

    /// Repeated phrase terms reuse one posting list and one decoded position stream.
    std::vector<std::string_view> unique_tokens;
    std::vector<const TokenPostingsInfo *> unique_infos;
    std::vector<size_t> term_to_unique;
    term_to_unique.reserve(phrase_tokens.size());
    for (const auto & token : phrase_tokens)
    {
        auto it = all_token_infos.find(token);
        if (it == all_token_infos.end() || !(it->second->header & PostingsSerialization::Flags::HasPositions))
            return {};

        size_t unique_idx = 0;
        while (unique_idx < unique_tokens.size() && unique_tokens[unique_idx] != token)
            ++unique_idx;
        if (unique_idx == unique_tokens.size())
        {
            unique_tokens.emplace_back(token);
            unique_infos.push_back(it->second.get());
        }
        term_to_unique.push_back(unique_idx);
    }

    /// Candidate rows = intersection of the phrase tokens' postings. The full per-token posting
    /// list is also the rank space the blocked position stream is addressed in.
    std::vector<PostingList> token_postings;
    token_postings.reserve(unique_tokens.size());
    for (size_t u = 0; u < unique_tokens.size(); ++u)
    {
        token_postings.push_back(readAllPostingsForToken(unique_tokens[u], *unique_infos[u]));
        if (token_postings.back().cardinality() == 0)
            return {};
    }

    PostingList intersection = token_postings[0];
    for (size_t u = 1; u < token_postings.size(); ++u)
    {
        intersection &= token_postings[u];
        if (intersection.cardinality() == 0)
            return {};
    }

    PaddedPODArray<UInt32> candidates(intersection.cardinality());
    intersection.toUint32Array(candidates.data());
    ProfileEvents::increment(ProfileEvents::TextIndexPhraseCandidates, candidates.size());

    /// A single-term "phrase" needs no positional check: every row containing the token matches.
    if (term_to_unique.size() == 1)
        return candidates;

    /// Bounded-memory chunked phrase match: precompute per-token candidate ranks, then process
    /// candidates in fixed chunks. Per chunk, decode only that chunk's covering blocks per token
    /// (token-sequential; consecutive blocks skip the reseek) into small reused buffers, run the
    /// two-pointer adjacency with per-candidate early-exit, and keep only matching doc ids. The full
    /// candidate position set is never materialized (the old per-token arrays cost ~GiB per phrase).
    const size_t pos_file_size = positions_stream->getFileSize();
    auto * data_buffer = positions_stream->getDataBuffer();

    std::vector<TextIndexBlockedPositionsCodec::Directory> dirs(unique_tokens.size());
    std::vector<PaddedPODArray<UInt64>> candidate_ranks(unique_tokens.size());
    size_t blocks_total = 0;
    UInt64 decode_us = 0;
    {
        Stopwatch prep_watch;
        PaddedPODArray<UInt32> posting_docs;
        for (size_t u = 0; u < unique_tokens.size(); ++u)
        {
            const auto & token_info = *unique_infos[u];
            /// Checked before seeking: an offset outside the stream would leave the buffer out of range.
            if ((token_info.position_bytes == 0) || (token_info.position_offset > pos_file_size)
                || (token_info.position_bytes > pos_file_size - token_info.position_offset))
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "Corrupt text index positions: blob of {} bytes at offset {} is outside the {}-byte stream",
                    token_info.position_bytes, token_info.position_offset, pos_file_size);
            positions_stream->seekToMark({token_info.position_offset, 0});
            const size_t available = token_info.position_bytes;
            /// Candidate ranks in this token's postings. Dense candidates: one linear walk over the
            /// materialized list beats per-candidate roaring rank(); sparse: rank() wins.
            const auto & postings = token_postings[u];

            /// readDirectory rejects a blob whose document count disagrees with the postings. That
            /// equality is what bounds a rank below num_docs, so the block index needs no check.
            dirs[u] = TextIndexBlockedPositionsCodec::readDirectory(
                *data_buffer, token_info.position_offset, postings.cardinality(), available);
            blocks_total += dirs[u].numBlocks();

            auto & ranks = candidate_ranks[u];
            ranks.resize(candidates.size());
            if (const UInt64 postings_cardinality = postings.cardinality(); candidates.size() * 16 >= postings_cardinality)
            {
                posting_docs.resize(postings_cardinality);
                postings.toUint32Array(posting_docs.data());
                size_t doc_idx = 0;
                for (size_t i = 0; i < candidates.size(); ++i)
                {
                    while (posting_docs[doc_idx] < candidates[i])
                        ++doc_idx;
                    ranks[i] = doc_idx; /// candidates are members: posting_docs[doc_idx] == candidates[i]
                }
            }
            else
            {
                /// roaring rank() is 1-based for the smallest element; candidates are members.
                for (size_t i = 0; i < candidates.size(); ++i)
                    ranks[i] = postings.rank(candidates[i]) - 1;
            }
        }
        decode_us += prep_watch.elapsedMicroseconds();
    }

    size_t blocks_read = 0;
    UInt64 block_bytes_read = 0;
    UInt64 block_decode_us = 0;
    std::vector<UInt32> block_local_ranks;

    /// Decode candidates [lo, hi) of token `u` into offsets/positions (offsets seeded with a leading
    /// 0; indices chunk-relative). Blocks decode in ascending order, reseeking only on a block gap.
    auto decode_chunk = [&](size_t u, size_t lo, size_t hi, PaddedPODArray<UInt32> & offsets, PaddedPODArray<UInt32> & positions)
    {
        Stopwatch sw;
        const auto & dir = dirs[u];
        const auto & ranks = candidate_ranks[u];
        offsets.clear();
        positions.clear();
        offsets.push_back(0);
        size_t previous_block = std::numeric_limits<size_t>::max();
        for (size_t idx = lo; idx < hi;)
        {
            const size_t block_idx = ranks[idx] / TextIndexBlockedPositionsCodec::BLOCK_DOCS;
            block_local_ranks.clear();
            block_local_ranks.push_back(static_cast<UInt32>(ranks[idx] % TextIndexBlockedPositionsCodec::BLOCK_DOCS));
            ++idx;
            while (idx < hi && ranks[idx] / TextIndexBlockedPositionsCodec::BLOCK_DOCS == block_idx)
            {
                block_local_ranks.push_back(static_cast<UInt32>(ranks[idx] % TextIndexBlockedPositionsCodec::BLOCK_DOCS));
                ++idx;
            }
            if (previous_block == std::numeric_limits<size_t>::max() || block_idx != previous_block + 1)
                positions_stream->seekToMark({dir.block_offsets[block_idx], 0});
            TextIndexBlockedPositionsCodec::decodeBlock(
                *data_buffer, dir, block_idx, block_local_ranks, offsets, positions, blocked_positions_scratch);
            previous_block = block_idx;
            ++blocks_read;
            block_bytes_read += dir.block_offsets[block_idx + 1] - dir.block_offsets[block_idx];
        }
        block_decode_us += sw.elapsedMicroseconds();
    };

    static constexpr size_t CHUNK = 1 << 16;
    PaddedPODArray<UInt32> matching;
    std::vector<PaddedPODArray<UInt32>> chunk_offsets(unique_tokens.size());
    std::vector<PaddedPODArray<UInt32>> chunk_positions(unique_tokens.size());
    UInt64 match_us = 0;

    for (size_t chunk_lo = 0; chunk_lo < candidates.size(); chunk_lo += CHUNK)
    {
        const size_t chunk_hi = std::min(candidates.size(), chunk_lo + CHUNK);
        for (size_t u = 0; u < unique_tokens.size(); ++u)
            decode_chunk(u, chunk_lo, chunk_hi, chunk_offsets[u], chunk_positions[u]);

        Stopwatch match_watch;
        TextIndexPhraseSearch::matchCandidatePositions(
            std::span<const UInt32>(candidates.data() + chunk_lo, chunk_hi - chunk_lo),
            chunk_offsets, chunk_positions, term_to_unique, matching);
        match_us += match_watch.elapsedMicroseconds();
    }
    decode_us += block_decode_us;

    ProfileEvents::increment(ProfileEvents::TextIndexPositionsDecodeMicroseconds, decode_us);
    ProfileEvents::increment(ProfileEvents::TextIndexPhraseMatchMicroseconds, match_us);
    ProfileEvents::increment(ProfileEvents::TextIndexPositionsBlocksRead, blocks_read);
    ProfileEvents::increment(ProfileEvents::TextIndexPositionsBlocksTotal, blocks_total);
    ProfileEvents::increment(ProfileEvents::TextIndexPositionsBytesRead, block_bytes_read);
    return matching;
}

void MergeTreeReaderTextIndex::applyPostingsPhrase(
    IColumn & column,
    const TextSearchQueryPtr & search_query,
    size_t row_offset,
    size_t num_rows)
{
    if (!positions_stream || search_query->getPhraseTokens().empty())
    {
        column.insertManyDefaults(num_rows);
        return;
    }

    auto cache_key = search_query->getHash();
    auto doc_ids_it = phrase_search_doc_ids.find(cache_key);

    if (doc_ids_it == phrase_search_doc_ids.end())
    {
        /// Phrase result is a posting list (sorted doc-ids): computed once per (part, query) via the postings cache (Phrase key), shared across the part's readers.
        auto phrase_key = TextIndexPostingsCache::hash(
            granule->getIndexIdForCaches(), cache_key, static_cast<UInt8>(TextIndexPostingsCacheKind::Phrase));

        auto cell = condition_text->postingsCache()->getOrSet(phrase_key, [&]
        {
            /// The header deserialization rejects any codec but Blocked, so the part's positions
            /// are always the blocked candidate-driven layout here.
            chassert(static_cast<TextIndexPositionCodec::Encoding>(granule->getPositionsCodec()) == TextIndexPositionCodec::Encoding::BlockedPfor);
            ProfileEvents::increment(ProfileEvents::TextIndexPhraseSearches);
            return std::make_shared<TextIndexPostingsCacheCell>(
                std::make_shared<PaddedPODArray<UInt32>>(phraseSearchBlocked(*search_query)));
        });

        doc_ids_it = phrase_search_doc_ids.emplace(cache_key, std::get<PaddedPODArrayPtr>(cell->value)).first;
    }

    const auto & matching_doc_ids = *doc_ids_it->second;
    const size_t window_end = row_offset + num_rows;
    const auto * window_begin = std::ranges::lower_bound(matching_doc_ids, row_offset);
    const auto * window_last = std::lower_bound(window_begin, matching_doc_ids.end(), window_end);
    appendMatchingRows(column, std::span<const UInt32>(window_begin, window_last), row_offset, num_rows);
}

void MergeTreeReaderTextIndex::fillColumnFallback(
    IColumn & column,
    const String & column_name,
    const Block & physical_block,
    size_t offset,
    size_t num_rows) const
{
    auto it = fallback_expressions.find(column_name);
    chassert(it != fallback_expressions.end());

    /// Build a block slice for this granule: cut [offset, offset + num_rows) from each physical column.
    Block slice;
    for (const auto & col : physical_block)
        slice.insert({col.column->cut(offset, num_rows), col.type, col.name});

    /// Execute the virtual column's default expression (the original search predicate) on the slice.
    /// After execution the block contains both the physical columns and the computed virtual column.
    it->second->execute(slice);

    /// The predicate result can be sparse/const (inputs may be sparse), so make it full before the dense cast.
    const auto & result_col = slice.getByName(column_name);
    auto result_full = result_col.column->convertToFullIfWrapped();
    const auto & result_data = assert_cast<const ColumnUInt8 &>(*result_full).getData();
    chassert(result_data.size() == num_rows);

    auto & column_data = assert_cast<ColumnUInt8 &>(column).getData();
    const size_t old_size = column_data.size();
    column_data.resize(old_size + num_rows);
    memcpy(&column_data[old_size], result_data.data(), num_rows);
}

void MergeTreeReaderTextIndex::setPrecomputedGranule(const IndexGranulesMap & granules)
{
    auto it = granules.find(index.index->index.name);

    if (it != granules.end() && it->second)
    {
        resetCursors();
        postings_blocks.clear();
        setIndexGranule(it->second);
    }
}

MergeTreeReaderPtr createMergeTreeReaderTextIndex(
    const IMergeTreeReader * main_reader,
    const MergeTreeIndexWithCondition & index,
    const NamesAndTypesList & columns_to_read,
    MergeTreeIndexGranulePtr index_granule,
    BM25StatePtr bm25_score_state,
    TopKThresholdTrackerPtr bm25_threshold_tracker)
{
    return std::make_unique<MergeTreeReaderTextIndex>(main_reader, index, columns_to_read, std::move(index_granule), std::move(bm25_score_state), std::move(bm25_threshold_tracker));
}

}
