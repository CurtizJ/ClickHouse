#pragma once

#include <Columns/IColumn_fwd.h>
#include <Core/Field.h>
#include <DataTypes/IDataType.h>

#include <string_view>


namespace DB
{

/** The comparison rule of the string family.
  *
  * Two values compare as if the shorter one were right-padded with zero bytes whenever at least one of them
  * is a `FixedString`, and byte for byte when both are `String`: `toFixedString('a', 2) = 'a'` and
  * `toFixedString('a', 2) = 'a\0'`, but `'a' != 'a\0'`. Equality is then equality without trailing zero
  * bytes, and the order is the order of the values without trailing zero bytes.
  *
  * Every function that compares or searches values follows this rule, and so does every index and optimization
  * that stands in for such a function. A value alone cannot tell whether the rule applies, because `Field` has
  * no `FixedString` type and a cast between `String` and `FixedString` changes the bytes, so the rule is decided
  * by the declared types of the two sides.
  */

/// Whether comparing values of these types ignores trailing zero bytes somewhere: a `String` or `FixedString`
/// paired with a `FixedString` of another type. Looks through `Nullable` and `LowCardinality` and pairs the
/// elements of `Array`, `Map` and `Tuple`.
bool comparesZeroPadded(const DataTypePtr & left, const DataTypePtr & right);

inline std::string_view withoutTrailingZeros(std::string_view value)
{
    return value.substr(0, value.find_last_not_of('\0') + 1);
}

/// Removes trailing zero bytes from the values of a `String` or `FixedString` column, also under `Const`, `Nullable` and
/// `LowCardinality`; a `FixedString` becomes a `String`.
ColumnPtr removeTrailingZeros(const ColumnPtr & column);

/// Removes trailing zero bytes from the string values that `comparesZeroPadded` applies to. `column` is of
/// `left`, `right` or their common type; a `FixedString` becomes a `String`. Exact comparison and hashing of the
/// results of both sides then follow the rule.
ColumnPtr removePaddingForComparison(const ColumnPtr & column, const DataTypePtr & left, const DataTypePtr & right);
Field removePaddingForComparison(const Field & value, const DataTypePtr & left, const DataTypePtr & right);

/// The stored values of a `String` or `FixedString` column that equal a string constant under the rule.
struct StoredStringMatch
{
    enum class Kind : uint8_t
    {
        /// No stored value equals the constant: it is longer than the `FixedString` without its trailing zero bytes.
        None,
        /// Exactly `value`.
        Exact,
        /// `value` followed by any number of zero bytes: a `FixedString` constant against a `String` column.
        WithTrailingZeros,
    };

    Kind kind;
    String value;

    /// Whether the constant equals the default value of the column, `''` or all zero bytes, which an absent map key or
    /// JSON path reads.
    bool equalsDefault(const DataTypePtr & column_type) const;
};

/// `constant_type` is the declared type of the constant; `nullptr`, `Variant` and `Dynamic` count as `FixedString`.
StoredStringMatch matchStoredString(std::string_view constant, const DataTypePtr & constant_type, const DataTypePtr & column_type);

}
