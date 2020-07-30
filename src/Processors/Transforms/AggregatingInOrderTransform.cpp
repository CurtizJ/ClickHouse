#include <Processors/Transforms/AggregatingInOrderTransform.h>
#include <DataTypes/DataTypeLowCardinality.h>

namespace DB
{

AggregatingInOrderTransform::AggregatingInOrderTransform(
    Block header, AggregatingTransformParamsPtr params_,
    const SortDescription & group_by_description_, size_t res_block_size_)
    : AggregatingInOrderTransform(std::move(header), std::move(params_)
    , group_by_description_, res_block_size_, std::make_unique<ManyAggregatedData>(1), 0)
{
}

AggregatingInOrderTransform::AggregatingInOrderTransform(
    Block header, AggregatingTransformParamsPtr params_,
    const SortDescription & group_by_description_, size_t res_block_size_,
    ManyAggregatedDataPtr many_data_, size_t current_variant)
    : IProcessor({std::move(header)}, {params_->getCustomHeader(false)})
    , res_block_size(res_block_size_)
    , params(std::move(params_))
    , group_by_description(group_by_description_)
    , aggregate_columns(params->params.aggregates_size)
    , many_data(std::move(many_data_))
    , variants(*many_data->variants[current_variant])
{
    /// We won't finalize states in order to merge same states (generated due to multi-thread execution) in AggregatingSortedTransform
    res_header = params->getCustomHeader(false);

    /// Replace column names to column position in description_sorted.
    for (auto & column_description : group_by_description)
    {
        if (!column_description.column_name.empty())
        {
            column_description.column_number = res_header.getPositionByName(column_description.column_name);
            column_description.column_name.clear();
        }
    }
}

AggregatingInOrderTransform::~AggregatingInOrderTransform() = default;

namespace
{

template <typename LeftColumns, typename RightColumns>
bool equals(const LeftColumns & lhs, const RightColumns & rhs, size_t i, size_t j, const SortDescription & descr)
{
    for (const auto & elem : descr)
    {
        size_t ind = elem.column_number;
        int res = lhs[ind]->compareAt(i, j, *rhs[ind], elem.nulls_direction);
        if (res)
            return false;
    }

    return true;
}

template <typename Columns>
bool equals(const Columns & columns, size_t i, size_t j, const SortDescription & descr)
{
    return equals(columns, columns, i, j, descr);
}

struct Range
{
    Range(size_t left_, size_t right_, bool start_of_group_)
        : left(left_), right(right_), start_of_group(start_of_group_) {}

    size_t left;
    size_t right;
    bool start_of_group;
}; 

std::vector<size_t> splitByRanges(const Columns & columns, const SortDescription & descr, size_t & cnt)
{
    assert(!columns.empty());
    size_t rows_num = columns[0]->size();
    if (!rows_num)
        return {};

    std::vector<size_t> positions;
    std::vector<Range> stack = { Range{0, rows_num - 1, true} };

    while (!stack.empty())
    {
        auto range = std::move(stack.back());
        stack.pop_back();
        
        if (range.start_of_group)
            positions.push_back(range.left);

        if (range.left < range.right)
            ++cnt;
        
        if (range.left < range.right && !equals(columns, range.left, range.right, descr))
        {
            size_t mid = (range.left + range.right) / 2;
            bool are_equals = equals(columns, mid, mid + 1, descr);

            ++cnt;

            stack.emplace_back(mid + 1, range.right, !are_equals);
            stack.emplace_back(range.left, mid, false);
        }
    }

    return positions;
}

}


void AggregatingInOrderTransform::consume(Chunk chunk)
{
    size_t rows = chunk.getNumRows();
    if (rows == 0)
        return;

    if (!is_consume_started)
    {
        LOG_TRACE(log, "Aggregating in order");
        is_consume_started = true;
    }
    src_rows += rows;
    src_bytes += chunk.bytes();

    Columns materialized_columns;
    Columns key_columns(params->params.keys_size);
    for (size_t i = 0; i < params->params.keys_size; ++i)
    {
        materialized_columns.push_back(chunk.getColumns().at(params->params.keys[i])->convertToFullColumnIfConst());
        key_columns[i] = materialized_columns.back();
    }

    Aggregator::NestedColumnsHolder nested_columns_holder;
    Aggregator::AggregateFunctionInstructions aggregate_function_instructions;
    params->aggregator.prepareAggregateInstructions(chunk.getColumns(), aggregate_columns, materialized_columns, aggregate_function_instructions, nested_columns_holder);

    /// If we don't have a block we create it and fill with first key
    if (!cur_block_size)
    {
        res_key_columns.resize(params->params.keys_size);
        res_aggregate_columns.resize(params->params.aggregates_size);

        for (size_t i = 0; i < params->params.keys_size; ++i)
        {
            res_key_columns[i] = res_header.safeGetByPosition(i).type->createColumn();
        }
        for (size_t i = 0; i < params->params.aggregates_size; ++i)
        {
            res_aggregate_columns[i] = res_header.safeGetByPosition(i + params->params.keys_size).type->createColumn();
        }
        params->aggregator.createStatesAndFillKeyColumnsWithSingleKey(variants, key_columns, 0, res_key_columns);
        ++cur_block_size;
    }

    auto borders = splitByRanges(key_columns, group_by_description, cnt);
    borders.push_back(rows);

    assert(std::is_sorted(borders.begin(), borders.end()));

    ++cnt;

    /// Finalize key from previous chunk.
    if (!equals(res_key_columns, key_columns, cur_block_size - 1, 0, group_by_description))
    {
        params->aggregator.fillAggregateColumnsWithSingleKey(variants, res_aggregate_columns);
        params->aggregator.createStatesAndFillKeyColumnsWithSingleKey(variants, key_columns, 0, res_key_columns);
        ++cur_block_size;
    }
        
    for (size_t i = 1; i < borders.size(); ++i)
    {
        size_t key_begin = borders[i - 1];
        size_t key_end = borders[i];

        /// Add data from interval with current key to aggregate state.
        params->aggregator.executeOnIntervalWithoutKeyImpl(
            variants.without_key, key_begin, key_end,
            aggregate_function_instructions.data(), variants.aggregates_pool);

        if (key_end != rows)
        {
            params->aggregator.fillAggregateColumnsWithSingleKey(variants, res_aggregate_columns);

            /// If res_block_size is reached we have to stop consuming and generate the block. Save the extra rows into new chunk.
            if (cur_block_size == res_block_size)
            {
                Columns source_columns = chunk.detachColumns();

                for (auto & source_column : source_columns)
                    source_column = source_column->cut(key_begin, rows - key_begin);

                current_chunk = Chunk(source_columns, rows - key_begin);
                block_end_reached = true;
                need_generate = true;
                cur_block_size = 0;
                return;
            }

            /// We create a new state for the new key and update res_key_columns
            params->aggregator.createStatesAndFillKeyColumnsWithSingleKey(variants, key_columns, key_begin, res_key_columns);
            ++cur_block_size;
        }
    }

    block_end_reached = false;
}


void AggregatingInOrderTransform::work()
{
    if (is_consume_finished || need_generate)
    {
        generate();
    }
    else
    {
        consume(std::move(current_chunk));
    }
}


IProcessor::Status AggregatingInOrderTransform::prepare()
{
    auto & output = outputs.front();
    auto & input = inputs.back();

    /// Check can output.
    if (output.isFinished())
    {
        input.close();
        return Status::Finished;
    }

    if (!output.canPush())
    {
        input.setNotNeeded();
        return Status::PortFull;
    }

    if (block_end_reached)
    {
        if (need_generate)
        {
            return Status::Ready;
        }
        else
        {
            output.push(std::move(to_push_chunk));
            return Status::Ready;
        }
    }
    else
    {
        if (is_consume_finished)
        {
            output.push(std::move(to_push_chunk));
            output.finish();
            LOG_TRACE(log, "Aggregated. {} to {} rows (from {})", src_rows, res_rows,
                                        formatReadableSizeWithBinarySuffix(src_bytes));

            std::cerr << "rows: " << src_rows << "compares: " << cnt << "\n";
            return Status::Finished;
        }
        if (input.isFinished())
        {
            is_consume_finished = true;
            return Status::Ready;
        }
    }
    if (!input.hasData())
    {
        input.setNeeded();
        return Status::NeedData;
    }
    current_chunk = input.pull(!is_consume_finished);
    return Status::Ready;
}

void AggregatingInOrderTransform::generate()
{
    if (cur_block_size && is_consume_finished)
        params->aggregator.fillAggregateColumnsWithSingleKey(variants, res_aggregate_columns);

    Block res = res_header.cloneEmpty();

    for (size_t i = 0; i < res_key_columns.size(); ++i)
    {
        res.getByPosition(i).column = std::move(res_key_columns[i]);
    }
    for (size_t i = 0; i < res_aggregate_columns.size(); ++i)
    {
        res.getByPosition(i + res_key_columns.size()).column = std::move(res_aggregate_columns[i]);
    }
    to_push_chunk = convertToChunk(res);
    res_rows += to_push_chunk.getNumRows();
    need_generate = false;
}


}
