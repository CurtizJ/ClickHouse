#include <Storages/Statistics/StatisticDefaults.h>
#include <IO/WriteHelpers.h>
#include <Columns/IColumn.h>
#include <IO/ReadHelpers.h>
#include <Columns/ColumnSparse.h>
#include <Common/Logger.h>
#include <Common/logger_useful.h>

namespace DB
{

StatisticDefaults::StatisticDefaults(const SingleStatisticsDescription & statistics_description)
    : IStatistics(statistics_description)
{
}

void StatisticDefaults::build(const ColumnPtr & column)
{
    size_t rows = column->size();
    double ratio = column->getRatioOfDefaultRows(ColumnSparse::DEFAULT_ROWS_SEARCH_SAMPLE_RATIO);

    rows_count += rows;
    num_defaults += static_cast<UInt64>(ratio * static_cast<double>(rows));
}

void StatisticDefaults::merge(const StatisticsPtr & other_stats)
{
    const StatisticDefaults * other = typeid_cast<const StatisticDefaults *>(other_stats.get());
    num_defaults += other->num_defaults;
    rows_count += other->rows_count;
}

void StatisticDefaults::serialize(WriteBuffer & buf)
{
    writeIntBinary(rows_count, buf);
    writeIntBinary(num_defaults, buf);
}

void StatisticDefaults::deserialize(ReadBuffer & buf)
{
    readIntBinary(rows_count, buf);
    readIntBinary(num_defaults, buf);
}

UInt64 StatisticDefaults::estimateDefaults() const
{
    return num_defaults;
}

String StatisticDefaults::getNameForLogs() const
{
    return fmt::format("Defaults: {}", num_defaults);
}

bool defaultsStatisticsValidator(const SingleStatisticsDescription & /*description*/, const DataTypePtr & /*data_type*/)
{
    return true;
}

StatisticsPtr defaultsStatisticsCreator(const SingleStatisticsDescription & description, const DataTypePtr & /*data_type*/)
{
    return std::make_shared<StatisticDefaults>(description);
}

}
