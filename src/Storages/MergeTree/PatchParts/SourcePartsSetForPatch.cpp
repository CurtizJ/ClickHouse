#include <Storages/MergeTree/PatchParts/SourcePartsSetForPatch.h>
#include <Columns/ColumnLowCardinality.h>
#include <Columns/ColumnString.h>
#include <Storages/MergeTree/IMergeTreeDataPart.h>
#include <Storages/MergeTree/MergeTreeVirtualColumns.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteHelpers.h>
#include "Common/logger_useful.h"

namespace DB
{

namespace ErrorCodes
{
    extern const int DUPLICATE_DATA_PART;
    extern const int INCORRECT_DATA;
}

void SourcePartsSetForPatch::addSourcePart(const String & name, UInt64 data_version)
{
    if (min_max_versions_by_part.contains(name))
        throw Exception(ErrorCodes::DUPLICATE_DATA_PART, "Source part {} already exists", name);

    if (empty())
    {
        min_data_version = data_version;
        max_data_version = data_version;
    }
    else
    {
        min_data_version = std::min(min_data_version, data_version);
        max_data_version = std::max(max_data_version, data_version);
    }

    source_parts_by_version[data_version].add(name);
    min_max_versions_by_part[name] = {data_version, data_version};
}

void SourcePartsSetForPatch::addMutationCommands(UInt64 data_version, const MutationCommands & commands)
{
    if (commands_by_version.contains(data_version))
        throw Exception(ErrorCodes::DUPLICATE_DATA_PART, "Mutation commands for data version {} already exist", data_version);

    commands_by_version[data_version] = commands;
}

MutationCommands SourcePartsSetForPatch::getMutationCommandsForParts(const Names & source_names, UInt64 source_data_version) const
{
    if (source_names.empty())
        return {};

    UInt64 min_version = std::numeric_limits<UInt64>::max();
    UInt64 max_version = 0;

    for (const auto & name : source_names)
    {
        min_version = std::min(min_version, getMinDataVersion(name));
        max_version = std::max(max_version, getMaxDataVersion(name));
    }

    if (max_version <= source_data_version)
        return {};

    min_version = std::max(min_version, source_data_version);

    auto lo = commands_by_version.lower_bound(min_version);
    auto hi = commands_by_version.upper_bound(max_version);

    MutationCommands commands;
    for (auto jt = lo; jt != hi; ++jt)
        commands.insert(commands.end(), jt->second.begin(), jt->second.end());

    return commands;
}

void SourcePartsSetForPatch::buildSourcePartsSet()
{
    min_data_version = 0;
    max_data_version = 0;
    source_parts_by_version.clear();

    bool is_first = true;
    for (const auto & [part_name, source_part_info] : min_max_versions_by_part)
    {
        source_parts_by_version[source_part_info.max_version].add(part_name);

        if (std::exchange(is_first, false))
        {
            min_data_version = source_part_info.min_version;
            max_data_version = source_part_info.max_version;
        }
        else
        {
            min_data_version = std::min(min_data_version, source_part_info.min_version);
            max_data_version = std::max(max_data_version, source_part_info.max_version);
        }
    }
}

PatchParts SourcePartsSetForPatch::getPatchParts(const MergeTreePartInfo & original_part, const DataPartPtr & patch_part) const
{
    UInt64 data_version = original_part.getDataVersion();
    auto it = source_parts_by_version.upper_bound(data_version);

    if (it == source_parts_by_version.end())
        return {};

    PatchParts patch_parts;
    auto part_name = original_part.getPartNameV1();

    NameSet names_for_join;
    bool has_merge = false;

    for (; it != source_parts_by_version.end(); ++it)
    {
        auto covered_parts = it->second.getPartsCoveredBy(original_part);

        if (covered_parts.size() == 1 && covered_parts.front() == part_name)
            has_merge = true;
        else
            std::move(covered_parts.begin(), covered_parts.end(), std::inserter(names_for_join, names_for_join.end()));
    }

    if (has_merge)
    {
        patch_parts.push_back(PatchPartInfo
        {
            .mode = PatchMode::Merge,
            .part = patch_part,
            .source_parts = {part_name},
            .source_data_version = original_part.getDataVersion(),
        });
    }

    if (!names_for_join.empty())
    {
        for (const auto & name : names_for_join)
        {
            auto min_version = getMinDataVersion(name);
            auto max_version = getMaxDataVersion(name);

            auto lo = commands_by_version.lower_bound(min_version);
            auto hi = commands_by_version.upper_bound(max_version);

            for (auto jt = lo; jt != hi; ++jt)
            {
                LOG_DEBUG(getLogger("KEK"), "commands for part {}: {}", name, jt->second.toString());
            }
        }

        patch_parts.push_back(PatchPartInfo
        {
            .mode = PatchMode::Join,
            .part = patch_part,
            .source_parts = Names(names_for_join.begin(), names_for_join.end()),
            .source_data_version = original_part.getDataVersion(),
        });
    }

    return patch_parts;
}

SourcePartsSetForPatch SourcePartsSetForPatch::build(const Block & block, const MutationCommands & commands, UInt64 data_version)
{
    const auto & column_part_name = block.getByName("_part").column;
    const auto & part_name_lc = assert_cast<const ColumnLowCardinality &>(*column_part_name);
    const auto & part_name_dict = part_name_lc.getDictionary().getNestedColumn();
    const auto & part_name_str = assert_cast<const ColumnString &>(*part_name_dict);

    SourcePartsSetForPatch parts_set;
    for (size_t i = 0; i < part_name_str.size(); ++i)
    {
        auto part_name = part_name_str.getDataAt(i).toString();

        /// LowCardinality dictionary always has default value.
        if (!part_name.empty())
            parts_set.addSourcePart(part_name, data_version);
    }

    parts_set.addMutationCommands(data_version, commands);
    return parts_set;
}

SourcePartsSetForPatch SourcePartsSetForPatch::merge(const DataPartsVector & source_parts)
{
    SourcePartsSetForPatch merged_set;

    for (const auto & part : source_parts)
    {
        const auto & set = part->getSourcePartsSet();

        for (const auto & [part_name, source_info] : set.min_max_versions_by_part)
        {
            auto [it, inserted] = merged_set.min_max_versions_by_part.emplace(part_name, source_info);

            if (!inserted)
            {
                auto & merge_info = it->second;

                merge_info.min_version = std::min(merge_info.min_version, source_info.min_version);
                merge_info.max_version = std::max(merge_info.max_version, source_info.max_version);
            }
        }

        for (const auto & [data_version, commands] : set.commands_by_version)
            merged_set.addMutationCommands(data_version, commands);
    }

    merged_set.buildSourcePartsSet();
    return merged_set;
}

void SourcePartsSetForPatch::writeSourcePartsSet(WriteBuffer & out) const
{
    writeBinaryLittleEndian(VERSION_WITH_NUM_ROWS, out);
    writeBinaryLittleEndian(min_max_versions_by_part.size(), out);

    for (const auto & [part_name, source_info] : min_max_versions_by_part)
    {
        writeStringBinary(part_name, out);
        writeBinaryLittleEndian(source_info.min_version, out);
        writeBinaryLittleEndian(source_info.max_version, out);
    }
}

void SourcePartsSetForPatch::readSourcePartsSet(ReadBuffer & in)
{
    readBinaryLittleEndian(version, in);

    if (version > VERSION_WITH_NUM_ROWS)
        throw Exception(ErrorCodes::INCORRECT_DATA, "Invalid version of SourcePartsSetForPatch: {}", std::to_string(version));

    UInt64 num_parts;
    readBinaryLittleEndian(num_parts, in);

    for (size_t i = 0; i < num_parts; ++i)
    {
        String part_name;
        readStringBinary(part_name, in);

        auto & source_part_info = min_max_versions_by_part[part_name];
        readBinaryLittleEndian(source_part_info.min_version, in);
        readBinaryLittleEndian(source_part_info.max_version, in);
    }

    buildSourcePartsSet();
}

void SourcePartsSetForPatch::writePatchCommands(WriteBuffer & out) const
{
    UNUSED(out);
}

void SourcePartsSetForPatch::readPatchCommands(ReadBuffer & in)
{
    UNUSED(in);
}

SourcePartsSetForPatch buildSourceSetForPatch(Block & block, const MutationCommands & commands, UInt64 data_version)
{
    /// Need to update data version column because it contains data version
    /// of source part, but we store the data version of updated data in patch part.
    auto & data_version_column = block.getByName(PartDataVersionColumn::name).column;
    data_version_column = PartDataVersionColumn::type->createColumnConst(block.rows(), data_version)->convertToFullColumnIfConst();
    return SourcePartsSetForPatch::build(block, commands, data_version);
}

}
