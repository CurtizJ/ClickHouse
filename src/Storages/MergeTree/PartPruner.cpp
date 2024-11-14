#include <Storages/MergeTree/PartPruner.h>
#include <Storages/MergeTree/MergeTreeData.h>
#include <Storages/VirtualColumnUtils.h>

namespace DB
{

/// If possible, filter using expression on virtual columns.
/// Example: SELECT count() FROM table WHERE _part = 'part_name'
/// If expression found, return a set with allowed part names (std::nullopt otherwise).
static std::optional<std::unordered_set<String>> filterPartsByVirtualColumns(
    const StorageMetadataPtr & metadata_snapshot,
    const MergeTreeData & data,
    const MergeTreeData::DataPartsVector & parts,
    const ActionsDAG * filter_dag,
    ContextPtr context)
{
    auto sample = data.getHeaderWithVirtualsForFilter(metadata_snapshot);
    auto dag = VirtualColumnUtils::splitFilterDagForAllowedInputs(filter_dag->getOutputs().at(0), &sample);
    if (!dag)
        return {};

    auto virtual_columns_block = data.getBlockWithVirtualsForFilter(metadata_snapshot, parts);
    VirtualColumnUtils::filterBlockWithExpression(VirtualColumnUtils::buildFilterExpression(std::move(*dag), context), virtual_columns_block);
    return VirtualColumnUtils::extractSingleValueFromBlock<String>(virtual_columns_block, "_part");
}

void PartPruner::applyFilters(
    const ActionsDAG * filter_actions_dag,
    const MergeTreeData & data,
    const MergeTreeDataPartsVector & parts,
    const StorageMetadataPtr & metadata_snapshot,
    const ContextPtr & context)
{
    if (!filter_actions_dag)
        return;

    if (!parts.empty())
        part_values = filterPartsByVirtualColumns(metadata_snapshot, data, parts, filter_actions_dag, context);

    if (metadata_snapshot->hasPartitionKey())
        partition_pruner.emplace(metadata_snapshot, filter_actions_dag, context, /*strict=*/ false);

    auto minmax_column_names = data.getMinMaxColumnsNames(metadata_snapshot);

    if (!minmax_column_names.empty())
    {
        auto minmax_expression_actions = data.getMinMaxExpression(metadata_snapshot, ExpressionActionsSettings::fromContext(context));
        minmax_types = data.getMinMaxColumnsTypes(metadata_snapshot);
        minmax_condition.emplace(filter_actions_dag, context, minmax_column_names, minmax_expression_actions);
    }
}

bool PartPruner::canBePruned(const IMergeTreeDataPart & part, size_t num_granules)
{
    if (part_values && part_values->find(part.name) == part_values->end())
        return true;

    counters.num_initial_selected_parts += 1;
    counters.num_initial_selected_granules += num_granules;

    if (!part.minmax_idx->initialized)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Found a non-empty part with uninitialized minmax_idx. It's a bug");

    if (minmax_condition && !minmax_condition->checkInHyperrectangle(part.minmax_idx->hyperrectangle, minmax_types).can_be_true)
        return true;

    counters.num_parts_after_minmax += 1;
    counters.num_granules_after_minmax += num_granules;

    if (partition_pruner && partition_pruner->canBePruned(part))
        return true;

    counters.num_parts_after_partition_pruner += 1;
    counters.num_granules_after_partition_pruner += num_granules;

    return false;
}

bool PartPruner::isUseless() const
{
    if (part_values)
        return false;

    if (minmax_condition && !minmax_condition->alwaysUnknownOrTrue())
        return false;

    if (partition_pruner && !partition_pruner->isUseless())
        return false;

    return true;
}

bool PartPruner::canPruneAllParts() const
{
    if (part_values && part_values->empty())
        return true;

    return false;
}

}
