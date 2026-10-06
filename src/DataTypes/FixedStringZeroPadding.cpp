#include <DataTypes/FixedStringZeroPadding.h>

#include <Columns/ColumnArray.h>
#include <Columns/ColumnConst.h>
#include <Columns/ColumnFixedString.h>
#include <Columns/ColumnLowCardinality.h>
#include <Columns/ColumnMap.h>
#include <Columns/ColumnNullable.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnTuple.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeFixedString.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <DataTypes/DataTypeMap.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeTuple.h>
#include <Common/assert_cast.h>
#include <Common/typeid_cast.h>


namespace DB
{

bool comparesZeroPadded(const DataTypePtr & left, const DataTypePtr & right)
{
    if (!left || !right)
        return false;

    auto left_type = removeLowCardinalityAndNullable(left);
    auto right_type = removeLowCardinalityAndNullable(right);

    if (const auto * left_array = typeid_cast<const DataTypeArray *>(left_type.get()))
    {
        const auto * right_array = typeid_cast<const DataTypeArray *>(right_type.get());
        return right_array && comparesZeroPadded(left_array->getNestedType(), right_array->getNestedType());
    }

    if (const auto * left_map = typeid_cast<const DataTypeMap *>(left_type.get()))
    {
        const auto * right_map = typeid_cast<const DataTypeMap *>(right_type.get());
        return right_map
            && (comparesZeroPadded(left_map->getKeyType(), right_map->getKeyType())
                || comparesZeroPadded(left_map->getValueType(), right_map->getValueType()));
    }

    if (const auto * left_tuple = typeid_cast<const DataTypeTuple *>(left_type.get()))
    {
        const auto * right_tuple = typeid_cast<const DataTypeTuple *>(right_type.get());
        if (!right_tuple || left_tuple->getElements().size() != right_tuple->getElements().size())
            return false;

        for (size_t i = 0; i < left_tuple->getElements().size(); ++i)
            if (comparesZeroPadded(left_tuple->getElement(i), right_tuple->getElement(i)))
                return true;
        return false;
    }

    /// Two `FixedString`s of the same width have the same padding, so the rule and an exact comparison agree.
    return isStringOrFixedString(left_type) && isStringOrFixedString(right_type)
        && (isFixedString(left_type) || isFixedString(right_type)) && !left_type->equals(*right_type);
}

static ColumnPtr stringWithoutTrailingZeros(const ColumnString & column)
{
    const size_t size = column.size();
    bool has_trailing_zeros = false;
    for (size_t i = 0; i < size && !has_trailing_zeros; ++i)
        has_trailing_zeros = column.getDataAt(i).ends_with('\0');

    if (!has_trailing_zeros)
        return column.getPtr();

    auto result = ColumnString::create();
    result->reserve(size);
    for (size_t i = 0; i < size; ++i)
    {
        const auto value = withoutTrailingZeros(column.getDataAt(i));
        result->insertData(value.data(), value.size());
    }
    return result;
}

static ColumnPtr fixedStringWithoutTrailingZeros(const ColumnFixedString & column)
{
    const size_t size = column.size();
    auto result = ColumnString::create();
    result->reserve(size);
    for (size_t i = 0; i < size; ++i)
    {
        const auto value = withoutTrailingZeros(column.getDataAt(i));
        result->insertData(value.data(), value.size());
    }
    return result;
}

ColumnPtr removeTrailingZeros(const ColumnPtr & column)
{
    if (const auto * column_const = typeid_cast<const ColumnConst *>(column.get()))
        return ColumnConst::create(removeTrailingZeros(column_const->getDataColumnPtr()), column_const->size());

    if (const auto * column_low_cardinality = typeid_cast<const ColumnLowCardinality *>(column.get()))
        return removeTrailingZeros(column_low_cardinality->convertToFullColumn());

    if (const auto * column_nullable = typeid_cast<const ColumnNullable *>(column.get()))
        return ColumnNullable::create(removeTrailingZeros(column_nullable->getNestedColumnPtr()), column_nullable->getNullMapColumnPtr());

    if (const auto * column_string = typeid_cast<const ColumnString *>(column.get()))
        return stringWithoutTrailingZeros(*column_string);

    return fixedStringWithoutTrailingZeros(assert_cast<const ColumnFixedString &>(*column));
}

ColumnPtr removePaddingForComparison(const ColumnPtr & column, const DataTypePtr & left, const DataTypePtr & right)
{
    if (!comparesZeroPadded(left, right))
        return column;

    if (const auto * column_const = typeid_cast<const ColumnConst *>(column.get()))
        return ColumnConst::create(removePaddingForComparison(column_const->getDataColumnPtr(), left, right), column_const->size());

    if (const auto * column_low_cardinality = typeid_cast<const ColumnLowCardinality *>(column.get()))
        return removePaddingForComparison(column_low_cardinality->convertToFullColumn(), left, right);

    if (const auto * column_nullable = typeid_cast<const ColumnNullable *>(column.get()))
        return ColumnNullable::create(
            removePaddingForComparison(column_nullable->getNestedColumnPtr(), left, right), column_nullable->getNullMapColumnPtr());

    auto left_type = removeLowCardinalityAndNullable(left);
    auto right_type = removeLowCardinalityAndNullable(right);

    if (const auto * column_array = typeid_cast<const ColumnArray *>(column.get()))
    {
        const auto & left_array = assert_cast<const DataTypeArray &>(*left_type);
        const auto & right_array = assert_cast<const DataTypeArray &>(*right_type);
        return ColumnArray::create(
            removePaddingForComparison(column_array->getDataPtr(), left_array.getNestedType(), right_array.getNestedType()),
            column_array->getOffsetsPtr());
    }

    if (const auto * column_map = typeid_cast<const ColumnMap *>(column.get()))
    {
        const auto & left_map = assert_cast<const DataTypeMap &>(*left_type);
        const auto & right_map = assert_cast<const DataTypeMap &>(*right_type);
        return ColumnMap::create(
            removePaddingForComparison(column_map->getNestedColumnPtr(), left_map.getNestedType(), right_map.getNestedType()));
    }

    if (const auto * column_tuple = typeid_cast<const ColumnTuple *>(column.get()))
    {
        const auto & left_tuple = assert_cast<const DataTypeTuple &>(*left_type);
        const auto & right_tuple = assert_cast<const DataTypeTuple &>(*right_type);
        Columns elements(column_tuple->tupleSize());
        for (size_t i = 0; i < elements.size(); ++i)
            elements[i] = removePaddingForComparison(column_tuple->getColumnPtr(i), left_tuple.getElement(i), right_tuple.getElement(i));
        return ColumnTuple::create(std::move(elements));
    }

    if (const auto * column_string = typeid_cast<const ColumnString *>(column.get()))
        return stringWithoutTrailingZeros(*column_string);

    if (const auto * column_fixed_string = typeid_cast<const ColumnFixedString *>(column.get()))
        return fixedStringWithoutTrailingZeros(*column_fixed_string);

    return column;
}

Field removePaddingForComparison(const Field & value, const DataTypePtr & left, const DataTypePtr & right)
{
    if (!comparesZeroPadded(left, right))
        return value;

    auto left_type = removeLowCardinalityAndNullable(left);
    auto right_type = removeLowCardinalityAndNullable(right);

    switch (value.getType())
    {
        case Field::Types::String:
            return String(withoutTrailingZeros(value.safeGet<String>()));
        case Field::Types::Array:
        {
            const auto & left_array = assert_cast<const DataTypeArray &>(*left_type);
            const auto & right_array = assert_cast<const DataTypeArray &>(*right_type);
            Array result;
            result.reserve(value.safeGet<Array>().size());
            for (const auto & element : value.safeGet<Array>())
                result.push_back(removePaddingForComparison(element, left_array.getNestedType(), right_array.getNestedType()));
            return result;
        }
        case Field::Types::Map:
        {
            const auto & left_map = assert_cast<const DataTypeMap &>(*left_type);
            const auto & right_map = assert_cast<const DataTypeMap &>(*right_type);
            const DataTypes left_pair{left_map.getKeyType(), left_map.getValueType()};
            const DataTypes right_pair{right_map.getKeyType(), right_map.getValueType()};
            Map result;
            result.reserve(value.safeGet<Map>().size());
            for (const auto & pair : value.safeGet<Map>())
            {
                const auto & key_value = pair.safeGet<Tuple>();
                result.push_back(Tuple{
                    removePaddingForComparison(key_value[0], left_pair[0], right_pair[0]),
                    removePaddingForComparison(key_value[1], left_pair[1], right_pair[1])});
            }
            return result;
        }
        case Field::Types::Tuple:
        {
            const auto & left_tuple = assert_cast<const DataTypeTuple &>(*left_type);
            const auto & right_tuple = assert_cast<const DataTypeTuple &>(*right_type);
            const auto & elements = value.safeGet<Tuple>();
            Tuple result;
            result.reserve(elements.size());
            for (size_t i = 0; i < elements.size(); ++i)
                result.push_back(removePaddingForComparison(elements[i], left_tuple.getElement(i), right_tuple.getElement(i)));
            return result;
        }
        default:
            return value;
    }
}

bool StoredStringMatch::equalsDefault(const DataTypePtr & column_type) const
{
    if (kind == Kind::WithTrailingZeros)
        return value.empty();

    return kind == Kind::Exact
        && (value.empty() || (isFixedString(removeLowCardinalityAndNullable(column_type)) && withoutTrailingZeros(value).empty()));
}

StoredStringMatch matchStoredString(std::string_view constant, const DataTypePtr & constant_type, const DataTypePtr & column_type)
{
    const std::string_view stripped = withoutTrailingZeros(constant);

    if (const auto * fixed_string_type = typeid_cast<const DataTypeFixedString *>(removeLowCardinalityAndNullable(column_type).get()))
    {
        if (stripped.size() > fixed_string_type->getN())
            return {StoredStringMatch::Kind::None, {}};

        String value(stripped);
        value.resize(fixed_string_type->getN(), '\0');
        return {StoredStringMatch::Kind::Exact, std::move(value)};
    }

    const auto type = constant_type ? removeLowCardinalityAndNullable(constant_type) : nullptr;
    if (!type || isFixedString(type) || isVariant(type) || isDynamic(type))
        return {StoredStringMatch::Kind::WithTrailingZeros, String(stripped)};

    return {StoredStringMatch::Kind::Exact, String(constant)};
}

}
