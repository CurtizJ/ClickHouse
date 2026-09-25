#include <Columns/Collator.h>
#include <Core/Field.h>
#include <Core/Joins.h>
#include <Core/SortDescription.h>
#include <DataTypes/DataTypeTuple.h>
#include <Functions/IFunction.h>
#include <Interpreters/IJoin.h>
#include <Interpreters/JoinOperator.h>
#include <Interpreters/TableJoin.h>
#include <Processors/QueryPlan/ExpressionStep.h>
#include <Processors/QueryPlan/FilterStep.h>
#include <Processors/QueryPlan/JoinStep.h>
#include <Processors/QueryPlan/JoinStepLogical.h>
#include <Processors/QueryPlan/LimitStep.h>
#include <Processors/QueryPlan/Optimizations/Optimizations.h>
#include <Processors/QueryPlan/QueryPlan.h>
#include <Processors/QueryPlan/ReadFromMergeTree.h>
#include <Processors/QueryPlan/SortingStep.h>
#include <Processors/QueryPlan/SourceStepWithFilter.h>
#include <Processors/QueryPlan/UnionStep.h>
#include <Formats/FormatFilterInfo.h>
#include <Common/SipHash.h>

namespace DB::QueryPlanOptimizations
{

namespace
{

/// True if a value of this type can contain a floating-point number anywhere inside it - directly,
/// or nested in a `Nullable`, `Array`, `Tuple`, `Map`, ... (`forEachChild` recurses on its own).
bool typeCanContainFloat(const DataTypePtr & type)
{
    if (isFloat(type))
        return true;
    bool found = false;
    type->forEachChild([&](const IDataType & child) { found = found || isFloat(child); });
    return found;
}

/// A value derived from the block it is evaluated on (`rowNumberInBlock`, `blockSize`, `rand`): the
/// threshold filter below such a computation changes the blocks it sees, and so its result.
bool dependsOnItsBlock(const ActionsDAG & actions)
{
    for (const auto & node : actions.getNodes())
        if (node.type == ActionsDAG::ActionType::FUNCTION
            && (node.function_base->isStateful() || !node.function_base->isDeterministicInScopeOfQuery()))
            return true;

    return false;
}

/// Deterministic hash of the planning-time parameters of a TopK plan. Used by `updateQueryConditionCache`
/// to partition QCC entries by TopK plan, so the same query reuses cached granule decisions and a different
/// TopK plan (different LIMIT, sort column, direction, NULLS FIRST/LAST, COLLATE, etc.) gets a fresh entry.
UInt64 getTopKConditionHash(const TopKFilterInfo & info, const SortColumnDescription & sort_column_description)
{
    SipHash hash;
    hash.update(info.column_name);
    const String type_name = info.data_type->getName();
    hash.update(type_name);
    hash.update(info.num_sort_columns);
    hash.update(info.limit_n);
    hash.update(info.direction);
    hash.update(sort_column_description.nulls_direction);
    if (sort_column_description.collator)
        hash.update(sort_column_description.collator->getLocale());
    return hash.get64();
}

/// The top-K optimization of one `ORDER BY ... LIMIT n` being placed into the plan below its sorting step.
struct TopKFilterPlacement
{
    const Optimization::ExtraSettings & settings;
    const SortColumnDescription & sort_column_description;
    size_t num_sort_columns;
    size_t limit;
    TopKThresholdTrackerPtr threshold_tracker;

    /// Whether the dynamic filter may be placed. Otherwise only the skip index of a direct read is used.
    bool dynamic_filtering;

    /// Whether the placed optimizations use the threshold, so that the sorting step has to publish it.
    bool uses_threshold = false;
};

/// Where the walk of `placeTopKFilter` has reached a source from.
struct TopKFilterPath
{
    /// Through `Expression` and `Filter` steps only: the rows the source reads reach the sorting step, less the
    /// filtered ones. Only then the top-K marks of the skip index are a superset of the result.
    bool direct = true;
    /// A `Filter` step was passed.
    bool filtered = false;
};

/// The read applies the dynamic filter as its first PREWHERE read step (see `MergeTreeSelectProcessor::getPrewhereActions`),
/// and a direct read selects the top-K marks by a minmax skip index on the sort column.
void placeIntoMergeTreeRead(
    ReadFromMergeTree & read, const String & column_name, const DataTypePtr & type, TopKFilterPath path, TopKFilterPlacement & placement)
{
    /// FINAL queries deduplicate overlapping parts via merging sorted transforms, which need all the
    /// rows of a key to find the row that wins.
    if (read.isQueryWithFinal())
        return;

    /// Another `ORDER BY ... LIMIT` owns the read, e.g. the one `tryTopKThroughJoin` adds below a join.
    if (read.isSelectedForTopKFilterOptimization())
        return;

    const auto & read_columns = read.getAllColumnNames();
    if (std::ranges::find(read_columns, column_name) == read_columns.end())
        return;

    const auto * read_column = read.getOutputHeader()->findByName(column_name);
    if (!read_column || !read_column->type->equals(*type))
        return;

    bool use_dynamic_filtering = placement.dynamic_filtering;

    /// When read-in-order optimization is enabled and the sort column is a prefix
    /// of the storage's sorting key, the engine will read data in sorted order.
    /// TopK dynamic filtering is counterproductive in this case: once the threshold
    /// is established, the filter rejects all subsequent rows (they are beyond
    /// the threshold in sorted order), preventing the LIMIT from triggering early
    /// pipeline cancellation, and causing a full table scan instead.
    if (use_dynamic_filtering && placement.settings.read_in_order)
    {
        const auto & sorting_key = read.getStorageMetadata()->getSortingKey();
        if (!sorting_key.column_names.empty() && sorting_key.column_names[0] == column_name)
            use_dynamic_filtering = false;
    }

    /// The filter goes before the PREWHERE and the row-level filter, which then see fewer rows.
    if ((read.getPrewhereInfo() && dependsOnItsBlock(read.getPrewhereInfo()->prewhere_actions))
        || (read.getRowLevelFilter() && dependsOnItsBlock(read.getRowLevelFilter()->actions)))
        use_dynamic_filtering = false;

    /// The skip-index top-k path ranks granules via raw Field comparison
    /// (MinMaxGranuleItem::operator<) which does not respect nulls_direction
    /// or collation. Restrict it to types where raw Field ordering matches
    /// ORDER BY semantics. This check mirrors the guard in
    /// ReadFromMergeTree::buildIndexes for defense-in-depth.
    const bool use_skip_index = path.direct
        && placement.settings.use_skip_indexes_for_top_k
        && type->isValueRepresentedByNumber()
        && !type->isNullable()
        && !placement.sort_column_description.collator
        && read.isSkipIndexAvailableForTopK(column_name);

    if (!use_dynamic_filtering && !use_skip_index)
        return;

    /// A row-level policy filter restricts the rows inside the reader just like a `WHERE` / `PREWHERE`,
    /// so it must count as a `where_clause` as well. Otherwise a query filtered only by a row policy leaves
    /// `where_clause == false`, `MergeTreeDataSelectExecutor` enables `perform_top_k_optimization` and narrows
    /// the read to the top-K marks before the policy runs: the policy then discards the rows in those marks
    /// and the query returns fewer rows than the `LIMIT` - or none at all - even though later marks hold rows
    /// the policy keeps. The rows of a read below a join or a union are not the rows the sorting step sees
    /// either, so `where_clause` keeps `getTopKMarks` from selecting the marks there as well.
    const bool where_clause = !path.direct || path.filtered || read.getPrewhereInfo() || read.getRowLevelFilter();

    TopKFilterInfo info{
        column_name,
        type,
        placement.num_sort_columns,
        placement.limit,
        placement.sort_column_description.direction,
        where_clause,
        placement.threshold_tracker,
        /*condition_hash=*/ 0};
    info.dynamic_filtering = use_dynamic_filtering;
    info.condition_hash = getTopKConditionHash(info, placement.sort_column_description);
    read.setTopKColumn(info);

    placement.uses_threshold |= use_dynamic_filtering || (use_skip_index && placement.settings.use_skip_indexes_on_data_read);
}

/// TopN dynamic filtering for sources that read data formats (e.g. Parquet files). The filter is
/// delivered through `FormatTopKFilterInfo` rather than a filter step: the format appends the
/// threshold filter to its own filtering pipeline, and can additionally use the threshold to
/// skip whole row groups and pages by their statistics. The filter only ever removes rows that
/// cannot enter the top-K heap above, so it composes with any `WHERE` or `PREWHERE` the source
/// already has, in any order.
void placeIntoFormatSource(SourceStepWithFilterBase & source, const String & column_name, const DataTypePtr & type, TopKFilterPlacement & placement)
{
    if (!placement.dynamic_filtering)
        return;

    /// `ORDER BY` sorts `nan` together with `NULL` (see `SortColumnDescription::nulls_direction`),
    /// but the comparison functions behind `__topKFilter` do not: a `nan` can become the published
    /// threshold and then reject every finite value, or be dropped under `NULLS FIRST`. That is a
    /// pre-existing defect of the `MergeTree` path, tracked in
    /// https://github.com/ClickHouse/ClickHouse/issues/116705. Formats add a second, independent
    /// hazard: `nan` values are legally absent from Parquet min/max statistics, so a finite range
    /// cannot prove that a row group holds no `nan` row that must sort first. Keep floating-point
    /// sort keys off this path until both are `nan`-aware.
    if (typeCanContainFloat(type))
        return;

    /// The sort column must be one of the source's outputs with an unchanged type: the source
    /// compares its own column against thresholds the sorting transforms above produce from that
    /// very column.
    const auto * source_column = source.getOutputHeader()->findByName(column_name);
    if (!source_column || !source_column->type->equals(*type))
        return;

    /// Being in the output header is necessary but not sufficient: the source must physically
    /// read the column itself. `ReadFromFile` appends virtual columns (`_path`, `_file`, ...) and
    /// Hive partition columns after the format has read the file, so the format could never
    /// evaluate the threshold against them; the source decides from its format-facing header.
    if (!source.supportsTopKDynamicFilter(*source_column))
        return;

    auto info = std::make_shared<FormatTopKFilterInfo>();
    info->column_name = column_name;
    info->threshold_tracker = placement.threshold_tracker;
    source.setTopKFilter(std::move(info));

    placement.uses_threshold = true;
}

/// The name of the column `column_name` of the output of `dag` in its input, if `dag` passes it through
/// unchanged: the output node is an `INPUT`, or a chain of `ALIAS`es ending at one.
std::optional<String> findPassThroughInput(const ActionsDAG & dag, const String & column_name)
{
    const auto * node = dag.tryFindInOutputs(column_name);
    if (!node)
        return {};

    while (node->type == ActionsDAG::ActionType::ALIAS)
        node = node->children.front();

    if (node->type != ActionsDAG::ActionType::INPUT)
        return {};

    return node->result_name;
}

/// The left input of a join, if the column `column_name` of the join's output comes from it and every left
/// row produces its output rows on its own, all with its value of the column. Then a left row the threshold
/// rejects produces no row of the result, and the other left rows produce the same rows without it.
///
/// `RIGHT` and `FULL` joins emit a row with default left columns for every unmatched right row, and filtering
/// the left input would add such rows. `INNER ANY` matches every key once on both sides, so a filtered-out
/// left row could make another left row of the same key match. The right input is not considered: a hash join
/// consumes it before the first row reaches the sorting step, while there is no threshold yet.
QueryPlan::Node * findJoinInputOfColumn(QueryPlan::Node & join_node, const String & column_name, const DataTypePtr & type)
{
    if (join_node.children.size() != 2)
        return nullptr;

    JoinKind kind = JoinKind::Inner;
    JoinStrictness strictness = JoinStrictness::Unspecified;
    if (const auto * logical = typeid_cast<const JoinStepLogical *>(join_node.step.get()))
    {
        kind = logical->getJoinOperator().kind;
        strictness = logical->getJoinOperator().strictness;
    }
    else if (const auto * physical = typeid_cast<const JoinStep *>(join_node.step.get()); physical && physical->getJoin())
    {
        kind = physical->getJoin()->getTableJoin().kind();
        strictness = physical->getJoin()->getTableJoin().strictness();
    }
    else
    {
        return nullptr;
    }

    const bool left_rows_are_independent = kind == JoinKind::Left
        || kind == JoinKind::Cross
        || kind == JoinKind::Comma
        || (kind == JoinKind::Inner && strictness != JoinStrictness::Any);

    if (!left_rows_are_independent)
        return nullptr;

    const auto & input_headers = join_node.step->getInputHeaders();
    const auto * left_column = input_headers[0]->findByName(column_name);
    if (!left_column || !left_column->type->equals(*type) || input_headers[1]->has(column_name))
        return nullptr;

    return join_node.children[0];
}

/// Pushes the top-K optimization on the column `column_name` of the output of `node` down into the sources:
/// through steps that keep the column unchanged and do not depend on how the rows are split into blocks, and,
/// for the dynamic filter, into the left input of a join and into every input of a union. A read applies the
/// filter while reading, and a direct one also selects the top-K marks by its skip index. Where no source can
/// take the filter, it is not placed: the partial sorting of the sorting step filters by the threshold anyway.
void placeTopKFilter(QueryPlan::Node * node, String column_name, const DataTypePtr & type, TopKFilterPath path, TopKFilterPlacement & placement)
{
    while (true)
    {
        IQueryPlanStep * step = node->step.get();

        if (auto * read = typeid_cast<ReadFromMergeTree *>(step))
            return placeIntoMergeTreeRead(*read, column_name, type, path, placement);

        if (auto * source = dynamic_cast<SourceStepWithFilterBase *>(step))
            return placeIntoFormatSource(*source, column_name, type, placement);

        const ActionsDAG * dag = nullptr;
        if (const auto * expression_step = typeid_cast<const ExpressionStep *>(step))
        {
            dag = &expression_step->getExpression();
        }
        else if (const auto * filter_step = typeid_cast<const FilterStep *>(step))
        {
            dag = &filter_step->getExpression();
            path.filtered = true;
        }

        if (dag)
        {
            /// `arrayJoin` changes the number of rows: a threshold filter below it sees the rows before
            /// the expansion, which are not the rows the sorting step sees. See #82279.
            if (node->children.size() != 1 || dag->hasArrayJoin() || dependsOnItsBlock(*dag))
                return;

            auto input_name = findPassThroughInput(*dag, column_name);
            if (!input_name)
                return;

            column_name = std::move(*input_name);
            node = node->children.front();
            continue;
        }

        /// Below a join or a union only the dynamic filter applies.
        if (!placement.dynamic_filtering)
            return;

        /// Plan-based parallel replicas ship the inputs of joins and unions to the replicas, while a read
        /// with the filter has to stay local (see `applyParallelReplicas`). Keep to the reads of the sorting
        /// step's own query there.
        if (placement.settings.enable_parallel_replicas)
            return;

        path.direct = false;

        if (auto * join_input = findJoinInputOfColumn(*node, column_name, type))
        {
            node = join_input;
            continue;
        }

        if (typeid_cast<UnionStep *>(step))
        {
            const size_t position = step->getOutputHeader()->getPositionByName(column_name);
            for (size_t input = 0; input < node->children.size(); ++input)
            {
                const auto & input_column = step->getInputHeaders()[input]->getByPosition(position);
                if (input_column.type->equals(*type))
                    placeTopKFilter(node->children[input], input_column.name, type, path, placement);
            }
        }

        return;
    }
}

}

size_t tryOptimizeTopK(QueryPlan::Node * parent_node, QueryPlan::Nodes & /*nodes*/, const Optimization::ExtraSettings & settings)
{
    /// Both top-K paths rely on a runtime `TopKThresholdTracker` shared between
    /// `SortingStep` and the steps below it: the dynamic filter compares the sort
    /// column with it, the skip-index path uses it to skip granules while reading.
    /// The tracker cannot be transmitted to remote workers, so when the plan is
    /// going to be distributed, a remote node would read without the threshold and
    /// return excess rows.
    if (settings.make_distributed_plan)
        return 0;

    QueryPlan::Node * node = parent_node;

    auto * limit_step = typeid_cast<LimitStep *>(node->step.get());
    if (!limit_step)
        return 0;
    if (node->children.size() != 1)
        return 0;

    /// Cannot support LIMIT 10 WITH TIES because we don't know how many rows will be output
    if (limit_step->withTies())
        return 0;

    /// TopK filtering can skip source rows, so it is incompatible with exact rows_before_limit_at_least.
    if (limit_step->alwaysReadTillEnd())
        return 0;

    QueryPlan::Node * sorting_node = node->children.front();
    auto * sorting_step = typeid_cast<SortingStep *>(sorting_node->step.get());
    if (!sorting_step)
        return 0;
    if (sorting_node->children.size() != 1)
        return 0;

    /// The plan is traversed again after other optimizations, and some plans are optimized more than once
    /// (StorageMerge child plans, set subplans). Place the filter of a sorting step once.
    if (sorting_step->hasTopKThresholdTracker())
        return 0;

    size_t n = limit_step->getLimitForSorting();
    if (!n || (settings.max_limit_for_top_k_optimization && n > settings.max_limit_for_top_k_optimization))
        return 0;

    SortingStep::Type sorting_step_type = sorting_step->getType();
    if (sorting_step_type != SortingStep::Type::Full)
        return 0;

    const auto & sort_description = sorting_step->getSortDescription();
    const size_t num_sort_columns = sort_description.size();
    const auto & sort_col_desc = sort_description.front();
    const auto & sort_column = sorting_step->getInputHeaders().front()->getByName(sort_col_desc.column_name);

    /// Dynamic and Variant columns cannot be reliably filtered: their lessOrEquals
    /// returns Nullable(UInt8) rather than UInt8, causing an "Unexpected return type"
    /// logical error when the filter is executed. Comparison functions also
    /// reject zero-sized tuples even though ORDER BY supports them. Skip the optimization
    /// for these types.
    ///
    /// For variable-length types (e.g. String, Array, Map, Tuple containing variable-length
    /// elements), the per-row threshold comparison cost can exceed its savings — most notably
    /// when the column's lex-min value dominates and few granules can be skipped. Gate that
    /// path behind an explicit opt-in. Nullable and Tuple of fixed-length types are still
    /// considered fixed-length (haveMaximumSizeOfValue forwards through them).
    const bool sort_column_is_variable_length = !sort_column.type->haveMaximumSizeOfValue();
    const auto * sort_column_tuple_type = typeid_cast<const DataTypeTuple *>(sort_column.type.get());
    const bool use_dynamic_filtering = settings.use_top_k_dynamic_filtering
        && !isDynamic(sort_column.type)
        && !isVariant(sort_column.type)
        && (!sort_column_tuple_type || !sort_column_tuple_type->getElements().empty())
        && (!sort_column_is_variable_length || settings.use_top_k_dynamic_filtering_for_variable_length_types);

    if (!use_dynamic_filtering && !settings.use_skip_indexes_for_top_k)
        return 0;

    ///TopKThresholdTracker acts as a link between 3 components
    ///                                MergeTreeReaderIndex::canSkipMark() (skip whole granule using minmax index)
    ///                                  /
    ///         PartialSortingTransform/MergeSortingTransform --> ("publish" threshold value as sorting progresses)
    ///                                  \
    ///                                __topKFilter() (the first PREWHERE read step, or a format's own filter)
    auto threshold_tracker = std::make_shared<TopKThresholdTracker>(sort_col_desc);
    TopKFilterPlacement placement{settings, sort_col_desc, num_sort_columns, n, threshold_tracker, use_dynamic_filtering};
    placeTopKFilter(sorting_node->children.front(), sort_col_desc.column_name, sort_column.type, TopKFilterPath{}, placement);

    if (placement.uses_threshold)
        sorting_step->setTopKThresholdTracker(threshold_tracker);

    return 0;
}

}
