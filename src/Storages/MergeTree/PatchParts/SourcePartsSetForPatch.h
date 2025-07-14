#pragma once
#include <Storages/MergeTree/PatchParts/PatchPartInfo.h>
#include <Storages/MergeTree/ActiveDataPartSet.h>
#include <Core/Block.h>
#include "Storages/MutationCommands.h"

namespace DB
{

class ReadBuffer;
class WriteBuffer;

/** A helper index of source parts for which updated data is stored in the patch part.
  * It is used to get patches for the regular parts.
  */
class SourcePartsSetForPatch
{
public:
    static constexpr UInt8 INITIAL_VERSION = 0;
    static constexpr UInt8 VERSION_WITH_NUM_ROWS = 1;
    static constexpr auto SOURCE_PARTS_SET_FILENAME = "source_parts.dat";
    static constexpr auto PATCH_COMMANDS_FILENAME = "patch_commands.dat";

    SourcePartsSetForPatch() = default;

    bool empty() const { return min_max_versions_by_part.empty(); }
    UInt64 getMinDataVersion() const { return min_data_version; }
    UInt64 getMaxDataVersion() const { return max_data_version; }

    UInt64 getMinDataVersion(const String & part_name) const { return min_max_versions_by_part.at(part_name).min_version; }
    UInt64 getMaxDataVersion(const String & part_name) const { return min_max_versions_by_part.at(part_name).max_version; }

    void addSourcePart(const String & name, UInt64 data_version);
    void addMutationCommands(UInt64 data_version, const MutationCommands & commands);
    PatchParts getPatchParts(const MergeTreePartInfo & original_part, const DataPartPtr & patch_part) const;
    MutationCommands getMutationCommandsForParts(const Names & source_names, UInt64 source_data_version) const;

    static SourcePartsSetForPatch build(const Block & block, const MutationCommands & commands, UInt64 data_version);
    static SourcePartsSetForPatch merge(const DataPartsVector & source_parts);

    void writeSourcePartsSet(WriteBuffer & out) const;
    void readSourcePartsSet(ReadBuffer & in);

    void writePatchCommands(WriteBuffer & out) const;
    void readPatchCommands(ReadBuffer & in);

    struct SourcePartInfo
    {
        UInt64 min_version = 0;
        UInt64 max_version = 0;
    };

private:
    void buildSourcePartsSet();

    UInt8 version = INITIAL_VERSION;

    /// Max data version -> part set that contains all parts from min_max_versions_by_part with this max data version.
    /// Can be reconstructed from source_parts_by_version.
    std::map<UInt64, ActiveDataPartSet> source_parts_by_version;

    /// Part name -> min and max version of updated data stored in patch part for the source part.
    /// Serialized to the file on disk.
    std::map<String, SourcePartInfo> min_max_versions_by_part;

    /// Data version -> mutation commands that were applied to the source part to get the updated data.
    std::map<UInt64, MutationCommands> commands_by_version;

    UInt64 min_data_version = 0;
    UInt64 max_data_version = 0;
};

/// Returns set with source parts with _part column from block and data_version.
/// Updates _data_version in block with const value (data_version).
SourcePartsSetForPatch buildSourceSetForPatch(Block & block, const MutationCommands & commands, UInt64 data_version);

}
