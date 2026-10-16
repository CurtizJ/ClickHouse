#include <Functions/hasAnyAllTokens.h>

#include <Columns/ColumnFixedString.h>
#include <Columns/ColumnNullable.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnNothing.h>
#include <Common/FunctionDocumentation.h>
#include <Common/VectorWithMemoryTracking.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <Core/Settings.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/DataTypeNullable.h>
#include <Functions/FunctionFactory.h>
#include <Functions/FunctionHelpers.h>
#include <Functions/TokenSearchArgumentTypes.h>
#include <Interpreters/Context.h>
#include <Interpreters/ITokenizer.h>
#include <Interpreters/TokenizerFactory.h>

#include <absl/container/flat_hash_map.h>
#include <boost/dynamic_bitset.hpp>

namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}

namespace
{

constexpr size_t arg_input = 0;
constexpr size_t arg_needles = 1;
/// The minimum number of matching tokens of `hasAnyTokens`, if the third argument is an integer.
constexpr size_t arg_min_matches = 2;

/// Whether the third argument is the minimum number of matching tokens rather than the tokenizer.
template <class HasTokensTraits>
bool hasMinMatchesArgument(const ColumnsWithTypeAndName & arguments)
{
    return HasTokensTraits::mode == HasAnyAllTokensMode::Any && arguments.size() > arg_min_matches
        && isMinMatchesArgumentType(*arguments[arg_min_matches].type);
}

/// Compaction drops tokens covered by longer ones; valid only for `hasAllTokens`.
TokensWithPosition initializeSearchTokens(const ColumnsWithTypeAndName & arguments, const ITokenizer & tokenizer, std::string_view function_name, bool compact_tokens)
{
    if (arguments.size() < 2)
        return {};

    auto column_needles = arguments[arg_needles].column;
    if (!column_needles)
        return {};

    /// At plan time, constant columns coming from ActionsDAG nodes are normalized to size 0
    /// (see ActionsDAG::addColumn). For non-const columns we can only extract a value when
    /// there's at least one row.
    if (!isColumnConst(*column_needles) && column_needles->empty())
        return {};

    Field needles_field = (*column_needles)[0];
    if (needles_field.isNull())
        return {};

    TokensWithPosition search_tokens;
    VectorWithMemoryTracking<String> tokens_array;

    if (needles_field.getType() == Field::Types::String)
    {
        auto tokens_str = needles_field.safeGet<String>();
        tokenizer.stringToTokens(tokens_str.data(), tokens_str.size(), tokens_array);
        if (compact_tokens)
            tokens_array = tokenizer.compactTokens(tokens_array);
    }
    else if (needles_field.getType() == Field::Types::Array)
    {
        Array array_data = needles_field.safeGet<Array>();

        for (const auto & element : array_data)
        {
            if (element.getType() != Field::Types::String)
                throw Exception(ErrorCodes::BAD_ARGUMENTS, "Needles argument for function '{}' has unsupported type of column '{}'", function_name, column_needles->getName());

            tokens_array.emplace_back(element.safeGet<String>());
        }
    }
    else
    {
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Needles argument for function '{}' has unsupported type of column '{}'", function_name, column_needles->getName());
    }

    size_t pos = 0;
    for (const auto & token : tokens_array)
    {
        if (search_tokens.emplace(token, pos).second)
            ++pos;
    }
    return search_tokens;
}
}

template <class HasTokensTraits>
FunctionHasAnyAllTokensOverloadResolver<HasTokensTraits>::FunctionHasAnyAllTokensOverloadResolver(ContextPtr)
{
}

template <class HasTokensTraits>
DataTypePtr FunctionHasAnyAllTokensOverloadResolver<HasTokensTraits>::getReturnTypeImpl(const ColumnsWithTypeAndName & arguments) const
{
    FunctionArgumentDescriptors mandatory_args
    {
        {"input", static_cast<FunctionArgumentDescriptor::TypeValidator>(&isStringOrFixedStringOrArrayOfStringOrFixedString), nullptr, "String, FixedString, Array(String) or Array(FixedString)"},
        {"needles", static_cast<FunctionArgumentDescriptor::TypeValidator>(&isStringOrArrayOfStringType), isColumnConst, "const String or const Array(String)"}
    };

    if (hasMinMatchesArgument<HasTokensTraits>(arguments))
        mandatory_args.push_back({"minimum_should_match", static_cast<FunctionArgumentDescriptor::TypeValidator>(&isMinMatchesArgumentType), isColumnConst, "const integer"});

    FunctionArgumentDescriptors optional_args
    {
        {"tokenizer", static_cast<FunctionArgumentDescriptor::TypeValidator>(&isString), isColumnConst, "const String"}
    };

    validateFunctionArguments(name, arguments, mandatory_args, optional_args);

    DataTypePtr return_type = std::make_shared<DataTypeNumber<UInt8>>();

    /// Propagate nullability: if the input column is Nullable, the result is Nullable(UInt8).
    if (arguments[arg_input].type->isNullable())
        return_type = std::make_shared<DataTypeNullable>(std::make_shared<DataTypeNumber<UInt8>>());

    return return_type;
}

template <class HasTokensTraits>
FunctionBasePtr FunctionHasAnyAllTokensOverloadResolver<HasTokensTraits>::buildImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr & return_type) const
{
    const bool has_min_matches = hasMinMatchesArgument<HasTokensTraits>(arguments);
    const size_t arg_tokenizer = has_min_matches ? arg_min_matches + 1 : arg_min_matches;

    const auto tokenizer_name = arguments.size() <= arg_tokenizer || !arguments[arg_tokenizer].column
        ? SplitByNonAlphaTokenizer::getExternalName()
        : arguments[arg_tokenizer].column->getDataAt(0);

    size_t min_matches = 1;
    if (has_min_matches && arguments[arg_min_matches].column)
    {
        const auto & column = *arguments[arg_min_matches].column;
        if (isUInt(arguments[arg_min_matches].type) ? column.getUInt(0) == 0 : column.getInt(0) < 1)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Argument 'minimum_should_match' of function '{}' must be a positive integer", getName());
        min_matches = column.getUInt(0);
    }

    auto tokenizer = TokenizerFactory::instance().get(tokenizer_name);
    auto search_tokens = initializeSearchTokens(arguments, *tokenizer, getName(), HasTokensTraits::mode == HasAnyAllTokensMode::All);
    DataTypes argument_types{std::from_range_t{}, arguments | std::views::transform([](auto & elem) { return elem.type; })};
    return std::make_shared<FunctionBaseHasAnyAllTokens<HasTokensTraits>>(
        std::move(tokenizer), std::move(search_tokens), min_matches, std::move(argument_types), return_type);
}

template <class HasTokensTraits>
ExecutableFunctionPtr FunctionBaseHasAnyAllTokens<HasTokensTraits>::prepare(const ColumnsWithTypeAndName &) const
{
    return std::make_unique<ExecutableFunctionHasAnyAllTokens<HasTokensTraits>>(tokenizer, search_tokens, min_matches);
}

namespace
{

struct HasAnyTokensMatcher
{
    explicit HasAnyTokensMatcher(const TokensWithPosition & tokens_)
        : tokens(tokens_)
    {
    }

    template <typename OnMatchCallback>
    auto operator()(OnMatchCallback && onMatchCallback)
    {
        return [&](const char * token_start, size_t token_len)
        {
            if (tokens.contains(std::string_view(token_start, token_len)))
            {
                onMatchCallback();
                return true;
            }

            return false;
        };
    }

    void reset() { /* nothing to reset */ }

private:
    const TokensWithPosition & tokens;
};

/// Matches when `required_tokens` distinct tokens are found: all of them for `hasAllTokens`,
/// at least `min_matches` of them for `hasAnyTokens`.
struct HasAllTokensMatcher
{
    HasAllTokensMatcher(const TokensWithPosition & tokens_, size_t required_tokens_)
        : tokens(tokens_)
        , required_tokens(required_tokens_)
        , mask(tokens.size())
        , num_set_bits(0)
    {
    }

    template <typename OnMatchCallback>
    auto operator()(OnMatchCallback && onMatchCallback)
    {
        return [&](const char * token_start, size_t token_len)
        {
            if (auto it = tokens.find(std::string_view(token_start, token_len)); it != tokens.end())
            {
                num_set_bits += !mask.test_set(it->second);

                if (num_set_bits == required_tokens)
                {
                    onMatchCallback();
                    return true;
                }
            }

            return false;
        };
    }

    void reset()
    {
        mask.reset();
        num_set_bits = 0;
    }

private:
    const TokensWithPosition & tokens;
    size_t required_tokens;
    boost::dynamic_bitset<> mask;
    UInt64 num_set_bits;
};

template <typename T>
concept StringColumnType = std::same_as<T, ColumnString> || std::same_as<T, ColumnFixedString>;
using ArrayOffset = ColumnArray::Offset;

template <typename Matcher>
concept MatcherType = std::same_as<Matcher, HasAnyTokensMatcher> || std::same_as<Matcher, HasAllTokensMatcher>;

/// Execute on Array(String) or Array(FixedString) column
void searchOnArray(
    const ColumnArray::Offsets & offsets,
    const StringColumnType auto & input_string,
    const ColumnNullable * null_map,
    PaddedPODArray<UInt8> & col_result,
    size_t input_rows_count,
    const ITokenizer * tokenizer,
    MatcherType auto matcher)
{
    ArrayOffset current_offset = 0;
    for (size_t i = 0; i < input_rows_count; ++i)
    {
        const ArrayOffset array_size = offsets[i] - current_offset;
        col_result[i] = false;
        matcher.reset();

        for (size_t j = 0; j < array_size; ++j)
        {
            const size_t element_idx = current_offset + j;

            if (null_map && null_map->isNullAt(element_idx))
                continue;

            std::string_view input = input_string.getDataAt(element_idx);

            forEachToken(*tokenizer, input.data(), input.size(), matcher([&] { col_result[i] = true; }));

            if (col_result[i])
                break;
        }

        current_offset = offsets[i];
    }
}

/// Execute on String column
void searchOnString(
    const StringColumnType auto & col_input,
    PaddedPODArray<UInt8> & col_result,
    size_t input_rows_count,
    const ITokenizer * tokenizer,
    MatcherType auto matcher)
{
    for (size_t i = 0; i < input_rows_count; ++i)
    {
        std::string_view input = col_input.getDataAt(i);
        col_result[i] = false;
        matcher.reset();

        forEachToken(*tokenizer, input.data(), input.size(), matcher([&] { col_result[i] = true; }));
    }
}

template <class HasTokensTraits>
void executeString(
    const StringColumnType auto & col_input,
    PaddedPODArray<UInt8> & col_result,
    size_t input_rows_count,
    const ITokenizer * tokenizer,
    const TokensWithPosition & tokens,
    size_t min_matches)
{
    if (tokens.empty())
    {
        /// if no search tokens we explicitly return no matches to avoid potential undefined behavior in HasAllTokensMatcher
        col_result.assign(input_rows_count, UInt8(0));
        return;
    }

    col_result.resize(input_rows_count);

    if constexpr (HasTokensTraits::mode == HasAnyAllTokensMode::Any)
    {
        if (min_matches > 1)
            searchOnString(col_input, col_result, input_rows_count, tokenizer, HasAllTokensMatcher(tokens, min_matches));
        else
            searchOnString(col_input, col_result, input_rows_count, tokenizer, HasAnyTokensMatcher(tokens));
    }
    else if constexpr (HasTokensTraits::mode == HasAnyAllTokensMode::All)
        searchOnString(col_input, col_result, input_rows_count, tokenizer, HasAllTokensMatcher(tokens, tokens.size()));
    else
        static_assert(false, "Unknown search mode value detected");
}

template <class HasTokensTraits>
void executeArray(
    const ColumnArray * array,
    const StringColumnType auto & input_string,
    const ColumnNullable * null_map,
    PaddedPODArray<UInt8> & col_result,
    const ITokenizer * tokenizer,
    const TokensWithPosition & tokens,
    size_t min_matches)
{
    const auto & offsets = array->getOffsets();
    const size_t input_size = offsets.size();

    if (tokens.empty())
    {
        /// if no search tokens we explicitly return no matches to avoid potential undefined behavior in HasAllTokensMatcher
        col_result.assign(input_size, UInt8(0));
        return;
    }

    col_result.resize(input_size);

    if constexpr (HasTokensTraits::mode == HasAnyAllTokensMode::Any)
    {
        if (min_matches > 1)
            searchOnArray(offsets, input_string, null_map, col_result, input_size, tokenizer, HasAllTokensMatcher(tokens, min_matches));
        else
            searchOnArray(offsets, input_string, null_map, col_result, input_size, tokenizer, HasAnyTokensMatcher(tokens));
    }
    else if constexpr (HasTokensTraits::mode == HasAnyAllTokensMode::All)
        searchOnArray(offsets, input_string, null_map, col_result, input_size, tokenizer, HasAllTokensMatcher(tokens, tokens.size()));
    else
        static_assert(false, "Unknown search mode value detected");
}

template <class HasTokensTraits>
void executeStringOrArray(
    ColumnPtr col_input,
    PaddedPODArray<UInt8> & col_result,
    size_t input_rows_count,
    const ITokenizer * tokenizer,
    const TokensWithPosition & tokens,
    size_t min_matches)
{
    if (const auto * col_input_string = checkAndGetColumn<ColumnString>(col_input.get()))
        executeString<HasTokensTraits>(*col_input_string, col_result, input_rows_count, tokenizer, tokens, min_matches);
    else if (const auto * col_input_fixedstring = checkAndGetColumn<ColumnFixedString>(col_input.get()))
        executeString<HasTokensTraits>(*col_input_fixedstring, col_result, input_rows_count, tokenizer, tokens, min_matches);
    else if (const auto * col_input_array = checkAndGetColumn<ColumnArray>(col_input.get()))
    {
         /// Array elements may be Nullable(String) or Nullable(FixedString); unwrap and pass the null map.
        const IColumn * data_col = &col_input_array->getData();
        const ColumnNullable * null_map = checkAndGetColumn<ColumnNullable>(data_col);
        const IColumn * actual_data = null_map ? &null_map->getNestedColumn() : data_col;

        if (const auto * input_string = checkAndGetColumn<ColumnString>(actual_data))
            executeArray<HasTokensTraits>(col_input_array, *input_string, null_map, col_result, tokenizer, tokens, min_matches);
        else if (const auto * input_fixedstring = checkAndGetColumn<ColumnFixedString>(actual_data))
            executeArray<HasTokensTraits>(col_input_array, *input_fixedstring, null_map, col_result, tokenizer, tokens, min_matches);
        else
            /// `Array(Nothing)`, the type of `[]`: rows without tokens.
            col_result.assign(input_rows_count, UInt8(0));
    }
}

}

template <class HasTokensTraits>
ColumnPtr ExecutableFunctionHasAnyAllTokens<HasTokensTraits>::executeImpl(
    const ColumnsWithTypeAndName & arguments, const DataTypePtr &, size_t input_rows_count) const
{
    if (input_rows_count == 0)
        return ColumnVector<UInt8>::create();

    auto col_result = ColumnVector<UInt8>::create();

    /// No row has more distinct tokens than searched.
    if (search_tokens.empty() || min_matches > search_tokens.size())
    {
        col_result->getData().assign(input_rows_count, UInt8(0));
        return col_result;
    }

    ColumnPtr col_input = arguments[arg_input].column;

    if (tokenizer->isStateful())
    {
        /// Stateful tokenizers mutate internal state during execution and cannot be shared across
        /// threads; use a per-execution clone.
        auto stateful_tokenizer = tokenizer->clone();
        executeStringOrArray<HasTokensTraits>(col_input, col_result->getData(), input_rows_count, stateful_tokenizer.get(), search_tokens, min_matches);
    }
    else
    {
        executeStringOrArray<HasTokensTraits>(col_input, col_result->getData(), input_rows_count, tokenizer.get(), search_tokens, min_matches);
    }

    return col_result;
}

template class ExecutableFunctionHasAnyAllTokens<HasAnyTokensTraits>;
template class ExecutableFunctionHasAnyAllTokens<HasAllTokensTraits>;

template class FunctionBaseHasAnyAllTokens<HasAnyTokensTraits>;
template class FunctionBaseHasAnyAllTokens<HasAllTokensTraits>;

REGISTER_FUNCTION(HasAnyTokens)
{
    FunctionDocumentation::Description description_hasAnyTokens = R"(
Returns 1, if at least one token in the `needle` string or array matches the `input` string, and 0 otherwise. If `input` is a column, returns all rows that satisfy this condition.
With the optional `minimum_should_match` argument, returns 1 only if at least `minimum_should_match` distinct tokens of `needles` match the `input` string.

<Note>
Column `input` should have a [text index](/reference/engines/table-engines/mergetree-family/textindexes) defined for optimal performance.
If no text index is defined, the function performs a brute-force column scan which is orders of magnitude slower than an index lookup.
</Note>

Prior to searching, the function tokenizes
- the `input` argument (always), and
- the `needle` argument (if given as a [String](/reference/data-types/string))
using the tokenizer specified for the text index.
If the column has no text index defined, the `splitByNonAlpha` tokenizer is used instead.
If the `needle` argument is of type [Array(String)](/reference/data-types/array), each array element is treated as a token — no additional tokenization takes place.
If the text index has a [preprocessor](/reference/engines/table-engines/mergetree-family/textindexes#preprocessor-argument-optional) expression configured, the preprocessor is applied to the needle (if given as a `String`) before tokenization.
If the text index has a [postprocessor](/reference/engines/table-engines/mergetree-family/textindexes#postprocessor-argument-optional) expression configured, the postprocessor is applied to needle tokens and the input tokens (i.e. both after tokenization).

Duplicate tokens are ignored.
For example, ['ClickHouse', 'ClickHouse'] is treated the same as ['ClickHouse'].

<Note>
When a text index defines a [preprocessor](/reference/engines/table-engines/mergetree-family/textindexes#creating-a-text-index) (for example `lowerUTF8`), `hasAnyTokens` applies it to `input` and, when `needles` is a [String](/reference/data-types/string), to `needles` before tokenization. When `needles` is an [Array(String)](/reference/data-types/array), its elements are passed through as-is and the preprocessor is not applied to them.
The preprocessor is part of the index definition, so it is applied wherever the function is evaluated: in the `WHERE`/`PREWHERE` filter, in the `SELECT` list, and independently of whether the index is read (e.g. `SETTINGS use_skip_indexes = 0`).
</Note>
    )";
    FunctionDocumentation::Syntax syntax_hasAnyTokens = R"(
hasAnyTokens(input, needles[, minimum_should_match][, tokenizer])
)";
    FunctionDocumentation::Arguments arguments_hasAnyTokens = {
        {"input", "The input column.", {"String", "FixedString", "Nullable(String)", "Nullable(FixedString)", "Array(String)", "Array(FixedString)", "Array(Nullable(String))", "Array(Nullable(FixedString))"}},
        {"needles", "Tokens to be searched.", {"String", "Array(String)"}},
        {"minimum_should_match", "The minimum number of distinct tokens of `needles` that must match. Optional, if not set explicitly, defaults to `1`.", {"const (U)Int*"}},
        {"tokenizer", "The tokenizer to use. Valid arguments are `splitByNonAlpha`, `splitByString`, `splitByRegexp`, `asciiCJK`, `chinese`, `icu('<locale>')`, `japanese`, `ngrams`, `sparseGrams`, and `array`. Optional, if not set explicitly, defaults to `splitByNonAlpha`.", {"const String"}},
    };
    FunctionDocumentation::ReturnedValue returned_value_hasAnyTokens = {"Returns `1`, if there were at least `minimum_should_match` matches (one by default). `0`, otherwise.", {"UInt8"}};
    FunctionDocumentation::Examples examples_hasAnyTokens = {
    {
        "Basic usage with a string needle",
        R"(
DROP TABLE IF EXISTS doc;

CREATE TABLE doc (
    id UInt32,
    msg String,
    INDEX idx(msg) TYPE text(tokenizer = splitByString(['()', '\\']))
)
ENGINE = MergeTree
ORDER BY id;

INSERT INTO doc VALUES (1, '()a,\\bc()d'), (2, '()\\a()bc\\d'), (3, ',()a\\,bc,(),d,');

SELECT count() FROM doc WHERE hasAnyTokens(msg, 'a\\d()');
        )",
        R"(
┌─count()─┐
│       3 │
└─────────┘
        )"
    },
    {
        "Specify needles to be searched for AS-IS (no tokenization) in an array",
        R"(
DROP TABLE IF EXISTS doc;

CREATE TABLE doc (
    id UInt32,
    msg String,
    INDEX idx(msg) TYPE text(tokenizer = splitByString(['()', '\\']))
)
ENGINE = MergeTree
ORDER BY id;

INSERT INTO doc VALUES (1, '()a,\\bc()d'), (2, '()\\a()bc\\d'), (3, ',()a\\,bc,(),d,');

SELECT count() FROM doc WHERE hasAnyTokens(msg, ['a', 'd']);
        )",
        R"(
┌─count()─┐
│       3 │
└─────────┘
        )"
    },
    {
        "Generate needles using the `tokens` function",
        R"(
DROP TABLE IF EXISTS doc;

CREATE TABLE doc (
    id UInt32,
    msg String,
    INDEX idx(msg) TYPE text(tokenizer = splitByString(['()', '\\']))
)
ENGINE = MergeTree
ORDER BY id;

INSERT INTO doc VALUES (1, '()a,\\bc()d'), (2, '()\\a()bc\\d'), (3, ',()a\\,bc,(),d,');

SELECT count() FROM doc WHERE hasAnyTokens(msg, tokens('a()d', 'splitByString', ['()', '\\']));
        )",
        R"(
┌─count()─┐
│       3 │
└─────────┘
        )"
    },
    {
        "Require at least two of the tokens",
        R"(
SELECT hasAnyTokens('failed to charge card', ['error', 'failed', 'charge', 'cache'], 2) AS res;
        )",
        R"(
┌─res─┐
│   1 │
└─────┘
        )"
    },
    {
        "Usage examples for array and map columns",
        R"(
DROP TABLE IF EXISTS log;

CREATE TABLE log (
    id UInt32,
    tags Array(String),
    attributes Map(String, String),
    INDEX idx_tags (tags) TYPE text(tokenizer = splitByNonAlpha),
    INDEX idx_attributes_keys mapKeys(attributes) TYPE text(tokenizer = array),
    INDEX idx_attributes_vals mapValues(attributes) TYPE text(tokenizer = array)
)
ENGINE = MergeTree
ORDER BY id;

INSERT INTO log VALUES
    (1, ['clickhouse', 'clickhouse cloud'], {'address': '192.0.0.1', 'log_level': 'INFO'}),
    (2, ['chdb'], {'embedded': 'true', 'log_level': 'DEBUG'});
        )",
        ""
    },
    {
        "Example with an array column",
        R"(
DROP TABLE IF EXISTS log;

CREATE TABLE log (
    id UInt32,
    tags Array(String),
    attributes Map(String, String),
    INDEX idx_tags (tags) TYPE text(tokenizer = splitByNonAlpha),
    INDEX idx_attributes_keys mapKeys(attributes) TYPE text(tokenizer = array),
    INDEX idx_attributes_vals mapValues(attributes) TYPE text(tokenizer = array)
)
ENGINE = MergeTree
ORDER BY id;

INSERT INTO log VALUES
    (1, ['clickhouse', 'clickhouse cloud'], {'address': '192.0.0.1', 'log_level': 'INFO'}),
    (2, ['chdb'], {'embedded': 'true', 'log_level': 'DEBUG'});

SELECT count() FROM log WHERE hasAnyTokens(tags, 'clickhouse');
        )",
        R"(
┌─count()─┐
│       1 │
└─────────┘
        )"
    },
    {
        "Example with mapKeys",
        R"(
DROP TABLE IF EXISTS log;

CREATE TABLE log (
    id UInt32,
    tags Array(String),
    attributes Map(String, String),
    INDEX idx_tags (tags) TYPE text(tokenizer = splitByNonAlpha),
    INDEX idx_attributes_keys mapKeys(attributes) TYPE text(tokenizer = array),
    INDEX idx_attributes_vals mapValues(attributes) TYPE text(tokenizer = array)
)
ENGINE = MergeTree
ORDER BY id;

INSERT INTO log VALUES
    (1, ['clickhouse', 'clickhouse cloud'], {'address': '192.0.0.1', 'log_level': 'INFO'}),
    (2, ['chdb'], {'embedded': 'true', 'log_level': 'DEBUG'});

SELECT count() FROM log WHERE hasAnyTokens(mapKeys(attributes), ['address', 'log_level']);
        )",
        R"(
┌─count()─┐
│       2 │
└─────────┘
        )"
    },
    {
        "Example with mapValues",
        R"(
DROP TABLE IF EXISTS log;

CREATE TABLE log (
    id UInt32,
    tags Array(String),
    attributes Map(String, String),
    INDEX idx_tags (tags) TYPE text(tokenizer = splitByNonAlpha),
    INDEX idx_attributes_keys mapKeys(attributes) TYPE text(tokenizer = array),
    INDEX idx_attributes_vals mapValues(attributes) TYPE text(tokenizer = array)
)
ENGINE = MergeTree
ORDER BY id;

INSERT INTO log VALUES
    (1, ['clickhouse', 'clickhouse cloud'], {'address': '192.0.0.1', 'log_level': 'INFO'}),
    (2, ['chdb'], {'embedded': 'true', 'log_level': 'DEBUG'});

SELECT count() FROM log WHERE hasAnyTokens(mapValues(attributes), ['192.0.0.1', 'DEBUG']);
        )",
        R"(
┌─count()─┐
│       2 │
└─────────┘
        )"
    }
    };
    FunctionDocumentation::IntroducedIn introduced_in_hasAnyTokens = {25, 10};
    FunctionDocumentation::Category category_hasAnyTokens = FunctionDocumentation::Category::StringSearch;
    FunctionDocumentation documentation_hasAnyTokens = {description_hasAnyTokens, syntax_hasAnyTokens, arguments_hasAnyTokens, {}, returned_value_hasAnyTokens, examples_hasAnyTokens, introduced_in_hasAnyTokens, category_hasAnyTokens};

    factory.registerFunction<FunctionHasAnyAllTokensOverloadResolver<HasAnyTokensTraits>>(documentation_hasAnyTokens);
    factory.registerAlias("hasAnyToken", HasAnyTokensTraits::name);
}

REGISTER_FUNCTION(HasAllTokens)
{
    FunctionDocumentation::Description description_hasAllTokens = R"(
Like [`hasAnyTokens`](#hasAnyTokens), but returns 1, if all tokens in the `needle` string or array match the `input` string, and 0 otherwise. If `input` is a column, returns all rows that satisfy this condition.

<Note>
Column `input` should have a [text index](/reference/engines/table-engines/mergetree-family/textindexes) defined for optimal performance.
If no text index is defined, the function performs a brute-force column scan which is orders of magnitude slower than an index lookup.
</Note>

Prior to searching, the function tokenizes
- the `input` argument (always), and
- the `needle` argument (if given as a [String](/reference/data-types/string))
using the tokenizer specified for the text index.
If the column has no text index defined, the `splitByNonAlpha` tokenizer is used instead.
If the `needle` argument is of type [Array(String)](/reference/data-types/array), each array element is treated as a token — no additional tokenization takes place.
If the text index has a [preprocessor](/reference/engines/table-engines/mergetree-family/textindexes#preprocessor-argument-optional) expression configured, the preprocessor is applied to the needle (if given as a `String`) before tokenization.
If the text index has a [postprocessor](/reference/engines/table-engines/mergetree-family/textindexes#postprocessor-argument-optional) expression configured, the postprocessor is applied to needle tokens and the input tokens (i.e. both after tokenization).

Duplicate tokens are ignored.
For example, needles = ['ClickHouse', 'ClickHouse'] is treated the same as ['ClickHouse'].

<Note>
When a text index defines a [preprocessor](/reference/engines/table-engines/mergetree-family/textindexes#creating-a-text-index) (for example `lowerUTF8`), `hasAllTokens` applies it to `input` and, when `needles` is a [String](/reference/data-types/string), to `needles` before tokenization. When `needles` is an [Array(String)](/reference/data-types/array), its elements are passed through as-is and the preprocessor is not applied to them.
The preprocessor is part of the index definition, so it is applied wherever the function is evaluated: in the `WHERE`/`PREWHERE` filter, in the `SELECT` list, and independently of whether the index is read (e.g. `SETTINGS use_skip_indexes = 0`).
</Note>
    )";
    FunctionDocumentation::Syntax syntax_hasAllTokens = R"(
hasAllTokens(input, needles[, tokenizer])
)";
    FunctionDocumentation::Arguments arguments_hasAllTokens = {
        {"input", "The input column.", {"String", "FixedString", "Nullable(String)", "Nullable(FixedString)", "Array(String)", "Array(FixedString)", "Array(Nullable(String))", "Array(Nullable(FixedString))"}},
        {"needles", "Tokens to be searched.", {"String", "Array(String)"}},
        {"tokenizer", "The tokenizer to use. Valid arguments are `splitByNonAlpha`, `splitByString`, `splitByRegexp`, `asciiCJK`, `chinese`, `icu('<locale>')`, `japanese`, `ngrams`, `sparseGrams`, and `array`. Optional, if not set explicitly, defaults to `splitByNonAlpha`.", {"const String"}},
    };
    FunctionDocumentation::ReturnedValue returned_value_hasAllTokens = {"Returns 1, if all needles match. 0, otherwise.", {"UInt8"}};
    FunctionDocumentation::Examples examples_hasAllTokens = {
    {
        "Basic usage with a string needle",
        R"(
DROP TABLE IF EXISTS doc;

CREATE TABLE doc (
    id UInt32,
    msg String,
    INDEX idx(msg) TYPE text(tokenizer = splitByString(['()', '\\']))
)
ENGINE = MergeTree
ORDER BY id;

INSERT INTO doc VALUES (1, '()a,\\bc()d'), (2, '()\\a()bc\\d'), (3, ',()a\\,bc,(),d,');

SELECT count() FROM doc WHERE hasAllTokens(msg, 'a\\d()');
        )",
        R"(
┌─count()─┐
│       1 │
└─────────┘
        )"
    },
    {
        "Specify needles to be searched for AS-IS (no tokenization) in an array",
        R"(
DROP TABLE IF EXISTS doc;

CREATE TABLE doc (
    id UInt32,
    msg String,
    INDEX idx(msg) TYPE text(tokenizer = splitByString(['()', '\\']))
)
ENGINE = MergeTree
ORDER BY id;

INSERT INTO doc VALUES (1, '()a,\\bc()d'), (2, '()\\a()bc\\d'), (3, ',()a\\,bc,(),d,');

SELECT count() FROM doc WHERE hasAllTokens(msg, ['a', 'd']);
        )",
        R"(
┌─count()─┐
│       1 │
└─────────┘
        )"
    },
    {
        "Generate needles using the `tokens` function",
        R"(
DROP TABLE IF EXISTS doc;

CREATE TABLE doc (
    id UInt32,
    msg String,
    INDEX idx(msg) TYPE text(tokenizer = splitByString(['()', '\\']))
)
ENGINE = MergeTree
ORDER BY id;

INSERT INTO doc VALUES (1, '()a,\\bc()d'), (2, '()\\a()bc\\d'), (3, ',()a\\,bc,(),d,');

SELECT count() FROM doc WHERE hasAllTokens(msg, tokens('a()d', 'splitByString', ['()', '\\']));
        )",
        R"(
┌─count()─┐
│       1 │
└─────────┘
        )"
    },
    {
        "Use a custom tokenizer via the 3rd argument",
        R"(
SELECT hasAllTokens('abcdef', 'abc', 'ngrams(3)');
        )",
        R"(
┌─hasAllTokens('abcdef', 'abc', 'ngrams(3)')─┐
│                                          1 │
└────────────────────────────────────────────┘
        )"
    },
    {
        "Usage examples for array and map columns",
        R"(
DROP TABLE IF EXISTS log;

CREATE TABLE log (
    id UInt32,
    tags Array(String),
    attributes Map(String, String),
    INDEX idx_tags (tags) TYPE text(tokenizer = splitByNonAlpha),
    INDEX idx_attributes_keys mapKeys(attributes) TYPE text(tokenizer = array),
    INDEX idx_attributes_vals mapValues(attributes) TYPE text(tokenizer = array)
)
ENGINE = MergeTree
ORDER BY id;

INSERT INTO log VALUES
    (1, ['clickhouse', 'clickhouse cloud'], {'address': '192.0.0.1', 'log_level': 'INFO'}),
    (2, ['chdb'], {'embedded': 'true', 'log_level': 'DEBUG'});
        )",
        ""
    },
    {
        "Example with an array column",
        R"(
DROP TABLE IF EXISTS log;

CREATE TABLE log (
    id UInt32,
    tags Array(String),
    attributes Map(String, String),
    INDEX idx_tags (tags) TYPE text(tokenizer = splitByNonAlpha),
    INDEX idx_attributes_keys mapKeys(attributes) TYPE text(tokenizer = array),
    INDEX idx_attributes_vals mapValues(attributes) TYPE text(tokenizer = array)
)
ENGINE = MergeTree
ORDER BY id;

INSERT INTO log VALUES
    (1, ['clickhouse', 'clickhouse cloud'], {'address': '192.0.0.1', 'log_level': 'INFO'}),
    (2, ['chdb'], {'embedded': 'true', 'log_level': 'DEBUG'});

SELECT count() FROM log WHERE hasAllTokens(tags, 'clickhouse');
        )",
        R"(
┌─count()─┐
│       1 │
└─────────┘
        )"
    },
    {
        "Example with mapKeys",
        R"(
DROP TABLE IF EXISTS log;

CREATE TABLE log (
    id UInt32,
    tags Array(String),
    attributes Map(String, String),
    INDEX idx_tags (tags) TYPE text(tokenizer = splitByNonAlpha),
    INDEX idx_attributes_keys mapKeys(attributes) TYPE text(tokenizer = array),
    INDEX idx_attributes_vals mapValues(attributes) TYPE text(tokenizer = array)
)
ENGINE = MergeTree
ORDER BY id;

INSERT INTO log VALUES
    (1, ['clickhouse', 'clickhouse cloud'], {'address': '192.0.0.1', 'log_level': 'INFO'}),
    (2, ['chdb'], {'embedded': 'true', 'log_level': 'DEBUG'});

SELECT count() FROM log WHERE hasAllTokens(mapKeys(attributes), ['address', 'log_level']);
        )",
        R"(
┌─count()─┐
│       1 │
└─────────┘
        )"
    },
    {
        "Example with mapValues",
        R"(
DROP TABLE IF EXISTS log;

CREATE TABLE log (
    id UInt32,
    tags Array(String),
    attributes Map(String, String),
    INDEX idx_tags (tags) TYPE text(tokenizer = splitByNonAlpha),
    INDEX idx_attributes_keys mapKeys(attributes) TYPE text(tokenizer = array),
    INDEX idx_attributes_vals mapValues(attributes) TYPE text(tokenizer = array)
)
ENGINE = MergeTree
ORDER BY id;

INSERT INTO log VALUES
    (1, ['clickhouse', 'clickhouse cloud'], {'address': '192.0.0.1', 'log_level': 'INFO'}),
    (2, ['chdb'], {'embedded': 'true', 'log_level': 'DEBUG'});

SELECT count() FROM log WHERE hasAllTokens(mapValues(attributes), ['192.0.0.1', 'DEBUG']);
        )",
        R"(
┌─count()─┐
│       0 │
└─────────┘
        )"
    }
    };
    FunctionDocumentation::IntroducedIn introduced_in_hasAllTokens = {25, 10};
    FunctionDocumentation::Category category_hasAllTokens = FunctionDocumentation::Category::StringSearch;
    FunctionDocumentation documentation_hasAllTokens = {description_hasAllTokens, syntax_hasAllTokens, arguments_hasAllTokens, {}, returned_value_hasAllTokens, examples_hasAllTokens, introduced_in_hasAllTokens, category_hasAllTokens};

    factory.registerFunction<FunctionHasAnyAllTokensOverloadResolver<HasAllTokensTraits>>(documentation_hasAllTokens);
    factory.registerAlias("hasAllToken", HasAllTokensTraits::name);
}
}
