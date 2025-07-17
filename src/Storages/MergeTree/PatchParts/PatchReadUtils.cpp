#include <Storages/MergeTree/PatchParts/PatchReadUtils.h>
#include <Storages/MergeTree/LoadedMergeTreeDataPartInfoForReader.h>
#include <Storages/MergeTree/MergeTreeVirtualColumns.h>

namespace ProfileEvents
{
    extern const Event ReadTasksWithAppliedPatches;
    extern const Event PatchesAppliedInAllReadTasks;
    extern const Event PatchesMergeAppliedInAllReadTasks;
    extern const Event PatchesJoinAppliedInAllReadTasks;
    extern const Event PatchesExpressionAppliedInAllReadTasks;
}

namespace DB
{

namespace
{

void convertPatchJoinToExpression(
    PatchPartInfoForReader & patch_part,
    PatchPartReadStats & stats,
    const MarkRanges & patch_ranges,
    const Names & patch_columns)
{
    if (patch_part.mode != PatchMode::Join)
        return;

    MarkRanges ranges_to_read;
    const auto & index_granularity = patch_part.part->getIndexGranularity();
    const auto & patch_part_name = patch_part.part->getPartName();

    for (const auto & range : patch_ranges)
    {
        if (!stats.join_patches_read_ranges.contains({patch_part_name, range}))
            ranges_to_read.push_back(range);
    }

    if (ranges_to_read.empty())
        return;

    size_t uncompressed_bytes_to_read = 0;
    size_t rows_to_read = index_granularity.getRowsCountInRanges(ranges_to_read);
    double ratio = static_cast<double>(rows_to_read) / index_granularity.getTotalRows();

    for (const auto & column_name : patch_columns)
    {
        auto column_size = patch_part.part->getColumnSize(column_name);
        uncompressed_bytes_to_read += static_cast<size_t>(column_size.data_uncompressed * ratio);
    }

    if (stats.join_patches_uncompressed_bytes + uncompressed_bytes_to_read <= stats.join_patches_max_uncompressed_bytes)
    {
        for (const auto & range : ranges_to_read)
            stats.join_patches_read_ranges.emplace(patch_part_name, range);

        stats.join_patches_uncompressed_bytes += uncompressed_bytes_to_read;
        return;
    }

    patch_part.mode = PatchMode::Expression;
}

NameSet addColumnsForPatchExpression(
    MergeTreeReadTaskColumns & result,
    PatchPartInfoForReader & patch_part,
    const StorageSnapshotPtr & storage_snapshot,
    const GetColumnsOptions & options)
{
    const auto * loaded_part_info = dynamic_cast<const LoadedMergeTreeDataPartInfoForReader *>(patch_part.part.get());
    if (!loaded_part_info)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Expected loaded part info for expression patch part");

    const auto & data_part = loaded_part_info->getDataPart();
    const auto & source_parts_set = data_part->getSourcePartsSet();
    auto commands = source_parts_set.getMutationCommandsForParts(patch_part.source_parts, patch_part.source_data_version);

    NameSet all_added_columns;
    NameSet columns_from_previous_steps;

    auto analyze_commands_for_step = [&](auto & step_columns)
    {
        Names new_step_columns;
        NameSet new_step_columns_set;

        for (const auto & column : step_columns)
            columns_from_previous_steps.insert(column.name);

        auto step_columns_set = step_columns.getNameSet();
        auto step_commands = AlterConversions::filterMutationCommands(new_step_columns, commands, step_columns_set);

        for (const auto & column_name : new_step_columns)
        {
            if (columns_from_previous_steps.emplace(column_name).second)
            {
                step_columns.push_back(storage_snapshot->getColumn(options, column_name));
                new_step_columns_set.insert(column_name);
            }
        }

        return new_step_columns_set;
    };

    for (size_t i = 0; i < result.pre_columns.size(); ++i)
    {
        auto new_step_columns_set = analyze_commands_for_step(result.pre_columns[i]);
        all_added_columns.insert(new_step_columns_set.begin(), new_step_columns_set.end());

        for (size_t j = i + 1; j < result.pre_columns.size(); ++j)
            result.pre_columns[j] = result.pre_columns[j].eraseNames(new_step_columns_set);

        result.columns = result.columns.eraseNames(new_step_columns_set);
    }

    auto new_step_columns_set = analyze_commands_for_step(result.columns);
    all_added_columns.insert(new_step_columns_set.begin(), new_step_columns_set.end());
    return all_added_columns;
}

}

Names addPatchPartsColumns(
    MergeTreeReadTaskColumns & task_columns,
    PatchPartsForReader & new_patches,
    std::vector<NamesAndTypesList> & new_patch_columns,
    PatchPartReadStats & patch_stats,
    const StorageSnapshotPtr & storage_snapshot,
    const GetColumnsOptions & options,
    const std::vector<MarkRanges> & patch_ranges,
    bool has_lightweight_delete)
{
    chassert(new_patches.size() == patch_ranges.size());
    if (new_patches.empty())
        return {};

    NameSet required_virtuals;
    NameSet all_added_columns;

    new_patch_columns.resize(new_patches.size());
    auto all_columns_to_read = task_columns.getAllColumnNames();

    for (size_t i = 0; i < new_patches.size(); ++i)
    {
        NameSet patch_columns_to_read_set;

        const auto & patch_part_columns = new_patches[i].part->getColumnsDescription();
        const auto & alter_conversions = new_patches[i].part->getAlterConversions();

        for (const auto & column_name : all_columns_to_read)
        {
            auto column_in_storage = storage_snapshot->getColumn(options, column_name);
            auto column_name_in_patch = column_in_storage.getNameInStorage();

            if (alter_conversions && alter_conversions->isColumnRenamed(column_name_in_patch))
                column_name_in_patch = alter_conversions->getColumnOldName(column_name_in_patch);

            if (!patch_part_columns.hasPhysical(column_name_in_patch))
                continue;

            /// Add requested column name, not the column name in patch, for correct query analysis and applying patches.
            /// This column name will be translated to the column name in patch in MergeTree reader.
            patch_columns_to_read_set.insert(column_name);
        }

        if (has_lightweight_delete && patch_part_columns.has(RowExistsColumn::name))
        {
            patch_columns_to_read_set.insert(RowExistsColumn::name);
        }

        auto patch_system_columns = getVirtualsRequiredForPatch(new_patches[i]);
        patch_columns_to_read_set.insert(patch_system_columns.begin(), patch_system_columns.end());
        Names patch_columns_to_read_names(patch_columns_to_read_set.begin(), patch_columns_to_read_set.end());

        convertPatchJoinToExpression(new_patches[i], patch_stats, patch_ranges[i], patch_columns_to_read_names);

        if (new_patches[i].mode == PatchMode::Expression)
        {
            patch_system_columns = getVirtualsRequiredForPatch(new_patches[i]);
            patch_columns_to_read_names = patch_system_columns;

            auto added_columns = addColumnsForPatchExpression(task_columns, new_patches[i], storage_snapshot, options);
            all_added_columns.insert(added_columns.begin(), added_columns.end());
        }

        new_patch_columns[i] = storage_snapshot->getColumnsByNames(options, patch_columns_to_read_names);
        required_virtuals.insert(patch_system_columns.begin(), patch_system_columns.end());
    }

    auto & first_step_columns = task_columns.pre_columns.empty() ? task_columns.columns : task_columns.pre_columns.front();
    auto first_step_columns_set = first_step_columns.getNameSet();

    for (const auto & virtual_name : required_virtuals)
    {
        if (!first_step_columns_set.contains(virtual_name))
        {
            auto column = storage_snapshot->getColumn(options, virtual_name);
            first_step_columns.push_back(std::move(column));
        }
    }

    return Names(all_added_columns.begin(), all_added_columns.end());
}

void incrementPatchPartsEventsForTask(const PatchPartsForReader & patch_parts)
{
    if (patch_parts.empty())
        return;

    ProfileEvents::increment(ProfileEvents::ReadTasksWithAppliedPatches);
    ProfileEvents::increment(ProfileEvents::PatchesAppliedInAllReadTasks, patch_parts.size());

    for (const auto & patch : patch_parts)
    {
        switch (patch.mode)
        {
            case PatchMode::Join:
                ProfileEvents::increment(ProfileEvents::PatchesJoinAppliedInAllReadTasks);
                break;
            case PatchMode::Merge:
                ProfileEvents::increment(ProfileEvents::PatchesMergeAppliedInAllReadTasks);
                break;
            case PatchMode::Expression:
                ProfileEvents::increment(ProfileEvents::PatchesExpressionAppliedInAllReadTasks);
                break;
        }
    }
}

}
