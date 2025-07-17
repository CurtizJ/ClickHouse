#pragma once
#include <Storages/MergeTree/PatchParts/PatchPartInfo.h>
#include <Storages/MergeTree/MergeTreeReadTask.h>

namespace DB
{

struct PatchPartReadStats
{
    size_t join_patches_uncompressed_bytes = 0;
    size_t join_patches_max_uncompressed_bytes = 1;
    std::set<std::pair<String, MarkRange>> join_patches_read_ranges;
};

Names addPatchPartsColumns(
    MergeTreeReadTaskColumns & task_columns,
    PatchPartsForReader & new_patches,
    std::vector<NamesAndTypesList> & new_patch_columns,
    PatchPartReadStats & patch_stats,
    const StorageSnapshotPtr & storage_snapshot,
    const GetColumnsOptions & options,
    const std::vector<MarkRanges> & patch_ranges,
    bool has_lightweight_delete);

void incrementPatchPartsEventsForTask(const PatchPartsForReader & patch_parts);

}
