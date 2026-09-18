#pragma once
#include <memory>
#include <base/types.h>

namespace DB
{

class IFunctionOverloadResolver;
using FunctionOverloadResolverPtr = std::shared_ptr<IFunctionOverloadResolver>;

struct TopKThresholdTracker;
using TopKThresholdTrackerPtr = std::shared_ptr<TopKThresholdTracker>;

FunctionOverloadResolverPtr createInternalFunctionTopKFilterResolver(TopKThresholdTrackerPtr threshold_tracker_);

/// Name of the filter column that `__topKFilter(column)` produces in an `ActionsDAG`.
String getTopKFilterColumnName(const String & column_name);

}
