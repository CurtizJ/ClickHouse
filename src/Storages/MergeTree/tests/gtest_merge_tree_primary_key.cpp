#include <gtest/gtest.h>

#include <fmt/format.h>

#include <Columns/ColumnNullable.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnsNumber.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesNumber.h>
#include <Storages/MergeTree/MergeTreePrimaryKey.h>

using namespace DB;

namespace
{

void checkFullColumn(const PrimaryKey & pk, size_t column_idx, const DataTypePtr & type, const IColumn & expected)
{
    auto full_column = pk.getFullColumn(column_idx, type);
    ASSERT_EQ(full_column->size(), expected.size());

    for (size_t i = 0; i < expected.size(); ++i)
        EXPECT_EQ((*full_column)[i], expected[i]) << "at row " << i;
}

}

TEST(MergeTreePrimaryKey, Empty)
{
    PrimaryKey pk;
    EXPECT_TRUE(pk.empty());
    EXPECT_EQ(pk.getNumColumns(), 0u);
    EXPECT_EQ(pk.getNumRows(), 0u);

    PrimaryKey pk_from_empty(Columns{}, /*try_compress=*/ true);
    EXPECT_TRUE(pk_from_empty.empty());
}

TEST(MergeTreePrimaryKey, CompressedUInt64)
{
    static constexpr size_t num_rows = 10000;

    auto column = ColumnUInt64::create();
    for (size_t i = 0; i < num_rows; ++i)
        column->insertValue(i * 8192);

    ColumnPtr raw = std::move(column);
    PrimaryKey pk(Columns{raw}, /*try_compress=*/ true);

    ASSERT_EQ(pk.getNumColumns(), 1u);
    ASSERT_EQ(pk.getNumRows(), num_rows);
    EXPECT_TRUE(pk.isColumnCompressed(0));
    EXPECT_LT(pk.allocatedBytes(), raw->allocatedBytes());

    for (size_t i = 0; i < num_rows; ++i)
        EXPECT_EQ(pk.get(0, i), Field(i * 8192)) << "at row " << i;

    EXPECT_TRUE(pk.equalAt(0, 3, 3));
    EXPECT_FALSE(pk.equalAt(0, 3, 4));
    EXPECT_FALSE(pk.isNullAt(0, 0));

    checkFullColumn(pk, 0, std::make_shared<DataTypeUInt64>(), *raw);
}

TEST(MergeTreePrimaryKey, CompressedNarrowUInts)
{
    static constexpr size_t num_rows = 10000;

    auto column_uint16 = ColumnUInt16::create();
    auto column_uint32 = ColumnUInt32::create();

    for (size_t i = 0; i < num_rows; ++i)
    {
        column_uint16->insertValue(static_cast<UInt16>(i / 1000));
        column_uint32->insertValue(static_cast<UInt32>(i / 10));
    }

    ColumnPtr raw_uint16 = std::move(column_uint16);
    ColumnPtr raw_uint32 = std::move(column_uint32);

    PrimaryKey pk(Columns{raw_uint16, raw_uint32}, /*try_compress=*/ true);

    ASSERT_EQ(pk.getNumColumns(), 2u);
    EXPECT_TRUE(pk.isColumnCompressed(0));
    EXPECT_TRUE(pk.isColumnCompressed(1));

    for (size_t i = 0; i < num_rows; ++i)
    {
        EXPECT_EQ(pk.get(0, i), Field(i / 1000)) << "at row " << i;
        EXPECT_EQ(pk.get(1, i), Field(i / 10)) << "at row " << i;
    }

    checkFullColumn(pk, 0, std::make_shared<DataTypeUInt16>(), *raw_uint16);
    checkFullColumn(pk, 1, std::make_shared<DataTypeUInt32>(), *raw_uint32);
}

TEST(MergeTreePrimaryKey, CompressedString)
{
    auto column = ColumnString::create();
    std::vector<String> values;

    for (size_t i = 0; i < 10000; ++i)
    {
        values.push_back(i % 100 == 0 ? "" : fmt::format("str_{:07}", i));
        column->insertData(values.back().data(), values.back().size());
    }

    ColumnPtr raw = std::move(column);
    PrimaryKey pk(Columns{raw}, /*try_compress=*/ true);

    ASSERT_EQ(pk.getNumColumns(), 1u);
    EXPECT_TRUE(pk.isColumnCompressed(0));
    EXPECT_LT(pk.allocatedBytes(), raw->allocatedBytes());

    for (size_t i = 0; i < values.size(); ++i)
        EXPECT_EQ(pk.get(0, i), Field(values[i])) << "at row " << i;

    EXPECT_TRUE(pk.equalAt(0, 0, 100));
    EXPECT_FALSE(pk.equalAt(0, 1, 2));

    checkFullColumn(pk, 0, std::make_shared<DataTypeString>(), *raw);
}

TEST(MergeTreePrimaryKey, IncompressibleStaysRaw)
{
    static constexpr size_t num_rows = 10000;

    /// Values that span the whole range of UInt64 in each block cannot be bit-packed
    /// with less than 64 bits per value, so the raw column must be kept.
    auto column = ColumnUInt64::create();
    UInt64 state = 42;
    for (size_t i = 0; i < num_rows; ++i)
    {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        column->insertValue(state);
    }

    /// Deallocate the extra memory reserved for column growth,
    /// otherwise the bit-packed form wins the comparison of allocated bytes.
    column->shrinkToFit();
    ColumnPtr raw = std::move(column);
    PrimaryKey pk(Columns{raw}, /*try_compress=*/ true);

    EXPECT_FALSE(pk.isColumnCompressed(0));
    EXPECT_EQ(pk.getRawColumn(0).get(), raw.get());

    for (size_t i : {size_t(0), size_t(123), num_rows - 1})
        EXPECT_EQ(pk.get(0, i), (*raw)[i]);
}

TEST(MergeTreePrimaryKey, NullableStaysRaw)
{
    auto nested = ColumnUInt64::create();
    auto null_map = ColumnUInt8::create();

    for (size_t i = 0; i < 100; ++i)
    {
        nested->insertValue(i);
        null_map->insertValue(i % 3 == 0);
    }

    ColumnPtr raw = ColumnNullable::create(std::move(nested), std::move(null_map));
    PrimaryKey pk(Columns{raw}, /*try_compress=*/ true);

    EXPECT_FALSE(pk.isColumnCompressed(0));

    for (size_t i = 0; i < 100; ++i)
    {
        EXPECT_EQ(pk.isNullAt(0, i), i % 3 == 0) << "at row " << i;
        EXPECT_EQ(pk.get(0, i), (*raw)[i]) << "at row " << i;
    }
}

TEST(MergeTreePrimaryKey, NoCompression)
{
    auto column = ColumnUInt64::create();
    for (size_t i = 0; i < 10000; ++i)
        column->insertValue(i);

    ColumnPtr raw = std::move(column);
    PrimaryKey pk(Columns{raw}, /*try_compress=*/ false);

    EXPECT_FALSE(pk.isColumnCompressed(0));
    EXPECT_EQ(pk.getRawColumn(0).get(), raw.get());
}
