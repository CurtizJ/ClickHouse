#include <Storages/MergeTree/TextIndexAnalyzer.h>
#include <Storages/MergeTree/PostingListBlockCodec.h>
#include <Columns/ColumnString.h>
#include <Common/ProfileEvents.h>
#include <Common/StringUtils.h>
#include <Common/typeid_cast.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ProfileEvents
{
    extern const Event TextIndexUseHint;
    extern const Event TextIndexDiscardHint;
    extern const Event TextIndexUsedEmbeddedPostings;
    extern const Event TextIndexAnalysisPackedBlocksDecoded;
    extern const Event TextIndexAnalysisPackedBlocksSkipped;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}


const PostingList & TokenPostings::getBitmap()
{
    if (bitmap)
        return *bitmap;

    if (!bitmap_of_array)
        bitmap_of_array = buildPostingListFromSortedRowIds(*array);

    return *bitmap_of_array;
}

const PaddedPODArrayPtr & TokenPostings::getArray()
{
    if (!array)
    {
        auto rows = std::make_shared<PaddedPODArray<UInt32>>(bitmap->cardinality());
        bitmap->toUint32Array(rows->data());
        array = std::move(rows);
    }

    return array;
}

namespace
{

/// Like `std::lower_bound`, but looks from the beginning with exponentially growing steps first, so a value close to
/// the beginning is found in a few steps that stay in the cache. Used to look up the sorted rows of a small array in
/// a large one one by one.
const UInt32 * gallopLowerBound(const UInt32 * first, const UInt32 * last, UInt32 value)
{
    const size_t size = last - first;
    size_t bound = 1;

    while (bound < size && first[bound - 1] < value)
        bound *= 2;

    return std::lower_bound(first + bound / 2, first + std::min(bound, size), value);
}

/// Merges two sorted arrays of unique rows. A much smaller array is merged into the larger one by looking up
/// its rows there and copying the runs of the larger array between them.
PaddedPODArrayPtr uniteSortedArrays(const PaddedPODArray<UInt32> & lhs, const PaddedPODArray<UInt32> & rhs)
{
    const auto & small = lhs.size() <= rhs.size() ? lhs : rhs;
    const auto & large = lhs.size() <= rhs.size() ? rhs : lhs;

    auto result = std::make_shared<PaddedPODArray<UInt32>>(small.size() + large.size());
    UInt32 * out = result->data();

    if (small.size() * 32 < large.size())
    {
        const UInt32 * pos = large.begin();
        for (const UInt32 row : small)
        {
            const UInt32 * next = gallopLowerBound(pos, large.end(), row);
            out = std::copy(pos, next, out);
            pos = next;

            if (pos == large.end() || *pos != row)
                *out++ = row;
        }

        out = std::copy(pos, large.end(), out);
    }
    else
    {
        out = std::set_union(small.begin(), small.end(), large.begin(), large.end(), out);
    }

    result->resize(out - result->data());
    return result;
}

/// Intersects two sorted arrays of unique rows. The rows of a much smaller array are looked up in the larger one.
PaddedPODArrayPtr intersectSortedArrays(const PaddedPODArray<UInt32> & lhs, const PaddedPODArray<UInt32> & rhs)
{
    const auto & small = lhs.size() <= rhs.size() ? lhs : rhs;
    const auto & large = lhs.size() <= rhs.size() ? rhs : lhs;

    auto result = std::make_shared<PaddedPODArray<UInt32>>(small.size());
    UInt32 * out = result->data();

    if (small.size() * 32 < large.size())
    {
        const UInt32 * pos = large.begin();
        for (const UInt32 row : small)
        {
            pos = gallopLowerBound(pos, large.end(), row);
            if (pos == large.end())
                break;

            if (*pos == row)
                *out++ = row;
        }
    }
    else
    {
        out = std::set_intersection(small.begin(), small.end(), large.begin(), large.end(), out);
    }

    result->resize(out - result->data());
    return result;
}

}

namespace
{

/// The number of rows two sorted arrays of unique rows have in common. The rows of the smaller array are looked up in the larger one.
size_t countCommonRows(const PaddedPODArray<UInt32> & lhs, const PaddedPODArray<UInt32> & rhs)
{
    const auto & small = lhs.size() <= rhs.size() ? lhs : rhs;
    const auto & large = lhs.size() <= rhs.size() ? rhs : lhs;

    size_t num_common = 0;
    const UInt32 * pos = large.begin();

    for (const UInt32 row : small)
    {
        pos = gallopLowerBound(pos, large.end(), row);
        if (pos == large.end())
            break;

        num_common += *pos == row;
    }

    return num_common;
}

}

FoldedPostings::FoldedPostings(TokenPostings & postings, bool as_array)
{
    if (as_array)
        array = postings.getArray();
    else
        bitmap = postings.getBitmap();
}

size_t FoldedPostings::cardinality() const
{
    chassert(!isUnion() || is_finalized);

    if (array)
        return array->size();

    return isUnion() ? union_cardinality : bitmap.cardinality();
}

bool FoldedPostings::hasRowsInRange(UInt32 begin, UInt32 end) const
{
    chassert(!isUnion() || is_finalized);

    const auto has_rows_in_range = [&](const PaddedPODArray<UInt32> & rows)
    {
        const auto * it = std::lower_bound(rows.begin(), rows.end(), begin);
        return it != rows.end() && *it <= end;
    };

    if (array)
        return has_rows_in_range(*array);

    if (isUnion())
        return std::ranges::any_of(arrays_to_unite, [&](const auto & rows) { return has_rows_in_range(*rows); });

    /// An allocation-free check that the bitmap has a value in the closed range of rows.
    return roaring::api::roaring_bitmap_intersect_with_range(&bitmap.roaring, begin, static_cast<UInt64>(end) + 1);
}

PostingList FoldedPostings::getBitmapInRange(UInt32 begin, UInt32 end) const
{
    if (isArray())
    {
        const auto & rows = *getArray();
        const auto * range_begin = std::lower_bound(rows.begin(), rows.end(), begin);
        const auto * range_end = std::upper_bound(range_begin, rows.end(), end);
        return buildPostingListFromSortedRowIds(std::span(range_begin, range_end));
    }

    PostingList range;
    range.addRangeClosed(begin, end);
    return bitmap & range;
}

PostingList FoldedPostings::getBitmap() const
{
    return isArray() ? buildPostingListFromSortedRowIds(*getArray()) : bitmap;
}

PaddedPODArrayPtr FoldedPostings::getArray() const
{
    chassert(!isUnion() || is_finalized);

    if (array)
        return array;

    std::call_once(single_array_once, [&]
    {
        if (isUnion())
        {
            single_array = uniteSortedArrays(*arrays_to_unite.front(), *arrays_to_unite.back());
            return;
        }

        auto bitmap_array = std::make_shared<PaddedPODArray<UInt32>>(bitmap.cardinality());
        bitmap.toUint32Array(bitmap_array->data());
        single_array = std::move(bitmap_array);
    });

    return single_array;
}

std::vector<PaddedPODArrayPtr> FoldedPostings::getArrays() const
{
    chassert(!isUnion() || is_finalized);
    return isUnion() ? arrays_to_unite : std::vector<PaddedPODArrayPtr>{getArray()};
}

void FoldedPostings::unite(TokenPostings & postings)
{
    chassert(!is_finalized);

    if (!isArray())
    {
        bitmap |= postings.getBitmap();
        return;
    }

    if (array)
        arrays_to_unite.push_back(std::move(array));

    arrays_to_unite.push_back(postings.getArray());
}

void FoldedPostings::intersect(TokenPostings & postings)
{
    chassert(!isUnion());

    if (!array)
    {
        bitmap &= postings.getBitmap();
        return;
    }

    if (postings.hasArray())
    {
        array = intersectSortedArrays(*array, *postings.getArray());
        return;
    }

    const auto & token_bitmap = postings.getBitmap();
    auto result = std::make_shared<PaddedPODArray<UInt32>>();
    result->reserve(array->size());
    roaring::BulkContext context;

    for (const UInt32 row : *array)
    {
        if (token_bitmap.containsBulk(context, row))
            result->push_back(row);
    }

    array = std::move(result);
}

void FoldedPostings::intersect(const PostingListSegment & segment, IPostingListBlockCodec & block_codec)
{
    if (!array)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Cannot intersect the bitmap of folded postings with a posting list segment");

    chassert(!isUnion());

    const auto & block_last_row_ids = segment.block_last_row_ids;
    const auto * blocks_end = block_last_row_ids.begin() + segment.block_count;

    UInt32 values[IPostingListBlockCodec::BLOCK_SIZE];
    size_t num_values = 0;
    size_t value_idx = 0;
    size_t block_idx = 0;
    bool is_block_decoded = false;
    size_t num_decoded_blocks = 0;

    auto result = std::make_shared<PaddedPODArray<UInt32>>();
    result->reserve(array->size());

    for (const UInt32 row : *array)
    {
        /// Move to the first block that ends at or after the row. The rows are sorted, so the blocks are visited in order.
        if (row > block_last_row_ids[block_idx])
        {
            block_idx = std::lower_bound(block_last_row_ids.begin() + block_idx + 1, blocks_end, row) - block_last_row_ids.begin();
            if (block_idx == segment.block_count)
                break;

            is_block_decoded = false;
        }

        /// The row falls between the blocks, so the posting list does not contain it.
        if (row < segment.getBlockFirstRowId(block_idx))
            continue;

        if (!is_block_decoded)
        {
            num_values = segment.decodeBlock(block_idx, block_codec, values);
            value_idx = 0;
            is_block_decoded = true;
            ++num_decoded_blocks;
        }

        while (value_idx < num_values && values[value_idx] < row)
            ++value_idx;

        if (value_idx < num_values && values[value_idx] == row)
            result->push_back(row);
    }

    array = std::move(result);

    ProfileEvents::increment(ProfileEvents::TextIndexAnalysisPackedBlocksDecoded, num_decoded_blocks);
    ProfileEvents::increment(ProfileEvents::TextIndexAnalysisPackedBlocksSkipped, segment.block_count - num_decoded_blocks);
}

void FoldedPostings::finalize()
{
    if (!isUnion() || is_finalized)
        return;

    is_finalized = true;

    /// The largest array, usually the posting list of a frequent token, is kept as is: merging it would copy all its rows.
    std::ranges::nth_element(arrays_to_unite, arrays_to_unite.begin(), [](const auto & lhs, const auto & rhs) { return lhs->size() > rhs->size(); });
    auto largest = std::move(arrays_to_unite.front());
    std::vector<PaddedPODArrayPtr> others(std::make_move_iterator(arrays_to_unite.begin() + 1), std::make_move_iterator(arrays_to_unite.end()));
    arrays_to_unite.clear();

    if (others.empty())
    {
        array = std::move(largest);
        return;
    }

    /// Merge the two smallest of the other arrays first, so that every row is copied only a logarithmic number of times.
    const auto is_larger = [](const PaddedPODArrayPtr & lhs, const PaddedPODArrayPtr & rhs) { return lhs->size() > rhs->size(); };
    std::ranges::make_heap(others, is_larger);

    while (others.size() > 1)
    {
        std::ranges::pop_heap(others, is_larger);
        auto smallest = std::move(others.back());
        others.pop_back();

        std::ranges::pop_heap(others, is_larger);
        others.back() = uniteSortedArrays(*smallest, *others.back());
        std::ranges::push_heap(others, is_larger);
    }

    union_cardinality = largest->size() + others.front()->size() - countCommonRows(*largest, *others.front());
    arrays_to_unite = {std::move(largest), std::move(others.front())};
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

PostingList TextIndexAnalyzer::ReadableRows::clipPostings(const PostingList & postings)
{
    if (ranges_bitmap.isEmpty())
    {
        /// Lazily build the single combined bitmap of readable rows used to clip token postings.
        /// `addRangeClosed` stores contiguous ranges as run containers, so this stays compact (O(number of ranges)).
        for (const auto & range : ranges)
            ranges_bitmap.addRangeClosed(static_cast<UInt32>(range.begin), static_cast<UInt32>(range.end));
    }

    return postings & ranges_bitmap;
}

void TextIndexAnalyzer::ReadableRows::clipRows(PaddedPODArray<UInt32> & rows) const
{
    size_t num_kept = 0;
    const UInt32 * it = rows.begin();

    for (const auto & range : ranges)
    {
        it = std::lower_bound(it, rows.cend(), range.begin);
        const UInt32 * range_end = std::upper_bound(it, rows.cend(), range.end);

        if (rows.data() + num_kept != it)
            memmove(rows.data() + num_kept, it, (range_end - it) * sizeof(UInt32));

        num_kept += range_end - it;
        it = range_end;

        if (it == rows.end())
            break;
    }

    rows.resize(num_kept);
}

size_t TextIndexAnalyzer::ReadableRows::getSizeInBytes() const
{
    return ranges.capacity() * sizeof(RowsRange) + ranges_bitmap.getSizeInBytes();
}

void TextIndexAnalyzer::QueryBuilder::markFailed()
{
    is_failed = true;
    postings.reset();
    approximate_postings.reset();
    rows_range.reset();
    num_live_tokens = 0;
}

void TextIndexAnalyzer::QueryBuilder::markBypassed()
{
    is_bypassed = true;
    /// Keep `postings` and `rows_range` for index analysis in `mayBeTrueOnGranule`.
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

void TextIndexAnalyzer::QueryBuilder::addTokenInfo(
    std::string_view token, TokenPostingsInfoPtr token_info, RowsRange token_rows_range, TokenPostings * embedded_postings)
{
    /// A pattern scan delivers known tokens again, so embedded postings are added only with a new token.
    if (is_failed || tokens.contains(token))
        return;

    tokens[token] = token_info;
    addRowsRange(token_rows_range);

    if (embedded_postings)
        addPostings(*embedded_postings);
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

void TextIndexAnalyzer::QueryBuilder::addPostings(TokenPostings & token_postings)
{
    if (is_failed)
        return;

    ++num_read_postings;
    bool need_all_tokens = query->getSearchMode() == TextSearchMode::All || query->getSearchMode() == TextSearchMode::Phrase;

    if (!postings)
        postings.emplace(token_postings, fold_into_arrays);
    else if (need_all_tokens)
        postings->intersect(token_postings);
    else
        postings->unite(token_postings);

    /// `All` mode fails as soon as the running intersection of readable postings becomes empty.
    if (need_all_tokens && postings->isEmpty())
        markFailed();
}

void TextIndexAnalyzer::QueryBuilder::addApproximatePostings(const PostingList & token_postings)
{
    if (is_failed)
        return;

    ++num_approximate_postings;

    if (!approximate_postings)
        approximate_postings = token_postings;
    else
        *approximate_postings |= token_postings;
}

bool TextIndexAnalyzer::QueryBuilder::canApproximatePostings() const
{
    if (!query->getPatterns().empty())
        return false;

    const auto search_mode = query->getSearchMode();
    return search_mode == TextSearchMode::Any || (search_mode == TextSearchMode::All && query->getTokens().size() == 1);
}

void TextIndexAnalyzer::QueryBuilder::intersectPostings(const PostingListSegment & segment, IPostingListBlockCodec & block_codec)
{
    if (is_failed)
        return;

    ++num_read_postings;
    postings->intersect(segment, block_codec);

    if (postings->isEmpty())
        markFailed();
}

TextIndexAnalyzer::TextIndexAnalyzer(const MergeTreeIndexConditionText & condition_text, IPostingListCodec::Type postings_codec_type)
{
    global_search_mode = condition_text.getGlobalSearchMode();
    fold_into_arrays = postings_codec_type != IPostingListCodec::Type::None;
    bool all_patterns_have_automata = true;
    bool has_general_pattern = false;

    for (const auto & [hash, query] : condition_text.getAllSearchQueries())
    {
        auto & query_builder = query_builders[hash];
        query_builder.query = query;
        /// A pattern can match many tokens: their union is cheaper to fold into a bitmap.
        query_builder.fold_into_arrays = fold_into_arrays && query->getPatterns().empty();

        for (const auto & token : query->getTokens())
        {
            if (queries_by_token[token].insert(hash).second)
                ++query_builder.num_live_tokens;
        }

        for (const auto & pattern : query->getPatterns())
        {
            queries_by_pattern[&pattern].insert(hash);
            has_general_pattern |= pattern.getMatchKind() == RegexpMatchKind::General;
        }
        for (const auto & automaton : query->getPatternAutomata())
        {
            if (automaton)
                pattern_cursors.emplace_back(*automaton);
            else
                all_patterns_have_automata = false;
        }
    }

    /// Prefix and exact literals already have a cheaper contiguous block-range scan.
    /// Infix patterns without a rejecting prefix retain the SIMD literal filter.
    if (!all_patterns_have_automata || !has_general_pattern)
        pattern_cursors.clear();
}

TextIndexDictionaryDFA::Cursor::Result TextIndexAnalyzer::nextPatternToken(std::string_view token, String & lower_bound)
{
    using Result = TextIndexDictionaryDFA::Cursor::Result;
    bool found = false;
    String candidate;
    for (auto & cursor : pattern_cursors)
    {
        auto result = cursor.next(token, candidate);
        if (result == Result::Match)
            return Result::Match;
        if (result == Result::Seek && (!found || candidate < lower_bound))
        {
            lower_bound = candidate;
            found = true;
        }
    }
    return found ? Result::Seek : Result::Exhausted;
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

    /// Clip the embedded postings to the readable rows once.
    std::optional<TokenPostings> embedded_postings;
    if (!token_info->embedded_postings.empty())
    {
        tokens_with_postings.emplace(token);
        ProfileEvents::increment(ProfileEvents::TextIndexUsedEmbeddedPostings);

        auto rows = std::make_shared<PaddedPODArray<UInt32>>();
        rows->insert(token_info->embedded_postings.begin(), token_info->embedded_postings.end());
        if (readable_rows)
            readable_rows->clipRows(*rows);

        embedded_postings.emplace(std::move(rows));
    }

    const bool no_readable_postings = embedded_postings && embedded_postings->isEmpty();
    processTokenOperation(token, [&](QueryBuilder & query_builder)
    {
        query_builder.addTokenInfo(token, token_info, token_rows_range, embedded_postings && !no_readable_postings ? &*embedded_postings : nullptr);
    });

    /// Same as in `addPostings`: a token without readable postings is missing.
    if (no_readable_postings)
    {
        processTokenOperation(token, [&](QueryBuilder & query_builder)
        {
            query_builder.addMissingToken(token);
        });

        queries_by_token.erase(token);
    }
}

void TextIndexAnalyzer::addPostings(std::string_view token, const PostingList & postings)
{
    tokens_with_postings.emplace(token);

    /// Clip the postings to the readable rows once.
    std::optional<PostingList> clipped_postings;
    const auto * postings_ptr = &postings;

    if (readable_rows)
    {
        clipped_postings = readable_rows->clipPostings(postings);
        postings_ptr = &*clipped_postings;
    }

    TokenPostings token_postings(*postings_ptr);
    foldPostings(token, token_postings);
}

void TextIndexAnalyzer::addPostings(std::string_view token, PaddedPODArray<UInt32> && rows)
{
    tokens_with_postings.emplace(token);

    /// Clip the postings to the readable rows once.
    if (readable_rows)
        readable_rows->clipRows(rows);

    TokenPostings token_postings(std::make_shared<PaddedPODArray<UInt32>>(std::move(rows)));
    foldPostings(token, token_postings);
}

void TextIndexAnalyzer::foldPostings(std::string_view token, TokenPostings & postings)
{
    /// A token without readable postings is missing.
    if (postings.isEmpty())
    {
        processTokenOperation(token, [&](QueryBuilder & query_builder)
        {
            query_builder.addMissingToken(token);
        });

        queries_by_token.erase(token);
        return;
    }

    processTokenOperation(token, [&](QueryBuilder & query_builder)
    {
        query_builder.addPostings(postings);
    });
}

bool TextIndexAnalyzer::canApproximatePostings(std::string_view token) const
{
    auto it = queries_by_token.find(token);
    if (it == queries_by_token.end())
        return false;

    bool has_active_queries = false;

    for (const auto & query_hash : it->second)
    {
        const auto & query_builder = query_builders.at(query_hash);
        if (query_builder.is_failed || query_builder.is_bypassed)
            continue;

        if (!query_builder.canApproximatePostings())
            return false;

        has_active_queries = true;
    }

    return has_active_queries;
}

void TextIndexAnalyzer::addApproximatePostings(std::string_view token, const PostingList & postings)
{
    /// The token is not added to `tokens_with_postings`: the readers read its posting list.
    if (postings.isEmpty())
    {
        processTokenOperation(token, [&](QueryBuilder & query_builder)
        {
            query_builder.addMissingToken(token);
        });

        queries_by_token.erase(token);
        return;
    }

    processTokenOperation(token, [&](QueryBuilder & query_builder)
    {
        query_builder.addApproximatePostings(postings);
    });
}

void TextIndexAnalyzer::finalize()
{
    for (auto & [_, query_builder] : query_builders)
    {
        if (query_builder.postings)
            query_builder.postings->finalize();
    }
}

bool TextIndexAnalyzer::canIntersectPostings(std::string_view token) const
{
    auto it = queries_by_token.find(token);
    if (!fold_into_arrays || it == queries_by_token.end())
        return false;

    bool has_active_queries = false;

    for (const auto & query_hash : it->second)
    {
        const auto & query_builder = query_builders.at(query_hash);
        if (query_builder.is_failed || query_builder.is_bypassed)
            continue;

        const auto search_mode = query_builder.query->getSearchMode();
        if (search_mode != TextSearchMode::All && search_mode != TextSearchMode::Phrase)
            return false;

        if (!query_builder.postings || !query_builder.postings->isArray())
            return false;

        has_active_queries = true;
    }

    return has_active_queries;
}

void TextIndexAnalyzer::intersectPostings(std::string_view token, const PostingListSegment & segment, IPostingListBlockCodec & block_codec)
{
    tokens_with_postings.emplace(token);

    processTokenOperation(token, [&](QueryBuilder & query_builder)
    {
        query_builder.intersectPostings(segment, block_codec);
    });
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

std::optional<std::vector<TextIndexAnalyzer::TokenKeyRange>> TextIndexAnalyzer::getPatternTokenKeyRanges() const
{
    if (queries_by_pattern.empty())
        return std::nullopt;

    std::vector<TokenKeyRange> key_ranges;
    key_ranges.reserve(queries_by_pattern.size());

    for (const auto & [pattern, _] : queries_by_pattern)
    {
        String literal(pattern->getRequiredSubstring());
        if (literal.empty())
            return std::nullopt;

        /// An anchored kind compares bytes: the constructor demotes a case-insensitive one to `General`.
        /// That is the order the dictionary is sorted in, so such a pattern matches inside one key range.
        switch (pattern->getMatchKind())
        {
            case RegexpMatchKind::Prefix:
                key_ranges.emplace_back(literal, firstStringThatIsGreaterThanAllStringsWithPrefix(literal));
                break;
            case RegexpMatchKind::Exact:
                key_ranges.emplace_back(literal, literal);
                break;
            case RegexpMatchKind::Suffix:
            case RegexpMatchKind::Substring:
            case RegexpMatchKind::General:
                return std::nullopt;
        }
    }

    return key_ranges;
}

bool TextIndexAnalyzer::canFilterTokensByLiterals() const
{
    if (queries_by_pattern.empty())
        return false;

    return std::ranges::all_of(queries_by_pattern, [](const auto & entry) { return !entry.first->getRequiredSubstring().empty(); });
}

void TextIndexAnalyzer::markPatternCandidateTokens(
    const OptimizedRegularExpression & pattern, const ColumnString & tokens, PaddedPODArray<UInt8> & candidate_marks)
{
    const auto & chars = tokens.getChars();
    const auto & offsets = tokens.getOffsets();
    const size_t literal_size = pattern.getRequiredSubstring().size();

    const UInt8 * const begin = chars.data();
    const UInt8 * const end = begin + chars.size();
    const UInt8 * pos = begin;
    size_t token_idx = 0;

    while (pos < end && end != (pos = pattern.searchRequiredSubstring(pos, end - pos)))
    {
        while (begin + offsets[token_idx] <= pos)
            ++token_idx;

        /// Tokens are stored back to back and are not zero-terminated, so an occurrence may straddle two of
        /// them. One that leaves the token cannot be followed by one inside it, which would start earlier.
        if (pos + literal_size <= begin + offsets[token_idx])
            candidate_marks[token_idx] = 1;

        pos = begin + offsets[token_idx];
        ++token_idx;
    }
}

void TextIndexAnalyzer::matchTokensByLiterals(
    const ColumnString & tokens, PaddedPODArray<UInt8> & candidate_marks, std::vector<size_t> & matched_indices)
{
    const size_t num_tokens = tokens.size();
    candidate_marks.assign(num_tokens, static_cast<UInt8>(0));

    for (const auto & [pattern, _] : queries_by_pattern)
        markPatternCandidateTokens(*pattern, tokens, candidate_marks);

    for (size_t token_idx = 0; token_idx < num_tokens; ++token_idx)
    {
        if (candidate_marks[token_idx] && addTokenToPatterns(tokens.getDataAt(token_idx)))
            matched_indices.push_back(token_idx);
    }
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
            double log_cardinality = query_builder.postings
                ? std::log(static_cast<double>(query_builder.postings->cardinality()))
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
            double not_in_any = query_builder.postings
                ? 1.0 - static_cast<double>(query_builder.postings->cardinality()) / n
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
            /// stop reactivating it; `postings`/`rows_range` are preserved for `mayBeTrueOnGranule`.
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

        if (query_builder.is_failed)
        {
            detachQueryFromTokens(query_hash, query_builder);

            /// One failed query in `All` global mode proves the whole conjunction false in this
            /// part; the remaining queries cannot contribute to the result, so fail them all.
            if (global_search_mode == TextSearchMode::All)
                markAllQueriesFailed();
        }
    }
}

}
