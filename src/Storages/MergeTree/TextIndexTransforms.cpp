#include <Storages/MergeTree/TextIndexTransforms.h>

#include <Analyzer/AggregationUtils.h>
#include <Analyzer/QueryTreeBuilder.h>
#include <Analyzer/Resolve/QueryAnalyzer.h>
#include <Analyzer/TableNode.h>
#include <Analyzer/WindowFunctionsUtils.h>
#include <Columns/ColumnArray.h>
#include <Columns/ColumnNullable.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnsNumber.h>
#include <Core/Block.h>
#include <Core/Defines.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeString.h>
#include <Functions/FunctionFactory.h>
#include <Interpreters/Context.h>
#include <Interpreters/ITokenizer.h>
#include <Interpreters/misc.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTSubquery.h>
#include <Parsers/ExpressionListParsers.h>
#include <Parsers/parseQuery.h>
#include <Planner/CollectTableExpressionData.h>
#include <Planner/PlannerContext.h>
#include <Planner/Utils.h>
#include <Storages/IndicesDescription.h>
#include <Storages/StorageDummy.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int ILLEGAL_COLUMN;
    extern const int INCORRECT_QUERY;
}

namespace
{

/// The placeholder for the value in a transform written over the index column.
constexpr char placeholder_name[] = "__text_index_value";

/// Bounds the per-granule token map state a filter can create; larger sets use the general vectorized path.
constexpr size_t max_filter_set_size = 8192;

const ArrayTokenizer array_tokenizer;

/// Replaces all AST subtrees whose canonical name equals `expression_name` with a plain identifier `identifier_name`.
void replaceExpressionToIdentifier(ASTPtr & ast, const String & expression_name, const String & identifier_name)
{
    if (!ast)
        return;

    if ((ast->as<ASTIdentifier>() || ast->as<ASTFunction>()) && ast->getColumnName() == expression_name)
    {
        ast = make_intrusive<ASTIdentifier>(identifier_name);
        return;
    }

    for (auto & child : ast->children)
        replaceExpressionToIdentifier(child, expression_name, identifier_name);
}

void collectIdentifierNames(const IAST & ast, NameSet & names)
{
    if (const auto * identifier = ast.as<ASTIdentifier>())
        names.insert(identifier->name());

    for (const auto & child : ast.children)
        collectIdentifierNames(*child, names);
}

/// A subquery or a table in `IN` builds a set that nothing fills outside a `SELECT` pipeline.
void checkTransformHasNoSubqueries(const IAST & ast, const String & transform_name)
{
    if (ast.as<ASTSubquery>())
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "The {} expression must not contain subqueries", transform_name);

    if (const auto * function = ast.as<ASTFunction>(); function && functionIsInOrGlobalInOperator(function->name))
    {
        const auto * arguments = function->arguments ? function->arguments->as<ASTExpressionList>() : nullptr;
        if (arguments && arguments->children.size() == 2
            && (arguments->children[1]->as<ASTIdentifier>() || arguments->children[1]->as<ASTTableIdentifier>()))
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "The {} expression must not contain a table in the 'IN' operator", transform_name);
    }

    for (const auto & child : ast.children)
        checkTransformHasNoSubqueries(*child, transform_name);
}

/// Formats a transform over the identifier `input_name` as a lambda with one argument, e.g. `x -> lower(x)`.
String serializeTransform(const ASTPtr & expression_ast, const String & input_name)
{
    NameSet identifier_names;
    collectIdentifierNames(*expression_ast, identifier_names);
    identifier_names.erase(input_name);

    /// Name the argument `x` unless the expression already uses that name, e.g. for its own lambda.
    String argument_name = "x";
    for (size_t i = 1; identifier_names.contains(argument_name); ++i)
        argument_name = "x" + std::to_string(i);

    ASTPtr body = expression_ast->clone();
    replaceExpressionToIdentifier(body, input_name, argument_name);

    /// Formatted as `x -> body`, as the parser marks a lambda.
    auto lambda = makeASTLambda({argument_name}, std::move(body));
    lambda->setIsOperator(true);
    lambda->setIsLambdaFunction(true);
    lambda->setKind(ASTFunction::Kind::LAMBDA_FUNCTION);
    return lambda->formatWithSecretsOneLine();
}

/// Parses a transform written as a lambda with one argument. Returns the name of the argument and the body.
std::pair<String, ASTPtr> parseTransform(const String & transform, const String & transform_name)
{
    ParserExpression parser;
    ASTPtr ast = parseQuery(parser, transform, "text index " + transform_name, 0, DBMS_DEFAULT_MAX_PARSER_DEPTH, DBMS_DEFAULT_MAX_PARSER_BACKTRACKS);

    const auto * lambda = ast->as<ASTFunction>();
    const auto * lambda_arguments = lambda && lambda->name == "lambda" && lambda->arguments && lambda->arguments->children.size() == 2
        ? lambda->arguments->children[0]->as<ASTFunction>()
        : nullptr;
    const auto * argument = lambda_arguments && lambda_arguments->name == "tuple" && lambda_arguments->arguments
            && lambda_arguments->arguments->children.size() == 1
        ? lambda_arguments->arguments->children[0]->as<ASTIdentifier>()
        : nullptr;

    if (!argument)
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "The {} must be a lambda function with one argument, e.g. `x -> lower(x)`, got '{}'", transform_name, transform);

    ASTPtr body = lambda->arguments->children[1];
    checkTransformHasNoSubqueries(*body, transform_name);
    return {argument->name(), std::move(body)};
}

/// Builds the actions of `expression_ast` over `input` with the analyzer, the only output being the expression.
ActionsDAG buildActionsDAG(const ASTPtr & expression_ast, const NameAndTypePair & input, const ContextPtr & context)
{
    auto execution_context = Context::createCopy(context ? context : Context::getGlobalContextInstance());

    /// Without a table alias, the input of the actions is named as the column.
    auto storage = std::make_shared<StorageDummy>(StorageID{"dummy", "dummy"}, ColumnsDescription(NamesAndTypesList{input}));
    auto table_expression = std::make_shared<TableNode>(std::move(storage), execution_context);

    auto expression = buildQueryTree(expression_ast, execution_context);
    QueryAnalyzer(/*only_analyze=*/ false).resolve(expression, table_expression, execution_context);
    assertNoAggregateFunctionNodes(expression, "in a text index transform");
    assertNoWindowFunctionNodes(expression, "in a text index transform");

    auto planner_context = std::make_shared<PlannerContext>(
        execution_context,
        std::make_shared<GlobalPlannerContext>(nullptr, nullptr, nullptr, FiltersForTableExpressionMap{}),
        SelectQueryOptions{});
    collectSetsAndSourceColumns(expression, planner_context, /*keep_alias_columns=*/ false);

    auto [actions_dag, correlated_subtrees] = buildActionsDAGFromExpressionNode(
        expression, {ColumnWithTypeAndName(nullptr, input.type, input.name)}, planner_context, {});
    correlated_subtrees.assertEmpty("in a text index transform");
    actions_dag.removeUnusedActions();

    return std::move(actions_dag);
}

bool isEmptyStringLiteral(const ASTPtr & ast)
{
    const auto * literal = ast->as<ASTLiteral>();
    return literal && literal->value.getType() == Field::Types::String && literal->value.safeGet<String>().empty();
}

bool isTokenIdentifier(const ASTPtr & ast, std::string_view token_name)
{
    const auto * identifier = ast->as<ASTIdentifier>();
    return identifier && identifier->name() == token_name;
}

/// Collects string literals from an `IN` right-hand side (literal / tuple / array); false if any isn't a string.
bool collectStringLiterals(const ASTPtr & ast, std::vector<String> & out)
{
    auto collect_from_container = [&out](const auto & elements) -> bool
    {
        for (const auto & element : elements)
        {
            if (element.getType() != Field::Types::String)
                return false;
            out.push_back(element.template safeGet<String>());
        }
        return true;
    };

    if (const auto * literal = ast->as<ASTLiteral>())
    {
        const Field & value = literal->value;
        if (value.getType() == Field::Types::String)
        {
            out.push_back(value.safeGet<String>());
            return true;
        }
        if (value.getType() == Field::Types::Tuple)
            return collect_from_container(value.safeGet<Tuple>());
        if (value.getType() == Field::Types::Array)
            return collect_from_container(value.safeGet<Array>());
        return false;
    }

    if (const auto * function = ast->as<ASTFunction>())
    {
        if ((function->name != "tuple" && function->name != "array") || !function->arguments)
            return false;
        for (const auto & child : function->arguments->children)
        {
            if (!collectStringLiterals(child, out))
                return false;
        }
        return true;
    }

    return false;
}

/// Extracts the drop set from `if`/`multiIf(token IN/NOT IN (<literals>), '', token)`; else nullopt.
std::optional<MergeTreeIndexTextInlineFilter> tryExtractInlineFilter(const ASTPtr & ast, std::string_view token_name)
{
    const auto * function = ast->as<ASTFunction>();
    if (!function || !function->arguments || function->arguments->children.size() != 3)
        return {};

    /// `if` and single-WHEN `multiIf` share the same 3-arg (cond, then, else) shape.
    if (function->name != "if" && function->name != "multiIf")
        return {};

    const auto & condition = function->arguments->children[0];
    const auto & then_branch = function->arguments->children[1];
    const auto & else_branch = function->arguments->children[2];

    bool drop_on_condition = false;
    if (isEmptyStringLiteral(then_branch) && isTokenIdentifier(else_branch, token_name))
        drop_on_condition = true;
    else if (isTokenIdentifier(then_branch, token_name) && isEmptyStringLiteral(else_branch))
        drop_on_condition = false;
    else
        return {};

    const auto * in_function = condition->as<ASTFunction>();
    if (!in_function || !in_function->arguments || in_function->arguments->children.size() != 2)
        return {};

    bool is_not_in = false;
    if (in_function->name == "in" || in_function->name == "globalIn")
        is_not_in = false;
    else if (in_function->name == "notIn" || in_function->name == "globalNotIn")
        is_not_in = true;
    else
        return {};

    if (!isTokenIdentifier(in_function->arguments->children[0], token_name))
        return {};

    std::vector<String> literals;
    if (!collectStringLiterals(in_function->arguments->children[1], literals) || literals.empty())
        return {};

    MergeTreeIndexTextInlineFilter filter;
    filter.drop_on_match = is_not_in ? !drop_on_condition : drop_on_condition;

    if (literals.size() > max_filter_set_size)
        return {};

    filter.tokens.reserve(literals.size());
    for (auto & literal : literals)
        filter.tokens.insert(std::move(literal));
    return filter;
}

String serializeIndexTransform(const ASTPtr & expression_ast, const IndexDescription & index)
{
    chassert(index.column_names.size() == 1);
    if (!expression_ast)
        return {};

    ASTPtr expression = expression_ast->clone();
    replaceExpressionToIdentifier(expression, index.column_names.front(), placeholder_name);
    return serializeTransform(expression, placeholder_name);
}

}

TextIndexTransforms::TextIndexTransforms(const ASTPtr & preprocessor_ast, const ASTPtr & postprocessor_ast, const IndexDescription & index)
    : TextIndexTransforms(
        serializeIndexTransform(preprocessor_ast, index),
        serializeIndexTransform(postprocessor_ast, index),
        index.data_types.front(),
        /*context=*/ nullptr)
{
}

TextIndexTransforms::TextIndexTransforms(
    const String & preprocessor_, const String & postprocessor_, const DataTypePtr & value_type, const ContextPtr & context)
    : serialized_preprocessor(normalizeTransform(preprocessor_, "preprocessor"))
    , serialized_postprocessor(normalizeTransform(postprocessor_, "postprocessor"))
{
    if (!serialized_preprocessor.empty())
    {
        const auto * array_type = typeid_cast<const DataTypeArray *>(value_type.get());
        preprocessor = buildTransform(serialized_preprocessor, "preprocessor", array_type ? array_type->getNestedType() : value_type, context);
        preprocessor_for_constant = buildTransform(serialized_preprocessor, "preprocessor", std::make_shared<DataTypeString>(), context);

        /// Only ASCII lower/upper applied directly to the value maps 1:1 by byte, so a token contains the same substrings as
        /// the original string and an ILIKE served from the dictionary agrees with ILIKE on the column. Nested expressions
        /// and lowerUTF8/upperUTF8 do not: ICU full case mapping turns non-ASCII characters into ASCII letters (`ß` into
        /// `SS`), inventing tokens that contain a needle which ILIKE never finds in the row.
        auto [argument_name, body] = parseTransform(serialized_preprocessor, "preprocessor");
        const auto * function = body->as<ASTFunction>();
        if (function && function->arguments && function->arguments->children.size() == 1)
        {
            const auto & name = getFunctionCanonicalNameIfAny(function->name);
            is_ascii_lower_or_upper = (name == "lower" || name == "upper") && isTokenIdentifier(function->arguments->children.front(), argument_name);
        }
    }

    if (!serialized_postprocessor.empty())
    {
        postprocessor = buildTransform(serialized_postprocessor, "postprocessor", std::make_shared<DataTypeString>(), context);

        /// Fast path: recognize IN/NOT IN filters so the granule builder can decide drops per distinct token.
        auto [argument_name, body] = parseTransform(serialized_postprocessor, "postprocessor");
        inline_filter = tryExtractInlineFilter(body, argument_name);
    }
}

std::shared_ptr<const TextIndexTransforms> TextIndexTransforms::createForFunction(
    const ColumnsWithTypeAndName & arguments, size_t first_argument, std::string_view function_name, const ContextPtr & context)
{
    auto read_transform = [&](size_t i) -> String
    {
        if (i >= arguments.size())
            return {};

        const auto & column = arguments[i].column;
        if (!column || !isColumnConst(*column) || (*column)[0].getType() != Field::Types::String)
            throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Argument {} of function '{}' must be a constant String", i + 1, function_name);

        return (*column)[0].safeGet<String>();
    };

    const String preprocessor = read_transform(first_argument);
    const String postprocessor = read_transform(first_argument + 1);

    /// A `NULL` input gives `NULL` without transforms. The default implementation for `LowCardinality` unwraps the input.
    const DataTypePtr value_type = removeLowCardinality(arguments.front().type);
    if ((preprocessor.empty() && postprocessor.empty()) || removeNullable(value_type)->onlyNull())
        return nullptr;

    return std::make_shared<TextIndexTransforms>(preprocessor, postprocessor, value_type, context);
}

String TextIndexTransforms::normalizeTransform(const String & transform, const String & transform_name)
{
    if (transform.empty())
        return {};

    auto [argument_name, body] = parseTransform(transform, transform_name);
    return serializeTransform(body, argument_name);
}

TextIndexTransforms::Transform TextIndexTransforms::buildTransform(
    const String & transform, const String & transform_name, const DataTypePtr & input_type, const ContextPtr & context)
{
    auto [input_name, body] = parseTransform(transform, transform_name);
    auto actions_dag = buildActionsDAG(body, {input_name, input_type}, context);

    const auto & outputs = actions_dag.getOutputs();
    if (outputs.size() != 1)
        throw Exception(ErrorCodes::INCORRECT_QUERY, "The {} expression must return a single column. Got {} output columns", transform_name, outputs.size());

    if (outputs.front()->type != ActionsDAG::ActionType::FUNCTION)
        throw Exception(ErrorCodes::INCORRECT_QUERY, "The {} expression must be a function. Got '{}' action type", transform_name, outputs.front()->type);

    if (actions_dag.hasNonDeterministic())
        throw Exception(ErrorCodes::INCORRECT_QUERY, "The {} expression must not contain non-deterministic functions", transform_name);

    if (actions_dag.hasArrayJoin())
        throw Exception(ErrorCodes::INCORRECT_QUERY, "The {} expression must not contain arrayJoin", transform_name);

    /// The postprocessor maps a token to a token, the preprocessor may keep a `FixedString` or `Nullable` value.
    const auto & output_type = outputs.front()->result_type;
    const bool is_string_output = transform_name == "postprocessor"
        ? isString(output_type)
        : isStringOrFixedString(removeNullable(removeLowCardinality(output_type)));

    if (!is_string_output)
        throw Exception(ErrorCodes::INCORRECT_QUERY,
            "The {} expression must return a String, got {} for an input of type {}", transform_name, output_type->getName(), input_type->getName());

    return {ExpressionActions(std::move(actions_dag)), std::move(input_name), input_type};
}

ColumnPtr TextIndexTransforms::executeTransform(const Transform & transform, ColumnPtr column)
{
    const size_t num_rows = column->size();
    Block block({ColumnWithTypeAndName(std::move(column), transform.input_type, transform.input_name)});
    transform.actions.execute(block, num_rows);
    return block.safeGetByPosition(0).column->convertToFullColumnIfConst();
}

std::pair<ColumnPtr, size_t> TextIndexTransforms::preprocessColumn(const ColumnPtr & column, size_t start_row, size_t n_rows) const
{
    if (!preprocessor)
        return {column, start_row};

    /// Only copy if needed
    ColumnPtr rows = start_row != 0 || n_rows != column->size() ? column->cut(start_row, n_rows) : column;

    if (const auto * column_array = typeid_cast<const ColumnArray *>(rows.get()))
        return {ColumnArray::create(executeTransform(*preprocessor, column_array->getDataPtr()), column_array->getOffsetsPtr()), 0};

    return {executeTransform(*preprocessor, std::move(rows)), 0};
}

ColumnPtr TextIndexTransforms::tokenizeColumn(const ITokenizer & tokenizer, const IColumn & column, size_t start_row, size_t n_rows) const
{
    chassert(postprocessor);

    /// Apply the postprocessor to the tokens of all rows in one execution. It maps each token 1:1, so the offsets stay.
    auto tokens = tokenizeToArray(tokenizer, column, start_row, n_rows);
    const auto & tokens_array = assert_cast<const ColumnArray &>(*tokens);
    return ColumnArray::create(executeTransform(*postprocessor, tokens_array.getDataPtr()), tokens_array.getOffsetsPtr());
}

String TextIndexTransforms::preprocessConstant(const String & value) const
{
    if (!preprocessor_for_constant)
        return value;

    auto input = preprocessor_for_constant->input_type->createColumnConst(1, Field(value));
    return String(executeTransform(*preprocessor_for_constant, std::move(input))->getDataAt(0));
}

VectorWithMemoryTracking<String> TextIndexTransforms::processTokens(VectorWithMemoryTracking<String> tokens) const
{
    if (!postprocessor || tokens.empty())
        return tokens;

    auto input = ColumnString::create();
    input->reserve(tokens.size());
    for (const auto & token : tokens)
        input->insertData(token.data(), token.size());

    auto result = executeTransform(*postprocessor, std::move(input));

    tokens.clear();
    tokens.reserve(result->size());
    for (size_t i = 0; i < result->size(); ++i)
    {
        if (auto token = result->getDataAt(i); !token.empty())
            tokens.emplace_back(token);
    }
    return tokens;
}

VectorWithMemoryTracking<String> TextIndexTransforms::stringToTokens(std::string_view value, const ITokenizer & tokenizer) const
{
    const String preprocessed = preprocessConstant(String(value));
    VectorWithMemoryTracking<String> tokens;
    tokenizer.stringToTokens(preprocessed.data(), preprocessed.size(), tokens);
    return processTokens(std::move(tokens));
}

ColumnPtr TextIndexTransforms::transformFunctionInput(const ColumnPtr & column, const ITokenizer & tokenizer, ColumnPtr & null_map) const
{
    auto result = recursiveRemoveLowCardinality(preprocessColumn(column, 0, column->size()).first);

    if (const auto * column_nullable = typeid_cast<const ColumnNullable *>(result.get()))
    {
        null_map = column_nullable->getNullMapColumnPtr();
        result = column_nullable->getNestedColumnPtr();
    }

    return postprocessor ? tokenizeColumn(tokenizer, *result, 0, result->size()) : result;
}

const ITokenizer & TextIndexTransforms::getSearchTokenizer(const ITokenizer & tokenizer) const
{
    /// The tokens are the elements of the transformed input; a dropped one is an empty element, which has no token.
    return postprocessor ? array_tokenizer : tokenizer;
}

ColumnPtr TextIndexTransforms::wrapFunctionResult(ColumnPtr result, ColumnPtr null_map, const DataTypePtr & result_type)
{
    if (!result_type->isNullable())
        return result;

    if (!null_map)
        null_map = ColumnUInt8::create(result->size(), UInt8(0));

    return ColumnNullable::create(std::move(result), std::move(null_map));
}

}
