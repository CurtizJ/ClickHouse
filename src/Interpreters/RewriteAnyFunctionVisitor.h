#pragma once

#include <unordered_set>

#include <Parsers/IAST.h>
#include <Interpreters/InDepthNodeVisitor.h>
#include <Storages/StorageInMemoryMetadata.h>


namespace DB
{

class ASTFunction;

/// Rewrite 'any' and 'anyLast' functions pushing them inside original function.
/// any(f(x, y, g(z))) -> f(any(x), any(y), g(any(z)))
class RewriteAnyFunctionMatcher
{
public:
    struct Data
    {
        Data(const StorageMetadataPtr & metadata_snapshot_)
            : metadata_snapshot(metadata_snapshot_)
        {
        }

        StorageMetadataPtr metadata_snapshot;
        std::unordered_map<IAST *, ASTPtr> rewritten;
    };

    static void visit(ASTPtr & ast, Data & data);
    static void visit(const ASTFunction &, ASTPtr & ast, Data & data);
    static bool needChildVisit(const ASTPtr & node, const ASTPtr & child);
};
using RewriteAnyFunctionVisitor = InDepthNodeVisitor<RewriteAnyFunctionMatcher, false>;

}
