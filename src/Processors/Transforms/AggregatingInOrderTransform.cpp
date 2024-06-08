#include <Processors/Transforms/AggregatingInOrderTransform.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <Storages/SelectQueryInfo.h>
#include <Core/SortCursor.h>
#include <Common/logger_useful.h>
#include <Common/formatReadable.h>
#include <Interpreters/sortBlock.h>
#include <base/range.h>

namespace DB
{

namespace
{

template <typename LeftColumns, typename RightColumns>
bool equals(const LeftColumns & lhs, const RightColumns & rhs, size_t i, size_t j, const SortDescriptionWithPositions & descr)
{
    for (const auto & elem : descr)
    {
        size_t ind = elem.column_number;
        if (lhs[ind]->compareAt(i, j, *rhs[ind], elem.base.nulls_direction))
            return false;
    }
    return true;
}

template <typename Columns>
bool equals(const Columns & columns, size_t i, size_t j, const SortDescriptionWithPositions & descr)
{
    return equals(columns, columns, i, j, descr);
}

Int64 getCurrentMemoryUsage()
{
    Int64 current_memory_usage = 0;
    if (auto * memory_tracker = CurrentThread::getMemoryTracker())
        current_memory_usage = memory_tracker->get();
    return current_memory_usage;
}

}

AggregatingInOrderTransform::AggregatingInOrderTransform(
    Block header,
    AggregatingTransformParamsPtr params_,
    const SortDescription & sort_description_for_merging,
    const SortDescription & group_by_description_,
    size_t max_block_size_, size_t max_block_bytes_)
    : AggregatingInOrderTransform(std::move(header), std::move(params_),
        sort_description_for_merging, group_by_description_,
        max_block_size_, max_block_bytes_,
        std::make_unique<ManyAggregatedData>(1), 0)
{
}

AggregatingInOrderTransform::AggregatingInOrderTransform(
    Block header,
    AggregatingTransformParamsPtr params_,
    const SortDescription & sort_description_for_merging,
    const SortDescription & group_by_description_,
    size_t max_block_size_,
    size_t max_block_bytes_,
    ManyAggregatedDataPtr many_data_,
    size_t current_variant)
    : IProcessor({std::move(header)}, {params_->getCustomHeader(false)})
    , max_block_size(max_block_size_)
    , max_block_bytes(max_block_bytes_)
    , aggregate_params(std::move(params_))
    , aggregates_mask(getAggregatesMask(aggregate_params->getHeader(), aggregate_params->params.aggregates))
    , sort_description(group_by_description_)
    , many_data(std::move(many_data_))
    , aggregate_variants(*many_data->variants[current_variant])
{
    /// We won't finalize states in order to merge same states (generated due to multi-thread execution) in AggregatingSortedTransform
    res_header = aggregate_params->getCustomHeader(/* final_= */ false);

    for (size_t i = 0; i < sort_description_for_merging.size(); ++i)
    {
        const auto & column_description = group_by_description_[i];
        group_by_description.emplace_back(column_description, res_header.getPositionByName(column_description.column_name));
    }

    /// group_by_description may contains duplicates, so we use keys_size from Aggregator::params
    if (sort_description_for_merging.size() < group_by_description_.size())
        group_by_key = true;
}

AggregatingInOrderTransform::~AggregatingInOrderTransform() = default;

void AggregatingInOrderTransform::InputData::init(const Block & header, const ColumnsMask & mask, ssize_t result_size_hint)
{
    current_border = 1;
    borders = {0, num_rows};

    materialized_columns.reserve(params.params.keys_size);
    key_columns.reserve(params.params.keys_size);
    key_columns_raw.reserve(params.params.keys_size);

    for (size_t i = 0; i < params.params.keys_size; ++i)
    {
        size_t pos = header.getPositionByName(params.params.keys[i]);

        materialized_columns.push_back(all_columns[pos]->convertToFullColumnIfConst());
        key_columns.push_back(materialized_columns.back());
        key_columns_raw.push_back(materialized_columns.back().get());

        key_columns.back()->getEqualRanges(borders, result_size_hint);
    }

    if (params.params.only_merge)
    {
        aggregate_columns_data.resize(params.params.aggregates_size);
        for (size_t i = 0, j = 0; i < all_columns.size(); ++i)
        {
            if (mask[i])
                aggregate_columns_data[j++] = &typeid_cast<const ColumnAggregateFunction &>(*all_columns[i]).getData();
        }
    }
    else
    {
        aggregate_columns.resize(params.params.aggregates_size);
        params.aggregator.prepareAggregateInstructions(
            all_columns,
            aggregate_columns,
            materialized_columns,
            aggregate_function_instructions,
            nested_columns_holder);
    }
}

void AggregatingInOrderTransform::OutputData::init(const Block & header, InputData & input)
{
    res_key_columns.resize(params.params.keys_size);
    for (size_t i = 0; i < params.params.keys_size; ++i)
        res_key_columns[i] = header.safeGetByPosition(i).type->createColumn();

    if (!group_by_key)
    {
        res_aggregate_columns.resize(params.params.aggregates_size);
        for (size_t i = 0; i < params.params.aggregates_size; ++i)
            res_aggregate_columns[i] = header.safeGetByPosition(i + params.params.keys_size).type->createColumn();

        params.aggregator.addArenasToAggregateColumns(variants, res_aggregate_columns);
    }

    createNewState(input, 0);
}

void AggregatingInOrderTransform::OutputData::addToAggregateColumns()
{
    if (!group_by_key)
        params.aggregator.addSingleKeyToAggregateColumns(variants, res_aggregate_columns);
}

void AggregatingInOrderTransform::OutputData::createNewState(InputData & input, size_t key_index)
{
    params.aggregator.createStatesAndFillKeyColumnsWithSingleKey(variants, input.key_columns, key_index, res_key_columns);
    ++num_rows;
}

void AggregatingInOrderTransform::OutputData::executeAggregation(
    InputData & input,
    size_t key_begin,
    size_t key_end,
    std::atomic_bool & is_cancelled)
{
    if (params.params.only_merge)
    {
        if (group_by_key)
            params.aggregator.mergeOnBlockSmall(variants, key_begin, key_end, input.aggregate_columns_data, input.key_columns_raw);
        else
            params.aggregator.mergeOnIntervalWithoutKey(variants, key_begin, key_end, input.aggregate_columns_data, is_cancelled);
    }
    else
    {
        if (group_by_key)
            params.aggregator.executeOnBlockSmall(variants, key_begin, key_end, input.key_columns_raw, input.aggregate_function_instructions.data());
        else
            params.aggregator.executeOnIntervalWithoutKey(variants, key_begin, key_end, input.aggregate_function_instructions.data());
    }
}

void AggregatingInOrderTransform::consume(InputData & input, OutputData & output)
{
    Int64 initial_memory_usage = getCurrentMemoryUsage();

    if (!is_consume_started)
    {
        LOG_TRACE(log, "Aggregating in order");
        is_consume_started = true;
    }

    if (input.num_rows == 0)
    {
        return;
    }

    if (input.borders.empty())
    {
        ssize_t result_size_hint = estimateCardinality(input.num_rows);
        input.init(inputs.front().getHeader(), aggregates_mask, result_size_hint);
    }

    /// If we don't have a block we create it and fill with first key
    if (!output.num_rows)
    {
        output.init(res_header, input);
    }
    else if (isNewKey(input, output))
    {
        output.addToAggregateColumns();
        output.createNewState(input, 0);
    }

    Int64 current_memory_usage = 0;

    for (size_t i = input.current_border; i < input.borders.size(); ++i)
    {
        size_t key_begin = input.borders[i - 1];
        size_t key_end = input.borders[i];

        current_memory_usage = std::max<Int64>(getCurrentMemoryUsage() - initial_memory_usage, 0);
        output.executeAggregation(input, key_begin, key_end, is_cancelled);

        /// Finalize last key aggregation state.
        if (key_end != input.num_rows)
        {
            /// If max_block_size is reached we have to stop consuming and generate the block.
            if (output.num_rows >= max_block_size || output.num_bytes + current_memory_usage >= max_block_bytes)
            {
                input.current_border = i;
                output.need_generate = true;
                output.num_bytes += current_memory_usage;
                return;
            }

            output.addToAggregateColumns();
            output.createNewState(input, key_end);
        }
    }

    input.current_border = input.borders.size();
    output.num_bytes += current_memory_usage;
}

void AggregatingInOrderTransform::work()
{
    if (current_output && current_output->need_generate)
    {
        generate(*current_output);
        current_output.reset();
    }
    else
    {
        chassert(current_input);
        if (!current_output)
            current_output.emplace(*aggregate_params, aggregate_variants, group_by_key);

        consume(*current_input, *current_output);

        if (current_input->isFinished())
            current_input.reset();
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

    if (current_output && current_output->need_generate)
    {
        return Status::Ready;
    }

    if (to_push_chunk)
    {
        output.push(std::move(to_push_chunk));
    }

    if (input.isFinished())
    {
        if (!current_output)
        {
            output.finish();
            LOG_DEBUG(log, "Aggregated. {} to {} rows (from {})", src_rows, res_rows, formatReadableSizeWithBinarySuffix(src_bytes));
            return Status::Finished;
        }

        current_output->need_generate = true;
        return Status::Ready;
    }

    if (current_input)
    {
        return Status::Ready;
    }

    if (!input.hasData())
    {
        input.setNeeded();
        return Status::NeedData;
    }

    if (auto chunk = input.pull(/*set_not_needed=*/ true))
    {
        src_rows += chunk.getNumRows();
        src_bytes += chunk.bytes();
        convertToFullIfSparse(chunk);
        current_input.emplace(std::move(chunk), *aggregate_params);
    }

    return Status::Ready;
}

void AggregatingInOrderTransform::generate(OutputData & output)
{
    chassert(output.need_generate);

    Block group_by_block;
    if (output.num_rows)
    {
        if (group_by_key)
            group_by_block = aggregate_params->aggregator.prepareBlockAndFillSingleLevel</*return_single_block*/ true>(output.variants, /*final=*/ false);

        output.addToAggregateColumns();
        output.variants.invalidate();
    }

    if (!group_by_key || !output.num_rows)
    {
        Block res = res_header.cloneEmpty();

        for (size_t i = 0; i < output.res_key_columns.size(); ++i)
            res.getByPosition(i).column = std::move(output.res_key_columns[i]);

        for (size_t i = 0; i < output.res_aggregate_columns.size(); ++i)
            res.getByPosition(i + output.res_key_columns.size()).column = std::move(output.res_aggregate_columns[i]);

        to_push_chunk = convertToChunk(res);
    }
    else
    {
        /// Sorting is required after aggregation, for proper merging, via
        /// FinishAggregatingInOrderTransform/MergingAggregatedBucketTransform
        sortBlock(group_by_block, sort_description);
        to_push_chunk = convertToChunk(group_by_block);
    }

    if (!to_push_chunk.getNumRows())
        return;

    /// Clear arenas to allow to free them, when chunk will reach the end of pipeline.
    /// It's safe clear them here, because columns with aggregate functions already holds them.
    aggregate_variants.aggregates_pools = { std::make_shared<Arena>() };
    aggregate_variants.aggregates_pool = aggregate_variants.aggregates_pools.at(0).get();

    /// Pass info about used memory by aggregate functions further.
    to_push_chunk.setChunkInfo(std::make_shared<ChunkInfoWithAllocatedBytes>(output.num_bytes));
    res_rows += to_push_chunk.getNumRows();
}

bool AggregatingInOrderTransform::isNewKey(const InputData & input, const OutputData & output)
{
    return !equals(output.res_key_columns, input.key_columns, output.num_rows - 1, 0, group_by_description);
}

ssize_t AggregatingInOrderTransform::estimateCardinality(size_t num_rows) const
{
    if (src_rows == 0)
        return -1;

    double rate = static_cast<double>(res_rows) / src_rows;
    return static_cast<ssize_t>(num_rows * rate);
}

FinalizeAggregatedTransform::FinalizeAggregatedTransform(Block header, AggregatingTransformParamsPtr params_)
    : ISimpleTransform({std::move(header)}, {params_->getHeader()}, true)
    , params(params_)
    , aggregates_mask(getAggregatesMask(params->getHeader(), params->params.aggregates))
{
}

void FinalizeAggregatedTransform::transform(Chunk & chunk)
{
    if (params->final)
    {
        finalizeChunk(chunk, aggregates_mask);
    }
    else if (!chunk.getChunkInfo())
    {
        auto info = std::make_shared<AggregatedChunkInfo>();
        chunk.setChunkInfo(std::move(info));
    }
}


}
