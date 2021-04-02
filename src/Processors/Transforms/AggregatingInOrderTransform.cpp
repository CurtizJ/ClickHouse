#include <Processors/Transforms/AggregatingInOrderTransform.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <Core/SortCursor.h>
#include <Columns/ColumnVector.h>
#include <ext/range.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}


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

// struct Range
// {
//     Range() = default;
//     Range(size_t left_, size_t right_, bool start_of_group_)
//         : left(left_), right(right_), start_of_group(start_of_group_) {}

//     size_t left = 0;
//     size_t right = 0;
//     bool start_of_group = false;
// };

template <typename T>
class Comparator
{
public:
    Comparator(const IColumn & column)
        : data(assert_cast<const ColumnVector<T> &>(column).getData()) {}

    bool operator()(size_t i, size_t j) { return data[i] == data[j]; }

private:
    const PaddedPODArray<T> & data;
};

}

size_t AggregatingInOrderTransform::aggregateOnIntervals(
    Aggregator::AggregateFunctionInstructions & instructions, const Columns & key_columns, size_t rows)
{
    if (rows == 0)
        return 0;

    // // std::vector<Range> stack = { Range{0, rows - 1, true} };
    // Range stack[1000];
    // ssize_t head = 0;
    // stack[head] = Range{0, rows - 1, true};
    // std::optional<size_t> start_of_interval;

    Comparator<UInt32> cmp(*key_columns[0]);

    size_t key_begin = 0;
    for (size_t i = 1; i < rows; ++i)
    {
        if (!cmp(i, i - 1))
        {
            size_t key_end = i;
            params->aggregator.executeOnIntervalWithoutKeyImpl(
                variants.without_key, key_begin, key_end,
                instructions.data(), variants.aggregates_pool);

            params->aggregator.addSingleKeyToAggregateColumns(variants, res_aggregate_columns);

            if (cur_block_size >= res_block_size)
                return key_end;

            /// We create a new state for the new key and update res_key_columns
            params->aggregator.createStatesAndFillKeyColumnsWithSingleKey(variants, key_columns, key_end, res_key_columns);
            ++cur_block_size;

            key_begin = i;
        }
    }

    // while (head >= 0)
    // {
    //     auto range = std::move(stack[head--]);
    //     // stack.pop_back();

    //     if (range.start_of_group)
    //     {
    //         if (start_of_interval)
    //         {
    //             size_t key_begin = *start_of_interval;
    //             size_t key_end = range.left;
    //             assert(key_end < rows);

    //             params->aggregator.executeOnIntervalWithoutKeyImpl(
    //                 variants.without_key, key_begin, key_end,
    //                 instructions.data(), variants.aggregates_pool);

    //             params->aggregator.addSingleKeyToAggregateColumns(variants, res_aggregate_columns);

    //             if (cur_block_size >= res_block_size)
    //                 return key_end;

    //             /// We create a new state for the new key and update res_key_columns
    //             params->aggregator.createStatesAndFillKeyColumnsWithSingleKey(variants, key_columns, key_end, res_key_columns);
    //             ++cur_block_size;
    //         }

    //         start_of_interval = range.left;
    //     }

    //     if (range.left < range.right && !cmp(range.left, range.right))
    //     {
    //         size_t mid = (range.left + range.right) / 2;
    //         bool are_equals = cmp(mid, mid + 1);

    //         // stack.emplace_back(mid + 1, range.right, !are_equals);
    //         // stack.emplace_back(range.left, mid, false);
    //         stack[++head] = Range{mid + 1, range.right, !are_equals};
    //         stack[++head] = Range{range.left, mid, false};
    //     }
    // }

    // assert(start_of_interval.has_value());
    params->aggregator.executeOnIntervalWithoutKeyImpl(
        variants.without_key, key_begin, rows,
        instructions.data(), variants.aggregates_pool);

    return rows;
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
    params->aggregator.prepareAggregateInstructions(chunk.getColumns(), aggregate_columns,
        materialized_columns, aggregate_function_instructions, nested_columns_holder);

    /// If we don't have a block we create it and fill with first key
    if (!cur_block_size)
    {
        res_key_columns.resize(params->params.keys_size);
        res_aggregate_columns.resize(params->params.aggregates_size);

        for (size_t i = 0; i < params->params.keys_size; ++i)
        {
            res_key_columns[i] = res_header.safeGetByPosition(i).type->createColumn();
            res_key_columns[i]->reserve(res_block_size);
        }

        for (size_t i = 0; i < params->params.aggregates_size; ++i)
        {
            res_aggregate_columns[i] = res_header.safeGetByPosition(i + params->params.keys_size).type->createColumn();
            res_aggregate_columns[i]->reserve(res_block_size);
        }

        params->aggregator.createStatesAndFillKeyColumnsWithSingleKey(variants, key_columns, 0, res_key_columns);
        params->aggregator.addArenasToAggregateColumns(variants, res_aggregate_columns);
        ++cur_block_size;
    }

    /// Finalize key from previous chunk.
    if (!equals(res_key_columns, key_columns, cur_block_size - 1, 0, group_by_description))
    {
        params->aggregator.addSingleKeyToAggregateColumns(variants, res_aggregate_columns);
        params->aggregator.createStatesAndFillKeyColumnsWithSingleKey(variants, key_columns, 0, res_key_columns);
        ++cur_block_size;
    }

    size_t key_end = aggregateOnIntervals(aggregate_function_instructions, key_columns, rows);
    if (key_end != rows)
    {
        Columns source_columns = chunk.detachColumns();

        for (auto & source_column : source_columns)
            source_column = source_column->cut(key_end, rows - key_end);

        current_chunk = Chunk(source_columns, rows - key_end);
        src_rows -= current_chunk.getNumRows();
        res_block_ready = true;
        need_generate = true;
        cur_block_size = 0;

        variants.without_key = nullptr;

        /// Arenas cannot be destroyed here, since later, in FinalizingSimpleTransform
        /// there will be finalizeChunk(), but even after
        /// finalizeChunk() we cannot destroy arena, since some memory
        /// from Arena still in use, so we attach it to the Chunk to
        /// remove it once it will be consumed.
        if (params->final)
        {
            if (variants.aggregates_pools.size() != 1)
                throw Exception("Too much arenas", ErrorCodes::LOGICAL_ERROR);

            Arenas arenas(1, std::make_shared<Arena>());
            std::swap(variants.aggregates_pools, arenas);
            variants.aggregates_pool = variants.aggregates_pools.at(0).get();

            chunk.setChunkInfo(std::make_shared<AggregatedArenasChunkInfo>(std::move(arenas)));
        }
    }
    else
    {
        res_block_ready = false;
    }
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

    if (res_block_ready)
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
    {
        params->aggregator.addSingleKeyToAggregateColumns(variants, res_aggregate_columns);
        variants.without_key = nullptr;
    }

    Block res = res_header.cloneEmpty();

    for (size_t i = 0; i < res_key_columns.size(); ++i)
        res.getByPosition(i).column = std::move(res_key_columns[i]);

    for (size_t i = 0; i < res_aggregate_columns.size(); ++i)
        res.getByPosition(i + res_key_columns.size()).column = std::move(res_aggregate_columns[i]);

    to_push_chunk = convertToChunk(res);
    res_rows += to_push_chunk.getNumRows();
    need_generate = false;
}


}
