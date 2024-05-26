#include <Storages/MergeTree/RowOrderOptimizer.h>

#include <Interpreters/sortBlock.h>
#include <base/sort.h>
#include <Common/PODArray.h>
#include <Common/iota.h>

#include <numeric>

namespace DB
{

namespace
{

/// Do the left and right row contain equal values in the sorting key columns (usually the primary key columns)
bool haveEqualSortingKeyValues(const Block & block, const SortDescription & sort_description, size_t left_row, size_t right_row)
{
    for (const auto & sort_column : sort_description)
    {
        const String & sort_col = sort_column.column_name;
        const IColumn & column = *block.getByName(sort_col).column;
        if (column.compareAt(left_row, right_row, column, 1) != 0)
            return false;
    }
    return true;
}

/// Returns the sorted indexes of all non-sorting-key columns.
std::vector<size_t> getOtherColumnIndexes(const Block & block, const SortDescription & sort_description)
{
    const size_t sorting_key_columns_count = sort_description.size();
    const size_t all_columns_count = block.columns();

    std::vector<size_t> other_column_indexes;
    other_column_indexes.reserve(all_columns_count - sorting_key_columns_count);

    if (sorting_key_columns_count == 0)
    {
        other_column_indexes.resize(block.columns());
        iota(other_column_indexes.begin(), other_column_indexes.end(), 0);
    }
    else
    {
        std::vector<size_t> sorted_column_indexes;
        sorted_column_indexes.reserve(sorting_key_columns_count);
        for (const SortColumnDescription & sort_column : sort_description)
        {
            size_t id = block.getPositionByName(sort_column.column_name);
            sorted_column_indexes.emplace_back(id);
        }
        ::sort(sorted_column_indexes.begin(), sorted_column_indexes.end());

        for (size_t i = 0; i < sorted_column_indexes.front(); ++i)
            other_column_indexes.push_back(i);
        for (size_t i = 0; i + 1 < sorted_column_indexes.size(); ++i)
            for (size_t id = sorted_column_indexes[i] + 1; id < sorted_column_indexes[i + 1]; ++id)
                other_column_indexes.push_back(id);
        for (size_t i = sorted_column_indexes.back() + 1; i < block.columns(); ++i)
            other_column_indexes.push_back(i);
    }
    return other_column_indexes;
}

/// Returns a set of equal row ranges (equivalence classes) with the same row values for all sorting key columns (usually primary key columns.)
/// Example with 2 PK columns, 2 other columns --> 3 equal ranges
///          pk1    pk2    c1    c2
///          ----------------------
///          1      1     a     b
///          1      1     b     e
///          --------
///          1      2     e     a
///          1      2     d     c
///          1      2     e     a
///          --------
///          2      1     a     3
///          ----------------------
EqualRanges getEqualRanges(const Block & block, const SortDescription & sort_description, const IColumn::Permutation & permutation)
{
    EqualRanges ranges;
    const size_t rows = block.rows();
    if (sort_description.empty())
    {
        ranges.push_back({0, rows});
    }
    else
    {
        for (size_t i = 0; i < rows;)
        {
            size_t j = i;
            while (j < rows && haveEqualSortingKeyValues(block, sort_description, permutation[i], permutation[j]))
                ++j;
            ranges.push_back({i, j});
            i = j;
        }
    }
    return ranges;
}

std::vector<size_t> getCardinalitiesInPermutedRange(
    const Block & block,
    const std::vector<size_t> & other_column_indexes,
    const IColumn::Permutation & permutation,
    const EqualRange & equal_range)
{
    std::vector<size_t> cardinalities(other_column_indexes.size());
    for (size_t i = 0; i < other_column_indexes.size(); ++i)
    {
        const ColumnPtr & column = block.getByPosition(i).column;
        cardinalities[i] = column->estimateCardinalityInPermutedRange(permutation, equal_range);
    }
    return cardinalities;
}

void updatePermutationInEqualRange(
    const Block & block,
    const std::vector<size_t> & other_column_indexes,
    IColumn::Permutation & permutation,
    const EqualRange & equal_range,
    const std::vector<size_t> & cardinalities)
{
    std::vector<size_t> column_order(other_column_indexes.size());
    iota(column_order.begin(), column_order.end(), 0);
    auto cmp = [&](size_t lhs, size_t rhs) -> bool { return cardinalities[lhs] < cardinalities[rhs]; };
    ::sort(column_order.begin(), column_order.end(), cmp);

    std::vector<EqualRange> ranges = {equal_range};
    for (size_t i : column_order)
    {
        const size_t column_id = other_column_indexes[i];
        const ColumnPtr & column = block.getByPosition(column_id).column;
        column->updatePermutation(IColumn::PermutationSortDirection::Ascending, IColumn::PermutationSortStability::Unstable, 0, 1, permutation, ranges);
    }
}

}

void RowOrderOptimizer::optimize(const Block & block, const SortDescription & description, IColumn::Permutation & permutation)
{
    if (block.columns() == 0)
        return; /// a table without columns, this should not happen in the first place ...

    if (permutation.empty())
    {
        const size_t rows = block.rows();
        permutation.resize(rows);
        iota(permutation.data(), rows, IColumn::Permutation::value_type(0));
    }

    const EqualRanges equal_ranges = getEqualRanges(block, description, permutation);
    const std::vector<size_t> other_columns_indexes = getOtherColumnIndexes(block, description);

    for (const auto & equal_range : equal_ranges)
    {
        if (equal_range.size() <= 1)
            continue;
        const std::vector<size_t> cardinalities = getCardinalitiesInPermutedRange(block, other_columns_indexes, permutation, equal_range);
        updatePermutationInEqualRange(block, other_columns_indexes, permutation, equal_range, cardinalities);
    }
}

}
