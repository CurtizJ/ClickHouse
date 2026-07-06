#pragma once

#include <Columns/IColumn.h>
#include <Core/Field.h>
#include <DataTypes/IDataType_fwd.h>

namespace DB
{

/** A column of the in-memory primary key of a data part.
  * The column is stored either as a raw IColumn or in a compressed form
  * which supports only access to values by row index.
  */
class IPrimaryKeyColumn
{
public:
    virtual ~IPrimaryKeyColumn() = default;

    virtual size_t size() const = 0;
    virtual bool empty() const { return size() == 0; }
    virtual bool isCompressed() const = 0;

    virtual size_t bytes() const = 0;
    virtual size_t allocatedBytes() const = 0;

    virtual void get(size_t row_idx, Field & field) const = 0;
    virtual bool equalAt(size_t lhs_idx, size_t rhs_idx) const = 0;
    virtual bool isNullAt(size_t row_idx) const = 0;

    /// Returns the column as a whole IColumn of the given type, decompressing it if it is stored compressed.
    virtual ColumnPtr toFullColumn(const DataTypePtr & type) const = 0;

    /// Returns the raw column, or nullptr if the column is stored compressed.
    virtual ColumnPtr tryGetRawColumn() const { return nullptr; }
};

using PrimaryKeyColumnPtr = std::unique_ptr<const IPrimaryKeyColumn>;

/** In-memory representation of the primary key of a data part (corresponds to the primary.idx file).
  * Contains each index_granularity-th value of the primary key tuple.
  *
  * To reduce memory usage, columns of unsigned integers and strings may be stored
  * in a compressed form (see BitPackedUInt64Array and BitPackedStringArray) which
  * supports only access to values by row index. A column is kept compressed only
  * if the compressed form allocates less memory than the raw column.
  *
  * Consumers that need a whole IColumn (e.g. the mergeTreeIndex table function or
  * the index analysis with chains of monotonic functions) should use getFullColumn
  * which decompresses the column if needed.
  */
struct PrimaryKey
{
    PrimaryKey() = default;
    PrimaryKey(Columns raw_columns, bool try_compress);

    PrimaryKey(const PrimaryKey &) = delete;
    PrimaryKey & operator=(const PrimaryKey &) = delete;
    PrimaryKey(PrimaryKey &&) = default;
    PrimaryKey & operator=(PrimaryKey &&) = default;

    /// The primary key may have fewer columns than in the table's metadata
    /// if suffix columns were dropped (see IMergeTreeDataPart::optimizeIndexColumns).
    size_t getNumColumns() const { return columns.size(); }
    size_t getNumRows() const { return num_rows; }
    bool empty() const { return columns.empty(); }

    const IPrimaryKeyColumn & getColumn(size_t column_idx) const { return *columns.at(column_idx); }
    bool isColumnCompressed(size_t column_idx) const { return getColumn(column_idx).isCompressed(); }

    void get(size_t column_idx, size_t row_idx, Field & field) const { columns[column_idx]->get(row_idx, field); }
    Field get(size_t column_idx, size_t row_idx) const;

    bool isNullAt(size_t column_idx, size_t row_idx) const { return columns[column_idx]->isNullAt(row_idx); }
    bool equalAt(size_t column_idx, size_t lhs_row_idx, size_t rhs_row_idx) const { return columns[column_idx]->equalAt(lhs_row_idx, rhs_row_idx); }

    /// Returns the column as a whole IColumn of the given type, decompressing it if it is stored compressed.
    ColumnPtr getFullColumn(size_t column_idx, const DataTypePtr & type) const { return getColumn(column_idx).toFullColumn(type); }

    /// Returns the raw column. Throws an exception if the column is stored compressed.
    /// May be used only for parts which are guaranteed to store raw columns (e.g. patch parts).
    ColumnPtr getRawColumn(size_t column_idx) const;

    size_t bytes() const;
    size_t allocatedBytes() const;

private:
    size_t num_rows = 0;
    std::vector<PrimaryKeyColumnPtr> columns;
};

using PrimaryKeyPtr = std::shared_ptr<const PrimaryKey>;

}
