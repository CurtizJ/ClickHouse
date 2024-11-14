#pragma once
#include <Storages/MergeTree/KeyCondition.h>
#include <Storages/MergeTree/PartitionPruner.h>
#include "DataTypes/IDataType.h"
#include "Storages/MergeTree/IMergeTreeDataPart.h"
#include "Storages/MergeTree/MergeTreeData.h"

namespace DB
{

class IMergeTreeDataPart;
using PartitionIdToMaxBlock = std::unordered_map<String, Int64>;

class PartPruner
{
public:
    void applyFilters(
        const ActionsDAG * filter_actions_dag,
        const MergeTreeData & data,
        const MergeTreeDataPartsVector & parts,
        const StorageMetadataPtr & metadata_snapshot,
        const ContextPtr & context);

    bool isUseless() const;
    bool canPruneAllParts() const;
    bool canBePruned(const IMergeTreeDataPart & part, size_t num_granules);

    struct PartFilterCounters
    {
        size_t num_initial_selected_parts = 0;
        size_t num_initial_selected_granules = 0;
        size_t num_parts_after_minmax = 0;
        size_t num_granules_after_minmax = 0;
        size_t num_parts_after_partition_pruner = 0;
        size_t num_granules_after_partition_pruner = 0;
    };

    void resetCounters() { counters = {}; }

private:
    /// TODO: comment
    std::optional<KeyCondition> minmax_condition;

    /// TODO: comment
    DataTypes minmax_types;

    /// TODO: comment
    std::optional<PartitionPruner> partition_pruner;

    /// TODO: comment
    std::optional<std::unordered_set<String>> part_values;

    /// TODO: comment
    PartFilterCounters counters;
};

}
