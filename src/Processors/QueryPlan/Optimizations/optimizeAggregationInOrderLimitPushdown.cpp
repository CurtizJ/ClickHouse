#include <Core/SortDescription.h>
#include <Processors/QueryPlan/AggregatingStep.h>
#include <Processors/QueryPlan/ExpressionStep.h>
#include <Processors/QueryPlan/FilterStep.h>
#include <Processors/QueryPlan/LimitStep.h>
#include <Processors/QueryPlan/Optimizations/Optimizations.h>
#include <Processors/QueryPlan/Optimizations/QueryPlanOptimizationSettings.h>
#include <Processors/QueryPlan/QueryPlan.h>
#include <Processors/QueryPlan/ReadFromMergeTree.h>
#include <Processors/QueryPlan/SortingStep.h>

namespace DB::QueryPlanOptimizations
{

namespace
{

/// Returns true if the AggregatingStep's GROUP BY description and the (optional)
/// downstream ORDER BY description match the requirements for LIMIT pushdown:
///   * ORDER BY (if present) is a prefix of GROUP BY keys.
///   * Direction is uniform (all ASC or all DESC) across ORDER BY columns.
bool sortPrefixIsCompatibleWithGroupBy(const SortDescription & order_by, const SortDescription & group_by)
{
    if (order_by.empty())
        return true;
    if (order_by.size() > group_by.size())
        return false;

    const int direction = order_by.front().direction;
    for (size_t i = 0; i < order_by.size(); ++i)
    {
        if (order_by[i].direction != direction)
            return false;
        if (order_by[i].column_name != group_by[i].column_name)
            return false;
    }
    return true;
}

}

/// Top-down pass triggered on a `LimitStep`. Walks the subtree to find a
/// candidate `AggregatingStep` and, when eligible, stamps it with the
/// `output_limit_for_in_order` field. The downstream pipeline-construction
/// branch in `AggregatingStep::transformPipeline` reads the field to keep the
/// post-aggregation stream single and globally sorted, so a downstream
/// `MergingSortedTransform` (produced by `SortingStep::FinishSorting`) can
/// honor `LIMIT` and propagate backpressure all the way to
/// `MergeTreeSelectProcessor`. The `has_filter_in_subtree` flag is used by
/// `optimizeReadInOrder.cpp:buildInputOrderInfo(AggregatingStep&)` to choose
/// a larger row-count budget for the parallelization hint when WHERE/PREWHERE
/// filters most rows out.
///
/// Eligibility (the in-order/sort-key prefix match itself is verified later by
/// `optimizeAggregationInOrder` calling `buildInputOrderInfo`; this pass only
/// stamps the candidate flag):
///   * Plain `LIMIT` (no WITH TIES, no WITH FILL — the latter would appear as
///     a `FillingStep` between LIMIT and SORT).
///   * Optional intermediate `ExpressionStep`/`FilterStep` (sort-preserving).
///   * Optional `SortingStep` whose description is a prefix of GROUP BY keys
///     with uniform direction.
///   * `AggregatingStep` with: `final == true`, no GROUPING SETS, no
///     `overflow_row`, and not yet flagged.
///   * No aggregate function appears in ORDER BY: enforced indirectly by
///     requiring ORDER BY columns to match GROUP BY keys.
void optimizeAggregationInOrderLimitPushdown(
    QueryPlan::Node & root_node, QueryPlan::Nodes &, const QueryPlanOptimizationSettings & settings)
{
    if (!settings.aggregation_in_order_limit_pushdown)
        return;

    auto * limit_step = typeid_cast<LimitStep *>(root_node.step.get());
    if (!limit_step)
        return;
    if (limit_step->withTies())
        return;
    if (root_node.children.size() != 1)
        return;

    const UInt64 group_limit = limit_step->getLimit() + limit_step->getOffset();
    if (group_limit == 0)
        return;

    bool has_filter_in_subtree = false;
    SortingStep * sorting_step = nullptr;
    AggregatingStep * aggregating_step = nullptr;
    QueryPlan::Node * node = root_node.children.front();

    /// Walk down through sort-preserving steps (Expression/Filter) and at most
    /// one SortingStep, until we reach the AggregatingStep.
    while (node)
    {
        if (auto * agg = typeid_cast<AggregatingStep *>(node->step.get()))
        {
            aggregating_step = agg;
            break;
        }

        if (auto * sort = typeid_cast<SortingStep *>(node->step.get()))
        {
            if (sorting_step)
                return; /// More than one SortingStep — bail.
            if (sort->getType() != SortingStep::Type::Full)
                return; /// Already converted; the LIMIT was already pushed in some other path.
            if (sort->hasPartitions())
                return;
            sorting_step = sort;
        }
        else if (typeid_cast<ExpressionStep *>(node->step.get()))
        {
            /// ExpressionStep is sort-preserving for the columns the SortingStep
            /// references; rely on `optimizeAggregationInOrder` (which uses
            /// `buildSortingDAG`) for the deeper analysis.
        }
        else if (typeid_cast<FilterStep *>(node->step.get()))
        {
            has_filter_in_subtree = true;
        }
        else
        {
            return; /// Unknown intermediate step — don't push.
        }

        if (node->children.size() != 1)
            return;
        node = node->children.front();
    }

    if (!aggregating_step)
        return;
    if (aggregating_step->isGroupingSets())
        return;
    if (!aggregating_step->getFinal())
        return;
    if (aggregating_step->getParams().overflow_row)
        return;
    if (aggregating_step->getOutputLimitForInOrder().has_value())
        return; /// Already stamped — pass is idempotent.

    /// Build the GROUP BY sort description from the AggregatingStep's keys.
    /// (`group_by_sort_description` is empty until `applyOrder` is called by
    /// `optimizeAggregationInOrder`, so we cannot read it here. Instead, use
    /// the keys directly and let the prefix check still work — the names are
    /// what matter; direction will become uniform inside the AggregatingStep.)
    if (sorting_step)
    {
        SortDescription group_by_keys;
        group_by_keys.reserve(aggregating_step->getParams().keys.size());
        for (const auto & key : aggregating_step->getParams().keys)
            group_by_keys.emplace_back(key, sorting_step->getSortDescription().front().direction);

        if (!sortPrefixIsCompatibleWithGroupBy(sorting_step->getSortDescription(), group_by_keys))
            return;
    }

    /// Detect WHERE/PREWHERE on the leaf read step (in addition to any
    /// FilterStep hits we counted while walking down). Descend below the
    /// AggregatingStep through linear chains; we only need a yes/no signal.
    if (!has_filter_in_subtree)
    {
        QueryPlan::Node * scan = node;
        while (scan && !has_filter_in_subtree)
        {
            if (typeid_cast<FilterStep *>(scan->step.get()))
                has_filter_in_subtree = true;
            else if (auto * read_mt = typeid_cast<ReadFromMergeTree *>(scan->step.get()))
            {
                if (read_mt->getPrewhereInfo() || read_mt->getQueryInfo().row_level_filter)
                    has_filter_in_subtree = true;
                break;
            }

            if (scan->children.size() != 1)
                break;
            scan = scan->children.front();
        }
    }

    aggregating_step->applyLimit(group_limit, has_filter_in_subtree);
}

}
