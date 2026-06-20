#pragma once

#include <Core/MergeTreeSerializationEnums.h>
#include <Core/Names.h>
#include <Core/Types_fwd.h>
#include <DataTypes/Serializations/ISerialization.h>
#include <DataTypes/Serializations/SerializationInfoSettings.h>
#include <Storages/Statistics/Statistics.h>
#include <map>

namespace Poco::JSON
{
class Object;
}

namespace DB
{

class ReadBuffer;
class ReadBuffer;
class WriteBuffer;
class NamesAndTypesList;
class Block;

/** Contains information about kind of serialization of column and its subcolumns.
 *  Also contains information about content of columns,
 *  that helps to choose kind of serialization of column.
 *
 *  Currently has only information about number of default rows,
 *  that helps to choose sparse serialization.
 *
 *  Should be extended, when new kinds of serialization will be implemented.
 */
class SerializationInfo
{
public:
    using Settings = SerializationInfoSettings;
    SerializationInfo(ISerialization::KindStack kind_stack_, const SerializationInfoSettings & settings_);

    virtual ~SerializationInfo() = default;

    virtual bool hasCustomSerialization() const { return kind_stack.size() > 1; }
    virtual bool structureEquals(const SerializationInfo & rhs) const { return typeid(*this) == typeid(rhs); }

    virtual std::shared_ptr<SerializationInfo> clone() const;

    virtual std::shared_ptr<SerializationInfo> createWithType(
        const IDataType & old_type,
        const IDataType & new_type,
        const SerializationInfoSettings & new_settings) const;

    virtual void serialializeKindStackBinary(WriteBuffer & out) const;
    virtual void deserializeFromKindsBinary(ReadBuffer & in);

    /// Streaming writers for `serialization.json`. `writeJSON` emits only the serialization kind
    /// (and column name); `writeJSONWithStats` additionally emits `num_rows`/`num_defaults` taken
    /// from statistics (used by versions before `WITHOUT_DATA` that store the default count on disk).
    virtual void writeJSON(WriteBuffer & out, const String * name) const;
    virtual void writeJSONWithStats(WriteBuffer & out, const String * name, const Estimate & stats) const;

    /// Poco-based reader/writer of the kind. `toJSON` is used for introspection/tests; `fromJSON`
    /// reads the kind only, while `fromJSONWithStats` also reads `num_rows`/`num_defaults` into `stats`.
    virtual void toJSON(Poco::JSON::Object & object) const;
    virtual void fromJSON(const Poco::JSON::Object & object);
    virtual void fromJSONWithStats(const Poco::JSON::Object & object, Estimate & stats);

    void setKindStack(ISerialization::KindStack kind_stack_) { kind_stack = kind_stack_; }
    void appendToKindStack(ISerialization::Kind kind) { kind_stack.push_back(kind); }
    const SerializationInfoSettings & getSettings() const { return settings; }
    ISerialization::KindStack getKindStack() const { return kind_stack; }

protected:
    /// When `stats` is not null, also writes `num_rows`/`num_defaults` after the kind/name fields.
    virtual void writeJSONFields(WriteBuffer & out, const String * name, const Estimate * stats) const;

    const SerializationInfoSettings settings;
    ISerialization::KindStack kind_stack;
};

using SerializationInfoPtr = std::shared_ptr<const SerializationInfo>;
using MutableSerializationInfoPtr = std::shared_ptr<SerializationInfo>;

using SerializationInfos = std::vector<SerializationInfoPtr>;
using MutableSerializationInfos = std::vector<MutableSerializationInfoPtr>;

/// The order is important because info is serialized to part metadata.
class SerializationInfoByName : public std::map<String, MutableSerializationInfoPtr>
{
public:
    using Settings = SerializationInfoSettings;

    explicit SerializationInfoByName(const Settings & settings_);
    SerializationInfoByName(const NamesAndTypesList & columns, const Settings & settings_);

    SerializationInfoPtr tryGet(const String & name) const;
    MutableSerializationInfoPtr tryGet(const String & name);
    ISerialization::KindStack getKindStack(const String & column_name) const;

    void writeJSON(WriteBuffer & out) const;
    void writeJSONWithStats(WriteBuffer & out, const Estimates & stats) const;

    SerializationInfoByName clone() const;

    const Settings & getSettings() const { return settings; }

    MergeTreeSerializationInfoVersion getVersion() const;

    bool needsPersistence() const;

private:
    template <typename ElementWriter>
    void writeJSONImpl(WriteBuffer & out, ElementWriter && write_element) const;

    /// This field stores all configuration options that are not tied to a
    /// specific column entry in `SerializationInfoByName`. For example:
    /// - Per-type serialization versions (`types_serialization_versions`), e.g.,
    ///   specifying different versions for `String` or other types.
    ///
    /// Design notes:
    /// - We intentionally keep such options out of per-column `SerializationInfo` entries,
    ///   because the mere existence of a `SerializationInfo` entry triggers
    ///   sparse encoding logic. This would produce misleading content in
    ///   `serializations.json` for types that do not support sparse encoding.
    ///
    /// - By storing them centrally in `settings`, we avoid polluting
    ///   per-column entries and maintain a clear separation between
    ///   "global defaults" and "per-column overrides".
    ///
    /// - The default constructor was removed. Constructors now require
    ///   explicit `SerializationInfoSettings`, ensuring that in MergeTree
    ///   or other engines, the correct settings must always be provided for
    ///   consistent serialization behavior.
    Settings settings;
};

struct SerializationInfosLoadResult
{
    SerializationInfoByName infos;
    std::optional<Estimates> stats;
};

SerializationInfosLoadResult loadSerializationInfosFromBuffer(ReadBuffer & in);
SerializationInfosLoadResult loadSerializationInfosFromString(const std::string & str);
SerializationInfoByName loadSerializationInfosFromStatistics(const ColumnsStatistics & statistics, const SerializationInfoSettings & settings);

/// Create empty `Basic` statistics objects for every sparse-capable column.
ColumnsStatistics getImplicitStatisticsForSparseSerialization(const NamesAndTypesList & columns, const SerializationInfoSettings & settings);
ColumnsStatistics getImplicitStatisticsForSparseSerialization(const Block & block, const SerializationInfoSettings & settings);

/// Add implicit serialization `Basic` statistics into `statistics` for every column that lacks `Basic`, so the
/// per-column `num_defaults` is computed by the regular statistics machinery. Returns the columns whose `Basic`
/// was added implicitly (to be removed before writing the statistics files; see `getStatisticsToPersist`).
NameSet addImplicitSerializationStatistics(ColumnsStatistics & statistics, const ColumnsStatistics & implicit_statistics);
NameSet addImplicitSerializationStatistics(ColumnsStatistics & statistics, const NamesAndTypesList & columns, const SerializationInfoSettings & settings);

/// Statistics to persist into the part's statistics files: the implicitly added serialization `Basic` stats
/// (`implicit_serialization_statistics`) are removed and columns left empty are dropped. NOTE: removes the
/// implicit `Basic` objects in place, so the caller must already have extracted any needed estimates.
ColumnsStatistics getStatisticsToPersist(const ColumnsStatistics & statistics, const NameSet & implicit_serialization_statistics);

/// Write `serialization.json`.
/// For `WITHOUT_DATA` and newer only the serialization kind is written and `estimates` is ignored.
/// Older versions additionally write the per-column row/default counts taken from `estimates`.
void writeSerializationInfosJSON(WriteBuffer & out, const SerializationInfoByName & infos, const Estimates & estimates);

}
