#include <DataTypes/Serializations/SerializationInfo.h>

#include <Columns/ColumnSparse.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/IDataType.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteHelpers.h>
#include <Core/Block.h>
#include <base/EnumReflection.h>

#include <Poco/JSON/JSON.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Stringifier.h>
#include <Poco/JSON/Parser.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}

namespace
{

constexpr auto KEY_VERSION = "version";
constexpr auto KEY_NUM_ROWS = "num_rows";
constexpr auto KEY_COLUMNS = "columns";
constexpr auto KEY_NUM_DEFAULTS = "num_defaults";
constexpr auto KEY_KIND = "kind";
constexpr auto KEY_NAME = "name";

constexpr auto KEY_TYPES_SERIALIZATION_VERSIONS = "types_serialization_versions";
constexpr auto KEY_STRING_SERIALIZATION_VERSION = "string";
constexpr auto KEY_NULLABLE_SERIALIZATION_VERSION = "nullable";
constexpr auto KEY_MAP_SERIALIZATION_VERSION = "map";
constexpr auto KEY_PROPAGATE_DATA_TYPES_SERIALIZATION_VERSIONS_TO_NESTED_TYPES = "propagate_types_serialization_versions_to_nested_types";

}

SerializationInfo::SerializationInfo(ISerialization::KindStack kind_stack_, const Settings & settings_)
    : settings(settings_), kind_stack(kind_stack_)
{
}

MutableSerializationInfoPtr SerializationInfo::clone() const
{
    return std::make_shared<SerializationInfo>(kind_stack, settings);
}

/// Returns true if all rows with default values of type 'lhs'
/// are mapped to default values of type 'rhs' after conversion.
static bool preserveDefaultsAfterConversion(const IDataType & lhs, const IDataType & rhs)
{
    if (lhs.equals(rhs))
        return true;

    bool lhs_is_columned_as_numeric = isColumnedAsNumber(lhs) || isColumnedAsDecimal(lhs);
    bool rhs_is_columned_as_numeric = isColumnedAsNumber(rhs) || isColumnedAsDecimal(rhs);

    if (lhs_is_columned_as_numeric && rhs_is_columned_as_numeric)
        return true;

    if (isStringOrFixedString(lhs) && isStringOrFixedString(rhs))
        return true;

    return false;
}

std::shared_ptr<SerializationInfo> SerializationInfo::createWithType(
    const IDataType & old_type,
    const IDataType & new_type,
    const Settings & new_settings) const
{
    ISerialization::KindStack new_kind_stack;
    for (auto kind : kind_stack)
    {
        if (kind == ISerialization::Kind::SPARSE
            && (!new_settings.canUseSparseSerialization(new_type) || !preserveDefaultsAfterConversion(old_type, new_type)))
            continue;
        new_kind_stack.push_back(kind);
    }

    auto new_info = new_type.createSerializationInfo(new_settings);
    new_info->kind_stack = new_kind_stack;
    return new_info;
}

namespace
{

enum class KindStackBinarySerializationType : UInt8
{
    /// First 4 added for compatibility with old versions where we didn't have kind stack but a single kind.
    DEFAULT = 0,
    SPARSE = 1, /// stack: {Default, Sparse}
    DETACHED = 2,  /// stack: {Default, Detached}
    DETACHED_OVER_SPARSE = 3,  /// stack: {Default, Sparse, Detached}
    REPLICATED = 4,  /// stack: {Default, Replicated}

    COMBINATION = 5, /// other stacks, serialized as number of kinds and all kinds one after another.
};

}

void SerializationInfo::serialializeKindStackBinary(WriteBuffer & out) const
{
    if (kind_stack == ISerialization::KindStack{ISerialization::Kind::DEFAULT})
    {
        writeBinary(static_cast<UInt8>(KindStackBinarySerializationType::DEFAULT), out);
    }
    else if (kind_stack == ISerialization::KindStack{ISerialization::Kind::DEFAULT, ISerialization::Kind::SPARSE})
    {
        writeBinary(static_cast<UInt8>(KindStackBinarySerializationType::SPARSE), out);
    }
    else if (kind_stack == ISerialization::KindStack{ISerialization::Kind::DEFAULT, ISerialization::Kind::DETACHED})
    {
        writeBinary(static_cast<UInt8>(KindStackBinarySerializationType::DETACHED), out);
    }
    else if (kind_stack == ISerialization::KindStack{ISerialization::Kind::DEFAULT, ISerialization::Kind::SPARSE, ISerialization::Kind::DETACHED})
    {
        writeBinary(static_cast<UInt8>(KindStackBinarySerializationType::DETACHED_OVER_SPARSE), out);
    }
    else if (kind_stack == ISerialization::KindStack{ISerialization::Kind::DEFAULT, ISerialization::Kind::REPLICATED})
    {
        writeBinary(static_cast<UInt8>(KindStackBinarySerializationType::REPLICATED), out);
    }
    else
    {
        writeBinary(static_cast<UInt8>(KindStackBinarySerializationType::COMBINATION), out);
        writeVarUInt(kind_stack.size(), out);
        for (auto kind : kind_stack)
            writeBinary(static_cast<UInt8>(kind), out);
    }
}

void SerializationInfo::deserializeFromKindsBinary(ReadBuffer & in)
{
    UInt8 type;
    readBinary(type, in);
    auto maybe_type = magic_enum::enum_cast<KindStackBinarySerializationType>(type);
    if (!maybe_type)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "Unknown serialization kind type {}", UInt32(type));

    switch (*maybe_type)
    {
        case KindStackBinarySerializationType::DEFAULT:
            kind_stack = {ISerialization::Kind::DEFAULT};
            break;
        case KindStackBinarySerializationType::SPARSE:
            kind_stack = {ISerialization::Kind::DEFAULT, ISerialization::Kind::SPARSE};
            break;
        case KindStackBinarySerializationType::DETACHED:
            kind_stack = {ISerialization::Kind::DEFAULT, ISerialization::Kind::DETACHED};
            break;
        case KindStackBinarySerializationType::DETACHED_OVER_SPARSE:
            kind_stack = {ISerialization::Kind::DEFAULT, ISerialization::Kind::SPARSE, ISerialization::Kind::DETACHED};
            break;
        case KindStackBinarySerializationType::REPLICATED:
            kind_stack = {ISerialization::Kind::DEFAULT, ISerialization::Kind::REPLICATED};
            break;
        case KindStackBinarySerializationType::COMBINATION:
        {
            size_t num_kinds;
            readVarUInt(num_kinds, in);
            for (size_t i = 0; i != num_kinds; ++i)
            {
                UInt8 kind;
                readBinary(kind, in);
                auto maybe_kind = magic_enum::enum_cast<ISerialization::Kind>(kind);
                if (!maybe_kind)
                    throw Exception(ErrorCodes::CORRUPTED_DATA, "Unknown serialization kind {}", UInt32(kind));
                kind_stack.push_back(*maybe_kind);
            }

            break;
        }
    }
}

void SerializationInfo::toJSON(Poco::JSON::Object & object) const
{
    object.set(KEY_KIND, ISerialization::kindStackToString(kind_stack));
}

void SerializationInfo::toJSONWithStats(Poco::JSON::Object & object, const Estimate & stats) const
{
    object.set(KEY_KIND, ISerialization::kindStackToString(kind_stack));
    object.set(KEY_NUM_DEFAULTS, stats.estimated_defaults.value_or(0));
    object.set(KEY_NUM_ROWS, stats.rows_count);
}

void SerializationInfo::fromJSON(const Poco::JSON::Object & object)
{
    if (!object.has(KEY_KIND))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "Missed field '{}' in SerializationInfo", KEY_KIND);

    kind_stack = ISerialization::stringToKindStack(object.getValue<String>(KEY_KIND));
}

void SerializationInfo::fromJSONWithStats(const Poco::JSON::Object & object, Estimate & stats)
{
    if (!object.has(KEY_KIND) || !object.has(KEY_NUM_DEFAULTS) || !object.has(KEY_NUM_ROWS))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "Missed field '{}' or '{}' or '{}' in SerializationInfo of columns",
            KEY_KIND, KEY_NUM_DEFAULTS, KEY_NUM_ROWS);

    stats.types.insert(StatisticsType::Defaults);
    stats.rows_count = object.getValue<size_t>(KEY_NUM_ROWS);
    stats.estimated_defaults = object.getValue<size_t>(KEY_NUM_DEFAULTS);
    kind_stack = ISerialization::stringToKindStack(object.getValue<String>(KEY_KIND));
}

SerializationInfoByName::SerializationInfoByName(const SerializationInfo::Settings & settings_)
    : settings(settings_)
{
    /// If all type-specific versions remain at their defaults, downgrade to BASIC to avoid emitting a WITH_TYPES format
    /// unnecessarily. This prevents an avoidable version bump and preserves maximum compatibility with older servers.
    settings.tryDowngradeToBasic();
}

SerializationInfoByName::SerializationInfoByName(const NamesAndTypesList & columns, const SerializationInfo::Settings & settings_)
    : SerializationInfoByName(settings_)
{
    if (settings.isAlwaysDefault())
        return;

    for (const auto & column : columns)
    {
        if (settings.canUseSparseSerialization(*column.type))
            emplace(column.name, column.type->createSerializationInfo(settings));
    }
}

SerializationInfoPtr SerializationInfoByName::tryGet(const String & name) const
{
    auto it = find(name);
    return it == end() ? nullptr : it->second;
}

MutableSerializationInfoPtr SerializationInfoByName::tryGet(const String & name)
{
    auto it = find(name);
    return it == end() ? nullptr : it->second;
}

ISerialization::KindStack SerializationInfoByName::getKindStack(const String & column_name) const
{
    auto it = find(column_name);
    return it != end() ? it->second->getKindStack() : ISerialization::KindStack{ISerialization::Kind::DEFAULT};
}

MergeTreeSerializationInfoVersion SerializationInfoByName::getVersion() const
{
    return settings.version;
}

bool SerializationInfoByName::needsPersistence() const
{
    return !empty() || getVersion() > MergeTreeSerializationInfoVersion::BASIC;
}

template <typename ElementWriter>
void SerializationInfoByName::writeJSONImpl(WriteBuffer & out, ElementWriter && write_element) const
{
    auto version = getVersion();
    Poco::JSON::Object object;
    Poco::JSON::Array column_infos;

    for (const auto & [name, info] : *this)
    {
        Poco::JSON::Object info_json;
        info_json.set(KEY_NAME, name);
        write_element(name, *info, info_json);
        column_infos.add(std::move(info_json)); /// NOLINT
    }

    object.set(KEY_VERSION, static_cast<uint8_t>(version));
    object.set(KEY_COLUMNS, std::move(column_infos)); /// NOLINT

    if (version >= MergeTreeSerializationInfoVersion::WITH_TYPES)
    {
        Poco::JSON::Object type_versions_obj;
        type_versions_obj.set(KEY_STRING_SERIALIZATION_VERSION, static_cast<size_t>(settings.string_serialization_version));

        if (settings.nullable_serialization_version != MergeTreeNullableSerializationVersion::BASIC)
            type_versions_obj.set(KEY_NULLABLE_SERIALIZATION_VERSION, static_cast<size_t>(settings.nullable_serialization_version));

        if (settings.map_serialization_version != MergeTreeMapSerializationVersion::BASIC)
            type_versions_obj.set(KEY_MAP_SERIALIZATION_VERSION, static_cast<size_t>(settings.map_serialization_version));

        object.set(KEY_TYPES_SERIALIZATION_VERSIONS, type_versions_obj);

        /// Write flag propagate_types_serialization_versions_to_nested_types only if it's set,
        /// so old versions can read this info if the flag is disabled.
        if (settings.propagate_types_serialization_versions_to_nested_types)
            object.set(KEY_PROPAGATE_DATA_TYPES_SERIALIZATION_VERSIONS_TO_NESTED_TYPES, settings.propagate_types_serialization_versions_to_nested_types);
    }

    std::ostringstream oss;     // STYLE_CHECK_ALLOW_STD_STRING_STREAM
    oss.exceptions(std::ios::failbit);
    Poco::JSON::Stringifier::stringify(object, oss);

    writeString(oss.str(), out);
}

void SerializationInfoByName::writeJSON(WriteBuffer & out) const
{
    writeJSONImpl(out,
        [&](const String &, const SerializationInfo & info, Poco::JSON::Object & info_json)
        {
            info.toJSON(info_json);
        });
}

void SerializationInfoByName::writeJSONWithStats(WriteBuffer & out, const Estimates & stats) const
{
    writeJSONImpl(out,
        [&](const String & name, const SerializationInfo & info, Poco::JSON::Object & info_json)
        {
            auto it = stats.find(name);
            if (it == stats.end())
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Missed statistics for column {}", name);

            info.toJSONWithStats(info_json, it->second);
        });
}

SerializationInfoByName SerializationInfoByName::clone() const
{
    SerializationInfoByName res(settings);
    for (const auto & [name, info] : *this)
        res.emplace(name, info->clone());
    return res;
}

SerializationInfosLoadResult loadSerializationInfosFromString(const std::string & json_str)
{
    Poco::JSON::Parser parser;
    auto object = parser.parse(json_str).extract<Poco::JSON::Object::Ptr>();

    if (!object->has(KEY_VERSION))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "Missed version of serialization infos");

    MergeTreeSerializationInfoVersion version = MergeTreeSerializationInfoVersion::BASIC;
    {
        auto version_value = static_cast<std::underlying_type_t<MergeTreeSerializationInfoVersion>>(object->getValue<size_t>(KEY_VERSION));
        auto maybe_enum = magic_enum::enum_cast<MergeTreeSerializationInfoVersion>(version_value);
        if (!maybe_enum)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "Unknown version of serialization infos ({})", version_value);

        version = *maybe_enum;
    }

    Poco::JSON::Array::Ptr columns_array;
    Poco::JSON::Object::Ptr type_versions_obj;
    bool propagate_types_serialization_versions_to_nested_types = false;

    for (const auto & [key, value] : *object)
    {
        if (key == KEY_VERSION)
        {
            continue;
        }
        else if (key == KEY_COLUMNS)
        {
            columns_array = value.extract<Poco::JSON::Array::Ptr>();
        }
        else if (version >= MergeTreeSerializationInfoVersion::WITH_TYPES && key == KEY_TYPES_SERIALIZATION_VERSIONS)
        {
            type_versions_obj = value.extract<Poco::JSON::Object::Ptr>();
        }
        else if (key == KEY_PROPAGATE_DATA_TYPES_SERIALIZATION_VERSIONS_TO_NESTED_TYPES)
        {
            propagate_types_serialization_versions_to_nested_types = value.extract<bool>();
        }
        else
        {
            throw Exception(ErrorCodes::CORRUPTED_DATA, "Unexpected field '{}' in MergeTreeSerializationInfo JSON", key);
        }
    }

    MergeTreeStringSerializationVersion string_serialization_version = MergeTreeStringSerializationVersion::SINGLE_STREAM;
    MergeTreeNullableSerializationVersion nullable_serialization_version = MergeTreeNullableSerializationVersion::BASIC;
    MergeTreeMapSerializationVersion map_serialization_version = MergeTreeMapSerializationVersion::BASIC;

    if (version >= MergeTreeSerializationInfoVersion::WITH_TYPES)
    {
        /// types_serialization_versions is mandatory in WITH_TYPES mode
        if (!type_versions_obj)
        {
            throw Exception(
                ErrorCodes::CORRUPTED_DATA,
                "Missing mandatory field 'types_serialization_versions' in MergeTreeSerializationInfo (version WITH_TYPES)");
        }

        for (const auto & [type_name, value] : *type_versions_obj)
        {
            auto version_value = static_cast<std::underlying_type_t<MergeTreeStringSerializationVersion>>(value.convert<size_t>());
            if (type_name == KEY_STRING_SERIALIZATION_VERSION)
            {
                auto maybe_enum = magic_enum::enum_cast<MergeTreeStringSerializationVersion>(version_value);
                if (!maybe_enum.has_value())
                    throw Exception(ErrorCodes::CORRUPTED_DATA, "Invalid version {} for type '{}'", version_value, type_name);

                string_serialization_version = *maybe_enum;
            }
            else if (type_name == KEY_NULLABLE_SERIALIZATION_VERSION)
            {
                auto maybe_enum = magic_enum::enum_cast<MergeTreeNullableSerializationVersion>(version_value);
                if (!maybe_enum.has_value())
                    throw Exception(ErrorCodes::CORRUPTED_DATA, "Invalid version {} for type '{}'", version_value, type_name);

                nullable_serialization_version = *maybe_enum;
            }
            else if (type_name == KEY_MAP_SERIALIZATION_VERSION)
            {
                auto maybe_enum = magic_enum::enum_cast<MergeTreeMapSerializationVersion>(version_value);
                if (!maybe_enum.has_value())
                    throw Exception(ErrorCodes::CORRUPTED_DATA, "Invalid version {} for type '{}'", version_value, type_name);
                map_serialization_version = *maybe_enum;
            }
            else
            {
                throw Exception(ErrorCodes::CORRUPTED_DATA, "Unknown field '{}' in types_serialization_versions", type_name);
            }
        }
    }

    SerializationInfoSettings settings(
        1.0 /* Doesn't matter when constructing from JSON */,
        false /* Cannot choose kind when constructing from JSON */,
        version,
        string_serialization_version,
        nullable_serialization_version,
        map_serialization_version,
        propagate_types_serialization_versions_to_nested_types);

    std::optional<Estimates> stats;
    SerializationInfoByName infos(settings);

    if (version >= MergeTreeSerializationInfoVersion::WITHOUT_DATA)
    {
        stats = Estimates();
    }

    if (!columns_array)
        return {infos, stats};

    for (const auto & elem : *columns_array)
    {
        const auto & elem_object = elem.extract<Poco::JSON::Object::Ptr>();

        if (!elem_object->has(KEY_NAME))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "Missed field '{}' in serialization infos", KEY_NAME);

        auto name = elem_object->getValue<String>(KEY_NAME);
        auto kind_stack = ISerialization::KindStack{ISerialization::Kind::DEFAULT};
        auto info = std::make_shared<SerializationInfo>(kind_stack, settings);

        if (version >= MergeTreeSerializationInfoVersion::WITHOUT_DATA)
        {
            info->fromJSON(*elem_object);
        }
        else
        {
            auto & current_stats = stats.value()[name];
            info->fromJSONWithStats(*elem_object, current_stats);
        }

        infos.emplace(name, std::move(info));
    }

    return {infos, stats};
}

SerializationInfosLoadResult loadSerializationInfosFromBuffer(ReadBuffer & in)
{
    String json_str;
    readString(json_str, in);
    return loadSerializationInfosFromString(json_str);
}

SerializationInfoByName loadSerializationInfosFromStatistics(const ColumnsStatistics & statistics, const SerializationInfoSettings & settings)
{
    SerializationInfoByName infos(settings);
    if (settings.isAlwaysDefault())
        return infos;

    for (const auto & [column_name, column_stats] : statistics)
    {
        size_t num_rows = column_stats->getNumRows();
        auto data_type = column_stats->getDataType();
        const auto & stats = column_stats->getStats();

        if (data_type->supportsSparseSerialization() && stats.contains(StatisticsType::Defaults))
        {
            size_t num_defaults = stats.at(StatisticsType::Defaults)->estimateDefaults();
            Float64 ratio = static_cast<Float64>(num_defaults) / static_cast<Float64>(num_rows);

            if (ratio > settings.ratio_of_defaults_for_sparse)
            {
                auto kind_stack = ISerialization::KindStack{ISerialization::Kind::DEFAULT, ISerialization::Kind::SPARSE};
                infos.emplace(column_name, std::make_shared<SerializationInfo>(kind_stack, settings));
                continue;
            }
        }

        auto kind_stack = ISerialization::KindStack{ISerialization::Kind::DEFAULT};
        infos.emplace(column_name, std::make_shared<SerializationInfo>(kind_stack, settings));
    }

    return infos;
}

ColumnsStatistics getImplicitStatisticsForSparseSerialization(const Block & block, const SerializationInfoSettings & settings)
{
    if (settings.ratio_of_defaults_for_sparse >= 1.0)
        return {};

    ColumnsStatistics statistics;
    for (const auto & column : block)
    {
        if (!column.type->supportsSparseSerialization())
            continue;

        ColumnStatisticsDescription desc;
        SingleStatisticsDescription stat_desc(StatisticsType::Defaults, nullptr, true);

        desc.data_type = column.type;
        desc.types_to_desc.emplace(StatisticsType::Defaults, std::move(stat_desc));

        statistics.emplace(column.name, MergeTreeStatisticsFactory::instance().get(desc));
    }

    return statistics;
}

}
