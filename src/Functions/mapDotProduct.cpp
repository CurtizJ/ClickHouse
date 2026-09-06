#include <Columns/ColumnConst.h>
#include <Columns/ColumnFixedString.h>
#include <Columns/ColumnLowCardinality.h>
#include <Columns/ColumnMap.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnTuple.h>
#include <Columns/ColumnVector.h>
#include <Columns/ColumnsNumber.h>
#include <Common/HashTable/ClearableHashMap.h>
#include <Common/PODArray.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <DataTypes/DataTypeMap.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/NumberTraits.h>
#include <DataTypes/getLeastSupertype.h>
#include <Functions/FunctionFactory.h>
#include <Functions/FunctionHelpers.h>
#include <Functions/IFunction.h>
#include <Functions/castTypeToEither.h>
#include <Interpreters/castColumn.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int ILLEGAL_COLUMN;
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int LOGICAL_ERROR;
}

namespace
{

/** mapDotProduct(map1, map2) - the sum of products of the values of the keys that exist in both maps.
  * It is used to score sparse vectors (e.g. learned sparse embeddings) stored as `Map(token, weight)`.
  *
  * The keys of both maps must be either strings or integers. Integer keys of different widths and
  * `String`/`FixedString` keys are converted to a common type. The values are converted to the result
  * type, which is inferred from the value types in the same way as in `arrayDotProduct`.
  *
  * One of the maps is put into a hash table and the other one probes it. When one argument is constant
  * (the common case: a query vector against a column of document vectors), the hash table is built once
  * for all rows, and if the keys of the other argument are `LowCardinality`, the lookup is done once per
  * dictionary entry instead of once per key. If a map contains a key several times, the values of the
  * repeated key are summed up, so the result does not depend on which of the maps is put into the hash table.
  */

/// Reads the keys of a String or FixedString column.
template <typename ColumnType>
struct StringKeys
{
    using Key = std::string_view;

    explicit StringKeys(const IColumn & column_) : column(assert_cast<const ColumnType &>(column_)) {}
    Key operator[](size_t i) const { return column.ColumnType::getDataAt(i); }

    const ColumnType & column;
};

/// Reads the keys of an integer column as their 64-bit patterns. Both arguments are converted
/// to the same integer type beforehand, so equal keys have equal patterns.
template <typename T>
struct IntegerKeys
{
    using Key = UInt64;

    explicit IntegerKeys(const IColumn & column_) : data(assert_cast<const ColumnVector<T> &>(column_).getData()) {}
    Key operator[](size_t i) const { return static_cast<UInt64>(data[i]); }

    const typename ColumnVector<T>::Container & data;
};

template <typename Key, typename Mapped>
struct KeysHashTable;

template <typename Mapped>
struct KeysHashTable<std::string_view, Mapped>
{
    using Hash = DefaultHash<std::string_view>;
    using Type = ClearableHashMap<std::string_view, Mapped, Hash, ClearableHashMapCellWithSavedHash<std::string_view, Mapped, Hash>>;
};

template <typename Mapped>
struct KeysHashTable<UInt64, Mapped>
{
    using Type = ClearableHashMap<UInt64, Mapped>;
};

/// One argument of the function unwrapped from the Map column.
struct MapArgument
{
    /// The keys converted to the common key type. May be LowCardinality if the dictionary lookup path is used.
    ColumnPtr keys;
    /// The values converted to the result type.
    ColumnPtr values;
    const ColumnArray::Offsets * offsets = nullptr;
    bool is_const = false;

    size_t begin(size_t row) const { return is_const ? 0 : (*offsets)[row - 1]; }
    size_t end(size_t row) const { return is_const ? (*offsets)[0] : (*offsets)[row]; }
};

template <typename ResultType, typename Keys>
struct Kernel
{
    using Key = typename Keys::Key;
    using Table = typename KeysHashTable<Key, ResultType>::Type;

    static NO_SANITIZE_UNDEFINED void fill(Table & table, const Keys & keys, const ResultType * values, size_t begin, size_t end)
    {
        for (size_t i = begin; i < end; ++i)
            table[keys[i]] += values[i];
    }

    static NO_SANITIZE_UNDEFINED ResultType apply(const Table & table, const Keys & keys, const ResultType * values, size_t begin, size_t end)
    {
        ResultType sum{};
        for (size_t i = begin; i < end; ++i)
        {
            if (const auto * cell = table.find(keys[i]))
                sum += cell->getMapped() * values[i];
        }
        return sum;
    }

    /// The same as apply, but the keys are LowCardinality and the lookup results for the dictionary are precomputed.
    template <typename IndexType>
    static NO_SANITIZE_UNDEFINED ResultType applyLowCardinality(
        const PaddedPODArray<IndexType> & indexes,
        const PaddedPODArray<UInt8> & dictionary_found,
        const PaddedPODArray<ResultType> & dictionary_weights,
        const ResultType * values,
        size_t begin,
        size_t end)
    {
        ResultType sum{};
        for (size_t i = begin; i < end; ++i)
        {
            size_t index = indexes[i];
            if (dictionary_found[index])
                sum += dictionary_weights[index] * values[i];
        }
        return sum;
    }
};

class FunctionMapDotProduct : public IFunction
{
public:
    static constexpr auto name = "mapDotProduct";
    static FunctionPtr create(ContextPtr) { return std::make_shared<FunctionMapDotProduct>(); }

    String getName() const override { return name; }
    size_t getNumberOfArguments() const override { return 2; }
    bool useDefaultImplementationForConstants() const override { return true; }
    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return true; }

    /// The default implementation converts LowCardinality columns to full ones recursively, including the keys of a Map.
    /// We want to see the LowCardinality keys as is to look up the dictionary instead of every key.
    bool useDefaultImplementationForLowCardinalityColumns() const override { return false; }

    DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
    {
        const auto * left = checkAndGetDataType<DataTypeMap>(arguments[0].get());
        const auto * right = checkAndGetDataType<DataTypeMap>(arguments[1].get());

        if (!left || !right)
        {
            throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                "Arguments of function {} must be maps, got {} and {}",
                getName(), arguments[0]->getName(), arguments[1]->getName());
        }

        /// Validates the key types.
        getCommonKeyType(*left, *right);
        return getResultType(*left, *right);
    }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr & result_type, size_t input_rows_count) const override
    {
        using ResultTypes = TypeList<
            DataTypeUInt16, DataTypeUInt32, DataTypeUInt64,
            DataTypeInt16, DataTypeInt32, DataTypeInt64,
            DataTypeFloat32, DataTypeFloat64>;

        const auto & left_type = assert_cast<const DataTypeMap &>(*arguments[0].type);
        const auto & right_type = assert_cast<const DataTypeMap &>(*arguments[1].type);
        auto key_type = getCommonKeyType(left_type, right_type);

        bool left_is_const = isColumnConst(*arguments[0].column);
        bool right_is_const = isColumnConst(*arguments[1].column);

        /// The dictionary of LowCardinality keys is looked up directly only when the other argument is constant,
        /// because only then the hash table is the same for all rows.
        auto left = prepareArgument(arguments[0], left_is_const, key_type, result_type, right_is_const && !left_is_const);
        auto right = prepareArgument(arguments[1], right_is_const, key_type, result_type, left_is_const && !right_is_const);
        ColumnPtr result;

        bool valid = castTypeToEither(ResultTypes{}, result_type.get(), [&](const auto & type)
        {
            using ResultType = typename std::decay_t<decltype(type)>::FieldType;
            result = executeWithResultType<ResultType>(key_type, left, right, input_rows_count);
            return true;
        });

        if (!valid)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected result type {} of function {}", result_type->getName(), getName());

        return result;
    }

private:
    DataTypePtr getCommonKeyType(const DataTypeMap & left, const DataTypeMap & right) const
    {
        auto left_key_type = removeLowCardinality(left.getKeyType());
        auto right_key_type = removeLowCardinality(right.getKeyType());

        bool both_strings = isStringOrFixedString(left_key_type) && isStringOrFixedString(right_key_type);
        bool both_integers = isNativeInteger(left_key_type) && isNativeInteger(right_key_type);

        if (!both_strings && !both_integers)
        {
            throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                "Keys of the maps in function {} must be both strings or both integers, got {} and {}",
                getName(), left.getKeyType()->getName(), right.getKeyType()->getName());
        }

        return getLeastSupertype(DataTypes{left_key_type, right_key_type});
    }

    DataTypePtr getResultType(const DataTypeMap & left, const DataTypeMap & right) const
    {
        using ValueTypes = TypeList<
            DataTypeBFloat16, DataTypeFloat32, DataTypeFloat64,
            DataTypeUInt8, DataTypeUInt16, DataTypeUInt32, DataTypeUInt64,
            DataTypeInt8, DataTypeInt16, DataTypeInt32, DataTypeInt64>;

        auto left_value_type = removeLowCardinality(left.getValueType());
        auto right_value_type = removeLowCardinality(right.getValueType());
        DataTypePtr result_type;

        bool valid = castTypeToEither(ValueTypes{}, left_value_type.get(), [&](const auto & left_type)
        {
            return castTypeToEither(ValueTypes{}, right_value_type.get(), [&](const auto & right_type)
            {
                using LeftType = typename std::decay_t<decltype(left_type)>::FieldType;
                using RightType = typename std::decay_t<decltype(right_type)>::FieldType;

                static constexpr bool both_float32 = std::is_same_v<LeftType, Float32> && std::is_same_v<RightType, Float32>;
                static constexpr bool both_bfloat16 = std::is_same_v<LeftType, BFloat16> && std::is_same_v<RightType, BFloat16>;

                /// The same rules as in arrayDotProduct.
                /// Same-type Float32 and BFloat16 accumulate to Float32,
                /// everything else uses the promoted arithmetic type.
                if constexpr (both_float32 || both_bfloat16)
                    result_type = std::make_shared<DataTypeFloat32>();
                else
                    result_type = std::make_shared<DataTypeNumber<typename NumberTraits::ResultOfAdditionMultiplication<LeftType, RightType>::Type>>();

                return true;
            });
        });

        if (!valid)
        {
            throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                "Values of the maps in function {} must be integers or floats, got {} and {}",
                getName(), left.getValueType()->getName(), right.getValueType()->getName());
        }

        return result_type;
    }

    MapArgument prepareArgument(
        const ColumnWithTypeAndName & argument,
        bool is_const,
        const DataTypePtr & key_type,
        const DataTypePtr & result_type,
        bool allow_low_cardinality_keys) const
    {
        const auto * map_column = is_const
            ? checkAndGetColumnConstData<ColumnMap>(argument.column.get())
            : checkAndGetColumn<ColumnMap>(argument.column.get());

        if (!map_column)
            throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Illegal column {} of argument of function {}", argument.column->getName(), getName());

        const auto & map_type = assert_cast<const DataTypeMap &>(*argument.type);

        MapArgument result;
        result.is_const = is_const;
        result.offsets = &map_column->getNestedColumn().getOffsets();
        result.keys = map_column->getNestedData().getColumnPtr(0);
        result.values = map_column->getNestedData().getColumnPtr(1);

        const auto & map_key_type = map_type.getKeyType();
        if (!removeLowCardinality(map_key_type)->equals(*key_type))
        {
            result.keys = castColumn({result.keys, map_key_type, ""}, key_type);
        }
        else if (const auto * low_cardinality = typeid_cast<const ColumnLowCardinality *>(result.keys.get()))
        {
            /// The dictionary is looked up in the hash table once per block, which pays off
            /// only if it is smaller than the number of keys in the block.
            if (!allow_low_cardinality_keys || low_cardinality->getDictionary().size() > low_cardinality->size())
                result.keys = low_cardinality->convertToFullColumn();
        }

        const auto & map_value_type = map_type.getValueType();
        if (!map_value_type->equals(*result_type))
            result.values = castColumn({result.values, map_value_type, ""}, result_type);

        return result;
    }

    template <typename ResultType>
    ColumnPtr executeWithResultType(const DataTypePtr & key_type, const MapArgument & left, const MapArgument & right, size_t input_rows_count) const
    {
        if (isString(key_type))
            return executeWithKeys<ResultType, StringKeys<ColumnString>>(left, right, input_rows_count);

        if (isFixedString(key_type))
            return executeWithKeys<ResultType, StringKeys<ColumnFixedString>>(left, right, input_rows_count);

        using IntegerKeyTypes = TypeList<
            DataTypeUInt8, DataTypeUInt16, DataTypeUInt32, DataTypeUInt64,
            DataTypeInt8, DataTypeInt16, DataTypeInt32, DataTypeInt64>;

        ColumnPtr result;
        bool valid = castTypeToEither(IntegerKeyTypes{}, key_type.get(), [&](const auto & type)
        {
            using KeyType = typename std::decay_t<decltype(type)>::FieldType;
            result = executeWithKeys<ResultType, IntegerKeys<KeyType>>(left, right, input_rows_count);
            return true;
        });

        if (!valid)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected key type {} of function {}", key_type->getName(), getName());

        return result;
    }

    template <typename ResultType, typename Keys>
    static ColumnPtr executeWithKeys(const MapArgument & left, const MapArgument & right, size_t input_rows_count)
    {
        /// Two constants are handled by the default implementation for constants.
        if (left.is_const == right.is_const)
            return executeWithKeysVector<ResultType, Keys>(left, right, input_rows_count);

        if (left.is_const)
            return executeWithKeysConst<ResultType, Keys>(left, right, input_rows_count);

        return executeWithKeysConst<ResultType, Keys>(right, left, input_rows_count);
    }

    template <typename ResultType>
    static const ResultType * getValues(const MapArgument & argument)
    {
        return assert_cast<const ColumnVector<ResultType> &>(*argument.values).getData().data();
    }

    /// Both arguments are columns: the smaller map of each row is put into the hash table and probed with the other one.
    template <typename ResultType, typename Keys>
    static ColumnPtr executeWithKeysVector(const MapArgument & left, const MapArgument & right, size_t input_rows_count)
    {
        using KernelType = Kernel<ResultType, Keys>;

        const auto * left_values = getValues<ResultType>(left);
        const auto * right_values = getValues<ResultType>(right);

        Keys left_keys(*left.keys);
        Keys right_keys(*right.keys);

        auto result_column = ColumnVector<ResultType>::create(input_rows_count);
        auto & result = result_column->getData();

        typename KernelType::Table table;
        for (size_t row = 0; row < input_rows_count; ++row)
        {
            table.clear();
            size_t left_begin = left.begin(row);
            size_t left_end = left.end(row);
            size_t right_begin = right.begin(row);
            size_t right_end = right.end(row);

            if (left_end - left_begin <= right_end - right_begin)
            {
                KernelType::fill(table, left_keys, left_values, left_begin, left_end);
                result[row] = KernelType::apply(table, right_keys, right_values, right_begin, right_end);
            }
            else
            {
                KernelType::fill(table, right_keys, right_values, right_begin, right_end);
                result[row] = KernelType::apply(table, left_keys, left_values, left_begin, left_end);
            }
        }

        return result_column;
    }

    /// The constant argument is put into the hash table once and probed with the column argument for every row.
    template <typename ResultType, typename Keys>
    static ColumnPtr executeWithKeysConst(const MapArgument & constant, const MapArgument & column, size_t input_rows_count)
    {
        using KernelType = Kernel<ResultType, Keys>;

        typename KernelType::Table table;
        Keys constant_keys(*constant.keys);
        KernelType::fill(table, constant_keys, getValues<ResultType>(constant), 0, constant.end(0));

        if (typeid_cast<const ColumnLowCardinality *>(column.keys.get()))
        {
            return executeWithKeysConstLowCardinality<ResultType, Keys>(table, column, input_rows_count);
        }

        const auto * column_values = getValues<ResultType>(column);
        Keys column_keys(*column.keys);

        auto result_column = ColumnVector<ResultType>::create(input_rows_count);
        auto & result = result_column->getData();

        for (size_t row = 0; row < input_rows_count; ++row)
            result[row] = KernelType::apply(table, column_keys, column_values, column.begin(row), column.end(row));

        return result_column;
    }

    /// The same as executeWithKeysConst, but the keys of the column are LowCardinality: every entry
    /// of the dictionary is looked up in the hash table once, then the rows only index the lookup results.
    template <typename ResultType, typename Keys, typename KernelType = Kernel<ResultType, Keys>>
    static ColumnPtr executeWithKeysConstLowCardinality(const KernelType::Table & table, const MapArgument & column, size_t input_rows_count)
    {
        const auto & low_cardinality = typeid_cast<const ColumnLowCardinality &>(*column.keys);
        const auto & dictionary = *low_cardinality.getDictionary().getNestedColumn();

        Keys dictionary_keys(dictionary);
        size_t dictionary_size = dictionary.size();

        PaddedPODArray<UInt8> dictionary_found(dictionary_size);
        PaddedPODArray<ResultType> dictionary_weights(dictionary_size);

        for (size_t i = 0; i < dictionary_size; ++i)
        {
            const auto * cell = table.find(dictionary_keys[i]);
            dictionary_found[i] = cell != nullptr;
            dictionary_weights[i] = cell ? cell->getMapped() : ResultType{};
        }

        const auto * column_values = getValues<ResultType>(column);
        auto result_column = ColumnVector<ResultType>::create(input_rows_count);
        auto & result = result_column->getData();

        /// Dispatch on the type of indexes once, outside of the loop over rows.
        const auto & indexes = low_cardinality.getIndexes();
        bool valid = castTypeToEither<ColumnUInt8, ColumnUInt16, ColumnUInt32, ColumnUInt64>(&indexes, [&](const auto & indexes_column)
        {
            for (size_t row = 0; row < input_rows_count; ++row)
                result[row] = KernelType::applyLowCardinality(indexes_column.getData(), dictionary_found, dictionary_weights, column_values, column.begin(row), column.end(row));
            return true;
        });

        if (!valid)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected type of indexes {} in LowCardinality column", indexes.getName());

        return result_column;
    }
};

}

REGISTER_FUNCTION(MapDotProduct)
{
    FunctionDocumentation::Description description = R"(
Computes the dot product of two maps: the sum of products of the values for the keys that exist in both maps.
Keys that exist in only one of the maps do not contribute to the result.

The function is intended for scoring sparse vectors, for example learned sparse embeddings, stored as maps from a token to its weight.
The typical usage is a constant query vector against a column of document vectors: in this case the query map is hashed once for all rows.
If a map contains the same key several times, the values of the repeated key are summed up.
)";
    FunctionDocumentation::Syntax syntax = "mapDotProduct(map1, map2)";
    FunctionDocumentation::Arguments arguments = {
        {"map1", "First map. The keys must be strings (`String`, `FixedString`, or `LowCardinality` of them) or integers, the values must be numbers.", {"Map(String | FixedString | (U)Int*, (U)Int* | Float* | BFloat16)"}},
        {"map2", "Second map. The keys must be of the same kind as in `map1` (strings or integers), the values must be numbers.", {"Map(String | FixedString | (U)Int*, (U)Int* | Float* | BFloat16)"}},
    };
    FunctionDocumentation::ReturnedValue returned_value = {R"(
The sum of products of the values for the keys that exist in both maps, or `0` if there are no common keys.

:::note
The return type is inferred from the value types in the same way as in [`arrayDotProduct`](/sql-reference/functions/array-functions#arrayDotProduct):
two `Float32` maps (or two `BFloat16` maps) return `Float32`, other combinations with floats return `Float64`, and integer values return a wider integer type.
:::
)", {"(U)Int*", "Float*"}};
    FunctionDocumentation::Examples examples = {
        {"Basic usage", "SELECT mapDotProduct(map('a', 1, 'b', 2, 'c', 3), map('b', 10, 'c', 20, 'd', 30)) AS res, toTypeName(res)", "80\tUInt16"},
        {"Scoring sparse vectors", R"(
CREATE TABLE docs (id UInt32, embedding Map(String, Float32)) ENGINE = Memory;
INSERT INTO docs VALUES (1, map('apple', 0.75, 'fruit', 0.5)), (2, map('banana', 0.875, 'fruit', 0.625)), (3, map('car', 1.0));

SELECT id, mapDotProduct(embedding, map('fruit', 1.0, 'apple', 0.5)) AS score
FROM docs
ORDER BY score DESC
LIMIT 2
)", "1\t0.875\n2\t0.625"},
    };
    FunctionDocumentation::IntroducedIn introduced_in = {26, 9};
    FunctionDocumentation::Category category = FunctionDocumentation::Category::Map;
    FunctionDocumentation documentation = {description, syntax, arguments, {}, returned_value, examples, introduced_in, category};

    factory.registerFunction<FunctionMapDotProduct>(documentation);
}

}
