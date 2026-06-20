#pragma once

#include <Storages/MergeTree/IDataPartStorage.h>
#include <Storages/MergeTree/IMergeTreeDataPart.h>
#include <Storages/MergeTree/IMergeTreeDataPartWriter.h>
#include <Storages/MergeTree/MergeTreeData.h>
#include <Storages/MergeTree/MergeTreeIndexGranularity.h>
#include <Common/Logger.h>

namespace DB
{

struct MergeTreeSettings;
using MergeTreeSettingsPtr = std::shared_ptr<const MergeTreeSettings>;

class IMergedBlockOutputStream
{
public:
    IMergedBlockOutputStream(
        MergeTreeSettingsPtr storage_settings_,
        MutableDataPartStoragePtr data_part_storage_,
        const StorageMetadataPtr & metadata_snapshot_,
        bool reset_columns_);

    virtual ~IMergedBlockOutputStream() = default;


    struct GatheredData
    {
        MergeTreeData::DataPart::Checksums checksums;
        ColumnsSubstreams columns_substreams;
        ColumnsStatistics statistics;
        /// Columns whose `Basic` statistics were added implicitly for serialization (to recompute the
        /// per-column `num_defaults` stored in `serialization.json` for versions before WITHOUT_DATA).
        /// They are part of `statistics` while building, and removed before the statistics files are written.
        NameSet implicit_serialization_statistics;
    };

    virtual void write(const Block & block) = 0;
    virtual void cancel() noexcept = 0;

    MergeTreeIndexGranularityPtr getIndexGranularity() const
    {
        return writer->getIndexGranularity();
    }

    MergeTreeWriterSettings getWriterSettings() const
    {
        return writer->getWriterSettings();
    }

    PlainMarksByName releaseCachedMarks()
    {
        return writer ? writer->releaseCachedMarks() : PlainMarksByName{};
    }

    PlainMarksByName releaseCachedIndexMarks()
    {
        return writer ? writer->releaseCachedIndexMarks() : PlainMarksByName{};
    }

    size_t getNumberOfOpenStreams() const
    {
        return writer->getNumberOfOpenStreams();
    }

protected:
    /// Remove all columns in @empty_columns. Also, clears checksums
    /// and columns array. Return set of removed files names.
    NameSet removeEmptyColumnsFromPart(
        const MergeTreeDataPartPtr & data_part,
        NamesAndTypesList & columns,
        const NameSet & empty_columns,
        SerializationInfoByName & serialization_infos,
        MergeTreeData::DataPart::Checksums & checksums);

    MergeTreeSettingsPtr storage_settings;
    LoggerPtr log;

    StorageMetadataPtr metadata_snapshot;

    MutableDataPartStoragePtr data_part_storage;
    MergeTreeDataPartWriterPtr writer;

    bool reset_columns = false;
};

using IMergedBlockOutputStreamPtr = std::shared_ptr<IMergedBlockOutputStream>;

}
