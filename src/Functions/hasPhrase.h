#pragma once

#include <Functions/IFunction.h>
#include <Interpreters/Context_fwd.h>
#include <Common/VectorWithMemoryTracking.h>

namespace DB
{

struct ITokenizer;
class TextIndexTransforms;

class ExecutableFunctionHasPhrase final : public IExecutableFunction
{
public:
    static constexpr auto name = "hasPhrase";

    ExecutableFunctionHasPhrase(
        std::shared_ptr<const ITokenizer> tokenizer_,
        VectorWithMemoryTracking<String> phrase_tokens_,
        VectorWithMemoryTracking<size_t> failure_table_,
        std::shared_ptr<const TextIndexTransforms> transforms_)
        : tokenizer(std::move(tokenizer_))
        , phrase_tokens(std::move(phrase_tokens_))
        , failure_table(std::move(failure_table_))
        , transforms(std::move(transforms_))
    {
    }

    String getName() const override { return name; }
    bool useDefaultImplementationForConstants() const override { return true; }
    bool useDefaultImplementationForNulls() const override;
    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr & result_type, size_t input_rows_count) const override;

private:
    std::shared_ptr<const ITokenizer> tokenizer;
    VectorWithMemoryTracking<String> phrase_tokens;
    VectorWithMemoryTracking<size_t> failure_table;
    std::shared_ptr<const TextIndexTransforms> transforms;
};

class FunctionBaseHasPhrase final : public IFunctionBase
{
public:
    static constexpr auto name = "hasPhrase";

    FunctionBaseHasPhrase(
        std::shared_ptr<const ITokenizer> tokenizer_,
        VectorWithMemoryTracking<String> phrase_tokens_,
        std::shared_ptr<const TextIndexTransforms> transforms_,
        DataTypes argument_types_,
        DataTypePtr result_type_)
        : tokenizer(std::move(tokenizer_))
        , phrase_tokens(std::move(phrase_tokens_))
        , transforms(std::move(transforms_))
        , argument_types(std::move(argument_types_))
        , result_type(std::move(result_type_))
    {
    }

    String getName() const override { return name; }
    const DataTypes & getArgumentTypes() const override { return argument_types; }
    const DataTypePtr & getResultType() const override { return result_type; }
    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo &) const override { return true; }

    ExecutableFunctionPtr prepare(const ColumnsWithTypeAndName &) const override;

private:
    std::shared_ptr<const ITokenizer> tokenizer;
    VectorWithMemoryTracking<String> phrase_tokens;
    std::shared_ptr<const TextIndexTransforms> transforms;
    DataTypes argument_types;
    DataTypePtr result_type;
};

class FunctionHasPhraseOverloadResolver final : public IFunctionOverloadResolver
{
public:
    static constexpr auto name = "hasPhrase";

    static FunctionOverloadResolverPtr create(ContextPtr context)
    {
        return std::make_unique<FunctionHasPhraseOverloadResolver>(context);
    }

    explicit FunctionHasPhraseOverloadResolver(ContextPtr context_);

    String getName() const override { return name; }
    size_t getNumberOfArguments() const override { return 0; }
    bool isVariadic() const override { return true; }
    ColumnNumbers getArgumentsThatAreAlwaysConstant() const override { return {1, 2, 3, 4}; }

    DataTypePtr getReturnTypeImpl(const ColumnsWithTypeAndName & arguments) const override;
    FunctionBasePtr buildImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr & return_type) const override;

private:
    ContextPtr context;
};

}
