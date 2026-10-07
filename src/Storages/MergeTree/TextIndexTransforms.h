#pragma once

#include <Core/ColumnsWithTypeAndName.h>
#include <Interpreters/Context_fwd.h>
#include <Interpreters/ExpressionActions.h>
#include <Parsers/IAST_fwd.h>
#include <Common/VectorWithMemoryTracking.h>

#include <absl/container/flat_hash_set.h>

namespace DB
{

struct IndexDescription;
struct ITokenizer;

/// Drop set extracted from a filter-only `if(token IN/NOT IN (<string literals>), '', token)` postprocessor.
/// Lets the granule builder decide, once per distinct token, whether it is dropped - so dropped tokens never
/// build posting lists while each occurrence is still hashed only once.
struct MergeTreeIndexTextInlineFilter
{
    struct Hash { using is_transparent = void; size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); } };
    struct Eq   { using is_transparent = void; bool operator()(std::string_view a, std::string_view b) const { return a == b; } };
    absl::flat_hash_set<std::string, Hash, Eq> tokens;
    bool drop_on_match = true;
};

/// The preprocessor and the postprocessor of a text index: lambdas with one argument, e.g. `x -> lower(x)`. The
/// preprocessor maps every value (every element of an `Array`) before tokenization, the postprocessor every token
/// after it, dropping those it maps to an empty string. The index build, the index analysis and the text-search
/// functions, which take them as arguments after the tokenizer, all apply them here, so they agree on the tokens.
class TextIndexTransforms
{
public:
    /// The transforms of a text index, written as expressions over the index column.
    TextIndexTransforms(const ASTPtr & preprocessor_ast, const ASTPtr & postprocessor_ast, const IndexDescription & index);

    /// The transforms written as lambdas, empty for none, for values of `value_type`. The functions of the
    /// lambdas are resolved with `context`.
    TextIndexTransforms(const String & preprocessor, const String & postprocessor, const DataTypePtr & value_type, const ContextPtr & context);

    /// The transforms passed to a text-search function as its arguments from `first_argument`. Null if there are none.
    static std::shared_ptr<const TextIndexTransforms> createForFunction(
        const ColumnsWithTypeAndName & arguments, size_t first_argument, std::string_view function_name, const ContextPtr & context);

    bool hasPreprocessor() const { return preprocessor.has_value(); }
    bool hasPostprocessor() const { return postprocessor.has_value(); }

    /// The lambdas in the form the text-search functions take them. Empty if there is no such transform.
    const String & getSerializedPreprocessor() const { return serialized_preprocessor; }
    const String & getSerializedPostprocessor() const { return serialized_postprocessor; }

    /// A lambda written in a query in the form of `getSerializedPreprocessor`, to compare it regardless of
    /// e.g. the argument name. Empty for an empty transform.
    static String normalizeTransform(const String & transform, const String & transform_name);

    /// True only when the preprocessor is exactly ASCII `lower` or `upper` of the value, which maps every byte in place.
    bool isASCIILowerOrUpperPreprocessor() const { return is_ascii_lower_or_upper; }

    /// Non-null when the postprocessor is an `if`/`multiIf(token IN/NOT IN (<string literals>), '', token)` filter.
    const MergeTreeIndexTextInlineFilter * getInlineFilter() const { return inline_filter ? &*inline_filter : nullptr; }

    /// Applies the preprocessor to the rows [start_row, start_row + n_rows) of the column. Returns the result and
    /// the position of the first of these rows in it.
    std::pair<ColumnPtr, size_t> preprocessColumn(const ColumnPtr & column, size_t start_row, size_t n_rows) const;

    /// The tokens of the rows [start_row, start_row + n_rows) of a preprocessed column, postprocessed, as `Array(String)`.
    /// A dropped token is an empty string, a `NULL` value or element has no tokens.
    ColumnPtr tokenizeColumn(const ITokenizer & tokenizer, const IColumn & column, size_t start_row, size_t n_rows) const;

    String preprocessConstant(const String & value) const;

    /// Applies the postprocessor to the tokens and drops those it maps to an empty string.
    VectorWithMemoryTracking<String> processTokens(VectorWithMemoryTracking<String> tokens) const;

    /// The tokens of a needle in order: preprocessed, tokenized and postprocessed.
    VectorWithMemoryTracking<String> stringToTokens(std::string_view value, const ITokenizer & tokenizer) const;

    /// The input of a text-search function to search in: the preprocessed values, with a postprocessor tokenized by
    /// `tokenizeColumn` to search with the tokenizer from `getSearchTokenizer`. Sets `null_map` to the rows whose
    /// preprocessed value is `NULL`, which the function takes as they are if there is a preprocessor (e.g. `ifNull`).
    ColumnPtr transformFunctionInput(const ColumnPtr & column, const ITokenizer & tokenizer, ColumnPtr & null_map) const;
    const ITokenizer & getSearchTokenizer(const ITokenizer & tokenizer) const;

    /// Wraps the result of a text-search function into `Nullable` if the result type is.
    static ColumnPtr wrapFunctionResult(ColumnPtr result, ColumnPtr null_map, const DataTypePtr & result_type);

private:
    struct Transform
    {
        ExpressionActions actions;
        String input_name;
        DataTypePtr input_type;
    };

    static Transform buildTransform(const String & transform, const String & transform_name, const DataTypePtr & input_type, const ContextPtr & context);
    static ColumnPtr executeTransform(const Transform & transform, ColumnPtr column);

    /// The preprocessor of a value (of an element of an `Array`) and of a constant needle.
    std::optional<Transform> preprocessor;
    std::optional<Transform> preprocessor_for_constant;
    std::optional<Transform> postprocessor;

    String serialized_preprocessor;
    String serialized_postprocessor;
    bool is_ascii_lower_or_upper = false;
    std::optional<MergeTreeIndexTextInlineFilter> inline_filter;
};

using TextIndexTransformsPtr = std::shared_ptr<const TextIndexTransforms>;

}
