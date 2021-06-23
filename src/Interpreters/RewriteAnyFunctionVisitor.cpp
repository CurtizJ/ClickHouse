#include <Common/typeid_cast.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTSubquery.h>
#include <Interpreters/RewriteAnyFunctionVisitor.h>
#include <AggregateFunctions/AggregateFunctionFactory.h>
#include <IO/WriteHelpers.h>
#include <Parsers/ASTTablesInSelectQuery.h>

namespace DB
{

namespace
{

bool extractIdentifiers(const ASTFunction & func, std::unordered_set<ASTPtr *> & identifiers)
{
    for (auto & arg : func.arguments->children)
    {
        if (const auto * arg_func = arg->as<ASTFunction>())
        {
            /// arrayJoin() is special and should not be optimized (think about
            /// it as a an aggregate function), otherwise wrong result will be
            /// produced:
            ///     SELECT *, any(arrayJoin([[], []])) FROM numbers(1) GROUP BY number
            ///     ┌─number─┬─arrayJoin(array(array(), array()))─┐
            ///     │      0 │ []                                 │
            ///     │      0 │ []                                 │
            ///     └────────┴────────────────────────────────────┘
            /// While should be:
            ///     ┌─number─┬─any(arrayJoin(array(array(), array())))─┐
            ///     │      0 │ []                                      │
            ///     └────────┴─────────────────────────────────────────┘
            if (arg_func->name == "arrayJoin")
                return false;

            if (arg_func->name == "lambda")
                return false;

            /// Do not optimize 'nullIf', because function 'any' skip NULLs and
            /// wrong result will be produced if we will apply it to arguments first.
            if (arg_func->name == "nullIf")
                return false;

            // We are looking for identifiers inside a function calculated inside
            // the aggregate function `any()`. Window or aggregate function can't
            // be inside `any`, but this check in GetAggregatesMatcher happens
            // later, so we have to explicitly skip these nested functions here.
            if (arg_func->is_window_function
                || AggregateFunctionFactory::instance().isAggregateFunctionName(
                    arg_func->name))
            {
                return false;
            }

            if (!extractIdentifiers(*arg_func, identifiers))
                return false;
        }
        else if (arg->as<ASTIdentifier>())
            identifiers.emplace(&arg);
    }

    return true;
}

}


void RewriteAnyFunctionMatcher::visit(ASTPtr & ast, Data & data)
{
    if (auto * func = ast->as<ASTFunction>())
        visit(*func, ast, data);
}

void RewriteAnyFunctionMatcher::visit(const ASTFunction & func, ASTPtr & ast, Data & data)
{
    if (!func.arguments || func.arguments->children.size() != 1 || !func.arguments->children[0])
        return;

    if (func.name != "any" && func.name != "anyLast")
        return;

    auto & func_arguments = func.arguments->children;
    const auto * first_arg_func = func_arguments[0]->as<ASTFunction>();
    if (!first_arg_func || first_arg_func->arguments->children.empty())
        return;

    /// We have rewritten this function.
    auto it = data.rewritten.find(ast.get());
    if (it != data.rewritten.end())
    {
        ast = it->second;
        return;
    }

    auto new_ast = func.clone();
    auto new_func = new_ast->as<ASTFunction>();
    auto & new_func_arguments = new_func->arguments->children;

    std::unordered_set<ASTPtr *> identifiers; /// implicit remove duplicates
    if (!extractIdentifiers(*new_func, identifiers) || identifiers.empty())
        return;

    /// Do not apply optimization, if we have nullable arguments,
    /// because, function `any` skips NULLs and result may be inconsistent,
    /// if we will apply `any` to arguments first.
    /// Consider we have table:
    /// ┌────A─┬────B─┐
    /// │    1 │ ᴺᵁᴸᴸ │
    /// │ ᴺᵁᴸᴸ │    2 │
    /// └──────┴──────┘
    /// `any(tuple(A, B))` will return (1, NULL)
    /// while `tuple(any(A), any(B))` will return (1, 2), which is incorrect.
    const auto & columns = data.metadata_snapshot->getColumns();
    for (const auto & ident : identifiers)
    {
        const auto & column_name = (*ident)->getColumnName();
        if (!columns.has(column_name) || columns.get(column_name).type->isNullable())
            return;
    }

    /// Wrap identifiers: any(f(x, y, g(z))) -> any(f(any(x), any(y), g(any(z))))
    for (auto * ast_to_change : identifiers)
    {
        ASTPtr identifier_ast = *ast_to_change;
        *ast_to_change = makeASTFunction(func.name);
        (*ast_to_change)->as<ASTFunction>()->arguments->children.emplace_back(identifier_ast);
    }

    data.rewritten.emplace(ast.get(), new_func_arguments[0]);

    /// Unwrap function: any(f(any(x), any(y), g(any(z)))) -> f(any(x), any(y), g(any(z)))
    new_func_arguments[0]->setAlias(func.alias);
    ast = new_func_arguments[0];
}

bool RewriteAnyFunctionMatcher::needChildVisit(const ASTPtr & node, const ASTPtr &)
{
    return !node->as<ASTSubquery>() &&
        !node->as<ASTTableExpression>() &&
        !node->as<ASTArrayJoin>();
}

}
