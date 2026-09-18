#include <Storages/MergeTree/TextIndexAnalyzer.h>
#include <Common/ProfileEvents.h>
#include <Common/typeid_cast.h>
#include <algorithm>
#include <cmath>

namespace ProfileEvents
{
    extern const Event TextIndexUseHint;
    extern const Event TextIndexDiscardHint;
    extern const Event TextIndexUsedEmbeddedPostings;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}


TextIndexAnalyzer::ReadableRows::ReadableRows(std::vector<RowsRange> ranges_)
    : ranges(std::move(ranges_))
{
}

std::optional<RowsRange> TextIndexAnalyzer::ReadableRows::clipRowsRange(const RowsRange & rows_range) const
{
    /// First readable range whose end reaches the span begin; ranges before it cannot overlap.
    auto it = std::lower_bound(
        ranges.begin(), ranges.end(), rows_range.begin,
        [](const RowsRange & range, size_t value) { return range.end < value; });

    std::optional<RowsRange> clipped;
    for (; it != ranges.end() && it->begin <= rows_range.end; ++it)
    {
        size_t begin = std::max(rows_range.begin, it->begin);
        size_t end = std::min(rows_range.end, it->end);

        if (begin > end)
            continue;

        if (!clipped)
            clipped = RowsRange(begin, end);
        else
            clipped->end = end; /// extend the coarse single-interval cover to the last overlap
    }

    return clipped;
}

const PostingList & TextIndexAnalyzer::ReadableRows::getBitmap()
{
    if (ranges_bitmap.isEmpty())
    {
        /// `addRangeClosed` stores contiguous ranges as run containers, so this stays compact (O(number of ranges)).
        for (const auto & range : ranges)
            ranges_bitmap.addRangeClosed(static_cast<UInt32>(range.begin), static_cast<UInt32>(range.end));
    }

    return ranges_bitmap;
}

void TextIndexAnalyzer::QueryBuilder::markFailed()
{
    is_failed = true;
    intersected_postings.reset();
    united_postings.reset();
    rows_range.reset();
    num_live_tokens = 0;
}

bool TextIndexAnalyzer::QueryBuilder::hasEmptyPostings() const
{
    if (intersected_postings)
        return intersected_postings->empty();

    if (united_postings)
        return united_postings->isEmpty();

    return true;
}

size_t TextIndexAnalyzer::QueryBuilder::getPostingsCardinality() const
{
    if (intersected_postings)
        return intersected_postings->size();

    if (united_postings)
        return united_postings->cardinality();

    return 0;
}

bool TextIndexAnalyzer::QueryBuilder::hasPostingsInRange(const RowsRange & range) const
{
    if (intersected_postings)
    {
        auto it = std::lower_bound(intersected_postings->begin(), intersected_postings->end(), range.begin);
        return it != intersected_postings->end() && *it <= range.end;
    }

    if (united_postings)
    {
        /// An allocation-free check that the folded posting list has a value in the closed range of rows.
        return roaring::api::roaring_bitmap_intersect_with_range(
            &united_postings->roaring,
            range.begin,
            static_cast<UInt64>(range.end) + 1);
    }

    return false;
}

PostingList TextIndexAnalyzer::QueryBuilder::getPostingsInRange(const RowsRange & range) const
{
    if (intersected_postings)
    {
        auto begin = std::lower_bound(intersected_postings->begin(), intersected_postings->end(), range.begin);
        auto end = std::upper_bound(begin, intersected_postings->end(), range.end);
        return PostingList(static_cast<size_t>(end - begin), begin);
    }

    if (united_postings)
    {
        /// A single run container, so the intersection touches only the containers of the range and never copies the bitmap.
        PostingList range_bitmap;
        range_bitmap.addRangeClosed(static_cast<UInt32>(range.begin), static_cast<UInt32>(range.end));
        return *united_postings & range_bitmap;
    }

    return {};
}

PostingList TextIndexAnalyzer::QueryBuilder::getPostingsAsBitmap() const
{
    if (intersected_postings)
        return PostingList(intersected_postings->size(), intersected_postings->data());

    if (united_postings)
        return *united_postings;

    return {};
}

void TextIndexAnalyzer::QueryBuilder::markBypassed()
{
    is_bypassed = true;
    /// Keep the folded postings and `rows_range` for index analysis in `mayBeTrueOnGranule`.
    /// Bypassing a query makes sense only for direct read optimization.
}

void TextIndexAnalyzer::QueryBuilder::addMissingToken(std::string_view token)
{
    tokens.erase(token);

    if (query->getSearchMode() == TextSearchMode::All || query->getSearchMode() == TextSearchMode::Phrase)
    {
        markFailed();
        return;
    }

    /// `Any` mode fails once none of its declared tokens can contribute.
    /// Pattern queries discover tokens dynamically, so the count applies only to pure-token queries.
    if (query->getPatterns().empty())
    {
        if (num_live_tokens > 0)
            --num_live_tokens;

        if (num_live_tokens == 0)
            markFailed();
    }
}

void TextIndexAnalyzer::QueryBuilder::addTokenInfo(std::string_view token, TokenPostingsInfoPtr token_info, RowsRange token_rows_range)
{
    if (is_failed || tokens.contains(token))
        return;

    tokens[token] = token_info;
    addRowsRange(token_rows_range);
}

void TextIndexAnalyzer::QueryBuilder::addRowsRange(RowsRange token_rows_range)
{
    if (is_failed)
        return;

    if (!rows_range)
    {
        rows_range = token_rows_range;
    }
    else if (query->getSearchMode() == TextSearchMode::Any)
    {
        rows_range = rows_range->unionWith(token_rows_range);
    }
    else if (query->getSearchMode() == TextSearchMode::All || query->getSearchMode() == TextSearchMode::Phrase)
    {
        rows_range = rows_range->intersectWith(token_rows_range);

        if (!rows_range)
            markFailed();
    }
}

TextIndexAnalyzer::TextIndexAnalyzer(const MergeTreeIndexConditionText & condition_text)
{
    global_search_mode = condition_text.getGlobalSearchMode();

    for (const auto & [hash, query] : condition_text.getAllSearchQueries())
    {
        auto & query_builder = query_builders[hash];
        query_builder.query = query;

        for (const auto & token : query->getTokens())
        {
            if (queries_by_token[token].insert(hash).second)
                ++query_builder.num_live_tokens;
        }

        for (const auto & pattern : query->getPatterns())
            queries_by_pattern[&pattern].insert(hash);
    }
}

const TextIndexAnalyzer::QueryBuilder & TextIndexAnalyzer::getQueryBuilder(const TextSearchQuery & query) const
{
    auto hash = query.getHash();
    auto it = query_builders.find(hash);

    if (it == query_builders.end())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Query builder not found for text search query with function '{}'", query.getFunctionName());

    return it->second;
}

void TextIndexAnalyzer::addMissingToken(std::string_view token)
{
    missing_tokens.emplace(token);

    processTokenOperation(token, [&](QueryBuilder & query_builder)
    {
        query_builder.addMissingToken(token);
    });
}

void TextIndexAnalyzer::addTokenInfo(std::string_view token, TokenPostingsInfoPtr token_info)
{
    all_token_infos[token] = token_info;

    /// Clip the token's row range to the readable rows once.
    chassert(!token_info->ranges.empty());
    RowsRange token_rows_range(token_info->ranges.front().begin, token_info->ranges.back().end);

    if (readable_rows)
    {
        auto clipped_range = readable_rows->clipRowsRange(token_rows_range);

        if (!clipped_range)
        {
            processTokenOperation(token, [&](QueryBuilder & query_builder)
            {
                query_builder.addMissingToken(token);
            });

            queries_by_token.erase(token);
            return;
        }

        token_rows_range = *clipped_range;
    }

    processTokenOperation(token, [&](QueryBuilder & query_builder)
    {
        query_builder.addTokenInfo(token, token_info, token_rows_range);
    });

    if (!token_info->embedded_postings.empty())
    {
        applyPostings(token, std::span<const UInt32>(token_info->embedded_postings.data(), token_info->embedded_postings.size()));
        ProfileEvents::increment(ProfileEvents::TextIndexUsedEmbeddedPostings);
    }
}

TextIndexAnalyzer::PostingsApplyPlan TextIndexAnalyzer::planApplyPostings(std::string_view token)
{
    PostingsApplyPlan plan;

    auto it = queries_by_token.find(token);
    if (it == queries_by_token.end())
        return plan;

    if (readable_rows)
    {
        plan.targets.readable_ranges = &readable_rows->getRanges();
        plan.targets.readable_bitmap = &readable_rows->getBitmap();
    }

    for (const auto & query_hash : it->second)
    {
        auto & query_builder = query_builders.at(query_hash);
        if (query_builder.is_failed || query_builder.is_bypassed)
            continue;

        if (query_builder.query->getSearchMode() == TextSearchMode::Any)
        {
            if (!query_builder.united_postings)
                query_builder.united_postings.emplace();

            plan.targets.unite.push_back({.postings = &*query_builder.united_postings});
            plan.unite_queries.push_back(query_hash);
        }
        else
        {
            bool initialized = query_builder.intersected_postings != nullptr;
            if (!initialized)
                query_builder.intersected_postings = std::make_shared<PaddedPODArray<UInt32>>();

            plan.targets.intersect.push_back({.rows = query_builder.intersected_postings.get(), .initialized = initialized});
            plan.intersect_queries.push_back(query_hash);
        }
    }

    return plan;
}

void TextIndexAnalyzer::finishApplyPostings(std::string_view token, const PostingsApplyPlan & plan)
{
    tokens_with_postings.emplace(token);

    for (size_t i = 0; i < plan.intersect_queries.size(); ++i)
    {
        const auto & query_hash = plan.intersect_queries[i];
        auto & query_builder = query_builders.at(query_hash);

        /// A query failed earlier in this loop (through `markAllQueriesFailed`) is already detached.
        if (query_builder.is_failed || query_builder.is_bypassed)
            continue;

        ++query_builder.num_read_postings;

        /// `All` mode fails as soon as the running intersection of readable postings becomes empty.
        if (query_builder.intersected_postings->empty())
            query_builder.markFailed();

        handleFailedQuery(query_hash, query_builder);
    }

    for (size_t i = 0; i < plan.unite_queries.size(); ++i)
    {
        const auto & query_hash = plan.unite_queries[i];
        auto & query_builder = query_builders.at(query_hash);

        if (query_builder.is_failed || query_builder.is_bypassed)
            continue;

        if (plan.targets.unite[i].num_applied == 0)
        {
            /// The token has no readable rows: the same as a token missing from the dictionary.
            /// A union never shrinks, so an empty bitmap means that nothing has been folded yet.
            if (query_builder.united_postings->isEmpty())
                query_builder.united_postings.reset();

            query_builder.addMissingToken(token);
        }
        else
        {
            ++query_builder.num_read_postings;
        }

        handleFailedQuery(query_hash, query_builder);
    }
}

void TextIndexAnalyzer::applyPostings(std::string_view token, std::span<const UInt32> sorted_postings)
{
    auto plan = planApplyPostings(token);

    if (!plan.targets.empty())
    {
        PostingsApplier applier(plan.targets);
        applier.applyRows(sorted_postings);
        applier.finish();
    }

    finishApplyPostings(token, plan);
}

void TextIndexAnalyzer::applyPostings(std::string_view token, const PostingList & postings)
{
    auto plan = planApplyPostings(token);

    if (!plan.targets.empty())
    {
        PostingsApplier applier(plan.targets);
        applier.applyBitmap(postings);
        applier.finish();
    }

    finishApplyPostings(token, plan);
}

void TextIndexAnalyzer::setReadableRows(std::vector<RowsRange> readable_ranges)
{
    readable_rows.reset();

    if (!readable_ranges.empty())
        readable_rows.emplace(std::move(readable_ranges));
}

bool TextIndexAnalyzer::addTokenToPatterns(std::string_view token)
{
    bool added = false;

    for (const auto & [pattern, query_hashes] : queries_by_pattern)
    {
        if (pattern->match(token.data(), token.size()))
        {
            added = true;

            for (const auto & query_hash : query_hashes)
                queries_by_token[token].emplace(query_hash);
        }
    }

    return added;
}

bool TextIndexAnalyzer::isTokenNeeded(std::string_view token) const
{
    auto it = queries_by_token.find(token);
    return it != queries_by_token.end() && !it->second.empty();
}

bool TextIndexAnalyzer::hasReadPostings(std::string_view token) const
{
    return tokens_with_postings.contains(token);
}

void TextIndexAnalyzer::bypassPatternQueries()
{
    QueryHashes all_pattern_queries;
    for (const auto & [_, query_hashes] : queries_by_pattern)
    {
        for (const auto & query_hash : query_hashes)
            all_pattern_queries.insert(query_hash);
    }

    for (const auto & query_hash : all_pattern_queries)
    {
        auto & query_builder = query_builders.at(query_hash);
        query_builder.markBypassed();
        query_builder.is_analysis_incomplete = true;

        for (const auto & [query_token, _] : query_builder.tokens)
            queries_by_token[query_token].erase(query_hash);
    }
}

double TextIndexAnalyzer::estimateQueryCardinality(const QueryBuilder & query_builder, size_t total_rows) const
{
    const auto & query = *query_builder.query;
    chassert(!query.getTokens().empty() || !query.getPatterns().empty());
    const double n = static_cast<double>(total_rows);

    switch (query.getSearchMode())
    {
        case TextSearchMode::All:
        /// A phrase requires all its tokens to be present.
        case TextSearchMode::Phrase:
        {
            /// |intersection| ≈ |C_read| * prod(|Ai|/n) over tokens whose postings are still unread.
            /// When no postings have been read yet, treat the read intersection as the universe (n).
            /// In log-space: log = log(|C_read|) + sum(log(|Ai|)) - num_unread * log(n).
            double log_cardinality = query_builder.hasPostings()
                ? std::log(static_cast<double>(query_builder.getPostingsCardinality()))
                : std::log(n);

            size_t num_unread = 0;
            for (const auto & token : query.getTokens())
            {
                auto it = query_builder.tokens.find(token);
                if (it == query_builder.tokens.end())
                    return 0;

                if (hasReadPostings(token))
                    continue;

                log_cardinality += std::log(static_cast<double>(it->second->cardinality));
                ++num_unread;
            }

            log_cardinality -= static_cast<double>(num_unread) * std::log(n);
            return std::exp(log_cardinality);
        }
        case TextSearchMode::Any:
        {
            /// |union| ≈ n * (1 - (1 - |C_read|/n) * prod(1 - |Ai|/n)) over tokens whose postings are still unread.
            double not_in_any = query_builder.hasPostings()
                ? 1.0 - static_cast<double>(query_builder.getPostingsCardinality()) / n
                : 1.0;

            /// A pattern query declares no tokens, it owns the ones the dictionary scan matched.
            if (query.getTokens().empty())
            {
                for (const auto & [token, token_info] : query_builder.tokens)
                {
                    if (hasReadPostings(token))
                        continue;

                    not_in_any *= (1.0 - static_cast<double>(token_info->cardinality) / n);
                }

                return n * (1.0 - not_in_any);
            }

            for (const auto & token : query.getTokens())
            {
                auto it = query_builder.tokens.find(token);
                if (it != query_builder.tokens.end() && hasReadPostings(token))
                    continue;

                /// Same reasoning as the prior reader-side estimate: a token absent from the
                /// sparse index was filtered as too common at build time ⟹ treat it as covering
                /// all rows, which makes the union saturate at n.
                double token_cardinality = (it == query_builder.tokens.end())
                    ? n
                    : static_cast<double>(it->second->cardinality);

                not_in_any *= (1.0 - token_cardinality / n);
            }

            return n * (1.0 - not_in_any);
        }
    }
}

void TextIndexAnalyzer::analyzeCardinalitiesAndBypassHints(double selectivity_threshold, size_t total_rows)
{
    if (total_rows == 0)
        return;

    const double cardinality_threshold = static_cast<double>(total_rows) * selectivity_threshold;

    for (auto & [_, query_builder] : query_builders)
    {
        if (query_builder.is_failed || query_builder.is_bypassed)
            continue;

        const auto & query = *query_builder.query;
        if (query.getDirectReadMode() != TextIndexDirectReadMode::Hint)
            continue;

        /// A pure-pattern query is estimated from the tokens the dictionary scan discovered.
        if (query.getTokens().empty() && query_builder.tokens.empty())
            continue;

        double estimated_cardinality = estimateQueryCardinality(query_builder, total_rows);

        if (estimated_cardinality <= cardinality_threshold)
        {
            ProfileEvents::increment(ProfileEvents::TextIndexUseHint);
        }
        else
        {
            /// Drop the query from `queries_by_token` so pattern discovery and `isTokenNeeded`
            /// stop reactivating it; the folded postings and `rows_range` are preserved for `mayBeTrueOnGranule`.
            query_builder.markBypassed();
            ProfileEvents::increment(ProfileEvents::TextIndexDiscardHint);

            auto hash = query.getHash();
            for (const auto & query_token : query.getTokens())
                queries_by_token[query_token].erase(hash);

            for (const auto & [query_token, _] : query_builder.tokens)
                queries_by_token[query_token].erase(hash);
        }
    }
}

void TextIndexAnalyzer::detachQueryFromTokens(const UInt128 & query_hash, const QueryBuilder & query_builder)
{
    /// Detach the full declared token set so yet-unseen tokens stop passing isTokenNeeded.
    for (const auto & query_token : query_builder.query->getTokens())
        queries_by_token[query_token].erase(query_hash);

    /// Also detach already-discovered dynamic pattern tokens (not in `query->getTokens`).
    for (const auto & [query_token, _] : query_builder.tokens)
        queries_by_token[query_token].erase(query_hash);
}

void TextIndexAnalyzer::markAllQueriesFailed()
{
    always_false = true;

    for (auto & [query_hash, query_builder] : query_builders)
    {
        if (query_builder.is_failed)
            continue;

        query_builder.markFailed();
        detachQueryFromTokens(query_hash, query_builder);
    }
}

template <typename Operation>
void TextIndexAnalyzer::processTokenOperation(std::string_view token, Operation && operation)
{
    /// Copy the set of query hashes before iterating, because
    /// erasing a failed query from queries_by_token below may
    /// mutate this very set (when query_token == token).
    auto token_queries = queries_by_token.at(token);

    for (const auto & query_hash : token_queries)
    {
        auto & query_builder = query_builders.at(query_hash);
        if (query_builder.is_failed || query_builder.is_bypassed)
            continue;

        operation(query_builder);
        handleFailedQuery(query_hash, query_builder);
    }
}

void TextIndexAnalyzer::handleFailedQuery(const UInt128 & query_hash, const QueryBuilder & query_builder)
{
    if (!query_builder.is_failed)
        return;

    detachQueryFromTokens(query_hash, query_builder);

    /// One failed query in `All` global mode proves the whole conjunction false in this
    /// part; the remaining queries cannot contribute to the result, so fail them all.
    if (global_search_mode == TextSearchMode::All)
        markAllQueriesFailed();
}

}
