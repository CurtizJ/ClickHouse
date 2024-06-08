#pragma once

#include <Core/SortDescription.h>
#include <Interpreters/Aggregator.h>
#include <Processors/ISimpleTransform.h>
#include <Processors/Transforms/AggregatingTransform.h>
#include <Processors/Transforms/finalizeChunk.h>
#include "Columns/IColumn.h"

namespace DB
{

struct InputOrderInfo;
using InputOrderInfoPtr = std::shared_ptr<const InputOrderInfo>;

struct ChunkInfoWithAllocatedBytes : public ChunkInfo
{
    explicit ChunkInfoWithAllocatedBytes(Int64 allocated_bytes_)
        : allocated_bytes(allocated_bytes_) {}
    Int64 allocated_bytes;
};

class AggregatingInOrderTransform : public IProcessor
{
public:
    AggregatingInOrderTransform(Block header, AggregatingTransformParamsPtr params,
                                const SortDescription & sort_description_for_merging,
                                const SortDescription & group_by_description_,
                                size_t max_block_size_, size_t max_block_bytes_,
                                ManyAggregatedDataPtr many_data, size_t current_variant);

    AggregatingInOrderTransform(Block header, AggregatingTransformParamsPtr params,
                                const SortDescription & sort_description_for_merging,
                                const SortDescription & group_by_description_,
                                size_t max_block_size_, size_t max_block_bytes_);

    ~AggregatingInOrderTransform() override;

    String getName() const override { return "AggregatingInOrderTransform"; }

    Status prepare() override;

    void work() override;

private:
    struct InputData
    {
        InputData(Chunk chunk_, AggregatingTransformParams & params_)
            : num_rows(chunk_.getNumRows()), all_columns(chunk_.detachColumns()), params(params_)
        {
        }

        size_t num_rows;
        Columns all_columns;
        AggregatingTransformParams & params;

        size_t current_border = 0;
        std::vector<size_t> borders;

        Columns materialized_columns;
        Columns key_columns;
        ColumnRawPtrs key_columns_raw;
        Aggregator::AggregateColumns aggregate_columns;

        Aggregator::NestedColumnsHolder nested_columns_holder;
        Aggregator::AggregateFunctionInstructions aggregate_function_instructions;
        Aggregator::AggregateColumnsConstData aggregate_columns_data;

        bool isFinished() const { return current_border == borders.size(); }
        void init(const Block & header, const ColumnsMask & mask, ssize_t result_size_hint);
    };

    struct OutputData
    {
        OutputData(AggregatingTransformParams & params_, AggregatedDataVariants & variants_, bool group_by_key_)
            : params(params_), variants(variants_), group_by_key(group_by_key_)
        {
        }

        AggregatingTransformParams & params;
        AggregatedDataVariants & variants;
        bool group_by_key;

        MutableColumns res_key_columns;
        MutableColumns res_aggregate_columns;

        size_t num_rows = 0;
        size_t num_bytes = 0;
        bool need_generate = false;

        void init(const Block & header, InputData & input);

        void addToAggregateColumns();
        void createNewState(InputData & input, size_t key_index);
        void executeAggregation(InputData & input, size_t key_begin, size_t key_end, std::atomic_bool & is_cancelled);
    };

    void generate(OutputData & output);
    void consume(InputData & input, OutputData & output);

    bool isNewKey(const InputData & input, const OutputData & output);
    ssize_t estimateCardinality(size_t num_rows) const;

    size_t max_block_size;
    size_t max_block_bytes;

    AggregatingTransformParamsPtr aggregate_params;
    ColumnsMask aggregates_mask;

    /// For sortBlock()
    SortDescription sort_description;
    SortDescriptionWithPositions group_by_description;
    bool group_by_key = false;

    std::optional<InputData> current_input;
    std::optional<OutputData> current_output;

    ManyAggregatedDataPtr many_data;
    AggregatedDataVariants & aggregate_variants;

    UInt64 src_rows = 0;
    UInt64 src_bytes = 0;
    UInt64 res_rows = 0;

    Block res_header;
    Chunk to_push_chunk;

    bool is_consume_started = false;
    LoggerPtr log = getLogger("AggregatingInOrderTransform");
};


class FinalizeAggregatedTransform : public ISimpleTransform
{
public:
    FinalizeAggregatedTransform(Block header, AggregatingTransformParamsPtr params_);

    void transform(Chunk & chunk) override;
    String getName() const override { return "FinalizeAggregatedTransform"; }

private:
    AggregatingTransformParamsPtr params;
    ColumnsMask aggregates_mask;
};


}
