#include <Storages/MergeTree/MergeTreePrimaryKey.h>

#include <optional>

#include <Columns/ColumnString.h>
#include <Columns/ColumnVector.h>
#include <Columns/ColumnsNumber.h>
#include <Common/BitPackedStringArray.h>
#include <Common/BitPackedUInt64Array.h>
#include <Common/typeid_cast.h>
#include <DataTypes/IDataType.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

namespace
{

ColumnPtr materializeColumn(const IPrimaryKeyColumn & pk_column, const DataTypePtr & type)
{
    size_t num_rows = pk_column.size();

    auto full_column = type->createColumn();
    full_column->reserve(num_rows);

    Field field;
    for (size_t row_idx = 0; row_idx < num_rows; ++row_idx)
    {
        pk_column.get(row_idx, field);
        full_column->insert(field);
    }

    return full_column;
}

class PrimaryKeyColumnRaw final : public IPrimaryKeyColumn
{
public:
    explicit PrimaryKeyColumnRaw(ColumnPtr column_) : column(std::move(column_)) {}

    size_t size() const override { return column->size(); }
    bool isCompressed() const override { return false; }

    size_t bytes() const override { return column->byteSize(); }
    size_t allocatedBytes() const override { return column->allocatedBytes(); }

    void get(size_t row_idx, Field & field) const override { column->get(row_idx, field); }
    bool isNullAt(size_t row_idx) const override { return column->isNullAt(row_idx); }

    bool equalAt(size_t lhs_idx, size_t rhs_idx) const override
    {
        return column->compareAt(lhs_idx, rhs_idx, *column, 1) == 0;
    }

    ColumnPtr toFullColumn(const DataTypePtr &) const override { return column; }
    ColumnPtr tryGetRawColumn() const override { return column; }

private:
    ColumnPtr column;
};

class PrimaryKeyColumnBitPackedUInt64 final : public IPrimaryKeyColumn
{
public:
    explicit PrimaryKeyColumnBitPackedUInt64(BitPackedUInt64Array data_) : data(std::move(data_)) {}

    size_t size() const override { return data.size(); }
    bool isCompressed() const override { return true; }

    size_t bytes() const override { return data.allocatedBytes(); }
    size_t allocatedBytes() const override { return data.allocatedBytes(); }

    void get(size_t row_idx, Field & field) const override { field = data.get(row_idx); }
    bool isNullAt(size_t) const override { return false; }
    bool equalAt(size_t lhs_idx, size_t rhs_idx) const override { return data.get(lhs_idx) == data.get(rhs_idx); }

    ColumnPtr toFullColumn(const DataTypePtr & type) const override { return materializeColumn(*this, type); }

private:
    BitPackedUInt64Array data;
};

class PrimaryKeyColumnBitPackedString final : public IPrimaryKeyColumn
{
public:
    explicit PrimaryKeyColumnBitPackedString(BitPackedStringArray data_) : data(std::move(data_)) {}

    size_t size() const override { return data.size(); }
    bool isCompressed() const override { return true; }

    size_t bytes() const override { return data.allocatedBytes(); }
    size_t allocatedBytes() const override { return data.allocatedBytes(); }

    void get(size_t row_idx, Field & field) const override { field = String(data.get(row_idx)); }
    bool isNullAt(size_t) const override { return false; }
    bool equalAt(size_t lhs_idx, size_t rhs_idx) const override { return data.get(lhs_idx) == data.get(rhs_idx); }

    ColumnPtr toFullColumn(const DataTypePtr & type) const override { return materializeColumn(*this, type); }

private:
    BitPackedStringArray data;
};

/// Returns the values of a column of unsigned integers bit-packed,
/// or nothing if the column is of another type.
std::optional<BitPackedUInt64Array> tryBitPackColumn(const IColumn & column)
{
    auto pack = [](const auto & data)
    {
        return BitPackedUInt64Array(std::span(data.data(), data.size()));
    };

    if (const auto * column_uint8 = typeid_cast<const ColumnUInt8 *>(&column))
        return pack(column_uint8->getData());

    if (const auto * column_uint16 = typeid_cast<const ColumnUInt16 *>(&column))
        return pack(column_uint16->getData());

    if (const auto * column_uint32 = typeid_cast<const ColumnUInt32 *>(&column))
        return pack(column_uint32->getData());

    if (const auto * column_uint64 = typeid_cast<const ColumnUInt64 *>(&column))
        return pack(column_uint64->getData());

    return {};
}

PrimaryKeyColumnPtr makePrimaryKeyColumn(ColumnPtr column, bool try_compress)
{
    if (try_compress)
    {
        if (const auto * column_string = typeid_cast<const ColumnString *>(column.get()))
        {
            BitPackedStringArray packed(column_string->getChars(), column_string->getOffsets());
            if (packed.allocatedBytes() < column->allocatedBytes())
                return std::make_unique<PrimaryKeyColumnBitPackedString>(std::move(packed));
        }
        else if (auto packed = tryBitPackColumn(*column); packed && packed->allocatedBytes() < column->allocatedBytes())
        {
            return std::make_unique<PrimaryKeyColumnBitPackedUInt64>(std::move(*packed));
        }
    }

    return std::make_unique<PrimaryKeyColumnRaw>(std::move(column));
}

}

PrimaryKey::PrimaryKey(Columns raw_columns, bool try_compress)
    : num_rows(raw_columns.empty() ? 0 : raw_columns.front()->size())
{
    columns.reserve(raw_columns.size());

    for (auto & column : raw_columns)
    {
        chassert(column->size() == num_rows);
        columns.push_back(makePrimaryKeyColumn(std::move(column), try_compress));
    }
}

Field PrimaryKey::get(size_t column_idx, size_t row_idx) const
{
    Field field;
    get(column_idx, row_idx, field);
    return field;
}

ColumnPtr PrimaryKey::getRawColumn(size_t column_idx) const
{
    auto raw_column = getColumn(column_idx).tryGetRawColumn();
    if (!raw_column)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Column {} of primary key is compressed in memory", column_idx);

    return raw_column;
}

size_t PrimaryKey::bytes() const
{
    size_t res = 0;
    for (const auto & column : columns)
        res += column->bytes();
    return res;
}

size_t PrimaryKey::allocatedBytes() const
{
    size_t res = 0;
    for (const auto & column : columns)
        res += column->allocatedBytes();
    return res;
}

}
