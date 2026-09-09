#include <Functions/FunctionTopKFilter.h>
#include <Columns/Collator.h>
#include <Columns/ColumnConst.h>
#include <Columns/ColumnSparse.h>
#include <Columns/ColumnsNumber.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeTuple.h>
#include <DataTypes/DataTypesNumber.h>
#include <Functions/FunctionFactory.h>
#include <Functions/FunctionHelpers.h>
#include <Functions/IFunction.h>
#include <Functions/IFunctionAdaptors.h>
#include <Interpreters/Context.h>
#include <Interpreters/convertFieldToType.h>
#include <Processors/TopKThresholdTracker.h>
#include <Common/logger_useful.h>

namespace DB
{

namespace
{

/// `Tuple` comparison functions reject an empty `Tuple` nested inside another `Tuple`,
/// while the column comparison path supports it. Other composite types have their
/// own comparison implementations, so only descend through `Tuple` and `Nullable`.
bool hasEmptyTuple(const DataTypePtr & type)
{
    const auto * nullable_type = typeid_cast<const DataTypeNullable *>(type.get());
    const auto & nested_type = nullable_type ? nullable_type->getNestedType() : type;
    const auto * tuple_type = typeid_cast<const DataTypeTuple *>(nested_type.get());
    if (!tuple_type)
        return false;

    if (tuple_type->getElements().empty())
        return true;

    for (const auto & element_type : tuple_type->getElements())
        if (hasEmptyTuple(element_type))
            return true;

    return false;
}

}

namespace ErrorCodes
{
extern const int LOGICAL_ERROR;
extern const int TOO_FEW_ARGUMENTS_FOR_FUNCTION;
}

class FunctionTopKFilter final : public IFunction
{
public:
    static constexpr auto name = "__topKFilter";

    explicit FunctionTopKFilter(TopKThresholdTrackerPtr threshold_tracker_)
        : threshold_tracker(threshold_tracker_)
    {
        if (!threshold_tracker_)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "FunctionTopKFilter got NULL threshold_tracker");

        direction = threshold_tracker_->getDirection();
        nulls_direction = threshold_tracker_->getNullsDirection();
        collator = threshold_tracker_->getCollator();

        if (!collator)
        {
            String comparator = "lessOrEquals";
            if (direction == -1)
                comparator = "greaterOrEquals";
            auto context = Context::getGlobalContextInstance();
            compare_function = FunctionFactory::instance().get(comparator, context);
        }
    }

    String getName() const override { return name; }

    bool isVariadic() const override { return false; }
    bool isInjective(const ColumnsWithTypeAndName &) const override { return false; }
    bool isSuitableForConstantFolding() const override { return false; }
    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return false; }
    bool isDeterministic() const override { return false; }
    bool isDeterministicInScopeOfQuery() const override { return false; }
    bool useDefaultImplementationForNulls() const override { return false; }
    /// Sparse columns are handled explicitly: see `executeSparse`. The default implementation
    /// expands the result to a full column whenever any explicit value fails the threshold
    /// (the common case), while `executeSparse` rebuilds the offsets and keeps the filter sparse.
    bool useDefaultImplementationForSparseColumns() const override { return false; }
    size_t getNumberOfArguments() const override { return 1; }

    DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
    {
        if (arguments.size() != 1)
            throw Exception(
                ErrorCodes::TOO_FEW_ARGUMENTS_FOR_FUNCTION,
                "Number of arguments for function {} can't be {}, should be 1",
                getName(),
                arguments.size());

        return std::make_shared<DataTypeUInt8>();
    }

    DataTypePtr getReturnTypeForDefaultImplementationForDynamic() const override { return std::make_shared<DataTypeUInt8>(); }

    bool useDefaultImplementationForConstants() const override { return true; }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr &, size_t input_rows_count) const override
    {
        if (input_rows_count == 0)
            return ColumnUInt8::create();

        if (threshold_tracker && threshold_tracker->isSet())
        {
            auto current_threshold = threshold_tracker->getValue();
            auto data_type = arguments[0].type;

            if (collator || data_type->isNullable() || isDynamic(data_type) || isVariant(data_type) || hasEmptyTuple(data_type))
            {
                auto argument = arguments[0];
                argument.column = argument.column->convertToFullColumnIfSparse();
                return executeGeneral(argument, current_threshold, data_type, input_rows_count);
            }

            if (const auto * sparse = checkAndGetColumn<ColumnSparse>(arguments[0].column.get()))
                return executeSparse(*sparse, arguments[0], current_threshold, data_type, input_rows_count);

            return executeVectorized(arguments[0], current_threshold, data_type, input_rows_count);
        }
        else
        {
            return DataTypeUInt8().createColumnConst(input_rows_count, true);
        }
    }

private:
    /// Fast path: vectorized less/greater for non-nullable, non-collation types.
    ColumnPtr executeVectorized(
        const ColumnWithTypeAndName & argument,
        const Field & current_threshold,
        const DataTypePtr & data_type,
        size_t input_rows_count) const
    {
        ColumnPtr threshold_column = data_type->createColumnConst(input_rows_count, convertFieldToType(current_threshold, *data_type));
        ColumnsWithTypeAndName args{argument, {threshold_column, data_type, {}}};
        auto elem_compare = compare_function->build(args);
        return elem_compare->execute(args, elem_compare->getResultType(), input_rows_count, false);
    }

    /// Sparse fast path: a sparse argument (e.g. the `_bm25_score` column produced by the text
    /// index reader) has explicit values only at a small subset of rows, the rest are implicit
    /// defaults. Compare only the explicit values against the threshold and rebuild the offsets
    /// with the passing rows, producing a canonical Sparse(UInt8) filter. Downstream this enables
    /// the sparse paths of the prewhere filtering (see `FilterWithCachedCount`).
    ColumnPtr executeSparse(
        const ColumnSparse & sparse,
        const ColumnWithTypeAndName & argument,
        const Field & current_threshold,
        const DataTypePtr & data_type,
        size_t input_rows_count) const
    {
        /// The first element of the values column is the implicit default of the remaining rows.
        ColumnWithTypeAndName values_arg{sparse.getValuesPtr(), data_type, argument.name};
        const size_t values_size = values_arg.column->size();
        auto res = executeVectorized(values_arg, current_threshold, data_type, values_size);

        if (isColumnConst(*res))
            return res->cloneResized(input_rows_count);

        const auto * res_uint8 = checkAndGetColumn<ColumnUInt8>(res.get());
        const auto & offsets_data = sparse.getOffsetsData();

        /// If the default value passes the threshold, the rows absent from the offsets pass too,
        /// so the result cannot be represented as a sparse filter. Expand to a full column
        /// (matching what the default implementation for sparse columns does).
        if (!res_uint8 || res_uint8->getData()[0])
            return res->createWithOffsets(offsets_data, *createColumnConst(res, 0), input_rows_count, /*shift=*/ 1);

        const auto & res_data = res_uint8->getData();

        auto new_offsets = ColumnUInt64::create();
        auto & new_offsets_data = new_offsets->getData();
        for (size_t i = 1; i < values_size; ++i)
            if (res_data[i])
                new_offsets_data.push_back(offsets_data[i - 1]);

        auto new_values = ColumnUInt8::create();
        auto & new_values_data = new_values->getData();
        new_values_data.resize_fill(new_offsets_data.size() + 1, 1);
        new_values_data[0] = 0;

        MutableColumnPtr new_values_ptr = std::move(new_values);
        MutableColumnPtr new_offsets_ptr = std::move(new_offsets);
        return ColumnSparse::create(std::move(new_values_ptr), std::move(new_offsets_ptr), input_rows_count);
    }

    /// General path for `Nullable`, collation-aware, and non-vectorizable `Tuple` types.
    ColumnPtr executeGeneral(
        const ColumnWithTypeAndName & argument,
        const Field & current_threshold,
        const DataTypePtr & data_type,
        size_t input_rows_count) const
    {
        const auto & col = *argument.column;

        auto threshold_field = convertFieldToType(current_threshold, *data_type);
        auto threshold_col = data_type->createColumn();
        threshold_col->insert(threshold_field);

        PaddedPODArray<Int8> compare_results;

        if (collator)
        {
            compare_results.resize(input_rows_count);
            for (size_t i = 0; i < input_rows_count; ++i)
            {
                int cmp = col.compareAtWithCollation(i, 0, *threshold_col, nulls_direction, *collator);
                compare_results[i] = static_cast<Int8>(direction * cmp);
            }
        }
        else
        {
            col.compareColumn(*threshold_col, 0, nullptr, compare_results, direction, nulls_direction);
        }

        auto result_col = ColumnUInt8::create(input_rows_count);
        auto & result_data = result_col->getData();
        for (size_t i = 0; i < input_rows_count; ++i)
            result_data[i] = compare_results[i] <= 0;

        return result_col;
    }

    TopKThresholdTrackerPtr threshold_tracker;
    FunctionOverloadResolverPtr compare_function;

    int direction;
    int nulls_direction;
    std::shared_ptr<Collator> collator;
};

FunctionOverloadResolverPtr createInternalFunctionTopKFilterResolver(TopKThresholdTrackerPtr threshold_tracker_)
{
    return std::make_shared<FunctionToOverloadResolverAdaptor>(std::make_shared<FunctionTopKFilter>(threshold_tracker_));
}

}
