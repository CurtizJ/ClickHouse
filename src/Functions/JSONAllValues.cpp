#include <Functions/IFunction.h>
#include <Functions/FunctionHelpers.h>
#include <Functions/FunctionFactory.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypeObject.h>
#include <DataTypes/DataTypesBinaryEncoding.h>
#include <DataTypes/Serializations/SerializationDynamic.h>
#include <Columns/ColumnObject.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnArray.h>
#include <Formats/FormatSettings.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int BAD_ARGUMENTS;
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
}

namespace
{

/// Returns all values from a JSON column as an array of strings, in sorted path order.
class FunctionJSONAllValues : public IFunction
{
public:
    static constexpr auto name = "JSONAllValues";

    static FunctionPtr create(ContextPtr) { return std::make_shared<FunctionJSONAllValues>(); }

    String getName() const override { return name; }
    size_t getNumberOfArguments() const override { return 1; }
    bool useDefaultImplementationForConstants() const override { return true; }
    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo &) const override { return false; }

    DataTypePtr getReturnTypeImpl(const DataTypes & data_types) const override
    {
        if (data_types.size() != 1)
            throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
                "Function {} requires single argument with type JSON", getName());

        if (data_types[0]->getTypeId() != TypeIndex::Object)
            throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                "Function {} requires argument with type JSON, got: {}",
                getName(), data_types[0]->getName());

        return std::make_shared<DataTypeArray>(std::make_shared<DataTypeString>());
    }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr &, size_t) const override
    {
        const auto & elem = arguments[0];
        const auto * column_object = typeid_cast<const ColumnObject *>(elem.column.get());

        if (!column_object)
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "Unexpected column type in function {}. Expected Object column, got {}",
                getName(), elem.column->getName());

        const auto & type_object = assert_cast<const DataTypeObject &>(*elem.type);
        return execute(*column_object, type_object);
    }

private:
    struct TypedPathInfo
    {
        std::string_view path;
        const IColumn * column;
        SerializationPtr serialization;
    };

    struct DynamicPathInfo
    {
        std::string_view path;
        const IColumn * column;
    };

    /// We merge typed and dynamic paths into a single sorted list, tagged by kind.
    struct PathEntry
    {
        std::string_view path;
        bool is_typed;
        size_t index; /// index into typed_infos or dynamic_infos
    };

    /// Cache entry for shared data types: avoids repeated createColumn and getDefaultSerialization calls.
    struct CachedTypeInfo
    {
        DataTypePtr type;
        SerializationPtr serialization;
        MutableColumnPtr column;
    };

    ColumnPtr execute(const ColumnObject & column_object, const DataTypeObject & type_object) const
    {
        auto res = ColumnArray::create(ColumnString::create());
        auto & offsets = res->getOffsets();
        auto & result_data = assert_cast<ColumnString &>(res->getData());

        FormatSettings format_settings;

        /// Collect typed + dynamic paths, sorted for deterministic output order.
        const auto & typed_path_types = type_object.getTypedPaths();
        const auto & typed_path_columns = column_object.getTypedPaths();
        const auto & dynamic_path_columns = column_object.getDynamicPaths();

        std::vector<TypedPathInfo> typed_infos;
        typed_infos.reserve(typed_path_types.size());
        for (const auto & [path, type] : typed_path_types)
        {
            auto it = typed_path_columns.find(path);
            typed_infos.push_back({path, it->second.get(), type->getDefaultSerialization()});
        }

        std::vector<DynamicPathInfo> dynamic_infos;
        dynamic_infos.reserve(dynamic_path_columns.size());

        for (const auto & [path, column] : dynamic_path_columns)
            dynamic_infos.push_back({path, column.get()});

        std::vector<PathEntry> sorted_paths;
        sorted_paths.reserve(typed_infos.size() + dynamic_infos.size());

        for (size_t idx = 0; idx < typed_infos.size(); ++idx)
            sorted_paths.push_back({typed_infos[idx].path, true, idx});

        for (size_t idx = 0; idx < dynamic_infos.size(); ++idx)
            sorted_paths.push_back({dynamic_infos[idx].path, false, idx});

        std::sort(sorted_paths.begin(), sorted_paths.end(),
            [](const PathEntry & a, const PathEntry & b) { return a.path < b.path; });

        auto dynamic_serialization = SerializationDynamic::create();

        /// Cache of reusable (data_type, serialization, column) structs keyed by type name,
        /// to avoid createColumn and getDefaultSerialization per shared data value.
        std::unordered_map<String, CachedTypeInfo> type_cache;

        const auto & shared_data_offsets = column_object.getSharedDataOffsets();
        const auto [shared_data_paths, shared_data_values] = column_object.getSharedDataPathsAndValues();

        for (size_t i = 0; i != shared_data_offsets.size(); ++i)
        {
            size_t start = shared_data_offsets[static_cast<ssize_t>(i) - 1];
            size_t end = shared_data_offsets[static_cast<ssize_t>(i)];

            /// Merge sorted typed+dynamic paths with sorted shared data paths (two-pointer merge).
            size_t sorted_paths_index = 0;
            for (size_t j = start; j != end; ++j)
            {
                auto shared_data_path = shared_data_paths->getDataAt(j);

                /// Emit typed/dynamic paths that sort before this shared data path.
                while (sorted_paths_index < sorted_paths.size() && sorted_paths[sorted_paths_index].path < shared_data_path)
                {
                    emitValue(sorted_paths[sorted_paths_index], i,
                        typed_infos, dynamic_infos, dynamic_serialization,
                        format_settings, result_data);

                    ++sorted_paths_index;
                }

                /// Emit the shared data value.
                emitSharedDataValue(shared_data_values->getDataAt(j), format_settings, result_data, type_cache);
            }

            /// Emit remaining typed/dynamic paths after all shared data for this row.
            for (; sorted_paths_index < sorted_paths.size(); ++sorted_paths_index)
            {
                emitValue(sorted_paths[sorted_paths_index], i,
                    typed_infos, dynamic_infos, dynamic_serialization,
                    format_settings, result_data);
            }

            offsets.push_back(result_data.size());
        }

        return res;
    }

    static void emitValue(
        const PathEntry & entry,
        size_t row,
        const std::vector<TypedPathInfo> & typed_infos,
        const std::vector<DynamicPathInfo> & dynamic_infos,
        const SerializationPtr & dynamic_serialization,
        const FormatSettings & format_settings,
        ColumnString & result_data)
    {
        if (entry.is_typed)
        {
            const auto & info = typed_infos[entry.index];
            serializeValueIntoResult(*info.serialization, *info.column, row, format_settings, result_data);
        }
        else
        {
            const auto & info = dynamic_infos[entry.index];
            if (info.column->isNullAt(row))
                return;

            serializeValueIntoResult(*dynamic_serialization, *info.column, row, format_settings, result_data);
        }
    }

    static void emitSharedDataValue(
        std::string_view value_data,
        const FormatSettings & format_settings,
        ColumnString & data,
        std::unordered_map<String, CachedTypeInfo> & type_cache)
    {
        ReadBufferFromMemory buf(value_data);
        auto type = decodeDataType(buf);

        if (isNothing(type))
            return;

        auto type_name = type->getName();
        auto [it, inserted] = type_cache.try_emplace(type_name);

        if (inserted)
            it->second = CachedTypeInfo{type, type->getDefaultSerialization(), type->createColumn()};

        auto & entry = it->second;
        entry.serialization->deserializeBinary(*entry.column, buf, format_settings);

        serializeValueIntoResult(*entry.serialization, *entry.column, 0, format_settings, data);
        entry.column->popBack(1);
    }

    static void serializeValueIntoResult(
        const ISerialization & serialization,
        const IColumn & source_column,
        size_t row,
        const FormatSettings & format_settings,
        ColumnString & result_data)
    {
        auto & result_chars = result_data.getChars();
        auto & result_offsets = result_data.getOffsets();

        {
            WriteBufferFromVector<ColumnString::Chars> out(result_chars);
            serialization.serializeText(source_column, row, out, format_settings);
        }

        result_offsets.push_back(result_chars.size());
    }
};

}

REGISTER_FUNCTION(JSONAllValues)
{
    FunctionDocumentation::Description description = R"(
Returns all values from each row in a JSON column as an array of strings.
Values are serialized in their text representation and ordered by their path names.
    )";
    FunctionDocumentation::Syntax syntax = "JSONAllValues(json)";
    FunctionDocumentation::Arguments arguments = {
        {"json", "JSON column.", {"JSON"}}
    };
    FunctionDocumentation::ReturnedValue returned_value = {"Returns an array of all values as strings in the JSON column.", {"Array(String)"}};
    FunctionDocumentation::Examples examples = {
    {
        "Usage example",
        R"(
CREATE TABLE test (json JSON(max_dynamic_paths=1)) ENGINE = Memory;
INSERT INTO test FORMAT JSONEachRow {"json" : {"a" : 42}}, {"json" : {"b" : "Hello"}}, {"json" : {"a" : [1, 2, 3], "c" : "2020-01-01"}}
SELECT json, JSONAllValues(json) FROM test;
        )",
        R"(
┌─json─────────────────────────────────┬─JSONAllValues(json)──────┐
│ {"a":"42"}                           │ ['42']                   │
│ {"b":"Hello"}                        │ ['Hello']                │
│ {"a":["1","2","3"],"c":"2020-01-01"} │ ['[1,2,3]','2020-01-01'] │
└──────────────────────────────────────┴──────────────────────────┘
        )"
    }
    };
    FunctionDocumentation::IntroducedIn introduced_in = {25, 6};
    FunctionDocumentation::Category category = FunctionDocumentation::Category::JSON;
    FunctionDocumentation documentation = {description, syntax, arguments, {}, returned_value, examples, introduced_in, category};
    factory.registerFunction<FunctionJSONAllValues>(documentation);
}

}
