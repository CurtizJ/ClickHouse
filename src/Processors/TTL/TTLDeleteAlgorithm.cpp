#include <Processors/TTL/TTLDeleteAlgorithm.h>
#include <Columns/ColumnConst.h>
#include <Columns/ColumnsNumber.h>

namespace DB
{

TTLDeleteAlgorithm::TTLDeleteAlgorithm(
    const TTLExpressions & ttl_expressions_, const TTLDescription & description_, const TTLInfo & old_ttl_info_, time_t current_time_, bool force_)
    : ITTLAlgorithm(ttl_expressions_, description_, old_ttl_info_, current_time_, force_)
{
    if (!isMinTTLExpired())
        new_ttl_info = old_ttl_info;

    if (isMaxTTLExpired())
        new_ttl_info.ttl_finished = true;
}

void TTLDeleteAlgorithm::execute(Block & block)
{
    if (!block || !isMinTTLExpired())
        return;

    auto ttl_column = executeExpressionAndGetColumn(ttl_expressions.expression, block, description.result_column);
    auto where_column = executeExpressionAndGetColumn(ttl_expressions.where_expression, block, description.where_result_column);

    if (where_column)
        where_column = where_column->convertToFullColumnIfConst();

    if (const auto * ttl_const = typeid_cast<const ColumnConst *>(ttl_column.get()))
    {
        executeConst(block, *ttl_const, where_column.get());
        return;
    }

    IColumn::Filter filter(block.rows(), 0U);
    buildTTLFilter(*ttl_column, filter);

    if (where_column)
        buildWhereFilter(*where_column, filter);

    applyFilter(block, filter);
}

void TTLDeleteAlgorithm::executeConst(Block & block, const ColumnConst & ttl_const, const IColumn * where_column)
{
    UInt32 ttl_value;
    if (ttl_const.getDataType() == TypeIndex::UInt16)
        ttl_value = static_cast<UInt32>(date_lut.fromDayNum(DayNum(ttl_const.getValue<UInt16>())));
    else if (ttl_const.getDataType() == TypeIndex::UInt32)
        ttl_value = ttl_const.getValue<UInt32>();
    else
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected type {} of result TTL column", ttl_const.getName());

    new_ttl_info.update(ttl_value);

    if (!isTTLExpired(ttl_value))
        return;

    if (where_column)
    {
        IColumn::Filter filter(block.rows(), 0U);
        buildWhereFilter(*where_column, filter);
        applyFilter(block, filter);
    }
    else
    {
        rows_removed += block.rows();
        block = block.cloneEmpty();
    }
}

void TTLDeleteAlgorithm::buildTTLFilter(const IColumn & ttl_column, IColumn::Filter & filter)
{
    if (const auto * ttl_date = typeid_cast<const ColumnUInt16 *>(&ttl_column))
    {
        const auto & ttl_data = ttl_date->getData();
        for (size_t i = 0; i < filter.size(); ++i)
        {
            UInt32 ttl_value = static_cast<UInt32>(date_lut.fromDayNum(DayNum(ttl_data[i])));
            filter[i] = ttl_value > current_time;
            new_ttl_info.update(ttl_value);
        }
    }
    else if (const auto * ttl_date_time = typeid_cast<const ColumnUInt32 *>(&ttl_column))
    {
        const auto & ttl_data = ttl_date_time->getData();
        for (size_t i = 0; i < filter.size(); ++i)
        {
            filter[i] = ttl_data[i] > current_time;
            new_ttl_info.update(ttl_data[i]);
        }
    }
    else
    {
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected type {} of result TTL column", ttl_column.getName());
    }
}

void TTLDeleteAlgorithm::buildWhereFilter(const IColumn & where_column, IColumn::Filter & filter)
{
    if (const auto * where_column_bool = typeid_cast<const ColumnUInt8 *>(&where_column))
    {
        for (size_t i = 0; i < filter.size(); ++i)
            filter[i] |= !where_column_bool->getData()[i];
    }
    else
    {
        for (size_t i = 0; i < filter.size(); ++i)
            filter[i] |= !where_column.getBool(i);
    }
}

void TTLDeleteAlgorithm::applyFilter(Block & block, const IColumn::Filter & filter)
{
    size_t old_size = block.rows();
    for (auto & column : block)
        column.column = column.column->filter(filter, -1);
    rows_removed += old_size - block.rows();
}

void TTLDeleteAlgorithm::finalize(const MutableDataPartPtr & data_part) const
{
    if (ttl_expressions.where_expression)
        data_part->ttl_infos.rows_where_ttl[description.result_column] = new_ttl_info;
    else
        data_part->ttl_infos.table_ttl = new_ttl_info;

    data_part->ttl_infos.updatePartMinMaxTTL(new_ttl_info.min, new_ttl_info.max);
}

}
